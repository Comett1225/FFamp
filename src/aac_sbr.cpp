#include "aac.h"
#include <algorithm>
#include <bit>
#include <cfloat>
#include <cmath>
#include <mutex>
#if defined(__x86_64__) || defined(_M_X64)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

namespace absent::aac {

#if defined(__x86_64__) || defined(_M_X64)
bool fastFma() {
    static const bool yes = [] {
        unsigned c = 0;
#if defined(_MSC_VER)
        int r[4];
        __cpuid(r, 1);
        c = unsigned(r[2]);
#else
        unsigned a, b, d;
        if (!__get_cpuid(1, &a, &b, &c, &d)) return false;
#endif
        if (!(c & (1u << 12)) || !(c & (1u << 27)) || !(c & (1u << 28))) return false;
        unsigned lo, hi;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        return (lo & 6) == 6;
    }();
    return yes;
}

__attribute__((target("fma"))) float fmaHardware(float a, float b, float c) { return __builtin_fmaf(a, b, c); }
#else
bool fastFma() { return false; }

float fmaHardware(float a, float b, float c) { return std::fma(a, b, c); }
#endif

namespace {

const int kEnvelopeOffset = 2;
const int kNoiseFloorOffset = 6;
const int kTimeSlots = 16;
const int kSynthesisSize = (1280 - 128) * 2;

enum { kTEnv15, kFEnv15, kTEnvBal15, kFEnvBal15, kTEnv30, kFEnv30, kTEnvBal30, kFEnvBal30, kTNoise30, kTNoiseBal30 };
enum { kFixFix, kFixVar, kVarFix, kVarVar };

struct Tables {
    Vlc sbr[10];
    Vlc ps[10];
    float pdReSmooth[512], pdImSmooth[512];
    float HA[46][8][4], HB[46][8][4];
    alignas(16) float f20[8][8][2];
    alignas(16) float f34a[12][8][2];
    alignas(16) float f34b[8][8][2];
    alignas(16) float f34c[4][8][2];
    alignas(16) float qFractAllpass[2][50][3][2];
    alignas(16) float phiFract[2][50][2];
};

void makeFilters(float (*filter)[8][2], const float* proto, int bands) {
    for (int q = 0; q < bands; q++) {
        for (int n = 0; n < 7; n++) {
            double theta = 2 * M_PI * (q + 0.5) * (n - 6) / bands;
            filter[q][n][0] = float(proto[n] * cos(theta));
            filter[q][n][1] = float(proto[n] * -sin(theta));
        }
    }
}

void psTables(Tables& t) {
    static const float ipdopdSin[] = {0, float(M_SQRT1_2), 1, float(M_SQRT1_2), 0, float(-M_SQRT1_2), -1, float(-M_SQRT1_2)};
    static const float ipdopdCos[] = {1, float(M_SQRT1_2), 0, float(-M_SQRT1_2), -1, float(-M_SQRT1_2), 0, float(M_SQRT1_2)};
    static const float iidParDequant[] = {
        0.05623413251903f, 0.12589254117942f, 0.19952623149689f, 0.31622776601684f, 0.44668359215096f, 0.63095734448019f, 0.79432823472428f, 1.0f,
        1.25892541179417f, 1.58489319246111f, 2.23872113856834f, 3.16227766016838f, 5.01187233627272f, 7.94328234724282f, 17.7827941003892f,
        0.00316227766017f, 0.00562341325190f, 0.01f,             0.01778279410039f, 0.03162277660168f, 0.05623413251903f, 0.07943282347243f,
        0.11220184543020f, 0.15848931924611f, 0.22387211385683f, 0.31622776601684f, 0.39810717055350f, 0.50118723362727f, 0.63095734448019f,
        0.79432823472428f, 1.0f,              1.25892541179417f, 1.58489319246111f, 1.99526231496888f, 2.51188643150958f, 3.16227766016838f,
        4.46683592150963f, 6.30957344480193f, 8.91250938133745f, 12.5892541179417f, 17.7827941003892f, 31.6227766016838f, 56.2341325190349f,
        100.0f,            177.827941003892f, 316.227766016837f};
    static const float iccInvq[] = {1, 0.937f, 0.84118f, 0.60092f, 0.36764f, 0, -0.589f, -1};
    static const float acosIccInvq[] = {0, 0.35685527f, 0.57133466f, 0.92614472f, 1.1943263f, float(M_PI / 2), 2.2006171f, float(M_PI)};
    static const int8_t fCenter20[] = {-3, -1, 1, 3, 5, 7, 10, 14, 18, 22};
    static const int8_t fCenter34[] = {2,  6,  10, 14, 18, 22, 26, 30, 34, -10, -6, -2, 51,  57, 15, 21,
                                       27, 33, 39, 45, 54, 66, 78, 42, 102, 66, 78, 90, 102, 114, 126, 90};
    static const float fractionalDelayLinks[] = {0.43f, 0.75f, 0.347f};
    const float fractionalDelayGain = 0.39f;
    static const float g0Q8[] = {0.00746082949812f, 0.02270420949825f, 0.04546865930473f, 0.07266113929591f, 0.09885108575264f, 0.11793710567217f, 0.125f};
    static const float g0Q12[] = {0.04081179924692f, 0.03812810994926f, 0.05144908135699f, 0.06399831151592f, 0.07428313801106f, 0.08100347892914f, 0.08333333333333f};
    static const float g1Q8[] = {0.01565675600122f, 0.03752716391991f, 0.05417891378782f, 0.08417044116767f, 0.10307344158036f, 0.12222452249753f, 0.125f};
    static const float g2Q4[] = {-0.05908211155639f, -0.04871498374946f, 0.0f, 0.07778723915851f, 0.16486303567403f, 0.23279856662996f, 0.25f};

    for (int pd0 = 0; pd0 < 8; pd0++) {
        float pd0Re = ipdopdCos[pd0], pd0Im = ipdopdSin[pd0];
        for (int pd1 = 0; pd1 < 8; pd1++) {
            float pd1Re = ipdopdCos[pd1], pd1Im = ipdopdSin[pd1];
            for (int pd2 = 0; pd2 < 8; pd2++) {
                float pd2Re = ipdopdCos[pd2], pd2Im = ipdopdSin[pd2];
                float reSmooth = 0.25f * pd0Re + 0.5f * pd1Re + pd2Re;
                float imSmooth = 0.25f * pd0Im + 0.5f * pd1Im + pd2Im;
                float pdMag = float(1 / hypot(imSmooth, reSmooth));
                t.pdReSmooth[pd0 * 64 + pd1 * 8 + pd2] = reSmooth * pdMag;
                t.pdImSmooth[pd0 * 64 + pd1 * 8 + pd2] = imSmooth * pdMag;
            }
        }
    }
    for (int iid = 0; iid < 46; iid++) {
        float c = iidParDequant[iid];
        float c1 = float(M_SQRT2) / sqrtf(1.0f + c * c);
        float c2 = c * c1;
        for (int icc = 0; icc < 8; icc++) {
            {
                float alpha = 0.5f * acosIccInvq[icc];
                float beta = alpha * (c1 - c2) * float(M_SQRT1_2);
                t.HA[iid][icc][0] = c2 * cosf(beta + alpha);
                t.HA[iid][icc][1] = c1 * cosf(beta - alpha);
                t.HA[iid][icc][2] = c2 * sinf(beta + alpha);
                t.HA[iid][icc][3] = c1 * sinf(beta - alpha);
            }
            {
                float rho = std::max(iccInvq[icc], 0.05f);
                float alpha = 0.5f * atan2f(2.0f * c * rho, c * c - 1.0f);
                float mu = c + 1.0f / c;
                mu = sqrtf(1 + (4 * rho * rho - 4) / (mu * mu));
                float gamma = atanf(sqrtf((1.0f - mu) / (1.0f + mu)));
                if (alpha < 0) alpha = float(alpha + M_PI / 2);
                float alphaC = cosf(alpha), alphaS = sinf(alpha), gammaC = cosf(gamma), gammaS = sinf(gamma);
                t.HB[iid][icc][0] = float(M_SQRT2 * alphaC * gammaC);
                t.HB[iid][icc][1] = float(M_SQRT2 * alphaS * gammaC);
                t.HB[iid][icc][2] = float(-M_SQRT2 * alphaS * gammaS);
                t.HB[iid][icc][3] = float(M_SQRT2 * alphaC * gammaS);
            }
        }
    }
    for (int k = 0; k < 30; k++) {
        double fCenter = k < 10 ? fCenter20[k] * 0.125 : k - 6.5f;
        for (int m = 0; m < 3; m++) {
            double theta = -M_PI * fractionalDelayLinks[m] * fCenter;
            t.qFractAllpass[0][k][m][0] = float(cos(theta));
            t.qFractAllpass[0][k][m][1] = float(sin(theta));
        }
        double theta = -M_PI * fractionalDelayGain * fCenter;
        t.phiFract[0][k][0] = float(cos(theta));
        t.phiFract[0][k][1] = float(sin(theta));
    }
    for (int k = 0; k < 50; k++) {
        double fCenter = k < 32 ? fCenter34[k] / 24.0 : k - 26.5f;
        for (int m = 0; m < 3; m++) {
            double theta = -M_PI * fractionalDelayLinks[m] * fCenter;
            t.qFractAllpass[1][k][m][0] = float(cos(theta));
            t.qFractAllpass[1][k][m][1] = float(sin(theta));
        }
        double theta = -M_PI * fractionalDelayGain * fCenter;
        t.phiFract[1][k][0] = float(cos(theta));
        t.phiFract[1][k][1] = float(sin(theta));
    }
    makeFilters(t.f20, g0Q8, 8);
    makeFilters(t.f34a, g0Q12, 12);
    makeFilters(t.f34b, g1Q8, 8);
    makeFilters(t.f34c, g2Q4, 4);
}

void buildPairs(Vlc& vlc, int bits, const uint8_t (*tab)[2], int count, int offset) {
    uint8_t lengths[128], symbols[128];
    for (int i = 0; i < count; i++) {
        symbols[i] = tab[i][0];
        lengths[i] = tab[i][1];
    }
    vlc.buildInOrder(bits, size_t(count), lengths, symbols, offset);
}

const Tables& tables() {
    static Tables* made = nullptr;
    static std::once_flag once;
    std::call_once(once, [] {
        auto* t = new Tables();
        const uint8_t (*tab)[2] = kSbrHuffman;
        for (int i = 0; i < 10; i++) {
            buildPairs(t->sbr[i], 9, tab, kSbrHuffmanCounts[i], kSbrHuffmanOffsets[i]);
            tab += kSbrHuffmanCounts[i];
        }
        tab = kPsHuffman;
        for (int i = 0; i < 10; i++) {
            buildPairs(t->ps[i], i <= 5 ? 9 : 5, tab, kPsHuffmanSizes[i], kPsHuffmanOffsets[i]);
            tab += kPsHuffmanSizes[i];
        }
        psTables(*t);
        made = t;
    });
    return *made;
}

float floatOf(uint32_t u) { return std::bit_cast<float>(u); }

float exp2fi(int x) {
    if (-126 <= x && x <= 128) return floatOf(uint32_t(x + 127) << 23);
    if (x > 128) return INFINITY;
    if (x > -150) return floatOf(1u << (x + 149));
    return 0;
}

const int8_t kNumEnvTab[2][4] = {{0, 1, 2, 4}, {1, 2, 3, 4}};
const int8_t kNrIidIccParTab[] = {10, 20, 34, 10, 20, 34};
const int8_t kNrIidOpdParTab[] = {5, 11, 17, 5, 11, 17};
enum { kIidDf1, kIidDt1, kIidDf0, kIidDt0, kIccDf, kIccDt, kIpdDf, kIpdDt, kOpdDf, kOpdDt };
const int kHuffIid[] = {kIidDf0, kIidDf1, kIidDt0, kIidDt1};

enum class Par { Iid, Icc, Ipdopd };

int readParData(BitReader& gb, PsCommon& ps, int8_t (*par)[34], int tableIdx, int e, int dt, Par kind) {
    const Vlc& vlc = tables().ps[tableIdx];
    int num = kind == Par::Iid ? ps.nrIidPar : kind == Par::Icc ? ps.nrIccPar : ps.nrIpdopdPar;
    auto bad = [&](int b) {
        if (kind == Par::Iid) return std::abs(int(ps.iidPar[e][b])) > 7 + 8 * ps.iidQuant;
        if (kind == Par::Icc) return unsigned(ps.iccPar[e][b]) > 7u;
        return false;
    };
    if (dt) {
        int ePrev = e ? e - 1 : ps.numEnvOld - 1;
        ePrev = std::max(ePrev, 0);
        for (int b = 0; b < num; b++) {
            int val = par[ePrev][b] + vlc.read(gb);
            if (kind == Par::Ipdopd) val &= 7;
            par[e][b] = int8_t(val);
            if (bad(b)) return -1;
        }
    } else {
        int val = 0;
        for (int b = 0; b < num; b++) {
            val += vlc.read(gb);
            if (kind == Par::Ipdopd) val &= 7;
            par[e][b] = int8_t(val);
            if (bad(b)) return -1;
        }
    }
    return 0;
}

int psReadExtension(BitReader& gb, PsCommon& ps, int id) {
    int64_t count = gb.position();
    if (id) return 0;
    ps.enableIpdopd = gb.bit();
    if (ps.enableIpdopd) {
        for (int e = 0; e < ps.numEnv; e++) {
            int dt = gb.bit();
            readParData(gb, ps, ps.ipdPar, dt ? kIpdDt : kIpdDf, e, dt, Par::Ipdopd);
            dt = gb.bit();
            readParData(gb, ps, ps.opdPar, dt ? kOpdDt : kOpdDf, e, dt, Par::Ipdopd);
        }
    }
    gb.skip(1);
    return int(gb.position() - count);
}

int psReadData(BitReader& host, PsCommon& ps, int bitsLeft) {
    BitReader gb = host;
    const int64_t start = gb.position();
    auto fail = [&] {
        ps.start = 0;
        host.skip(bitsLeft);
        memset(ps.iidPar, 0, sizeof ps.iidPar);
        memset(ps.iccPar, 0, sizeof ps.iccPar);
        memset(ps.ipdPar, 0, sizeof ps.ipdPar);
        memset(ps.opdPar, 0, sizeof ps.opdPar);
        return bitsLeft;
    };
    int header = gb.bit();
    if (header) {
        ps.enableIid = gb.bit();
        if (ps.enableIid) {
            int iidMode = int(gb.read(3));
            if (iidMode > 5) return fail();
            ps.nrIidPar = kNrIidIccParTab[iidMode];
            ps.iidQuant = iidMode > 2;
            ps.nrIpdopdPar = kNrIidOpdParTab[iidMode];
        }
        ps.enableIcc = gb.bit();
        if (ps.enableIcc) {
            ps.iccMode = int(gb.read(3));
            if (ps.iccMode > 5) return fail();
            ps.nrIccPar = kNrIidIccParTab[ps.iccMode];
        }
        ps.enableExt = gb.bit();
    }
    ps.frameClass = gb.bit();
    ps.numEnvOld = ps.numEnv;
    ps.numEnv = kNumEnvTab[ps.frameClass][gb.read(2)];
    ps.borderPosition[0] = -1;
    if (ps.frameClass) {
        for (int e = 1; e <= ps.numEnv; e++) {
            ps.borderPosition[e] = int(gb.read(5));
            if (ps.borderPosition[e] < ps.borderPosition[e - 1]) return fail();
        }
    } else {
        static const int log2Tab[] = {0, 0, 1, 1, 2};
        for (int e = 1; e <= ps.numEnv; e++) ps.borderPosition[e] = (e * 32 >> log2Tab[ps.numEnv]) - 1;
    }
    if (ps.enableIid) {
        for (int e = 0; e < ps.numEnv; e++) {
            int dt = gb.bit();
            if (readParData(gb, ps, ps.iidPar, kHuffIid[2 * dt + ps.iidQuant], e, dt, Par::Iid)) return fail();
        }
    } else {
        memset(ps.iidPar, 0, sizeof ps.iidPar);
    }
    if (ps.enableIcc) {
        for (int e = 0; e < ps.numEnv; e++) {
            int dt = gb.bit();
            if (readParData(gb, ps, ps.iccPar, dt ? kIccDt : kIccDf, e, dt, Par::Icc)) return fail();
        }
    } else {
        memset(ps.iccPar, 0, sizeof ps.iccPar);
    }
    if (ps.enableExt) {
        int cnt = int(gb.read(4));
        if (cnt == 15) cnt += int(gb.read(8));
        cnt *= 8;
        while (cnt > 7) {
            int id = int(gb.read(2));
            cnt -= 2 + psReadExtension(gb, ps, id);
        }
        if (cnt < 0) return fail();
        gb.skip(cnt);
    }
    if (!ps.numEnv || ps.borderPosition[ps.numEnv] < 31) {
        int source = ps.numEnv ? ps.numEnv - 1 : ps.numEnvOld - 1;
        if (source >= 0 && source != ps.numEnv) {
            if (ps.enableIid) memcpy(ps.iidPar + ps.numEnv, ps.iidPar + source, sizeof ps.iidPar[0]);
            if (ps.enableIcc) memcpy(ps.iccPar + ps.numEnv, ps.iccPar + source, sizeof ps.iccPar[0]);
            if (ps.enableIpdopd) {
                memcpy(ps.ipdPar + ps.numEnv, ps.ipdPar + source, sizeof ps.ipdPar[0]);
                memcpy(ps.opdPar + ps.numEnv, ps.opdPar + source, sizeof ps.opdPar[0]);
            }
        }
        if (ps.enableIid) {
            for (int b = 0; b < ps.nrIidPar; b++)
                if (std::abs(int(ps.iidPar[ps.numEnv][b])) > 7 + 8 * ps.iidQuant) return fail();
        }
        if (ps.enableIcc) {
            for (int b = 0; b < ps.nrIidPar; b++)
                if (unsigned(ps.iccPar[ps.numEnv][b]) > 7u) return fail();
        }
        ps.numEnv++;
        ps.borderPosition[ps.numEnv] = 31;
    }
    ps.is34bandsOld = ps.is34bands;
    if (ps.enableIid || ps.enableIcc) ps.is34bands = (ps.enableIid && ps.nrIidPar == 34) || (ps.enableIcc && ps.nrIccPar == 34);
    if (!ps.enableIpdopd) {
        memset(ps.ipdPar, 0, sizeof ps.ipdPar);
        memset(ps.opdPar, 0, sizeof ps.opdPar);
    }
    if (header) ps.start = 1;
    int consumed = int(gb.position() - start);
    if (consumed <= bitsLeft) {
        host.skip(consumed);
        return consumed;
    }
    return fail();
}

const float kG1Q2[] = {0.0f, 0.01899487526049f, 0.0f, -0.07293139167538f, 0.0f, 0.30596630545168f, 0.5f};

void hybridAnalysisDsp(float (*out)[2], float (*in)[2], const float (*filter)[8][2], ptrdiff_t stride, int n) {
    float inre0[6], inre1[6], inim0[6], inim1[6];
    for (int j = 0; j < 6; j++) {
        inre0[j] = in[j][0] + in[12 - j][0];
        inre1[j] = in[j][1] - in[12 - j][1];
        inim0[j] = in[j][1] + in[12 - j][1];
        inim1[j] = in[j][0] - in[12 - j][0];
    }
    for (int i = 0; i < n; i++) {
        const float(*f)[2] = filter[i];
        float pe = f[0][0] * inre0[0], qe = f[0][1] * inre1[0], se = f[0][0] * inim0[0], te = f[0][1] * inim1[0];
        float po = f[1][0] * inre0[1], qo = f[1][1] * inre1[1], so = f[1][0] * inim0[1], to = f[1][1] * inim1[1];
        for (int j = 2; j < 6; j += 2) {
            pe = mulAdd(f[j][0], inre0[j], pe);
            qe = mulAdd(f[j][1], inre1[j], qe);
            se = mulAdd(f[j][0], inim0[j], se);
            te = mulAdd(f[j][1], inim1[j], te);
            po = mulAdd(f[j + 1][0], inre0[j + 1], po);
            qo = mulAdd(f[j + 1][1], inre1[j + 1], qo);
            so = mulAdd(f[j + 1][0], inim0[j + 1], so);
            to = mulAdd(f[j + 1][1], inim1[j + 1], to);
        }
        const float re = (po - qo) + (pe - qe);
        const float im = (so + to) + (se + te);
        out[i * stride][0] = mulAdd(in[6][0], f[6][0], re);
        out[i * stride][1] = mulAdd(in[6][1], f[6][0], im);
    }
}

void hybrid2Re(float (*in)[2], float (*out)[32][2], const float filter[7], int len, int reverse) {
    for (int i = 0; i < len; i++, in++) {
        float reIn = filter[6] * in[6][0];
        float reOp = 0.0f;
        float imIn = filter[6] * in[6][1];
        float imOp = 0.0f;
        for (int j = 0; j < 6; j += 2) {
            reOp += filter[j + 1] * (in[j + 1][0] + in[12 - j - 1][0]);
            imOp += filter[j + 1] * (in[j + 1][1] + in[12 - j - 1][1]);
        }
        out[reverse][i][0] = reIn + reOp;
        out[reverse][i][1] = imIn + imOp;
        out[!reverse][i][0] = reIn - reOp;
        out[!reverse][i][1] = imIn - imOp;
    }
}

void hybrid6Cx(float (*in)[2], float (*out)[32][2], const float (*filter)[8][2], int len) {
    alignas(16) float temp[8][2];
    for (int i = 0; i < len; i++, in++) {
        hybridAnalysisDsp(temp, in, filter, 1, 8);
        out[0][i][0] = temp[6][0];
        out[0][i][1] = temp[6][1];
        out[1][i][0] = temp[7][0];
        out[1][i][1] = temp[7][1];
        out[2][i][0] = temp[0][0];
        out[2][i][1] = temp[0][1];
        out[3][i][0] = temp[1][0];
        out[3][i][1] = temp[1][1];
        out[4][i][0] = temp[2][0] + temp[5][0];
        out[4][i][1] = temp[2][1] + temp[5][1];
        out[5][i][0] = temp[3][0] + temp[4][0];
        out[5][i][1] = temp[3][1] + temp[4][1];
    }
}

void hybrid4812Cx(float (*in)[2], float (*out)[32][2], const float (*filter)[8][2], int n, int len) {
    for (int i = 0; i < len; i++, in++) hybridAnalysisDsp(out[0] + i, in, filter, 32, n);
}

void analysisInterleave(float (*out)[32][2], float L[2][38][64], int i, int len) {
    for (; i < 64; i++) {
        for (int j = 0; j < len; j++) {
            out[i][j][0] = L[0][j][i];
            out[i][j][1] = L[1][j][i];
        }
    }
}

void synthesisDeinterleave(float out[2][38][64], float (*in)[32][2], int i, int len) {
    for (; i < 64; i++) {
        for (int n = 0; n < len; n++) {
            out[0][n][i] = in[i][n][0];
            out[1][n][i] = in[i][n][1];
        }
    }
}

void hybridAnalysis(float out[91][32][2], float in[5][44][2], float L[2][38][64], int is34, int len) {
    const Tables& t = tables();
    for (int i = 0; i < 5; i++) {
        for (int j = 0; j < 38; j++) {
            in[i][j + 6][0] = L[0][j][i];
            in[i][j + 6][1] = L[1][j][i];
        }
    }
    if (is34) {
        hybrid4812Cx(in[0], out, t.f34a, 12, len);
        hybrid4812Cx(in[1], out + 12, t.f34b, 8, len);
        hybrid4812Cx(in[2], out + 20, t.f34c, 4, len);
        hybrid4812Cx(in[3], out + 24, t.f34c, 4, len);
        hybrid4812Cx(in[4], out + 28, t.f34c, 4, len);
        analysisInterleave(out + 27, L, 5, len);
    } else {
        hybrid6Cx(in[0], out, t.f20, len);
        hybrid2Re(in[1], out + 6, kG1Q2, len, 1);
        hybrid2Re(in[2], out + 8, kG1Q2, len, 0);
        analysisInterleave(out + 7, L, 3, len);
    }
    for (int i = 0; i < 5; i++) memcpy(in[i], in[i] + 32, 6 * sizeof in[i][0]);
}

void hybridSynthesis(float out[2][38][64], float in[91][32][2], int is34, int len) {
    if (is34) {
        for (int n = 0; n < len; n++) {
            memset(out[0][n], 0, 5 * sizeof out[0][n][0]);
            memset(out[1][n], 0, 5 * sizeof out[1][n][0]);
            for (int i = 0; i < 12; i++) {
                out[0][n][0] += in[i][n][0];
                out[1][n][0] += in[i][n][1];
            }
            for (int i = 0; i < 8; i++) {
                out[0][n][1] += in[12 + i][n][0];
                out[1][n][1] += in[12 + i][n][1];
            }
            for (int i = 0; i < 4; i++) {
                out[0][n][2] += in[20 + i][n][0];
                out[1][n][2] += in[20 + i][n][1];
                out[0][n][3] += in[24 + i][n][0];
                out[1][n][3] += in[24 + i][n][1];
                out[0][n][4] += in[28 + i][n][0];
                out[1][n][4] += in[28 + i][n][1];
            }
        }
        synthesisDeinterleave(out, in + 27, 5, len);
    } else {
        for (int n = 0; n < len; n++) {
            out[0][n][0] = in[0][n][0] + in[1][n][0] + in[2][n][0] + in[3][n][0] + in[4][n][0] + in[5][n][0];
            out[1][n][0] = in[0][n][1] + in[1][n][1] + in[2][n][1] + in[3][n][1] + in[4][n][1] + in[5][n][1];
            out[0][n][1] = in[6][n][0] + in[7][n][0];
            out[1][n][1] = in[6][n][1] + in[7][n][1];
            out[0][n][2] = in[8][n][0] + in[9][n][0];
            out[1][n][2] = in[8][n][1] + in[9][n][1];
        }
        synthesisDeinterleave(out, in + 7, 3, len);
    }
}

const float kDecaySlope = 0.05f;
const int kNrParBands[] = {20, 34};
const int kNrIpdopdBands[] = {11, 17};
const int kNrBands[] = {71, 91};
const int kDecayCutoff[] = {10, 32};
const int kNrAllpassBands[] = {30, 50};
const int kShortDelayBand[] = {42, 62};

void mapIdx10To20(int8_t* mapped, const int8_t* par, int full) {
    int b;
    if (full) {
        b = 9;
    } else {
        b = 4;
        mapped[10] = 0;
    }
    for (; b >= 0; b--) mapped[2 * b + 1] = mapped[2 * b] = par[b];
}

void mapIdx34To20(int8_t* m, const int8_t* p, int full) {
    m[0] = int8_t((2 * p[0] + p[1]) / 3);
    m[1] = int8_t((p[1] + 2 * p[2]) / 3);
    m[2] = int8_t((2 * p[3] + p[4]) / 3);
    m[3] = int8_t((p[4] + 2 * p[5]) / 3);
    m[4] = int8_t((p[6] + p[7]) / 2);
    m[5] = int8_t((p[8] + p[9]) / 2);
    m[6] = p[10];
    m[7] = p[11];
    m[8] = int8_t((p[12] + p[13]) / 2);
    m[9] = int8_t((p[14] + p[15]) / 2);
    m[10] = p[16];
    if (full) {
        m[11] = p[17];
        m[12] = p[18];
        m[13] = p[19];
        m[14] = int8_t((p[20] + p[21]) / 2);
        m[15] = int8_t((p[22] + p[23]) / 2);
        m[16] = int8_t((p[24] + p[25]) / 2);
        m[17] = int8_t((p[26] + p[27]) / 2);
        m[18] = int8_t((p[28] + p[29] + p[30] + p[31]) / 4);
        m[19] = int8_t((p[32] + p[33]) / 2);
    }
}

float halfSum(float a, float b) { return (a + b) * 0.5f; }

void mapVal34To20(float* p) {
    p[0] = (2 * p[0] + p[1]) * 0.33333333f;
    p[1] = (p[1] + 2 * p[2]) * 0.33333333f;
    p[2] = (2 * p[3] + p[4]) * 0.33333333f;
    p[3] = (p[4] + 2 * p[5]) * 0.33333333f;
    p[4] = halfSum(p[6], p[7]);
    p[5] = halfSum(p[8], p[9]);
    p[6] = p[10];
    p[7] = p[11];
    p[8] = halfSum(p[12], p[13]);
    p[9] = halfSum(p[14], p[15]);
    p[10] = p[16];
    p[11] = p[17];
    p[12] = p[18];
    p[13] = p[19];
    p[14] = halfSum(p[20], p[21]);
    p[15] = halfSum(p[22], p[23]);
    p[16] = halfSum(p[24], p[25]);
    p[17] = halfSum(p[26], p[27]);
    p[18] = (p[28] + p[29] + p[30] + p[31]) * 0.25f;
    p[19] = halfSum(p[32], p[33]);
}

void mapIdx10To34(int8_t* m, const int8_t* p, int full) {
    if (full) {
        m[33] = m[32] = m[31] = m[30] = m[29] = m[28] = p[9];
        m[27] = m[26] = m[25] = m[24] = p[8];
        m[23] = m[22] = m[21] = m[20] = p[7];
        m[19] = m[18] = p[6];
        m[17] = m[16] = p[5];
    } else {
        m[16] = 0;
    }
    m[15] = m[14] = m[13] = m[12] = p[4];
    m[11] = m[10] = p[3];
    m[9] = m[8] = m[7] = m[6] = p[2];
    m[5] = m[4] = m[3] = p[1];
    m[2] = m[1] = m[0] = p[0];
}

void mapIdx20To34(int8_t* m, const int8_t* p, int full) {
    if (full) {
        m[33] = p[19];
        m[32] = p[19];
        m[31] = p[18];
        m[30] = p[18];
        m[29] = p[18];
        m[28] = p[18];
        m[27] = p[17];
        m[26] = p[17];
        m[25] = p[16];
        m[24] = p[16];
        m[23] = p[15];
        m[22] = p[15];
        m[21] = p[14];
        m[20] = p[14];
        m[19] = p[13];
        m[18] = p[12];
        m[17] = p[11];
    }
    m[16] = p[10];
    m[15] = p[9];
    m[14] = p[9];
    m[13] = p[8];
    m[12] = p[8];
    m[11] = p[7];
    m[10] = p[6];
    m[9] = p[5];
    m[8] = p[5];
    m[7] = p[4];
    m[6] = p[4];
    m[5] = p[3];
    m[4] = int8_t((p[2] + p[3]) / 2);
    m[3] = p[2];
    m[2] = p[1];
    m[1] = int8_t((p[0] + p[1]) / 2);
    m[0] = p[0];
}

void mapVal20To34(float* p) {
    p[33] = p[19];
    p[32] = p[19];
    p[31] = p[18];
    p[30] = p[18];
    p[29] = p[18];
    p[28] = p[18];
    p[27] = p[17];
    p[26] = p[17];
    p[25] = p[16];
    p[24] = p[16];
    p[23] = p[15];
    p[22] = p[15];
    p[21] = p[14];
    p[20] = p[14];
    p[19] = p[13];
    p[18] = p[12];
    p[17] = p[11];
    p[16] = p[10];
    p[15] = p[9];
    p[14] = p[9];
    p[13] = p[8];
    p[12] = p[8];
    p[11] = p[7];
    p[10] = p[6];
    p[9] = p[5];
    p[8] = p[5];
    p[7] = p[4];
    p[6] = p[4];
    p[5] = p[3];
    p[4] = halfSum(p[2], p[3]);
    p[3] = p[2];
    p[2] = p[1];
    p[1] = halfSum(p[0], p[1]);
}

void addSquares(float* dst, const float (*src)[2], int n) {
    for (int i = 0; i < n; i++) dst[i] += src[i][0] * src[i][0] + src[i][1] * src[i][1];
}

void mulPairSingle(float (*dst)[2], float (*src0)[2], const float* src1, int n) {
    for (int i = 0; i < n; i++) {
        dst[i][0] = src0[i][0] * src1[i];
        dst[i][1] = src0[i][1] * src1[i];
    }
}

void decorrelate(float (*out)[2], float (*delay)[2], float (*apDelay)[37][2], const float phiFract[2], const float (*qFract)[2],
                 const float* transientGain, float gDecaySlope, int len) {
    static const float a[] = {0.65143905753106f, 0.56471812200776f, 0.48954165955695f};
    float ag[3];
    for (int m = 0; m < 3; m++) ag[m] = a[m] * gDecaySlope;
    for (int n = 0; n < len; n++) {
        float inRe = delay[n][0] * phiFract[0] - delay[n][1] * phiFract[1];
        float inIm = delay[n][0] * phiFract[1] + delay[n][1] * phiFract[0];
        for (int m = 0; m < 3; m++) {
            float aRe = ag[m] * inRe;
            float aIm = ag[m] * inIm;
            float linkRe = apDelay[m][n + 2 - m][0];
            float linkIm = apDelay[m][n + 2 - m][1];
            float fracRe = qFract[m][0];
            float fracIm = qFract[m][1];
            float apdRe = inRe;
            float apdIm = inIm;
            inRe = linkRe * fracRe - linkIm * fracIm;
            inRe -= aRe;
            inIm = linkRe * fracIm + linkIm * fracRe;
            inIm -= aIm;
            apDelay[m][n + 5][0] = apdRe + ag[m] * inRe;
            apDelay[m][n + 5][1] = apdIm + ag[m] * inIm;
        }
        out[n][0] = transientGain[n] * inRe;
        out[n][1] = transientGain[n] * inIm;
    }
}

void decorrelation(Ps& ps, float (*out)[32][2], const float (*s)[32][2], int is34) {
    alignas(16) float power[34][32];
    alignas(16) float transientGain[34][32];
    float* peakDecayNrg = ps.peakDecayNrg;
    float* powerSmooth = ps.powerSmooth;
    float* peakDecayDiffSmooth = ps.peakDecayDiffSmooth;
    const float transientImpact = 1.5f;
    const float aSmooth = 0.25f;
    const int8_t* kToI = is34 ? kKToI34 : kKToI20;
    const int n0 = 0, nL = 32;
    const float peakDecayFactor = 0.76592833836465f;
    const Tables& t = tables();
    memset(power, 0, sizeof power);
    if (is34 != ps.common.is34bandsOld) {
        memset(ps.peakDecayNrg, 0, sizeof ps.peakDecayNrg);
        memset(ps.powerSmooth, 0, sizeof ps.powerSmooth);
        memset(ps.peakDecayDiffSmooth, 0, sizeof ps.peakDecayDiffSmooth);
        memset(ps.delay, 0, sizeof ps.delay);
        memset(ps.apDelay, 0, sizeof ps.apDelay);
    }
    for (int k = 0; k < kNrBands[is34]; k++) addSquares(power[kToI[k]], s[k], nL - n0);
    for (int i = 0; i < kNrParBands[is34]; i++) {
        for (int n = n0; n < nL; n++) {
            float decayedPeak = peakDecayFactor * peakDecayNrg[i];
            peakDecayNrg[i] = std::max(decayedPeak, power[i][n]);
            powerSmooth[i] += aSmooth * (power[i][n] - powerSmooth[i]);
            peakDecayDiffSmooth[i] += aSmooth * (peakDecayNrg[i] - power[i][n] - peakDecayDiffSmooth[i]);
            float denom = transientImpact * peakDecayDiffSmooth[i];
            transientGain[i][n] = (denom > powerSmooth[i]) ? powerSmooth[i] / denom : 1.0f;
        }
    }
    int k = 0;
    for (; k < kNrAllpassBands[is34]; k++) {
        int b = kToI[k];
        float gDecaySlope = 1.f - kDecaySlope * float(k - kDecayCutoff[is34]);
        gDecaySlope = std::clamp(gDecaySlope, 0.f, 1.f);
        memcpy(ps.delay[k], ps.delay[k] + nL, 14 * sizeof ps.delay[k][0]);
        memcpy(ps.delay[k] + 14, s[k], 32 * sizeof ps.delay[k][0]);
        for (int m = 0; m < 3; m++) memcpy(ps.apDelay[k][m], ps.apDelay[k][m] + 32, 5 * sizeof ps.apDelay[k][m][0]);
        decorrelate(out[k], ps.delay[k] + 14 - 2, ps.apDelay[k], t.phiFract[is34][k], t.qFractAllpass[is34][k], transientGain[b], gDecaySlope,
                    nL - n0);
    }
    for (; k < kShortDelayBand[is34]; k++) {
        int i = kToI[k];
        memcpy(ps.delay[k], ps.delay[k] + nL, 14 * sizeof ps.delay[k][0]);
        memcpy(ps.delay[k] + 14, s[k], 32 * sizeof ps.delay[k][0]);
        mulPairSingle(out[k], ps.delay[k] + 14 - 14, transientGain[i], nL - n0);
    }
    for (; k < kNrBands[is34]; k++) {
        int i = kToI[k];
        memcpy(ps.delay[k], ps.delay[k] + nL, 14 * sizeof ps.delay[k][0]);
        memcpy(ps.delay[k] + 14, s[k], 32 * sizeof ps.delay[k][0]);
        mulPairSingle(out[k], ps.delay[k] + 14 - 1, transientGain[i], nL - n0);
    }
}

void remap34(int8_t (**mapped)[34], int8_t (*par)[34], int numPar, int numEnv, int full) {
    int8_t (*m)[34] = *mapped;
    if (numPar == 20 || numPar == 11) {
        for (int e = 0; e < numEnv; e++) mapIdx20To34(m[e], par[e], full);
    } else if (numPar == 10 || numPar == 5) {
        for (int e = 0; e < numEnv; e++) mapIdx10To34(m[e], par[e], full);
    } else {
        *mapped = par;
    }
}

void remap20(int8_t (**mapped)[34], int8_t (*par)[34], int numPar, int numEnv, int full) {
    int8_t (*m)[34] = *mapped;
    if (numPar == 34 || numPar == 17) {
        for (int e = 0; e < numEnv; e++) mapIdx34To20(m[e], par[e], full);
    } else if (numPar == 10 || numPar == 5) {
        for (int e = 0; e < numEnv; e++) mapIdx10To20(m[e], par[e], full);
    } else {
        *mapped = par;
    }
}

void stereoInterpolate(float (*l)[2], float (*r)[2], const float h[2][4], const float hStep[2][4], int len) {
    float h0 = h[0][0], h1 = h[0][1], h2 = h[0][2], h3 = h[0][3];
    for (int n = 0; n < len; n++) {
        float lRe = l[n][0], lIm = l[n][1], rRe = r[n][0], rIm = r[n][1];
        h0 += hStep[0][0];
        h1 += hStep[0][1];
        h2 += hStep[0][2];
        h3 += hStep[0][3];
        l[n][0] = h0 * lRe + h2 * rRe;
        l[n][1] = h0 * lIm + h2 * rIm;
        r[n][0] = h1 * lRe + h3 * rRe;
        r[n][1] = h1 * lIm + h3 * rIm;
    }
}

void stereoInterpolateIpdopd(float (*l)[2], float (*r)[2], const float h[2][4], const float hStep[2][4], int len) {
    float h00 = h[0][0], h10 = h[1][0], h01 = h[0][1], h11 = h[1][1];
    float h02 = h[0][2], h12 = h[1][2], h03 = h[0][3], h13 = h[1][3];
    for (int n = 0; n < len; n++) {
        float lRe = l[n][0], lIm = l[n][1], rRe = r[n][0], rIm = r[n][1];
        h00 += hStep[0][0];
        h01 += hStep[0][1];
        h02 += hStep[0][2];
        h03 += hStep[0][3];
        h10 += hStep[1][0];
        h11 += hStep[1][1];
        h12 += hStep[1][2];
        h13 += hStep[1][3];
        l[n][0] = h00 * lRe + h02 * rRe - h10 * lIm - h12 * rIm;
        l[n][1] = h00 * lIm + h02 * rIm + h10 * lRe + h12 * rRe;
        r[n][0] = h01 * lRe + h03 * rRe - h11 * lIm - h13 * rIm;
        r[n][1] = h01 * lIm + h03 * rIm + h11 * lRe + h13 * rRe;
    }
}

void ipdopdReset(int8_t* ipdHist, int8_t* opdHist) {
    for (int i = 0; i < 17; i++) opdHist[i] = ipdHist[i] = 0;
}

void stereoProcessing(Ps& ps, float (*l)[32][2], float (*r)[32][2], int is34) {
    PsCommon& c = ps.common;
    const Tables& t = tables();
    float (*H11)[6][34] = ps.H11;
    float (*H12)[6][34] = ps.H12;
    float (*H21)[6][34] = ps.H21;
    float (*H22)[6][34] = ps.H22;
    int8_t iidBuf[5][34], iccBuf[5][34], ipdBuf[5][34], opdBuf[5][34];
    int8_t (*iidMapped)[34] = iidBuf;
    int8_t (*iccMapped)[34] = iccBuf;
    int8_t (*ipdMapped)[34] = ipdBuf;
    int8_t (*opdMapped)[34] = opdBuf;
    const int8_t* kToI = is34 ? kKToI34 : kKToI20;
    const float (*lut)[8][4] = c.iccMode < 3 ? t.HA : t.HB;
    if (c.numEnvOld) {
        for (int j = 0; j < 2; j++) {
            memcpy(H11[j][0], H11[j][c.numEnvOld], sizeof H11[j][0]);
            memcpy(H12[j][0], H12[j][c.numEnvOld], sizeof H12[j][0]);
            memcpy(H21[j][0], H21[j][c.numEnvOld], sizeof H21[j][0]);
            memcpy(H22[j][0], H22[j][c.numEnvOld], sizeof H22[j][0]);
        }
    }
    if (is34) {
        remap34(&iidMapped, c.iidPar, c.nrIidPar, c.numEnv, 1);
        remap34(&iccMapped, c.iccPar, c.nrIccPar, c.numEnv, 1);
        if (c.enableIpdopd) {
            remap34(&ipdMapped, c.ipdPar, c.nrIpdopdPar, c.numEnv, 0);
            remap34(&opdMapped, c.opdPar, c.nrIpdopdPar, c.numEnv, 0);
        }
        if (!c.is34bandsOld) {
            for (int j = 0; j < 2; j++) {
                mapVal20To34(H11[j][0]);
                mapVal20To34(H12[j][0]);
                mapVal20To34(H21[j][0]);
                mapVal20To34(H22[j][0]);
            }
            ipdopdReset(ps.ipdHist, ps.opdHist);
        }
    } else {
        remap20(&iidMapped, c.iidPar, c.nrIidPar, c.numEnv, 1);
        remap20(&iccMapped, c.iccPar, c.nrIccPar, c.numEnv, 1);
        if (c.enableIpdopd) {
            remap20(&ipdMapped, c.ipdPar, c.nrIpdopdPar, c.numEnv, 0);
            remap20(&opdMapped, c.opdPar, c.nrIpdopdPar, c.numEnv, 0);
        }
        if (c.is34bandsOld) {
            for (int j = 0; j < 2; j++) {
                mapVal34To20(H11[j][0]);
                mapVal34To20(H12[j][0]);
                mapVal34To20(H21[j][0]);
                mapVal34To20(H22[j][0]);
            }
            ipdopdReset(ps.ipdHist, ps.opdHist);
        }
    }
    for (int e = 0; e < c.numEnv; e++) {
        for (int b = 0; b < kNrParBands[is34]; b++) {
            const float* hv = lut[iidMapped[e][b] + 7 + 23 * c.iidQuant][iccMapped[e][b]];
            float h11 = hv[0], h12 = hv[1], h21 = hv[2], h22 = hv[3];
            if (c.enableIpdopd && b < kNrIpdopdBands[is34]) {
                int opdIdx = ps.opdHist[b] * 8 + opdMapped[e][b];
                int ipdIdx = ps.ipdHist[b] * 8 + ipdMapped[e][b];
                float opdRe = t.pdReSmooth[opdIdx], opdIm = t.pdImSmooth[opdIdx];
                float ipdRe = t.pdReSmooth[ipdIdx], ipdIm = t.pdImSmooth[ipdIdx];
                ps.opdHist[b] = int8_t(opdIdx & 0x3F);
                ps.ipdHist[b] = int8_t(ipdIdx & 0x3F);
                float ipdAdjRe = opdRe * ipdRe + opdIm * ipdIm;
                float ipdAdjIm = opdIm * ipdRe - opdRe * ipdIm;
                float h11i = h11 * opdIm;
                h11 = h11 * opdRe;
                float h12i = h12 * ipdAdjIm;
                h12 = h12 * ipdAdjRe;
                float h21i = h21 * opdIm;
                h21 = h21 * opdRe;
                float h22i = h22 * ipdAdjIm;
                h22 = h22 * ipdAdjRe;
                H11[1][e + 1][b] = h11i;
                H12[1][e + 1][b] = h12i;
                H21[1][e + 1][b] = h21i;
                H22[1][e + 1][b] = h22i;
            }
            H11[0][e + 1][b] = h11;
            H12[0][e + 1][b] = h12;
            H21[0][e + 1][b] = h21;
            H22[0][e + 1][b] = h22;
        }
        for (int k = 0; k < kNrBands[is34]; k++) {
            alignas(16) float h[2][4] = {};
            alignas(16) float hStep[2][4] = {};
            int start = c.borderPosition[e];
            int stop = c.borderPosition[e + 1];
            float width = 1.f / float((stop - start) ? (stop - start) : 1);
            int b = kToI[k];
            h[0][0] = H11[0][e][b];
            h[0][1] = H12[0][e][b];
            h[0][2] = H21[0][e][b];
            h[0][3] = H22[0][e][b];
            if (c.enableIpdopd) {
                if ((is34 && k <= 13 && k >= 9) || (!is34 && k <= 1)) {
                    h[1][0] = -H11[1][e][b];
                    h[1][1] = -H12[1][e][b];
                    h[1][2] = -H21[1][e][b];
                    h[1][3] = -H22[1][e][b];
                } else {
                    h[1][0] = H11[1][e][b];
                    h[1][1] = H12[1][e][b];
                    h[1][2] = H21[1][e][b];
                    h[1][3] = H22[1][e][b];
                }
            }
            hStep[0][0] = (H11[0][e + 1][b] - h[0][0]) * width;
            hStep[0][1] = (H12[0][e + 1][b] - h[0][1]) * width;
            hStep[0][2] = (H21[0][e + 1][b] - h[0][2]) * width;
            hStep[0][3] = (H22[0][e + 1][b] - h[0][3]) * width;
            if (c.enableIpdopd) {
                hStep[1][0] = (H11[1][e + 1][b] - h[1][0]) * width;
                hStep[1][1] = (H12[1][e + 1][b] - h[1][1]) * width;
                hStep[1][2] = (H21[1][e + 1][b] - h[1][2]) * width;
                hStep[1][3] = (H22[1][e + 1][b] - h[1][3]) * width;
            }
            if (stop - start) {
                if (c.enableIpdopd) stereoInterpolateIpdopd(l[k] + 1 + start, r[k] + 1 + start, h, hStep, stop - start);
                else stereoInterpolate(l[k] + 1 + start, r[k] + 1 + start, h, hStep, stop - start);
            }
        }
    }
}

void psApply(Ps& ps, PsScratch& scratch, float L[2][38][64], float R[2][38][64], int top) {
    const int len = 32;
    int is34 = ps.common.is34bands;
    top += kNrBands[is34] - 64;
    if (top < kNrBands[is34]) memset(ps.delay + top, 0, size_t(kNrBands[is34] - top) * sizeof ps.delay[0]);
    if (top < kNrAllpassBands[is34]) memset(ps.apDelay + top, 0, size_t(kNrAllpassBands[is34] - top) * sizeof ps.apDelay[0]);
    hybridAnalysis(scratch.Lbuf, ps.inBuf, L, is34, len);
    decorrelation(ps, scratch.Rbuf, scratch.Lbuf, is34);
    stereoProcessing(ps, scratch.Lbuf, scratch.Rbuf, is34);
    hybridSynthesis(L, scratch.Lbuf, is34, len);
    hybridSynthesis(R, scratch.Rbuf, is34, len);
}

void makeBands(int16_t* bands, int start, int stop, int numBands) {
    float base = powf(float(stop) / float(start), 1.0f / float(numBands));
    float prod = float(start);
    int previous = start;
    for (int k = 0; k < numBands - 1; k++) {
        prod *= base;
        int present = int(lrintf(prod));
        bands[k] = int16_t(present - previous);
        previous = present;
    }
    bands[numBands - 1] = int16_t(stop - previous);
}

bool inTable(const int16_t* table, int lastEl, int16_t needle) {
    for (int i = 0; i <= lastEl; i++)
        if (table[i] == needle) return true;
    return false;
}

void makeFTableLim(Sbr& sbr) {
    if (sbr.limiterBands > 0) {
        static const float bandsWarped[3] = {1.32715174233856803909f, 1.18509277094158210129f, 1.11987160404675912501f};
        const float limBandsPerOctaveWarped = bandsWarped[sbr.limiterBands - 1];
        int16_t patchBorders[7];
        uint16_t* in = sbr.fTableLim + 1;
        uint16_t* out = sbr.fTableLim;
        patchBorders[0] = int16_t(sbr.kx[1]);
        for (int k = 1; k <= sbr.numPatches; k++) patchBorders[k] = int16_t(patchBorders[k - 1] + sbr.patchNumSubbands[k - 1]);
        memcpy(sbr.fTableLim, sbr.fTableLow, size_t(sbr.n[0] + 1) * sizeof sbr.fTableLow[0]);
        if (sbr.numPatches > 1) memcpy(sbr.fTableLim + sbr.n[0] + 1, patchBorders + 1, size_t(sbr.numPatches - 1) * sizeof patchBorders[0]);
        std::sort(sbr.fTableLim, sbr.fTableLim + sbr.numPatches + sbr.n[0]);
        sbr.nLim = sbr.n[0] + sbr.numPatches - 1;
        while (out < sbr.fTableLim + sbr.nLim) {
            if (float(*in) >= float(*out) * limBandsPerOctaveWarped) {
                *++out = *in++;
            } else if (*in == *out || !inTable(patchBorders, sbr.numPatches, int16_t(*in))) {
                in++;
                sbr.nLim--;
            } else if (!inTable(patchBorders, sbr.numPatches, int16_t(*out))) {
                *out = *in++;
                sbr.nLim--;
            } else {
                *++out = *in++;
            }
        }
    } else {
        sbr.fTableLim[0] = sbr.fTableLow[0];
        sbr.fTableLim[1] = sbr.fTableLow[sbr.n[0]];
        sbr.nLim = 1;
    }
}

int64_t readHeader(Sbr& sbr, BitReader& gb) {
    int64_t cnt = gb.position();
    int oldLimiterBands = sbr.limiterBands;
    sbr.start = 1;
    sbr.readyForDequant = 0;
    SpectrumParameters old = sbr.spectrum;
    sbr.ampResHeader = gb.bit();
    sbr.spectrum.startFreq = uint8_t(gb.read(4));
    sbr.spectrum.stopFreq = uint8_t(gb.read(4));
    sbr.spectrum.xoverBand = uint8_t(gb.read(3));
    gb.skip(2);
    int extra1 = gb.bit();
    int extra2 = gb.bit();
    if (extra1) {
        sbr.spectrum.freqScale = uint8_t(gb.read(2));
        sbr.spectrum.alterScale = uint8_t(gb.bit());
        sbr.spectrum.noiseBands = uint8_t(gb.read(2));
    } else {
        sbr.spectrum.freqScale = 2;
        sbr.spectrum.alterScale = 1;
        sbr.spectrum.noiseBands = 2;
    }
    if (memcmp(&old, &sbr.spectrum, sizeof old)) sbr.reset = 1;
    if (extra2) {
        sbr.limiterBands = int(gb.read(2));
        sbr.limiterGains = int(gb.read(2));
        sbr.interpolFreq = gb.bit();
        sbr.smoothingMode = gb.bit();
    } else {
        sbr.limiterBands = 2;
        sbr.limiterGains = 2;
        sbr.interpolFreq = 1;
        sbr.smoothingMode = 1;
    }
    if (sbr.limiterBands != oldLimiterBands && !sbr.reset) makeFTableLim(sbr);
    return gb.position() - cnt;
}

bool checkNMaster(int nMaster, int xoverBand) { return nMaster > 0 && xoverBand < nMaster; }

int makeFMaster(Sbr& sbr, const SpectrumParameters& spectrum) {
    const int8_t* offsets;
    switch (sbr.sampleRate) {
        case 16000: offsets = kSbrOffset[0]; break;
        case 22050: offsets = kSbrOffset[1]; break;
        case 24000: offsets = kSbrOffset[2]; break;
        case 32000: offsets = kSbrOffset[3]; break;
        case 44100:
        case 48000:
        case 64000: offsets = kSbrOffset[4]; break;
        case 88200:
        case 96000:
        case 128000:
        case 176400:
        case 192000: offsets = kSbrOffset[5]; break;
        default: return -1;
    }
    unsigned temp = sbr.sampleRate < 32000 ? 3000 : sbr.sampleRate < 64000 ? 4000 : 5000;
    unsigned startMin = ((temp << 7) + unsigned(sbr.sampleRate >> 1)) / unsigned(sbr.sampleRate);
    unsigned stopMin = ((temp << 8) + unsigned(sbr.sampleRate >> 1)) / unsigned(sbr.sampleRate);
    sbr.k[0] = int(startMin) + offsets[spectrum.startFreq];
    if (spectrum.stopFreq < 14) {
        int16_t stopDk[13];
        sbr.k[2] = int(stopMin);
        makeBands(stopDk, int(stopMin), 64, 13);
        std::sort(stopDk, stopDk + 13);
        for (int k = 0; k < spectrum.stopFreq; k++) sbr.k[2] += stopDk[k];
    } else if (spectrum.stopFreq == 14) {
        sbr.k[2] = 2 * sbr.k[0];
    } else if (spectrum.stopFreq == 15) {
        sbr.k[2] = 3 * sbr.k[0];
    } else {
        return -1;
    }
    sbr.k[2] = std::min(64, sbr.k[2]);
    int maxQmfSubbands = sbr.sampleRate <= 32000 ? 48 : sbr.sampleRate == 44100 ? 35 : 32;
    if (unsigned(sbr.k[2] - sbr.k[0]) > unsigned(maxQmfSubbands)) return -1;
    if (!spectrum.freqScale) {
        int dk = spectrum.alterScale + 1;
        sbr.nMaster = ((sbr.k[2] - sbr.k[0] + (dk & 2)) >> dk) << 1;
        if (!checkNMaster(sbr.nMaster, sbr.spectrum.xoverBand)) return -1;
        for (int k = 1; k <= sbr.nMaster; k++) sbr.fMaster[k] = uint16_t(dk);
        int k2diff = sbr.k[2] - sbr.k[0] - sbr.nMaster * dk;
        if (k2diff < 0) {
            sbr.fMaster[1]--;
            sbr.fMaster[2] = uint16_t(sbr.fMaster[2] - (k2diff < -1));
        } else if (k2diff) {
            sbr.fMaster[sbr.nMaster]++;
        }
        sbr.fMaster[0] = uint16_t(sbr.k[0]);
        for (int k = 1; k <= sbr.nMaster; k++) sbr.fMaster[k] = uint16_t(sbr.fMaster[k] + sbr.fMaster[k - 1]);
    } else {
        int halfBands = 7 - spectrum.freqScale;
        int twoRegions;
        int16_t vk0[49];
        if (49 * sbr.k[2] > 110 * sbr.k[0]) {
            twoRegions = 1;
            sbr.k[1] = 2 * sbr.k[0];
        } else {
            twoRegions = 0;
            sbr.k[1] = sbr.k[2];
        }
        int numBands0 = int(lrintf(float(halfBands) * log2f(float(sbr.k[1]) / float(sbr.k[0])))) * 2;
        if (numBands0 <= 0 || numBands0 > 48) return -1;
        vk0[0] = 0;
        makeBands(vk0 + 1, sbr.k[0], sbr.k[1], numBands0);
        std::sort(vk0 + 1, vk0 + 1 + numBands0);
        int vdk0Max = vk0[numBands0];
        vk0[0] = int16_t(sbr.k[0]);
        for (int k = 1; k <= numBands0; k++) {
            if (vk0[k] <= 0) return -1;
            vk0[k] = int16_t(vk0[k] + vk0[k - 1]);
        }
        if (twoRegions) {
            int16_t vk1[49];
            float invwarp = spectrum.alterScale ? 0.76923076923076923077f : 1.0f;
            int numBands1 = int(lrintf(float(halfBands) * invwarp * log2f(float(sbr.k[2]) / float(sbr.k[1])))) * 2;
            if (numBands1 <= 0 || numBands0 + numBands1 > 48) return -1;
            makeBands(vk1 + 1, sbr.k[1], sbr.k[2], numBands1);
            int vdk1Min = *std::min_element(vk1 + 1, vk1 + 1 + numBands1);
            if (vdk1Min < vdk0Max) {
                std::sort(vk1 + 1, vk1 + 1 + numBands1);
                int change = std::min(vdk0Max - vk1[1], (vk1[numBands1] - vk1[1]) >> 1);
                vk1[1] = int16_t(vk1[1] + change);
                vk1[numBands1] = int16_t(vk1[numBands1] - change);
            }
            std::sort(vk1 + 1, vk1 + 1 + numBands1);
            vk1[0] = int16_t(sbr.k[1]);
            for (int k = 1; k <= numBands1; k++) {
                if (vk1[k] <= 0) return -1;
                vk1[k] = int16_t(vk1[k] + vk1[k - 1]);
            }
            sbr.nMaster = numBands0 + numBands1;
            if (!checkNMaster(sbr.nMaster, sbr.spectrum.xoverBand)) return -1;
            for (int k = 0; k <= numBands0; k++) sbr.fMaster[k] = uint16_t(vk0[k]);
            for (int k = 0; k < numBands1; k++) sbr.fMaster[numBands0 + 1 + k] = uint16_t(vk1[k + 1]);
        } else {
            sbr.nMaster = numBands0;
            if (!checkNMaster(sbr.nMaster, sbr.spectrum.xoverBand)) return -1;
            for (int k = 0; k <= numBands0; k++) sbr.fMaster[k] = uint16_t(vk0[k]);
        }
    }
    return 0;
}

int calcPatches(Sbr& sbr) {
    int k, lastK = -1, lastMsb = -1, sb = 0;
    int msb = sbr.k[0];
    int usb = sbr.kx[1];
    int goalSb = ((1000 << 11) + (sbr.sampleRate >> 1)) / sbr.sampleRate;
    sbr.numPatches = 0;
    if (goalSb < sbr.kx[1] + sbr.m[1]) {
        for (k = 0; k < sbr.nMaster && sbr.fMaster[k] < goalSb; k++) {
        }
    } else {
        k = sbr.nMaster;
    }
    do {
        int odd = 0;
        if (k == lastK && msb == lastMsb) return -1;
        lastK = k;
        lastMsb = msb;
        for (int i = k; i >= 0 && (i == k || sb > (sbr.k[0] - 1 + msb - odd)); i--) {
            sb = sbr.fMaster[i];
            odd = (sb + sbr.k[0]) & 1;
        }
        if (sbr.numPatches > 5) return -1;
        sbr.patchNumSubbands[sbr.numPatches] = uint8_t(std::max(sb - usb, 0));
        sbr.patchStartSubband[sbr.numPatches] = uint8_t(sbr.k[0] - odd - sbr.patchNumSubbands[sbr.numPatches]);
        if (sbr.patchNumSubbands[sbr.numPatches] > 0) {
            usb = sb;
            msb = sb;
            sbr.numPatches++;
        } else {
            msb = sbr.kx[1];
        }
        if (sbr.fMaster[k] - sb < 3) k = sbr.nMaster;
    } while (sb != sbr.kx[1] + sbr.m[1]);
    if (sbr.numPatches > 1 && sbr.patchNumSubbands[sbr.numPatches - 1] < 3) sbr.numPatches--;
    return 0;
}

int makeFDerived(Sbr& sbr) {
    sbr.n[1] = sbr.nMaster - sbr.spectrum.xoverBand;
    sbr.n[0] = (sbr.n[1] + 1) >> 1;
    memcpy(sbr.fTableHigh, &sbr.fMaster[sbr.spectrum.xoverBand], size_t(sbr.n[1] + 1) * sizeof sbr.fMaster[0]);
    sbr.m[1] = sbr.fTableHigh[sbr.n[1]] - sbr.fTableHigh[0];
    sbr.kx[1] = sbr.fTableHigh[0];
    if (sbr.kx[1] + sbr.m[1] > 64 || sbr.m[1] < 0) return -1;
    if (sbr.kx[1] > 32) return -1;
    sbr.fTableLow[0] = sbr.fTableHigh[0];
    int temp = sbr.n[1] & 1;
    for (int k = 1; k <= sbr.n[0]; k++) sbr.fTableLow[k] = sbr.fTableHigh[2 * k - temp];
    sbr.nQ = std::max(1, int(lrintf(float(sbr.spectrum.noiseBands) * log2f(float(sbr.k[2]) / float(sbr.kx[1])))));
    if (sbr.nQ > 5) {
        sbr.nQ = 1;
        return -1;
    }
    sbr.fTableNoise[0] = sbr.fTableLow[0];
    temp = 0;
    for (int k = 1; k <= sbr.nQ; k++) {
        temp += (sbr.n[0] - temp) / (sbr.nQ + 1 - k);
        sbr.fTableNoise[k] = sbr.fTableLow[temp];
    }
    if (calcPatches(sbr) < 0) return -1;
    makeFTableLim(sbr);
    sbr.data[0].fIndexNoise = 0;
    sbr.data[1].fIndexNoise = 0;
    return 0;
}

void readBits1(BitReader& gb, uint8_t* vec, int elements) {
    for (int i = 0; i < elements; i++) vec[i] = uint8_t(gb.bit());
}

const int8_t kCeilLog2[] = {0, 1, 2, 2, 3, 3};

int readGrid(Sbr& sbr, BitReader& gb, SbrData& ch) {
    int bsPointer = 0;
    int absBordTrail = kTimeSlots;
    int numRelLead, numRelTrail;
    int numEnvOld = ch.numEnv;
    ch.freqRes[0] = ch.freqRes[ch.numEnv];
    ch.ampRes = unsigned(sbr.ampResHeader);
    ch.tEnvNumEnvOld = ch.tEnv[numEnvOld];
    unsigned frameClass = gb.read(2);
    switch (frameClass) {
        case kFixFix: {
            int numEnv = 1 << gb.read(2);
            if (numEnv > 5) return -1;
            ch.numEnv = numEnv;
            numRelLead = ch.numEnv - 1;
            if (ch.numEnv == 1) ch.ampRes = 0;
            ch.tEnv[0] = 0;
            ch.tEnv[ch.numEnv] = uint8_t(absBordTrail);
            absBordTrail = (absBordTrail + (ch.numEnv >> 1)) / ch.numEnv;
            for (int i = 0; i < numRelLead; i++) ch.tEnv[i + 1] = uint8_t(ch.tEnv[i] + absBordTrail);
            ch.freqRes[1] = uint8_t(gb.bit());
            for (int i = 1; i < ch.numEnv; i++) ch.freqRes[i + 1] = ch.freqRes[1];
            break;
        }
        case kFixVar:
            absBordTrail += int(gb.read(2));
            numRelTrail = int(gb.read(2));
            ch.numEnv = numRelTrail + 1;
            ch.tEnv[0] = 0;
            ch.tEnv[ch.numEnv] = uint8_t(absBordTrail);
            for (int i = 0; i < numRelTrail; i++) ch.tEnv[ch.numEnv - 1 - i] = uint8_t(ch.tEnv[ch.numEnv - i] - 2 * int(gb.read(2)) - 2);
            bsPointer = int(gb.read(kCeilLog2[ch.numEnv]));
            for (int i = 0; i < ch.numEnv; i++) ch.freqRes[ch.numEnv - i] = uint8_t(gb.bit());
            break;
        case kVarFix:
            ch.tEnv[0] = uint8_t(gb.read(2));
            numRelLead = int(gb.read(2));
            ch.numEnv = numRelLead + 1;
            ch.tEnv[ch.numEnv] = uint8_t(absBordTrail);
            for (int i = 0; i < numRelLead; i++) ch.tEnv[i + 1] = uint8_t(ch.tEnv[i] + 2 * int(gb.read(2)) + 2);
            bsPointer = int(gb.read(kCeilLog2[ch.numEnv]));
            readBits1(gb, ch.freqRes + 1, ch.numEnv);
            break;
        default: {
            ch.tEnv[0] = uint8_t(gb.read(2));
            absBordTrail += int(gb.read(2));
            numRelLead = int(gb.read(2));
            numRelTrail = int(gb.read(2));
            int numEnv = numRelLead + numRelTrail + 1;
            if (numEnv > 5) return -1;
            ch.numEnv = numEnv;
            ch.tEnv[ch.numEnv] = uint8_t(absBordTrail);
            for (int i = 0; i < numRelLead; i++) ch.tEnv[i + 1] = uint8_t(ch.tEnv[i] + 2 * int(gb.read(2)) + 2);
            for (int i = 0; i < numRelTrail; i++) ch.tEnv[ch.numEnv - 1 - i] = uint8_t(ch.tEnv[ch.numEnv - i] - 2 * int(gb.read(2)) - 2);
            bsPointer = int(gb.read(kCeilLog2[ch.numEnv]));
            readBits1(gb, ch.freqRes + 1, ch.numEnv);
            break;
        }
    }
    ch.frameClass = frameClass;
    if (bsPointer > ch.numEnv + 1) return -1;
    for (int i = 1; i <= ch.numEnv; i++)
        if (ch.tEnv[i - 1] >= ch.tEnv[i]) return -1;
    ch.numNoise = (ch.numEnv > 1) + 1;
    ch.tQ[0] = ch.tEnv[0];
    ch.tQ[ch.numNoise] = ch.tEnv[ch.numEnv];
    if (ch.numNoise > 1) {
        int idx;
        if (ch.frameClass == kFixFix) idx = ch.numEnv >> 1;
        else if (ch.frameClass & 1) idx = ch.numEnv - std::max(bsPointer - 1, 1);
        else idx = !bsPointer ? 1 : bsPointer == 1 ? ch.numEnv - 1 : bsPointer - 1;
        ch.tQ[1] = ch.tEnv[idx];
    }
    ch.ea[0] = -(ch.ea[1] != numEnvOld);
    ch.ea[1] = -1;
    if ((ch.frameClass & 1) && bsPointer) ch.ea[1] = ch.numEnv + 1 - bsPointer;
    else if (ch.frameClass == 2 && bsPointer > 1) ch.ea[1] = bsPointer - 1;
    return 0;
}

void copyGrid(SbrData& dst, const SbrData& src) {
    dst.freqRes[0] = dst.freqRes[dst.numEnv];
    dst.tEnvNumEnvOld = dst.tEnv[dst.numEnv];
    dst.ea[0] = -(dst.ea[1] != dst.numEnv);
    memcpy(dst.freqRes + 1, src.freqRes + 1, sizeof dst.freqRes - sizeof dst.freqRes[0]);
    memcpy(dst.tEnv, src.tEnv, sizeof dst.tEnv);
    memcpy(dst.tQ, src.tQ, sizeof dst.tQ);
    dst.numEnv = src.numEnv;
    dst.ampRes = src.ampRes;
    dst.numNoise = src.numNoise;
    dst.frameClass = src.frameClass;
    dst.ea[1] = src.ea[1];
}

void readDtdf(BitReader& gb, SbrData& ch) {
    readBits1(gb, ch.dfEnv, ch.numEnv);
    readBits1(gb, ch.dfNoise, ch.numNoise);
}

void readInvf(Sbr& sbr, BitReader& gb, SbrData& ch) {
    memcpy(ch.invfMode[1], ch.invfMode[0], 5);
    for (int i = 0; i < sbr.nQ; i++) ch.invfMode[0][i] = uint8_t(gb.read(2));
}

int readEnvelope(Sbr& sbr, BitReader& gb, SbrData& ch, int c) {
    const Tables& t = tables();
    int bits;
    const Vlc *tHuff, *fHuff;
    const int delta = (c == 1 && sbr.coupling == 1) + 1;
    const int odd = sbr.n[1] & 1;
    if (sbr.coupling && c) {
        if (ch.ampRes) {
            bits = 5;
            tHuff = &t.sbr[kTEnvBal30];
            fHuff = &t.sbr[kFEnvBal30];
        } else {
            bits = 6;
            tHuff = &t.sbr[kTEnvBal15];
            fHuff = &t.sbr[kFEnvBal15];
        }
    } else {
        if (ch.ampRes) {
            bits = 6;
            tHuff = &t.sbr[kTEnv30];
            fHuff = &t.sbr[kFEnv30];
        } else {
            bits = 7;
            tHuff = &t.sbr[kTEnv15];
            fHuff = &t.sbr[kFEnv15];
        }
    }
    for (int i = 0; i < ch.numEnv; i++) {
        const int count = sbr.n[ch.freqRes[i + 1]];
        if (ch.dfEnv[i]) {
            for (int j = 0; j < count; j++) {
                int k = j;
                if (ch.freqRes[i + 1] == ch.freqRes[i]) k = j;
                else if (ch.freqRes[i + 1]) k = (j + odd) >> 1;
                else k = j ? 2 * j - odd : 0;
                ch.envFacsQ[i + 1][j] = uint8_t(ch.envFacsQ[i][k] + delta * tHuff->read(gb));
                if (ch.envFacsQ[i + 1][j] > 127u) return -1;
            }
        } else {
            ch.envFacsQ[i + 1][0] = uint8_t(delta * int(gb.read(bits)));
            for (int j = 1; j < count; j++) {
                ch.envFacsQ[i + 1][j] = uint8_t(ch.envFacsQ[i + 1][j - 1] + delta * fHuff->read(gb));
                if (ch.envFacsQ[i + 1][j] > 127u) return -1;
            }
        }
    }
    memcpy(ch.envFacsQ[0], ch.envFacsQ[ch.numEnv], sizeof ch.envFacsQ[0]);
    return 0;
}

int readNoise(Sbr& sbr, BitReader& gb, SbrData& ch, int c) {
    const Tables& t = tables();
    const Vlc *tHuff, *fHuff;
    int delta = (c == 1 && sbr.coupling == 1) + 1;
    if (sbr.coupling && c) {
        tHuff = &t.sbr[kTNoiseBal30];
        fHuff = &t.sbr[kFEnvBal30];
    } else {
        tHuff = &t.sbr[kTNoise30];
        fHuff = &t.sbr[kFEnv30];
    }
    for (int i = 0; i < ch.numNoise; i++) {
        if (ch.dfNoise[i]) {
            for (int j = 0; j < sbr.nQ; j++) {
                ch.noiseFacsQ[i + 1][j] = uint8_t(ch.noiseFacsQ[i][j] + delta * tHuff->read(gb));
                if (ch.noiseFacsQ[i + 1][j] > 30u) return -1;
            }
        } else {
            ch.noiseFacsQ[i + 1][0] = uint8_t(delta * int(gb.read(5)));
            for (int j = 1; j < sbr.nQ; j++) {
                ch.noiseFacsQ[i + 1][j] = uint8_t(ch.noiseFacsQ[i + 1][j - 1] + delta * fHuff->read(gb));
                if (ch.noiseFacsQ[i + 1][j] > 30u) return -1;
            }
        }
    }
    memcpy(ch.noiseFacsQ[0], ch.noiseFacsQ[ch.numNoise], sizeof ch.noiseFacsQ[0]);
    return 0;
}

void readExtension(Sbr& sbr, const StreamConfig& config, BitReader& gb, int id, int& bitsLeft) {
    if (id == 2 && config.ps) {
        bitsLeft -= psReadData(gb, sbr.ps.common, bitsLeft);
    } else {
        gb.skip(bitsLeft);
        bitsLeft = 0;
    }
}

int readSingle(Sbr& sbr, BitReader& gb) {
    if (gb.bit()) gb.skip(4);
    if (readGrid(sbr, gb, sbr.data[0])) return -1;
    readDtdf(gb, sbr.data[0]);
    readInvf(sbr, gb, sbr.data[0]);
    if (readEnvelope(sbr, gb, sbr.data[0], 0) < 0) return -1;
    if (readNoise(sbr, gb, sbr.data[0], 0) < 0) return -1;
    if ((sbr.data[0].addHarmonicFlag = unsigned(gb.bit()))) readBits1(gb, sbr.data[0].addHarmonic, sbr.n[1]);
    return 0;
}

int readPair(Sbr& sbr, BitReader& gb) {
    if (gb.bit()) gb.skip(8);
    if ((sbr.coupling = gb.bit())) {
        if (readGrid(sbr, gb, sbr.data[0])) return -1;
        copyGrid(sbr.data[1], sbr.data[0]);
        readDtdf(gb, sbr.data[0]);
        readDtdf(gb, sbr.data[1]);
        readInvf(sbr, gb, sbr.data[0]);
        memcpy(sbr.data[1].invfMode[1], sbr.data[1].invfMode[0], sizeof sbr.data[1].invfMode[0]);
        memcpy(sbr.data[1].invfMode[0], sbr.data[0].invfMode[0], sizeof sbr.data[1].invfMode[0]);
        if (readEnvelope(sbr, gb, sbr.data[0], 0) < 0) return -1;
        if (readNoise(sbr, gb, sbr.data[0], 0) < 0) return -1;
        if (readEnvelope(sbr, gb, sbr.data[1], 1) < 0) return -1;
        if (readNoise(sbr, gb, sbr.data[1], 1) < 0) return -1;
    } else {
        if (readGrid(sbr, gb, sbr.data[0]) || readGrid(sbr, gb, sbr.data[1])) return -1;
        readDtdf(gb, sbr.data[0]);
        readDtdf(gb, sbr.data[1]);
        readInvf(sbr, gb, sbr.data[0]);
        readInvf(sbr, gb, sbr.data[1]);
        if (readEnvelope(sbr, gb, sbr.data[0], 0) < 0) return -1;
        if (readEnvelope(sbr, gb, sbr.data[1], 1) < 0) return -1;
        if (readNoise(sbr, gb, sbr.data[0], 0) < 0) return -1;
        if (readNoise(sbr, gb, sbr.data[1], 1) < 0) return -1;
    }
    if ((sbr.data[0].addHarmonicFlag = unsigned(gb.bit()))) readBits1(gb, sbr.data[0].addHarmonic, sbr.n[1]);
    if ((sbr.data[1].addHarmonicFlag = unsigned(gb.bit()))) readBits1(gb, sbr.data[1].addHarmonic, sbr.n[1]);
    return 0;
}

int64_t readData(Sbr& sbr, const StreamConfig& config, BitReader& gb, int idAac) {
    int64_t cnt = gb.position();
    sbr.idAac = idAac;
    sbr.readyForDequant = 1;
    if (idAac == kSce || idAac == kCce) {
        if (readSingle(sbr, gb)) {
            sbrTurnoff(sbr);
            return gb.position() - cnt;
        }
    } else if (idAac == kCpe) {
        if (readPair(sbr, gb)) {
            sbrTurnoff(sbr);
            return gb.position() - cnt;
        }
    } else {
        sbrTurnoff(sbr);
        return gb.position() - cnt;
    }
    if (gb.bit()) {
        int bitsLeft = int(gb.read(4));
        if (bitsLeft == 15) bitsLeft += int(gb.read(8));
        bitsLeft <<= 3;
        while (bitsLeft > 7) {
            bitsLeft -= 2;
            readExtension(sbr, config, gb, int(gb.read(2)), bitsLeft);
        }
        if (bitsLeft > 0) gb.skip(bitsLeft);
    }
    return gb.position() - cnt;
}

void sbrReset(Sbr& sbr) {
    int err = makeFMaster(sbr, sbr.spectrum);
    if (err >= 0) err = makeFDerived(sbr);
    if (err < 0) sbrTurnoff(sbr);
}

void dequant(Sbr& sbr, int idAac) {
    static const double exp2Tab[2] = {1, M_SQRT2};
    if (idAac == kCpe && sbr.coupling) {
        int panOffset = sbr.data[0].ampRes ? 12 : 24;
        for (int e = 1; e <= sbr.data[0].numEnv; e++) {
            for (int k = 0; k < sbr.n[sbr.data[0].freqRes[e]]; k++) {
                float temp1, temp2;
                const int q0 = sbr.data[0].envFacsQ[e][k], q1 = sbr.data[1].envFacsQ[e][k];
                if (sbr.data[0].ampRes) {
                    temp1 = exp2fi(q0 + 7);
                    temp2 = exp2fi(panOffset - q1);
                } else {
                    temp1 = float(exp2fi((q0 >> 1) + 7) * exp2Tab[q0 & 1]);
                    temp2 = float(exp2fi((panOffset - q1) >> 1) * exp2Tab[(panOffset - q1) & 1]);
                }
                if (temp1 > 1E20) temp1 = 1;
                float fac = temp1 / (1.0f + temp2);
                sbr.data[0].envFacs[e][k] = fac;
                sbr.data[1].envFacs[e][k] = fac * temp2;
            }
        }
        for (int e = 1; e <= sbr.data[0].numNoise; e++) {
            for (int k = 0; k < sbr.nQ; k++) {
                float temp1 = exp2fi(kNoiseFloorOffset - sbr.data[0].noiseFacsQ[e][k] + 1);
                float temp2 = exp2fi(12 - sbr.data[1].noiseFacsQ[e][k]);
                float fac = temp1 / (1.0f + temp2);
                sbr.data[0].noiseFacs[e][k] = fac;
                sbr.data[1].noiseFacs[e][k] = fac * temp2;
            }
        }
    } else {
        for (int ch = 0; ch < (idAac == kCpe) + 1; ch++) {
            SbrData& d = sbr.data[ch];
            for (int e = 1; e <= d.numEnv; e++) {
                for (int k = 0; k < sbr.n[d.freqRes[e]]; k++) {
                    const int q = d.envFacsQ[e][k];
                    if (d.ampRes) d.envFacs[e][k] = exp2fi(q + 6);
                    else d.envFacs[e][k] = float(exp2fi((q >> 1) + 6) * exp2Tab[q & 1]);
                    if (d.envFacs[e][k] > 1E20) d.envFacs[e][k] = 1;
                }
            }
            for (int e = 1; e <= d.numNoise; e++)
                for (int k = 0; k < sbr.nQ; k++) d.noiseFacs[e][k] = exp2fi(kNoiseFloorOffset - d.noiseFacsQ[e][k]);
        }
    }
}

void autocorrelate(const float x[40][2], float phi[3][2][2]) {
    auto pair = [&](int a, int b, float* lanes) {
        lanes[0] = x[a][0] * x[b][0];
        lanes[1] = x[a][1] * x[b][1];
        lanes[2] = x[a][0] * x[b][1];
        lanes[3] = x[a][1] * x[b][0];
    };
    auto add = [](float* acc, const float* lanes) {
        for (int k = 0; k < 4; k++) acc[k] += lanes[k];
    };
    float first1[4], first0[4], sum0[4], sum1[4], sum2[4], t[4];
    pair(0, 2, sum2);
    pair(0, 1, first1);
    pair(0, 0, first0);
    pair(1, 2, sum1);
    pair(1, 1, sum0);
    pair(1, 3, t);
    add(sum2, t);
    for (int i = 2; i < 38; i++) {
        pair(i, i + 1, t);
        add(sum1, t);
        pair(i, i + 2, t);
        add(sum2, t);
        pair(i, i, t);
        add(sum0, t);
    }
    float last1[4], last0[4], start1[4], start0[4];
    pair(38, 39, t);
    for (int k = 0; k < 4; k++) last1[k] = t[k] + sum1[k];
    pair(38, 38, t);
    for (int k = 0; k < 4; k++) last0[k] = t[k] + sum0[k];
    for (int k = 0; k < 4; k++) start1[k] = sum1[k] + first1[k];
    for (int k = 0; k < 4; k++) start0[k] = sum0[k] + first0[k];
    phi[0][0][0] = last1[0] + last1[1];
    phi[0][0][1] = last1[2] + -last1[3];
    phi[0][1][0] = sum2[0] + sum2[1];
    phi[0][1][1] = sum2[2] + -sum2[3];
    phi[2][1][0] = start0[0] + start0[1];
    phi[1][1][0] = start1[0] + start1[1];
    phi[1][1][1] = start1[2] + -start1[3];
    phi[1][0][0] = last0[0] + last0[1];
}

void hfInverseFilter(float (*alpha0)[2], float (*alpha1)[2], const float Xlow[32][40][2], int k0) {
    for (int k = 0; k < k0; k++) {
        alignas(16) float phi[3][2][2];
        autocorrelate(Xlow[k], phi);
        float dk = phi[2][1][0] * phi[1][0][0] - (phi[1][1][0] * phi[1][1][0] + phi[1][1][1] * phi[1][1][1]) / 1.000001f;
        if (!dk) {
            alpha1[k][0] = 0;
            alpha1[k][1] = 0;
        } else {
            float re = phi[0][0][0] * phi[1][1][0] - phi[0][0][1] * phi[1][1][1] - phi[0][1][0] * phi[1][0][0];
            float im = phi[0][0][0] * phi[1][1][1] + phi[0][0][1] * phi[1][1][0] - phi[0][1][1] * phi[1][0][0];
            alpha1[k][0] = re / dk;
            alpha1[k][1] = im / dk;
        }
        if (!phi[1][0][0]) {
            alpha0[k][0] = 0;
            alpha0[k][1] = 0;
        } else {
            float re = phi[0][0][0] + alpha1[k][0] * phi[1][1][0] + alpha1[k][1] * phi[1][1][1];
            float im = phi[0][0][1] + alpha1[k][1] * phi[1][1][0] - alpha1[k][0] * phi[1][1][1];
            alpha0[k][0] = -re / phi[1][0][0];
            alpha0[k][1] = -im / phi[1][0][0];
        }
        if (alpha1[k][0] * alpha1[k][0] + alpha1[k][1] * alpha1[k][1] >= 16.0f || alpha0[k][0] * alpha0[k][0] + alpha0[k][1] * alpha0[k][1] >= 16.0f) {
            alpha1[k][0] = 0;
            alpha1[k][1] = 0;
            alpha0[k][0] = 0;
            alpha0[k][1] = 0;
        }
    }
}

void chirp(Sbr& sbr, SbrData& ch) {
    static const float bwTab[] = {0.0f, 0.75f, 0.9f, 0.98f};
    for (int i = 0; i < sbr.nQ; i++) {
        float newBw;
        if (ch.invfMode[0][i] + ch.invfMode[1][i] == 1) newBw = 0.6f;
        else newBw = bwTab[ch.invfMode[0][i]];
        if (newBw < ch.bwArray[i]) newBw = 0.75f * newBw + 0.25f * ch.bwArray[i];
        else newBw = 0.90625f * newBw + 0.09375f * ch.bwArray[i];
        ch.bwArray[i] = newBw < 0.015625f ? 0.0f : newBw;
    }
}

void gainCalc(Sbr& sbr, SbrScratch& s, SbrData& ch, const int ea[2]) {
    static const float limgain[4] = {0.70795f, 1.0f, 1.41254f, 10000000000.0f};
    const int kx = sbr.kx[1];
    for (int e = 0; e < ch.numEnv; e++) {
        int delta = !((e == ea[1]) || (e == ea[0]));
        for (int k = 0; k < sbr.nLim; k++) {
            const int from = sbr.fTableLim[k] - kx, to = sbr.fTableLim[k + 1] - kx;
            float sum[2] = {0.0f, 0.0f};
            for (int m = from; m < to; m++) {
                const float temp = s.eOrigMapped[e][m] / (1.0f + s.qMapped[e][m]);
                s.qM[e][m] = sqrtf(temp * s.qMapped[e][m]);
                s.sM[e][m] = sqrtf(temp * float(ch.sIndexMapped[e + 1][m]));
                if (!s.sMapped[e][m]) {
                    s.gain[e][m] = sqrtf(s.eOrigMapped[e][m] / ((1.0f + s.eCurr[e][m]) * (1.0f + s.qMapped[e][m] * float(delta))));
                } else {
                    s.gain[e][m] = sqrtf(s.eOrigMapped[e][m] * s.qMapped[e][m] / ((1.0f + s.eCurr[e][m]) * (1.0f + s.qMapped[e][m])));
                }
                s.gain[e][m] += FLT_MIN;
            }
            for (int m = from; m < to; m++) {
                sum[0] += s.eOrigMapped[e][m];
                sum[1] += s.eCurr[e][m];
            }
            float gainMax = limgain[sbr.limiterGains] * sqrtf((FLT_EPSILON + sum[0]) / (FLT_EPSILON + sum[1]));
            gainMax = std::min(100000.f, gainMax);
            for (int m = from; m < to; m++) {
                float qMMax = s.qM[e][m] * gainMax / s.gain[e][m];
                s.qM[e][m] = std::min(s.qM[e][m], qMMax);
                s.gain[e][m] = std::min(s.gain[e][m], gainMax);
            }
            sum[0] = sum[1] = 0.0f;
            for (int m = from; m < to; m++) {
                sum[0] += s.eOrigMapped[e][m];
                sum[1] += s.eCurr[e][m] * s.gain[e][m] * s.gain[e][m] + s.sM[e][m] * s.sM[e][m] +
                          float(delta && !s.sM[e][m]) * s.qM[e][m] * s.qM[e][m];
            }
            float gainBoost = sqrtf((FLT_EPSILON + sum[0]) / (FLT_EPSILON + sum[1]));
            gainBoost = std::min(1.584893192f, gainBoost);
            for (int m = from; m < to; m++) {
                s.gain[e][m] *= gainBoost;
                s.qM[e][m] *= gainBoost;
                s.sM[e][m] *= gainBoost;
            }
        }
    }
}

void hfGFilt(float (*Y)[2], const float (*Xhigh)[40][2], const float* gFilt, int mMax, int ixh) {
    for (int m = 0; m < mMax; m++) {
        Y[m][0] = Xhigh[m][ixh][0] * gFilt[m];
        Y[m][1] = Xhigh[m][ixh][1] * gFilt[m];
    }
}

void hfApplyNoise(float (*Y)[2], const float* sM, const float* qFilt, int noise, float phiSign0, float phiSign1, int mMax) {
    for (int m = 0; m < mMax; m++) {
        float y0 = Y[m][0];
        float y1 = Y[m][1];
        noise = (noise + 1) & 0x1ff;
        if (sM[m]) {
            y0 += sM[m] * phiSign0;
            y1 += sM[m] * phiSign1;
        } else {
            y0 += qFilt[m] * kSbrNoise[noise][0];
            y1 += qFilt[m] * kSbrNoise[noise][1];
        }
        Y[m][0] = y0;
        Y[m][1] = y1;
        phiSign1 = -phiSign1;
    }
}

void hfAssemble(float Y1[38][64][2], const float Xhigh[64][40][2], Sbr& sbr, SbrScratch& s, SbrData& ch, const int ea[2]) {
    const int hSL = 4 * !sbr.smoothingMode;
    const int kx = sbr.kx[1];
    const int mMax = sbr.m[1];
    static const float hSmooth[5] = {0.33333333333333f, 0.30150283239582f, 0.21816949906249f, 0.11516383427084f, 0.03183050093751f};
    float (*gTemp)[48] = ch.gTemp;
    float (*qTemp)[48] = ch.qTemp;
    int indexNoise = int(ch.fIndexNoise);
    int indexSine = int(ch.fIndexSine);
    if (sbr.reset) {
        for (int i = 0; i < hSL; i++) {
            memcpy(gTemp[i + 2 * ch.tEnv[0]], s.gain[0], size_t(mMax) * sizeof s.gain[0][0]);
            memcpy(qTemp[i + 2 * ch.tEnv[0]], s.qM[0], size_t(mMax) * sizeof s.qM[0][0]);
        }
    } else if (hSL) {
        for (int i = 0; i < 4; i++) {
            memcpy(gTemp[i + 2 * ch.tEnv[0]], gTemp[i + 2 * ch.tEnvNumEnvOld], sizeof gTemp[0]);
            memcpy(qTemp[i + 2 * ch.tEnv[0]], qTemp[i + 2 * ch.tEnvNumEnvOld], sizeof qTemp[0]);
        }
    }
    for (int e = 0; e < ch.numEnv; e++) {
        for (int i = 2 * ch.tEnv[e]; i < 2 * ch.tEnv[e + 1]; i++) {
            memcpy(gTemp[hSL + i], s.gain[e], size_t(mMax) * sizeof s.gain[0][0]);
            memcpy(qTemp[hSL + i], s.qM[e], size_t(mMax) * sizeof s.qM[0][0]);
        }
    }
    for (int e = 0; e < ch.numEnv; e++) {
        for (int i = 2 * ch.tEnv[e]; i < 2 * ch.tEnv[e + 1]; i++) {
            alignas(16) float gFiltTab[48];
            alignas(16) float qFiltTab[48];
            float *gFilt, *qFilt;
            if (hSL && e != ea[0] && e != ea[1]) {
                gFilt = gFiltTab;
                qFilt = qFiltTab;
                for (int m = 0; m < mMax; m++) {
                    const int idx1 = i + hSL;
                    gFilt[m] = 0.0f;
                    qFilt[m] = 0.0f;
                    for (int j = 0; j <= hSL; j++) {
                        gFilt[m] += gTemp[idx1 - j][m] * hSmooth[j];
                        qFilt[m] += qTemp[idx1 - j][m] * hSmooth[j];
                    }
                }
            } else {
                gFilt = gTemp[i + hSL];
                qFilt = qTemp[i];
            }
            hfGFilt(Y1[i] + kx, Xhigh + kx, gFilt, mMax, i + kEnvelopeOffset);
            if (e != ea[0] && e != ea[1]) {
                const float phiSign = float(1 - 2 * (kx & 1));
                switch (indexSine) {
                    case 0: hfApplyNoise(Y1[i] + kx, s.sM[e], qFilt, indexNoise, 1.0f, 0.0f, mMax); break;
                    case 1: hfApplyNoise(Y1[i] + kx, s.sM[e], qFilt, indexNoise, 0.0f, phiSign, mMax); break;
                    case 2: hfApplyNoise(Y1[i] + kx, s.sM[e], qFilt, indexNoise, -1.0f, 0.0f, mMax); break;
                    default: hfApplyNoise(Y1[i] + kx, s.sM[e], qFilt, indexNoise, 0.0f, -phiSign, mMax); break;
                }
            } else {
                int idx = indexSine & 1;
                int A = (1 - ((indexSine + (kx & 1)) & 2));
                int B = (A ^ (-idx)) + idx;
                float* out = &Y1[i][kx][idx];
                const float* in = s.sM[e];
                int m;
                for (m = 0; m + 1 < mMax; m += 2) {
                    out[2 * m] += in[m] * float(A);
                    out[2 * m + 2] += in[m + 1] * float(B);
                }
                if (mMax & 1) out[2 * m] += in[m] * float(A);
            }
            indexNoise = (indexNoise + mMax) & 0x1ff;
            indexSine = (indexSine + 1) & 3;
        }
    }
    ch.fIndexNoise = unsigned(indexNoise);
    ch.fIndexSine = unsigned(indexSine);
}

void qmfAnalysis(PreciseMdct& mdct, const float* in, float* x, float W[2][32][32][2], int bufIdx) {
    const int nb = kTimeSlots * 64;
    memcpy(x, x + nb, (320 - 32) * sizeof x[0]);
    memcpy(x + 288, in, size_t(nb) * sizeof x[0]);
    float z[320];
    double zin[64], zout[64];
    for (int i = 0; i < kTimeSlots * 2; i++) {
        for (int k = 0; k < 320; k++) z[k] = kQmfWindowDown[k] * x[319 - k];
        for (int k = 0; k < 64; k++) z[k] = ((z[k] + z[k + 128]) + z[k + 256]) + (z[k + 64] + z[k + 192]);
        zin[0] = z[0];
        zin[1] = z[1];
        for (int k = 1; k < 31; k += 2) {
            zin[2 * k + 0] = -z[64 - k];
            zin[2 * k + 1] = z[k + 1];
            zin[2 * k + 2] = -z[63 - k];
            zin[2 * k + 3] = z[k + 2];
        }
        zin[2 * 31 + 0] = -z[64 - 31];
        zin[2 * 31 + 1] = z[31 + 1];
        mdct.half(zin, zout);
        for (int k = 0; k < 32; k += 2) {
            W[bufIdx][i][k][0] = -float(zout[63 - k]);
            W[bufIdx][i][k][1] = float(zout[k]);
            W[bufIdx][i][k + 1][0] = -float(zout[62 - k]);
            W[bufIdx][i][k + 1][1] = float(zout[k + 1]);
        }
        x += 32;
    }
}

template <bool Fused>
inline void windowSum(float* out, const float* v, const float* window, int len, unsigned div) {
    static const int vAt[10] = {0, 192, 256, 448, 512, 704, 768, 960, 1024, 1216};
    for (int k = 0; k < len; k++) out[k] = v[k] * window[k];
    for (int j = 1; j < 10; j++) {
        const float* vj = v + (vAt[j] >> div);
        const float* wj = window + j * len;
        for (int k = 0; k < len; k++) {
            if constexpr (Fused) {
                out[k] = __builtin_fmaf(vj[k], wj[k], out[k]);
            } else {
                const float product = vj[k] * wj[k];
                out[k] = product + out[k];
            }
        }
    }
}

#if defined(__x86_64__) || defined(_M_X64)
__attribute__((target("fma"))) void windowSumFused(float* out, const float* v, const float* window, int len, unsigned div) {
    windowSum<true>(out, v, window, len, div);
}
#else
void windowSumFused(float* out, const float* v, const float* window, int len, unsigned div) { windowSum<true>(out, v, window, len, div); }
#endif

void qmfSynthesis(PreciseMdct& mdct, float* out, float X[2][38][64], float* v0, int* vOff, unsigned div) {
    const float* window = div ? kQmfWindowDown : kQmfWindowUp;
    const int step = 128 >> div;
    double in0[64], in1[64], buf0[64], buf1[64];
    for (int i = 0; i < kTimeSlots * 2; i++) {
        if (*vOff < step) {
            int savedSamples = (1280 - 128) >> div;
            memcpy(&v0[kSynthesisSize - savedSamples], v0, size_t(savedSamples) * sizeof(float));
            *vOff = kSynthesisSize - savedSamples - step;
        } else {
            *vOff -= step;
        }
        float* v = v0 + *vOff;
        if (div) {
            for (int n = 0; n < 32; n++) {
                X[0][i][n] = -X[0][i][n];
                X[0][i][32 + n] = X[1][i][31 - n];
            }
            for (int n = 0; n < 64; n++) in0[n] = X[0][i][n];
            mdct.half(in0, buf0);
            for (int n = 0; n < 32; n++) {
                v[n] = float(buf0[63 - 2 * n]);
                v[63 - n] = -float(buf0[63 - 2 * n - 1]);
            }
        } else {
            for (int n = 0; n < 64; n++) {
                in0[n] = X[0][i][n];
                in1[n] = (n & 1) ? -X[1][i][n] : X[1][i][n];
            }
            mdct.half(in0, buf0);
            mdct.half(in1, buf1);
            for (int n = 0; n < 64; n++) {
                const float a = float(buf1[n]), b = float(buf0[63 - n]);
                v[n] = a - b;
                v[127 - n] = a + b;
            }
        }
        const int len = 64 >> div;
        if (fastFma()) windowSumFused(out, v, window, len, div);
        else windowSum<false>(out, v, window, len, div);
        out += len;
    }
}

void lfGen(Sbr& sbr, float Xlow[32][40][2], const float W[2][32][32][2], int bufIdx) {
    const int tHfGen = 8;
    const int iF = kTimeSlots * 2;
    memset(Xlow, 0, 32 * sizeof *Xlow);
    for (int k = 0; k < sbr.kx[1]; k++) {
        for (int i = tHfGen; i < iF + tHfGen; i++) {
            Xlow[k][i][0] = W[bufIdx][i - tHfGen][k][0];
            Xlow[k][i][1] = W[bufIdx][i - tHfGen][k][1];
        }
    }
    bufIdx = 1 - bufIdx;
    for (int k = 0; k < sbr.kx[0]; k++) {
        for (int i = 0; i < tHfGen; i++) {
            Xlow[k][i][0] = W[bufIdx][i + iF - tHfGen][k][0];
            Xlow[k][i][1] = W[bufIdx][i + iF - tHfGen][k][1];
        }
    }
}

void hfGenDsp(float (*Xhigh)[2], const float (*Xlow)[2], const float alpha0[2], const float alpha1[2], float bw, int start, int end) {
    const float a0 = alpha1[0] * bw * bw, a1 = alpha1[1] * bw * bw, a2 = alpha0[0] * bw, a3 = alpha0[1] * bw;
    for (int i = start; i < end; i++) {
        const float r2 = Xlow[i - 2][0], i2 = Xlow[i - 2][1], r1 = Xlow[i - 1][0], i1 = Xlow[i - 1][1];
        Xhigh[i][0] = ((-(i1 * a3) + -(i2 * a1)) + Xlow[i][0]) + (r2 * a0 + r1 * a2);
        Xhigh[i][1] = ((r1 * a3 + r2 * a1) + Xlow[i][1]) + (i2 * a0 + i1 * a2);
    }
}

int hfGen(Sbr& sbr, float Xhigh[64][40][2], const float Xlow[32][40][2], const float (*alpha0)[2], const float (*alpha1)[2], const float bwArray[5],
          const uint8_t* tEnv, int numEnv) {
    int g = 0;
    int k = sbr.kx[1];
    for (int j = 0; j < sbr.numPatches; j++) {
        for (int x = 0; x < sbr.patchNumSubbands[j]; x++, k++) {
            const int p = sbr.patchStartSubband[j] + x;
            while (g <= sbr.nQ && k >= sbr.fTableNoise[g]) g++;
            g--;
            if (g < 0 || k >= 64 || p >= 32) return -1;
            hfGenDsp(Xhigh[k] + kEnvelopeOffset, Xlow[p] + kEnvelopeOffset, alpha0[p], alpha1[p], bwArray[g], 2 * tEnv[0], 2 * tEnv[numEnv]);
        }
    }
    if (k < sbr.m[1] + sbr.kx[1]) memset(Xhigh + k, 0, size_t(sbr.m[1] + sbr.kx[1] - k) * sizeof *Xhigh);
    return 0;
}

void xGen(Sbr& sbr, float X[2][38][64], const float Y0[38][64][2], const float Y1[38][64][2], const float Xlow[32][40][2], int ch) {
    const int iF = kTimeSlots * 2;
    const int iTemp = std::max(2 * sbr.data[ch].tEnvNumEnvOld - iF, 0);
    memset(X, 0, 2 * sizeof *X);
    int k;
    for (k = 0; k < sbr.kx[0]; k++) {
        for (int i = 0; i < iTemp; i++) {
            X[0][i][k] = Xlow[k][i + kEnvelopeOffset][0];
            X[1][i][k] = Xlow[k][i + kEnvelopeOffset][1];
        }
    }
    for (; k < sbr.kx[0] + sbr.m[0]; k++) {
        for (int i = 0; i < iTemp; i++) {
            X[0][i][k] = Y0[i + iF][k][0];
            X[1][i][k] = Y0[i + iF][k][1];
        }
    }
    for (k = 0; k < sbr.kx[1]; k++) {
        for (int i = iTemp; i < 38; i++) {
            X[0][i][k] = Xlow[k][i + kEnvelopeOffset][0];
            X[1][i][k] = Xlow[k][i + kEnvelopeOffset][1];
        }
    }
    for (; k < sbr.kx[1] + sbr.m[1]; k++) {
        for (int i = iTemp; i < iF; i++) {
            X[0][i][k] = Y1[i][k][0];
            X[1][i][k] = Y1[i][k][1];
        }
    }
}

int mapping(Sbr& sbr, SbrScratch& s, SbrData& ch, int ea[2]) {
    memset(ch.sIndexMapped[1], 0, 7 * sizeof ch.sIndexMapped[1]);
    const int kx = sbr.kx[1];
    for (int e = 0; e < ch.numEnv; e++) {
        const int ilim = sbr.n[ch.freqRes[e + 1]];
        const uint16_t* table = ch.freqRes[e + 1] ? sbr.fTableHigh : sbr.fTableLow;
        if (sbr.kx[1] != table[0]) {
            sbrTurnoff(sbr);
            return -1;
        }
        for (int i = 0; i < ilim; i++)
            for (int m = table[i]; m < table[i + 1]; m++) s.eOrigMapped[e][m - kx] = ch.envFacs[e + 1][i];
        int k = (ch.numNoise > 1) && (ch.tEnv[e] >= ch.tQ[1]);
        for (int i = 0; i < sbr.nQ; i++)
            for (int m = sbr.fTableNoise[i]; m < sbr.fTableNoise[i + 1]; m++) s.qMapped[e][m - kx] = ch.noiseFacs[k + 1][i];
        for (int i = 0; i < sbr.n[1]; i++) {
            if (ch.addHarmonicFlag) {
                const int mid = (sbr.fTableHigh[i] + sbr.fTableHigh[i + 1]) >> 1;
                ch.sIndexMapped[e + 1][mid - kx] = uint8_t(ch.addHarmonic[i] * (e >= ea[1] || (ch.sIndexMapped[0][mid - kx] == 1)));
            }
        }
        for (int i = 0; i < ilim; i++) {
            int present = 0;
            for (int m = table[i]; m < table[i + 1]; m++) {
                if (ch.sIndexMapped[e + 1][m - kx]) {
                    present = 1;
                    break;
                }
            }
            memset(&s.sMapped[e][table[i] - kx], present, size_t(table[i + 1] - table[i]) * sizeof s.sMapped[e][0]);
        }
    }
    memcpy(ch.sIndexMapped[0], ch.sIndexMapped[ch.numEnv], sizeof ch.sIndexMapped[0]);
    return 0;
}

float sumSquare(const float (*x)[2], int n) {
    const float* f = &x[0][0];
    float acc0[4] = {}, acc1[4] = {};
    for (int blocks = n >> 3; blocks; blocks--, f += 16) {
        for (int k = 0; k < 4; k++) {
            acc0[k] += f[k] * f[k];
            acc1[k] += f[4 + k] * f[4 + k];
            acc0[k] += f[8 + k] * f[8 + k];
            acc1[k] += f[12 + k] * f[12 + k];
        }
    }
    for (int rest = (n & 7) >> 1; rest; rest--, f += 4)
        for (int k = 0; k < 4; k++) acc0[k] += f[k] * f[k];
    float lane[4];
    for (int k = 0; k < 4; k++) lane[k] = acc0[k] + acc1[k];
    return (lane[0] + lane[2]) + (lane[1] + lane[3]);
}

void envEstimate(float (*eCurr)[48], const float Xhigh[64][40][2], Sbr& sbr, SbrData& ch) {
    const int kx1 = sbr.kx[1];
    if (sbr.interpolFreq) {
        for (int e = 0; e < ch.numEnv; e++) {
            const float recipEnvSize = 0.5f / float(ch.tEnv[e + 1] - ch.tEnv[e]);
            int ilb = ch.tEnv[e] * 2 + kEnvelopeOffset;
            int iub = ch.tEnv[e + 1] * 2 + kEnvelopeOffset;
            if (ilb >= 40) return;
            for (int m = 0; m < sbr.m[1]; m++) {
                float sum = sumSquare(Xhigh[m + kx1] + ilb, iub - ilb);
                eCurr[e][m] = sum * recipEnvSize;
            }
        }
    } else {
        for (int e = 0; e < ch.numEnv; e++) {
            const int envSize = 2 * (ch.tEnv[e + 1] - ch.tEnv[e]);
            int ilb = ch.tEnv[e] * 2 + kEnvelopeOffset;
            int iub = ch.tEnv[e + 1] * 2 + kEnvelopeOffset;
            const uint16_t* table = ch.freqRes[e + 1] ? sbr.fTableHigh : sbr.fTableLow;
            if (ilb >= 40) return;
            for (int p = 0; p < sbr.n[ch.freqRes[e + 1]]; p++) {
                float sum = 0.0f;
                const int den = envSize * (table[p + 1] - table[p]);
                for (int k = table[p]; k < table[p + 1]; k++) sum += sumSquare(Xhigh[k] + ilb, iub - ilb);
                sum /= float(den);
                for (int k = table[p]; k < table[p + 1]; k++) eCurr[e][k - kx1] = sum;
            }
        }
    }
}

}

void sbrTurnoff(Sbr& sbr) {
    sbr.start = 0;
    sbr.readyForDequant = 0;
    sbr.kx[1] = 32;
    sbr.m[1] = 0;
    sbr.data[0].ea[1] = sbr.data[1].ea[1] = -1;
    memset(&sbr.spectrum, -1, sizeof sbr.spectrum);
}

void sbrInit(Sbr& sbr, int idAac) {
    tables();
    sbr.kx[0] = sbr.kx[1];
    sbr.idAac = idAac;
    sbrTurnoff(sbr);
    sbr.data[0].synthesisOffset = kSynthesisSize - (1280 - 128);
    sbr.data[1].synthesisOffset = kSynthesisSize - (1280 - 128);
}

int sbrDecodeExtension(Sbr& sbr, StreamConfig& config, BitReader& host, int crc, int cnt, int idAac) {
    int64_t numSbrBits = 0;
    BitReader gb = host;
    host.skip(int64_t(cnt) * 8 - 4);
    sbr.reset = 0;
    if (!sbr.sampleRate) sbr.sampleRate = 2 * config.sampleRate;
    if (!config.extSampleRate) config.extSampleRate = 2 * config.sampleRate;
    if (crc) {
        gb.skip(10);
        numSbrBits += 10;
    }
    sbr.kx[0] = sbr.kx[1];
    sbr.m[0] = sbr.m[1];
    sbr.kxAndMPushed = 1;
    numSbrBits++;
    if (gb.bit()) numSbrBits += readHeader(sbr, gb);
    if (sbr.reset) sbrReset(sbr);
    if (sbr.start) numSbrBits += readData(sbr, config, gb, idAac);
    int64_t numAlignBits = ((int64_t(cnt) << 3) - 4 - numSbrBits) & 7;
    int64_t bytesRead = (numSbrBits + numAlignBits + 4) >> 3;
    if (bytesRead > cnt) sbrTurnoff(sbr);
    return cnt;
}

void sbrApply(Sbr& sbr, SbrTools& tools, const StreamConfig& config, int idAac, float* left, float* right) {
    SbrScratch& s = tools.scratch;
    int downsampled = config.extSampleRate < sbr.sampleRate;
    int nch = idAac == kCpe ? 2 : 1;
    if (idAac != sbr.idAac) sbrTurnoff(sbr);
    if (sbr.start && !sbr.readyForDequant) sbrTurnoff(sbr);
    if (!sbr.kxAndMPushed) {
        sbr.kx[0] = sbr.kx[1];
        sbr.m[0] = sbr.m[1];
    } else {
        sbr.kxAndMPushed = 0;
    }
    if (sbr.start) {
        dequant(sbr, idAac);
        sbr.readyForDequant = 0;
    }
    for (int ch = 0; ch < nch; ch++) {
        SbrData& d = sbr.data[ch];
        qmfAnalysis(tools.analysis, ch ? right : left, d.analysis, d.W, d.Ypos);
        lfGen(sbr, s.Xlow, d.W, d.Ypos);
        d.Ypos ^= 1;
        if (sbr.start) {
            hfInverseFilter(s.alpha0, s.alpha1, s.Xlow, sbr.k[0]);
            chirp(sbr, d);
            hfGen(sbr, s.Xhigh, s.Xlow, s.alpha0, s.alpha1, d.bwArray, d.tEnv, d.numEnv);
            if (mapping(sbr, s, d, d.ea) == 0) {
                envEstimate(s.eCurr, s.Xhigh, sbr, d);
                gainCalc(sbr, s, d, d.ea);
                hfAssemble(d.Y[d.Ypos], s.Xhigh, sbr, s, d, d.ea);
            }
        }
        xGen(sbr, s.X[ch], d.Y[1 - d.Ypos], d.Y[d.Ypos], s.Xlow, ch);
    }
    if (config.ps == 1) {
        if (sbr.ps.common.start) psApply(sbr.ps, s.ps, s.X[0], s.X[1], sbr.kx[1] + sbr.m[1]);
        else memcpy(s.X[1], s.X[0], sizeof s.X[0]);
        nch = 2;
    }
    qmfSynthesis(tools.synthesis, left, s.X[0], sbr.data[0].synthesis, &sbr.data[0].synthesisOffset, unsigned(downsampled));
    if (nch == 2) qmfSynthesis(tools.synthesis, right, s.X[1], sbr.data[1].synthesis, &sbr.data[1].synthesisOffset, unsigned(downsampled));
}

}


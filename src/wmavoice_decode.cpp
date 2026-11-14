#include "audio_codec.h"
#include "mdct.h"
#include "read_common.h"
#include "wmavoice.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

namespace absent {

namespace {

using namespace voice;

const int kMaxLsps = 16;
const int kFrame = 160;
const int kHistory = 416;
const int kSuper = 480;
const int kCacheBytes = 256;

enum { AcbNone, AcbAsymmetric, AcbHamming };
enum { FcbSilence, FcbHardcoded, FcbAw, FcbPulses };

struct FrameType {
    uint8_t blocks, logBlocks, acb, fcb, doubles;
};

const FrameType kTypes[17] = {
    {1, 0, AcbNone, FcbSilence, 0},       {2, 1, AcbNone, FcbHardcoded, 0},    {2, 1, AcbAsymmetric, FcbAw, 0},
    {2, 1, AcbAsymmetric, FcbPulses, 2},  {2, 1, AcbAsymmetric, FcbPulses, 5}, {4, 2, AcbAsymmetric, FcbPulses, 0},
    {4, 2, AcbAsymmetric, FcbPulses, 2},  {4, 2, AcbAsymmetric, FcbPulses, 5}, {2, 1, AcbHamming, FcbPulses, 0},
    {2, 1, AcbHamming, FcbPulses, 2},     {2, 1, AcbHamming, FcbPulses, 5},    {4, 2, AcbHamming, FcbPulses, 0},
    {4, 2, AcbHamming, FcbPulses, 2},     {4, 2, AcbHamming, FcbPulses, 5},    {8, 3, AcbHamming, FcbPulses, 0},
    {8, 3, AcbHamming, FcbPulses, 2},     {8, 3, AcbHamming, FcbPulses, 5}};

int log2Of(unsigned v) { return v ? 31 - __builtin_clz(v) : 0; }
int ceilLog2(unsigned v) { return log2Of((v - 1u) << 1); }
int clip(int a, int lo, int hi) { return a < lo ? lo : a > hi ? hi : a; }
float fmax2(float a, float b) { return a > b ? a : b; }
float fmin2(float a, float b) { return a > b ? b : a; }
double fmax2d(double a, double b) { return a > b ? a : b; }
double fmin2d(double a, double b) { return a > b ? b : a; }

int toInt(double v) {
    if (!(v == v)) return 0;
    if (v >= 2147483647.0) return INT_MAX;
    if (v <= -2147483648.0) return INT_MIN;
    return int(v);
}

struct Bits {
    const uint8_t* data = nullptr;
    size_t bytes = 0;
    int64_t size = 0, index = 0;

    void init(const uint8_t* d, size_t readable, int64_t bits) {
        data = d;
        bytes = readable;
        size = bits;
        index = 0;
    }
    uint32_t peek(int n) const {
        if (!n) return 0;
        const size_t at = size_t(index >> 3);
        uint64_t v = 0;
        for (size_t i = 0; i < 8; ++i) v = v << 8 | (at + i < bytes ? data[at + i] : 0);
        return uint32_t((v << (index & 7)) >> (64 - n));
    }
    uint32_t read(int n) {
        const uint32_t v = peek(n);
        index = std::min(index + n, size + 8);
        return v;
    }
    bool bit() {
        const bool v = peek(1);
        if (index < size + 8) ++index;
        return v;
    }
    void skip(int n) { index = std::min(index + n, size + 8); }
    void skipLong(int64_t n) { index += std::clamp(n, -index, size + 8 - index); }
    int64_t left() const { return size - index; }
    int frameType() {
        const uint32_t v = peek(14);
        int level = 0;
        while (level < 6 && ((v >> (12 - 2 * level)) & 3) == 3) ++level;
        const int code = level < 6 ? int((v >> (12 - 2 * level)) & 3) : int(v & 3);
        skip(2 * level + 2);
        return 3 * level + code;
    }
};

struct State {
    double prevLsps[kMaxLsps];
    int lastPitch, lastAcb, pitchDiff;
    float silenceGain;
    int awExt, awRange, awPulses[2], awFirst[2], awNext;
    int counter, superframes;
    float gainErr[6];
    float excitation[kHistory];
    float synth[kMaxLsps];
    float agc, dcf[2];
    float zeroExc[kHistory + kSuper];
    float denoise[kFrame];
    int denoiseSize;
    float synthOut[0x80 + kMaxLsps];
    uint8_t cache[kCacheBytes + 64];
    int cacheSize, spillover, residual, skipNext;
    int64_t putCount;
};

struct WmaVoice : PacketDecoder {
    int blockAlign = 0, spilloverBits = 0, history = 0;
    bool apf = false, tiltCorrection = false, quiet = false, music = false;
    int denoiseStrength = 0, dcLevel = 0, lsps = 10, qMode = 0, defMode = 0;
    int minPitch = 0, maxPitch = 0, pitchBits = 0, blockPitchBits = 0, blockPitchRange = 0, deltaBits = 0, deltaRange = 0;
    uint16_t conv[4] = {};
    int8_t tree[25] = {};
    float sinTab[511] = {}, cosTab[511] = {};
    std::unique_ptr<State> st = std::make_unique<State>();
    std::unique_ptr<Transforms> tx;
    float tilted[0x82] = {}, coeffsPf[0x82] = {};
    Bits gb;
    std::vector<float>* sink = nullptr;
    int64_t made = 0;

    WmaVoice() = default;
    WmaVoice(const WmaVoice& o) : PacketDecoder(o) {
        blockAlign = o.blockAlign;
        spilloverBits = o.spilloverBits;
        history = o.history;
        apf = o.apf;
        tiltCorrection = o.tiltCorrection;
        denoiseStrength = o.denoiseStrength;
        dcLevel = o.dcLevel;
        lsps = o.lsps;
        qMode = o.qMode;
        defMode = o.defMode;
        minPitch = o.minPitch;
        maxPitch = o.maxPitch;
        pitchBits = o.pitchBits;
        blockPitchBits = o.blockPitchBits;
        blockPitchRange = o.blockPitchRange;
        deltaBits = o.deltaBits;
        deltaRange = o.deltaRange;
        memcpy(conv, o.conv, sizeof conv);
        memcpy(tree, o.tree, sizeof tree);
    }

    bool setup(const std::vector<uint8_t>& w, std::string* error) {
        auto fail = [&](const char* text) {
            if (error) *error = text;
            return false;
        };
        rate = int(le32(w.data() + 4));
        channels = 1;
        blockAlign = int(le16(w.data() + 12));
        size_t extra = w.size() >= 18 ? std::min<size_t>(le16(w.data() + 16), w.size() - 18) : 0;
        const uint8_t* e = w.data() + 18;
        if (le16(w.data()) == 0xFFFE && extra >= 22) {
            e += 22;
            extra -= 22;
        }
        if (extra != 46) return fail("Windows Media Audio 9 Voice track without its codec data");
        if (blockAlign <= 0 || blockAlign > (1 << 22)) return fail("Windows Media Audio track without a block size");
        const uint32_t flags = le32(e + 18);
        spilloverBits = 3 + ceilLog2(unsigned(blockAlign));
        apf = flags & 1;
        denoiseStrength = int((flags >> 2) & 0xF);
        if (denoiseStrength >= 12) return fail("Windows Media Audio 9 Voice track with a denoise strength over 11");
        tiltCorrection = flags & 0x40;
        dcLevel = int((flags >> 7) & 0xF);
        qMode = (flags & 0x2000) ? 1 : 0;
        defMode = (flags & 0x4000) ? 1 : 0;
        lsps = (flags & 0x1000) ? 16 : 10;
        Bits b;
        b.init(e + 22, 24, 24 * 8);
        int count[8] = {};
        memset(tree, 0xff, sizeof tree);
        for (int n = 0; n < 17; n++) {
            const int r = int(b.read(3));
            if (count[r] > 3) return fail("Windows Media Audio 9 Voice track with a broken frame type tree");
            tree[r * 3 + count[r]++] = int8_t(n);
        }
        if (rate < 1 || rate >= INT_MAX / (256 * 37)) return fail("Windows Media Audio 9 Voice sample rate not supported");
        minPitch = ((rate << 8) / 400 + 50) >> 8;
        maxPitch = ((rate << 8) * 37 / 2000 + 50) >> 8;
        const int range = maxPitch - minPitch;
        if (range <= 0) return fail("Windows Media Audio 9 Voice sample rate not supported");
        pitchBits = ceilLog2(unsigned(range));
        history = maxPitch + 8;
        if (minPitch < 1 || history > kHistory) return fail("Windows Media Audio 9 Voice sample rate not supported (322 to 22,097 Hz)");
        conv[0] = uint16_t(minPitch);
        conv[1] = uint16_t((range * 25) >> 6);
        conv[2] = uint16_t((range * 44) >> 6);
        conv[3] = uint16_t(maxPitch - 1);
        deltaRange = (range >> 3) & ~0xF;
        if (deltaRange <= 0) return fail("Windows Media Audio 9 Voice sample rate not supported");
        deltaBits = 1 + ceilLog2(unsigned(deltaRange));
        blockPitchRange = conv[2] + conv[3] + 1 + 2 * (conv[1] - 2 * minPitch);
        blockPitchBits = ceilLog2(unsigned(blockPitchRange));
        if (apf) {
            tx = std::make_unique<Transforms>();
            const float* window = sineWindow(8);
            memcpy(cosTab, window, 256 * sizeof(float));
            memcpy(&sinTab[255], cosTab, 256 * sizeof(float));
            for (int n = 0; n < 255; n++) {
                sinTab[n] = -sinTab[510 - n];
                cosTab[510 - n] = cosTab[n];
            }
        }
        reset();
        return true;
    }

    void defaultLsps() {
        for (int n = 0; n < lsps; n++) st->prevLsps[n] = M_PI * (n + 1.0) / (lsps + 1.0);
    }

    void reset() override {
        memset(st.get(), 0, sizeof(State));
        defaultLsps();
        st->lastPitch = 40;
        st->lastAcb = AcbNone;
        music = false;
    }

    void flush() {
        State& s = *st;
        s.agc = 0;
        s.cacheSize = 0;
        s.skipNext = 0;
        defaultLsps();
        memset(s.excitation, 0, sizeof s.excitation);
        memset(s.synth, 0, sizeof s.synth);
        memset(s.gainErr, 0, sizeof s.gainErr);
        if (apf) {
            memset(&s.synthOut[kMaxLsps - lsps], 0, sizeof(float) * size_t(lsps));
            memset(s.dcf, 0, sizeof s.dcf);
            memset(s.zeroExc, 0, sizeof(float) * size_t(history));
            memset(s.denoise, 0, sizeof s.denoise);
        }
    }

    bool save(std::vector<uint8_t>& out) const override {
        out.resize(sizeof(State));
        memcpy(out.data(), st.get(), sizeof(State));
        return true;
    }

    void load(const std::vector<uint8_t>& in) override {
        if (in.size() != sizeof(State)) return;
        memcpy(st.get(), in.data(), sizeof(State));
        State& s = *st;
        s.denoiseSize = std::clamp(s.denoiseSize, 0, kFrame);
        s.cacheSize = std::clamp(s.cacheSize, 0, kCacheBytes * 8);
        s.putCount = std::clamp<int64_t>(s.putCount, 0, kCacheBytes * 8);
        s.lastAcb = std::clamp(s.lastAcb, 0, 2);
        s.lastPitch = std::clamp(s.lastPitch, 0, maxPitch);
    }

    float tiltFactor(const float* lpcs, int n) {
        const float rh0 = 1.0 + dot(lpcs, lpcs, n);
        const float rh1 = lpcs[0] + dot(lpcs, &lpcs[1], n - 1);
        return rh1 / rh0;
    }

    void inputResponse(float* lpcsSrc, int fcb, float* coeffsDst, int remainder) {
        float last, mn = 15.0, mx = -15.0;
        float coeffs[0x82], lpcs[0x82], lpcsDct[0x82];
        memcpy(coeffs, coeffsDst, sizeof coeffs);
        tx->rdft(lpcs, lpcsSrc);
        auto logRange = [&](float& var, float value) {
            const float t = log10f(value);
            var = t;
            mx = fmax2(mx, t);
            mn = fmin2(mn, t);
        };
        logRange(last, lpcs[64] * lpcs[64]);
        for (int n = 1; n < 64; n++) logRange(lpcs[n], lpcs[n * 2] * lpcs[n * 2] + lpcs[n * 2 + 1] * lpcs[n * 2 + 1]);
        logRange(lpcs[0], lpcs[0] * lpcs[0]);
        const float range = mx - mn;
        lpcs[64] = last;
        const float irange = 64.0 / range;
        const float gainMul = range * (fcb == FcbHardcoded ? (5.0 / 13.0) : (5.0 / 14.7));
        const float angleMul = gainMul * (8.0 * M_LN10 / M_PI);
        for (int n = 0; n <= 64; n++) {
            int idx = int(lrint((mx - lpcs[n]) * irange - 1));
            idx = clip(idx, 0, 63);
            const float pwr = kDenoisePower[denoiseStrength][idx];
            lpcs[n] = angleMul * pwr;
            idx = toInt(fmin2d(fmax2d((pwr * gainMul - 0.0295) * 70.570526123, 0), INT_MAX / 2));
            if (idx > 127) coeffs[n] = kEnergy[127] * powf(1.0331663, float(idx - 127));
            else coeffs[n] = kEnergy[std::max(0, idx)];
        }
        tx->dct(lpcsDct, lpcs);
        tx->dst(lpcs, lpcsDct);
        int idx = 255 + clip(toInt(lpcs[64]), -255, 255);
        coeffs[0] = coeffs[0] * cosTab[idx];
        idx = 255 + clip(toInt(lpcs[64] - 2 * lpcs[63]), -255, 255);
        last = coeffs[64] * cosTab[idx];
        for (int n = 63;; n--) {
            idx = 255 + clip(toInt(-lpcs[64] - 2 * lpcs[n - 1]), -255, 255);
            coeffs[n * 2 + 1] = coeffs[n] * sinTab[idx];
            coeffs[n * 2] = coeffs[n] * cosTab[idx];
            if (!--n) break;
            idx = 255 + clip(toInt(lpcs[64] - 2 * lpcs[n - 1]), -255, 255);
            coeffs[n * 2 + 1] = coeffs[n] * sinTab[idx];
            coeffs[n * 2] = coeffs[n] * cosTab[idx];
        }
        coeffs[64] = last;
        tx->irdft(coeffsDst, coeffs);
        memset(&coeffsDst[remainder], 0, sizeof(float) * size_t(128 - remainder));
        if (tiltCorrection) {
            float mem = 0;
            coeffsDst[remainder - 1] = 0;
            tilt(&mem, -1.8 * tiltFactor(coeffsDst, remainder - 1), coeffsDst, remainder);
        }
        const float sq = (1.0 / 64.0) * sqrtf(1 / dot(coeffsDst, coeffsDst, remainder));
        for (int n = 0; n < remainder; n++) coeffsDst[n] *= sq;
    }

    void wiener(int fcb, float* synthPf, int size, const float* lpcs) {
        State& s = *st;
        int remainder = 0;
        if (fcb != FcbSilence) {
            float coeffsF[0x82], synthF[0x82];
            float mem = 0;
            tilted[0] = 1.0;
            memcpy(&tilted[1], lpcs, sizeof(float) * size_t(lsps));
            memset(&tilted[lsps + 1], 0, sizeof(float) * size_t(128 - lsps - 1));
            tilt(&mem, 0.7 * tiltFactor(lpcs, lsps), tilted, lsps + 2);
            remainder = std::min(127 - size, size - 1);
            inputResponse(tilted, fcb, coeffsPf, remainder);
            memset(&synthPf[size], 0, sizeof(float) * size_t(128 - size));
            tx->rdft(synthF, synthPf);
            tx->rdft(coeffsF, coeffsPf);
            synthF[0] *= coeffsF[0];
            synthF[1] *= coeffsF[1];
            for (int n = 1; n <= 64; n++) {
                const float v1 = synthF[n * 2], v2 = synthF[n * 2 + 1];
                synthF[n * 2] = v1 * coeffsF[n * 2] - v2 * coeffsF[n * 2 + 1];
                synthF[n * 2 + 1] = v2 * coeffsF[n * 2] + v1 * coeffsF[n * 2 + 1];
            }
            tx->irdft(synthPf, synthF);
        }
        if (s.denoiseSize) {
            const int lim = std::min(s.denoiseSize, size);
            for (int n = 0; n < lim; n++) synthPf[n] += s.denoise[n];
            s.denoiseSize -= lim;
            memmove(s.denoise, &s.denoise[size], sizeof(float) * size_t(std::max(0, std::min(s.denoiseSize, kFrame - size))));
        }
        if (fcb != FcbSilence) {
            const int lim = std::min(remainder, s.denoiseSize);
            for (int n = 0; n < lim; n++) s.denoise[n] += synthPf[size + n];
            if (lim < remainder) {
                memcpy(&s.denoise[lim], &synthPf[size + lim], sizeof(float) * size_t(remainder - lim));
                s.denoiseSize = remainder;
            }
        }
    }

    int kalman(int pitch, const float* in, float* out, int size) {
        float best = 0, d;
        int k = std::max(minPitch, pitch - 3);
        const int end = std::min(maxPitch, pitch + 3);
        const float* at = nullptr;
        do {
            d = dot(in, in - k, size);
            if (d > best) {
                best = d;
                at = in - k;
            }
        } while (++k <= end);
        if (best <= 0) return -1;
        d = dot(at, at, size);
        if (d <= 0) return -1;
        if (best <= d) d = d / (d + 0.6 * best);
        else d = 0.625;
        for (int n = 0; n < size; n++) out[n] = at[n] + d * (in[n] - at[n]);
        return 0;
    }

    void postfilter(const float* synth, float* samples, int size, const float* lpcs, float* zeroExc, int fcb, int pitch) {
        State& s = *st;
        float filterIn[kFrame / 2];
        float* synthPf = &s.synthOut[kMaxLsps];
        const float* input = zeroExc;
        zeroSynthesis(zeroExc, lpcs, synth, size, lsps);
        if (fcb >= FcbAw && !kalman(pitch, zeroExc, filterIn, size)) input = filterIn;
        synthesis(synthPf, lpcs, input, size, lsps);
        memcpy(&synthPf[-lsps], &synthPf[size - lsps], sizeof(float) * size_t(lsps));
        wiener(fcb, synthPf, size, lpcs);
        float speech = 0.0, post = 0.0;
        for (int i = 0; i < size; i++) {
            speech += fabsf(synth[i]);
            post += fabsf(synthPf[i]);
        }
        const float alpha = 0.99;
        const float factor = post == 0.0 ? 0.0 : (1.0 - alpha) * speech / post;
        float mem = s.agc;
        for (int i = 0; i < size; i++) {
            mem = alpha * mem + factor;
            samples[i] = synthPf[i] * mem;
        }
        s.agc = mem;
        if (dcLevel > 8) {
            static const float zeros[2] = {-1.99997, 1.0}, poles[2] = {-1.9330735188, 0.93589198496};
            secondOrder(samples, samples, zeros, poles, 0.93980580475, s.dcf, size);
        }
    }

    static void dequant(double* out, int num, const uint16_t* values, const uint16_t* sizes, int stages, const uint8_t* table, const double* mul, const double* base) {
        memset(out, 0, sizeof(double) * size_t(num));
        for (int n = 0; n < stages; n++) {
            const uint8_t* t = &table[values[n] * num];
            const double b = base[n], m = mul[n];
            for (int k = 0; k < num; k++) out[k] += b + m * t[k];
            table += sizes[n] * num;
        }
    }

    void lsp10i(Bits& g, double* out) {
        static const uint16_t sizes[4] = {256, 64, 32, 32};
        static const double mul[4] = {5.2187144800e-3, 1.4626986422e-3, 9.6179549166e-4, 1.1325736225e-3};
        static const double base[4] = {M_PI * -2.15522e-1, M_PI * -6.1646e-2, M_PI * -3.3486e-2, M_PI * -5.7408e-2};
        uint16_t v[4];
        v[0] = uint16_t(g.read(8));
        v[1] = uint16_t(g.read(6));
        v[2] = uint16_t(g.read(5));
        v[3] = uint16_t(g.read(5));
        dequant(out, 10, v, sizes, 4, kLsp10i, mul, base);
    }

    void lsp10r(Bits& g, double* i, const double* old, double* a1, double* a2) {
        static const uint16_t sizes[3] = {128, 64, 64};
        static const double mul[3] = {2.5807601174e-3, 1.2354460219e-3, 1.1763821673e-3};
        static const double base[3] = {M_PI * -1.07448e-1, M_PI * -5.2706e-2, M_PI * -5.1634e-2};
        const float(*ipol)[2][10] = qMode ? kLsp10InterB : kLsp10InterA;
        lsp10i(g, i);
        const int interpol = int(g.read(5));
        uint16_t v[3];
        v[0] = uint16_t(g.read(7));
        v[1] = uint16_t(g.read(6));
        v[2] = uint16_t(g.read(6));
        for (int n = 0; n < 10; n++) {
            const double delta = old[n] - i[n];
            a1[n] = ipol[interpol][0][n] * delta + i[n];
            a1[10 + n] = ipol[interpol][1][n] * delta + i[n];
        }
        dequant(a2, 20, v, sizes, 3, kLsp10r, mul, base);
    }

    void lsp16i(Bits& g, double* out) {
        static const uint16_t sizes[5] = {256, 64, 128, 64, 128};
        static const double mul[5] = {3.3439586280e-3, 6.9908173703e-4, 3.3216608306e-3, 1.0334960326e-3, 3.1899104283e-3};
        static const double base[5] = {M_PI * -1.27576e-1, M_PI * -2.4292e-2, M_PI * -1.28094e-1, M_PI * -3.2128e-2, M_PI * -1.29816e-1};
        uint16_t v[5];
        v[0] = uint16_t(g.read(8));
        v[1] = uint16_t(g.read(6));
        v[2] = uint16_t(g.read(7));
        v[3] = uint16_t(g.read(6));
        v[4] = uint16_t(g.read(7));
        dequant(out, 5, v, sizes, 2, kLsp16i1, mul, base);
        dequant(&out[5], 5, &v[2], &sizes[2], 2, kLsp16i2, &mul[2], &base[2]);
        dequant(&out[10], 6, &v[4], &sizes[4], 1, kLsp16i3, &mul[4], &base[4]);
    }

    void lsp16r(Bits& g, double* i, const double* old, double* a1, double* a2) {
        static const uint16_t sizes[3] = {128, 128, 128};
        static const double mul[3] = {1.2232979501e-3, 1.4062241527e-3, 1.6114744851e-3};
        static const double base[3] = {M_PI * -5.5830e-2, M_PI * -5.2908e-2, M_PI * -5.4776e-2};
        const float(*ipol)[2][16] = qMode ? kLsp16InterB : kLsp16InterA;
        lsp16i(g, i);
        const int interpol = int(g.read(5));
        uint16_t v[3];
        v[0] = uint16_t(g.read(7));
        v[1] = uint16_t(g.read(7));
        v[2] = uint16_t(g.read(7));
        for (int n = 0; n < 16; n++) {
            const double delta = old[n] - i[n];
            a1[n] = ipol[interpol][0][n] * delta + i[n];
            a1[16 + n] = ipol[interpol][1][n] * delta + i[n];
        }
        dequant(a2, 10, v, sizes, 1, kLsp16r1, mul, base);
        dequant(&a2[10], 10, &v[1], &sizes[1], 1, kLsp16r2, &mul[1], &base[1]);
        dequant(&a2[20], 12, &v[2], &sizes[2], 1, kLsp16r3, &mul[2], &base[2]);
    }

    void awCoords(Bits& g, const int* pitch) {
        static const int16_t start[94] = {-11, -9,  -7,  -5,  -3,  -1,  1,   3,   5,   7,   9,   11,  13,  15,  18,  17,  19,  20,  21,  22,  23,  24,  25,  26,
                                          27,  28,  29,  30,  31,  32,  33,  35,  37,  39,  41,  43,  45,  47,  49,  51,  53,  55,  57,  59,  61,  63,  65,  67,
                                          69,  71,  73,  75,  77,  79,  81,  83,  85,  87,  89,  91,  93,  95,  97,  99,  101, 103, 105, 107, 109, 111, 113, 115,
                                          117, 119, 121, 123, 125, 127, 129, 131, 133, 135, 137, 139, 141, 143, 145, 147, 149, 151, 153, 155, 157, 159};
        State& s = *st;
        s.awExt = 0;
        int bits = int(g.read(6));
        if (bits >= 54) {
            s.awExt = 1;
            bits += (bits - 54) * 3 + int(g.read(2));
        }
        s.awRange = std::min(pitch[0], pitch[1]) > 32 ? 24 : 16;
        int offset;
        for (offset = start[bits]; offset < 0; offset += pitch[0]) {}
        s.awPulses[0] = (pitch[0] - 1 + kFrame / 2 - offset) / pitch[0];
        s.awFirst[0] = offset - s.awRange / 2;
        offset += s.awPulses[0] * pitch[0];
        s.awPulses[1] = (pitch[1] - 1 + kFrame - offset) / pitch[1];
        s.awFirst[1] = offset - (kFrame + s.awRange) / 2;
        if (start[bits] < kFrame / 2) {
            while (s.awFirst[1] - pitch[1] + s.awRange > 0) s.awFirst[1] -= pitch[1];
            if (start[bits] < 0)
                while (s.awFirst[0] - pitch[0] + s.awRange > 0) s.awFirst[0] -= pitch[0];
        }
    }

    int awSet2(Bits& g, int block, Fixed& fcb) {
        State& s = *st;
        uint16_t maskMem[9];
        uint16_t* mask = maskMem + 2;
        int pulseOff = s.awFirst[block], range, startOff = 0;
        if (s.awPulses[block] > 0)
            while (pulseOff + s.awRange < 1) pulseOff += fcb.pitchLag;
        if (s.awPulses[0] > 0) {
            if (block == 0) {
                range = 32;
            } else {
                range = 8;
                if (s.awPulses[block] > 0) pulseOff = s.awNext;
            }
        } else {
            range = 16;
        }
        int pulseStart = s.awPulses[block] > 0 ? pulseOff - range / 2 : 0;
        mask[-2] = mask[-1] = 0;
        for (int i = 0; i < 5; i++) mask[i] = 0xFFFF;
        mask[5] = mask[6] = 0;
        if (s.awPulses[block] > 0)
            for (int idx = pulseOff; idx < kFrame / 2; idx += fcb.pitchLag) {
                int excl = s.awRange;
                uint16_t* p = &mask[idx >> 4];
                const int firstSh = 16 - (idx & 15);
                *p++ &= uint16_t(0xFFFFu << firstSh);
                excl -= firstSh;
                if (excl >= 16) {
                    *p++ = 0;
                    *p &= uint16_t(0xFFFF >> (excl - 16));
                } else {
                    *p &= uint16_t(0xFFFF >> excl);
                }
            }
        const int aidx = int(g.read(s.awPulses[0] > 0 ? 5 - 2 * block : 4));
        for (int n = 0; n <= aidx; pulseStart++) {
            int idx;
            for (idx = pulseStart; idx < 0; idx += fcb.pitchLag) {}
            if (idx >= kFrame / 2) {
                if (mask[0]) idx = 0x0F;
                else if (mask[1]) idx = 0x1F;
                else if (mask[2]) idx = 0x2F;
                else if (mask[3]) idx = 0x3F;
                else if (mask[4]) idx = 0x4F;
                else return -1;
                idx -= log2Of(mask[idx >> 4]);
            }
            if (mask[idx >> 4] & (0x8000 >> (idx & 15))) {
                mask[idx >> 4] &= uint16_t(~(0x8000 >> (idx & 15)));
                n++;
                startOff = idx;
            }
        }
        fcb.x[fcb.n] = startOff;
        fcb.y[fcb.n] = g.bit() ? -1.0 : 1.0;
        fcb.n++;
        const int n = (kFrame / 2 - startOff) % fcb.pitchLag;
        s.awNext = n ? fcb.pitchLag - n : 0;
        return 0;
    }

    void awSet1(Bits& g, int block, Fixed& fcb) {
        State& s = *st;
        int val = int(g.read(12 - 2 * (s.awExt && !block)));
        if (s.awPulses[block] > 0) {
            int pulses, vMask, iMask, sh;
            if (s.awRange == 24) {
                pulses = 3;
                vMask = 8;
                iMask = 7;
                sh = 4;
            } else {
                pulses = 4;
                vMask = 4;
                iMask = 3;
                sh = 3;
            }
            for (int n = pulses - 1; n >= 0; n--, val >>= sh) {
                fcb.y[fcb.n] = (val & vMask) ? -1.0 : 1.0;
                fcb.x[fcb.n] = (val & iMask) * pulses + n + s.awFirst[block];
                while (fcb.x[fcb.n] < 0) fcb.x[fcb.n] += fcb.pitchLag;
                if (fcb.x[fcb.n] < kFrame / 2) fcb.n++;
            }
        } else {
            const int num2 = (val & 0x1FF) >> 1;
            int delta, idx;
            if (num2 < 1 * 79) {
                delta = 1;
                idx = num2 + 1;
            } else if (num2 < 2 * 78) {
                delta = 3;
                idx = num2 + 1 - 1 * 77;
            } else if (num2 < 3 * 77) {
                delta = 5;
                idx = num2 + 1 - 2 * 76;
            } else {
                delta = 7;
                idx = num2 + 1 - 3 * 75;
            }
            const float v = (val & 0x200) ? -1.0 : 1.0;
            fcb.noRepeat |= 3 << fcb.n;
            fcb.x[fcb.n] = idx - delta;
            fcb.y[fcb.n] = v;
            fcb.x[fcb.n + 1] = idx;
            fcb.y[fcb.n + 1] = (val & 1) ? -v : v;
            fcb.n += 2;
        }
    }

    static int random(int counter, int block, int size) {
        static const unsigned table[9][2] = {{8332, 3 * 715827883U},  {4545, 0 * 390451573U},  {3124, 11 * 268435456U},
                                             {2380, 15 * 204522253U}, {1922, 23 * 165191050U}, {1612, 23 * 138547333U},
                                             {1388, 27 * 119304648U}, {1219, 16 * 104755300U}, {1086, 39 * 93368855U}};
        unsigned x = unsigned(int(int16_t(block)) * 1877 + counter);
        if (x >= 0xFFFF) x -= 0xFFFF;
        const unsigned y = x - 9 * unsigned((int64_t(477218589) * int64_t(x)) >> 32);
        const unsigned z = uint16_t(x * table[y][0] + unsigned((uint64_t(x) * uint64_t(table[y][1])) >> 32));
        return int(z % unsigned(1000 - size));
    }

    void hardcoded(Bits& g, int block, int size, const FrameType& type, float* excitation) {
        State& s = *st;
        int r;
        float gain;
        if (type.fcb == FcbSilence) {
            r = random(s.counter, block, size);
            gain = s.silenceGain;
        } else {
            r = int(g.read(8));
            gain = kGainUniversal[g.read(6)];
        }
        memset(s.gainErr, 0, sizeof s.gainErr);
        if (quiet) return;
        for (int n = 0; n < size; n++) excitation[n] = kStdCodebook[r + n] * gain;
    }

    void fcbAcb(Bits& g, int block, int size, int pitchSh2, const FrameType& type, float* excitation) {
        static const float coeff[6] = {0.8169, -0.06545, 0.1726, 0.0185, -0.0359, 0.0458};
        State& s = *st;
        float pulses[kFrame / 2];
        Fixed fcb;
        fcb.pitchLag = pitchSh2 >> 2;
        fcb.pitchFactor = 1.0;
        fcb.noRepeat = 0;
        fcb.n = 0;
        if (type.fcb == FcbAw) {
            awSet1(g, block, fcb);
            if (awSet2(g, block, fcb)) {
                const int r = random(s.counter, block, size);
                if (!quiet)
                    for (int n = 0; n < size; n++) excitation[n] = kStdCodebook[r + n] * s.silenceGain;
                g.skip(7 + 1);
                return;
            }
        } else {
            const int bits = 5 - type.logBlocks;
            fcb.noRepeat = -1;
            for (int n = 0; n < 5; n++) {
                const float sign = g.bit() ? 1.0 : -1.0;
                const int pos1 = int(g.read(bits));
                fcb.x[fcb.n] = n + 5 * pos1;
                fcb.y[fcb.n++] = sign;
                if (n < type.doubles) {
                    const int pos2 = int(g.read(bits));
                    fcb.x[fcb.n] = n + 5 * pos2;
                    fcb.y[fcb.n++] = (pos1 < pos2) ? -sign : sign;
                }
            }
        }
        const int idx = int(g.read(7));
        if (quiet) return;
        memset(pulses, 0, sizeof(float) * size_t(size));
        setFixed(pulses, fcb, 1.0, size);
        const float fcbGain = expf(float(dot(s.gainErr, coeff, 6) - 5.2409161640 + kGainFcb[idx]));
        const float acbGain = kGainAcb[idx];
        const float predErr = fmin2(fmax2(kGainFcb[idx], float(-2.9957322736)), float(1.6094379124));
        const int weight = 8 >> type.logBlocks;
        memmove(&s.gainErr[weight], s.gainErr, sizeof(float) * size_t(6 - weight));
        for (int n = 0; n < weight; n++) s.gainErr[n] = predErr;
        if (type.acb == AcbAsymmetric) {
            int len;
            for (int n = 0; n < size; n += len) {
                const int abs = block * size + n;
                const int pitchSh16 = (s.lastPitch << 16) + s.pitchDiff * abs;
                const int pitch = (pitchSh16 + 0x6FFF) >> 16;
                const int idxSh16 = ((pitch << 16) - pitchSh16) * 8 + 0x58000;
                const int frac = idxSh16 >> 16;
                if (s.pitchDiff) {
                    const int next = s.pitchDiff > 0 ? idxSh16 & ~0xFFFF : (idxSh16 + 0x10000) & ~0xFFFF;
                    len = clip((idxSh16 - next) / s.pitchDiff / 8, 1, size - n);
                } else {
                    len = size;
                }
                interpolate(&excitation[n], &excitation[n - pitch], kIpol1, 17, frac, 9, len);
            }
        } else {
            const int blockPitch = pitchSh2 >> 2;
            const int frac = pitchSh2 & 3;
            if (frac) {
                interpolate(excitation, &excitation[-blockPitch], kIpol2, 4, frac, 8, size);
            } else {
                for (int n = 0; n < size; n++) excitation[n] = excitation[n - blockPitch];
            }
        }
        weightedSum(excitation, excitation, pulses, acbGain, fcbGain, size);
    }

    void block(Bits& g, int index, int size, int pitchSh2, const double* lspsNow, const double* prev, const FrameType& type, float* excitation, float* synth) {
        if (type.acb == AcbNone) hardcoded(g, index, size, type, excitation);
        else fcbAcb(g, index, size, pitchSh2, type, excitation);
        if (quiet) return;
        double i[kMaxLsps];
        float lpcs[kMaxLsps];
        const float fac = (index + 0.5) / type.blocks;
        for (int n = 0; n < lsps; n++) i[n] = cos(prev[n] + fac * (lspsNow[n] - prev[n]));
        lspToLpc(i, lpcs, lsps >> 1);
        synthesis(synth, lpcs, excitation, size, lsps);
    }

    int frame(Bits& g, int index, float* samples, const double* lspsNow, const double* prev, float* excitation, float* synth) {
        State& s = *st;
        int pitch[8];
        int cur = 0, lastBlockPitch = 0;
        const int typeIndex = tree[g.frameType()];
        pitch[0] = INT_MAX;
        if (typeIndex < 0) return -1;
        const FrameType& type = kTypes[typeIndex];
        const int blockSize = kFrame / type.blocks;
        if (type.acb == AcbAsymmetric) {
            const int blocks2 = type.blocks << 1, logBlocks2 = type.logBlocks + 1;
            cur = minPitch + int(g.read(pitchBits));
            cur = std::min(cur, maxPitch - 1);
            if (s.lastAcb == AcbNone || 20 * std::abs(cur - s.lastPitch) > (cur + s.lastPitch)) s.lastPitch = cur;
            for (int n = 0; n < type.blocks; n++) {
                const int fac = n * 2 + 1;
                pitch[n] = (int(int16_t(fac)) * int(int16_t(cur)) + int(int16_t(blocks2 - fac)) * int(int16_t(s.lastPitch)) + type.blocks) >> logBlocks2;
            }
            s.pitchDiff = (cur - s.lastPitch) * (1 << 16) / kFrame;
        }
        if (type.fcb == FcbSilence) s.silenceGain = kGainSilence[g.read(8)];
        else if (type.fcb == FcbAw) awCoords(g, pitch);
        for (int n = 0; n < type.blocks; n++) {
            int sh2;
            if (type.acb == AcbHamming) {
                const int t1 = (conv[1] - conv[0]) << 2, t2 = (conv[2] - conv[1]) << 1, t3 = conv[3] - conv[2] + 1;
                int bp;
                if (n == 0) bp = int(g.read(blockPitchBits));
                else bp = lastBlockPitch - deltaRange + int(g.read(deltaBits));
                lastBlockPitch = clip(bp, deltaRange, blockPitchRange - deltaRange);
                if (bp < t1) {
                    sh2 = (conv[0] << 2) + bp;
                } else {
                    bp -= t1;
                    if (bp < t2) {
                        sh2 = (conv[1] << 2) + (bp << 1);
                    } else {
                        bp -= t2;
                        if (bp < t3) sh2 = (conv[2] + bp) << 2;
                        else sh2 = conv[3] << 2;
                    }
                }
                pitch[n] = sh2 >> 2;
            } else if (type.acb == AcbAsymmetric) {
                sh2 = pitch[n] << 2;
            } else {
                sh2 = 0;
            }
            block(g, n, blockSize, sh2, lspsNow, prev, type, &excitation[n * blockSize], &synth[n * blockSize]);
        }
        if (apf && !quiet) {
            double i[kMaxLsps];
            float lpcs[kMaxLsps];
            if (type.fcb >= FcbAw && pitch[0] == INT_MAX) return -1;
            for (int n = 0; n < lsps; n++) i[n] = cos(0.5 * (prev[n] + lspsNow[n]));
            lspToLpc(i, lpcs, lsps >> 1);
            postfilter(synth, samples, 80, lpcs, &s.zeroExc[history + kFrame * index], type.fcb, pitch[0]);
            for (int n = 0; n < lsps; n++) i[n] = cos(lspsNow[n]);
            lspToLpc(i, lpcs, lsps >> 1);
            postfilter(&synth[80], &samples[80], 80, lpcs, &s.zeroExc[history + kFrame * index + 80], type.fcb, pitch[0]);
        } else if (!quiet) {
            memcpy(samples, synth, kFrame * sizeof(float));
        }
        s.counter++;
        if (s.counter >= 0xFFFF) s.counter -= 0xFFFF;
        s.lastAcb = type.acb;
        if (type.acb == AcbNone) s.lastPitch = 0;
        else if (type.acb == AcbAsymmetric) s.lastPitch = cur;
        else s.lastPitch = pitch[type.blocks - 1];
        return 0;
    }

    static void stabilize(double* l, int num) {
        l[0] = std::max(l[0], 0.0015 * M_PI);
        for (int n = 1; n < num; n++) l[n] = std::max(l[n], l[n - 1] + 0.0125 * M_PI);
        l[num - 1] = std::min(l[num - 1], 0.9985 * M_PI);
        for (int n = 1; n < num; n++) {
            if (l[n] < l[n - 1]) {
                for (int m = 1; m < num; m++) {
                    const double tmp = l[m];
                    int k;
                    for (k = m - 1; k >= 0; k--) {
                        if (l[k] <= tmp) break;
                        l[k + 1] = l[k];
                    }
                    l[k + 1] = tmp;
                }
                break;
            }
        }
    }

    int superframe() {
        State& s = *st;
        Bits local;
        Bits* g = &gb;
        int samplesHere = kSuper;
        double l[3][kMaxLsps];
        const double* mean = lsps == 16 ? kMeanLsf16[defMode] : kMeanLsf10[defMode];
        float excitation[kHistory + kSuper + 12];
        float synth[kMaxLsps + kSuper];
        float samples[kSuper];
        if (!quiet) {
            memcpy(synth, s.synth, sizeof(float) * size_t(lsps));
            memcpy(excitation, s.excitation, sizeof(float) * size_t(history));
        }
        if (s.cacheSize > 0) {
            local.init(s.cache, sizeof s.cache, s.cacheSize);
            g = &local;
            s.cacheSize = 0;
        }
        if (!g->bit()) {
            music = true;
            return -1;
        }
        if (g->bit()) {
            samplesHere = int(g->read(12));
            if (samplesHere > kSuper) return -1;
        }
        if (s.residual) {
            double prev[kMaxLsps], a1[kMaxLsps * 2], a2[kMaxLsps * 2];
            for (int n = 0; n < lsps; n++) prev[n] = s.prevLsps[n] - mean[n];
            if (lsps == 10) lsp10r(*g, l[2], prev, a1, a2);
            else lsp16r(*g, l[2], prev, a1, a2);
            for (int n = 0; n < lsps; n++) {
                l[0][n] = mean[n] + (a1[n] - a2[n * 2]);
                l[1][n] = mean[n] + (a1[lsps + n] - a2[n * 2 + 1]);
                l[2][n] += mean[n];
            }
            for (int n = 0; n < 3; n++) stabilize(l[n], lsps);
        }
        for (int n = 0; n < 3; n++) {
            if (!s.residual) {
                if (lsps == 10) lsp10i(*g, l[n]);
                else lsp16i(*g, l[n]);
                for (int m = 0; m < lsps; m++) l[n][m] += mean[m];
                stabilize(l[n], lsps);
            }
            if (frame(*g, n, &samples[n * kFrame], l[n], n == 0 ? s.prevLsps : l[n - 1], &excitation[history + n * kFrame], &synth[lsps + n * kFrame])) return -1;
        }
        if (g->bit()) {
            const int r = int(g->read(4));
            g->skip(10 * (r + 1));
        }
        if (g->left() < 0) {
            flush();
            return -1;
        }
        made += samplesHere;
        memcpy(s.prevLsps, l[2], sizeof(double) * size_t(lsps));
        if (quiet) return 0;
        sink->insert(sink->end(), samples, samples + samplesHere);
        memcpy(s.synth, &synth[kSuper], sizeof(float) * size_t(lsps));
        memcpy(s.excitation, &excitation[kSuper], sizeof(float) * size_t(history));
        if (apf) memmove(s.zeroExc, &s.zeroExc[kSuper], sizeof(float) * size_t(history));
        return 0;
    }

    int header() {
        State& s = *st;
        unsigned count = 0, r;
        gb.skip(4);
        s.residual = gb.bit();
        do {
            if (gb.left() < 6 + spilloverBits) return -1;
            r = gb.read(6);
            count += r;
        } while (r == 0x3F);
        s.spillover = int(gb.read(spilloverBits));
        return gb.left() >= 0 ? int(std::min(count, unsigned(INT_MAX))) : -1;
    }

    void put(uint32_t v, int n) {
        State& s = *st;
        while (n > 0) {
            const size_t byte = size_t(s.putCount >> 3);
            const int free = 8 - int(s.putCount & 7);
            if (free == 8) s.cache[byte] = 0;
            const int take = std::min(free, n);
            s.cache[byte] |= uint8_t(((v >> (n - take)) & ((1u << take) - 1)) << (free - take));
            s.putCount += take;
            n -= take;
        }
    }

    void copyBits(const uint8_t* data, int size, int64_t nbits) {
        State& s = *st;
        int64_t remaining = gb.left();
        if (remaining < nbits || nbits > kCacheBytes * 8 - s.putCount) return;
        int64_t tail = remaining & 7;
        const int64_t bytes = remaining >> 3;
        if ((tail = std::min(tail, nbits)) > 0) put(gb.read(int(tail)), int(tail));
        const uint8_t* from = data + size - bytes;
        int64_t length = std::min(nbits - tail, bytes << 3);
        for (; length >= 8; length -= 8) put(*from++, 8);
        if (length > 0) put(uint32_t(*from >> (8 - length)), int(length));
    }

    int64_t call(const uint8_t* data, size_t total) {
        State& s = *st;
        static const uint8_t dummy[1] = {};
        int size = int(total);
        while (size > blockAlign) size -= blockAlign;
        const uint8_t* buf = size ? data : dummy;
        gb.init(buf, size ? total : 0, int64_t(size) * 8);
        if (!(size % blockAlign)) {
            if (!size) {
                s.spillover = 0;
                s.superframes = 0;
            } else {
                const int r = header();
                if (r < 0) return -1;
                s.superframes = r;
            }
            if (s.cacheSize > 0) {
                int64_t cnt = gb.index;
                if (cnt + s.spillover > int64_t(total) * 8) s.spillover = int(int64_t(total) * 8 - cnt);
                copyBits(buf, size, s.spillover);
                s.cacheSize = int(s.putCount);
                s.putCount = (s.putCount + 7) & ~int64_t(7);
                if (superframe() == 0) {
                    cnt += s.spillover;
                    s.skipNext = int(cnt & 7);
                    return cnt >> 3;
                }
                gb.skipLong(s.spillover - cnt + gb.index);
            } else if (s.spillover) {
                gb.skipLong(s.spillover);
            }
        } else if (s.skipNext) {
            gb.skip(s.skipNext);
        }
        s.cacheSize = 0;
        s.skipNext = 0;
        const int64_t pos = gb.left();
        if (s.superframes-- == 0) return size;
        if (s.superframes > 0) {
            if (superframe() < 0) return -1;
            const int64_t cnt = gb.index;
            s.skipNext = int(cnt & 7);
            return cnt >> 3;
        }
        if (pos > 0) {
            s.putCount = 0;
            copyBits(buf, size, pos);
            s.cacheSize = int(s.putCount);
        }
        return size;
    }

    void run(const std::vector<uint8_t>& packet) {
        const uint8_t* data = packet.data();
        size_t size = packet.size();
        for (int turn = 0; size > 0 && turn < (1 << 16); ++turn) {
            const int64_t used = call(data, size);
            if (used <= 0 || size_t(used) >= size) return;
            data += used;
            size -= size_t(used);
        }
    }

    void feed(const std::vector<uint8_t>& packet, std::vector<float>& to) override {
        sink = &to;
        run(packet);
        sink = nullptr;
    }

    void finish(std::vector<float>& to) override {
        sink = &to;
        for (int turn = 0; turn < 4; ++turn) {
            const int64_t before = made;
            call(nullptr, 0);
            if (made == before) break;
        }
        sink = nullptr;
    }

    bool counts(Reader& file, const Track& track, std::vector<int64_t>& frames) override {
        const size_t n = track.samples.size();
        frames.assign(n, 0);
        WmaVoice look(*this);
        look.reset();
        look.quiet = true;
        std::vector<uint8_t> packet;
        std::vector<float> none;
        for (size_t i = 0; i < n; ++i) {
            packet.resize(track.samples[i].size);
            look.made = 0;
            if (!packet.empty() && file.read(track, uint32_t(i), packet.data())) look.feed(packet, none);
            if (i + 1 == n) look.finish(none);
            frames[i] = look.made;
        }
        if (look.music) refusal = "this Windows Media Audio 9 Voice stream has music frames (Windows Media Audio 9 Professional inside Voice), which are not decoded";
        return true;
    }
};

}

std::unique_ptr<PacketDecoder> openWmaVoiceAudio(const Track& track, std::string* error) {
    const std::vector<uint8_t>& w = track.config;
    if (track.configType != fourcc("wfex") || w.size() < 16) {
        if (error) *error = "Windows Media Audio track without its format header";
        return nullptr;
    }
    auto audio = std::make_unique<WmaVoice>();
    if (!audio->setup(w, error)) return nullptr;
    audio->longestPacket = kSuper * 64;
    audio->prerollPackets = 1;
    return audio;
}

}

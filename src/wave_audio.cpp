#include "audio_codec.h"
#include "read_common.h"
#include "readers.h"
#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <cstring>

namespace absent {

namespace {

const int8_t kImaIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
const int8_t kImaIndex2[4] = {-1, 2, -1, 2};
const int8_t kImaIndex3[8] = {-1, -1, 1, 2, -1, -1, 1, 2};
const int8_t kImaIndex5[32] = {-1, -1, -1, -1, -1, -1, -1, -1, 1, 2, 4, 6, 8, 10, 13, 16, -1, -1, -1, -1, -1, -1, -1, -1, 1, 2, 4, 6, 8, 10, 13, 16};
const int8_t* const kImaIndexTables[4] = {kImaIndex2, kImaIndex3, kImaIndex, kImaIndex5};
const uint8_t kImaBlockSizes[4] = {4, 12, 4, 20};
const uint8_t kImaBlockSamples[4] = {16, 32, 8, 32};
const int16_t kImaStep[89] = {7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130,
    143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878,
    2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
    18500, 20350, 22385, 24623, 27086, 29794, 32767};
const int16_t kMsAdaptation[16] = {230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230};
const int kMsCoeff1[7] = {64, 128, 0, 48, 60, 115, 98};
const int kMsCoeff2[7] = {0, -64, 0, 16, 0, -52, -58};
const int16_t kYamahaScale[16] = {230, 230, 230, 230, 307, 409, 512, 614, 230, 230, 230, 230, 307, 409, 512, 614};
const int8_t kYamahaDiff[16] = {1, 3, 5, 7, 9, 11, 13, 15, -1, -3, -5, -7, -9, -11, -13, -15};

std::array<int16_t, 256> lawTable(bool alaw) {
    std::array<int16_t, 256> table{};
    for (int i = 0; i < 256; ++i) {
        int t;
        if (alaw) {
            int a = i ^ 0x55;
            t = a & 15;
            int seg = (a & 0x70) >> 4;
            t = seg ? (t + t + 1 + 32) << (seg + 2) : (t + t + 1) << 3;
            table[size_t(i)] = int16_t((a & 0x80) ? t : -t);
        } else {
            int u = ~i & 0xFF;
            t = ((u & 15) << 3) + 0x84;
            t <<= (u & 0x70) >> 4;
            table[size_t(i)] = int16_t((u & 0x80) ? 0x84 - t : t - 0x84);
        }
    }
    return table;
}

int16_t clip16(int v) { return int16_t(std::clamp(v, -32768, 32767)); }

struct Ima {
    int predictor = 0;
    int index = 0;
};

int16_t imaExpand(Ima& c, int nibble, int shift) {
    const int step = kImaStep[c.index];
    const int index = std::clamp(c.index + kImaIndex[nibble & 15], 0, 88);
    const int diff = ((2 * (nibble & 7) + 1) * step) >> shift;
    c.predictor = clip16((nibble & 8) ? c.predictor - diff : c.predictor + diff);
    c.index = index;
    return int16_t(c.predictor);
}

int16_t imaQtExpand(Ima& c, int nibble) {
    const int step = kImaStep[c.index];
    const int index = std::clamp(c.index + kImaIndex[nibble], 0, 88);
    int diff = step >> 3;
    if (nibble & 4) diff += step;
    if (nibble & 2) diff += step >> 1;
    if (nibble & 1) diff += step >> 2;
    c.predictor = clip16((nibble & 8) ? c.predictor - diff : c.predictor + diff);
    c.index = index;
    return int16_t(c.predictor);
}

int16_t imaWavExpand(Ima& c, uint32_t nibble, int bps) {
    const int shift = bps - 1;
    const int step = kImaStep[c.index];
    const int index = std::clamp(c.index + kImaIndexTables[bps - 2][nibble], 0, 88);
    const uint32_t delta = nibble & ((1u << shift) - 1);
    int diff = step >> shift;
    for (int i = 0; i < shift; ++i) diff += (step >> (shift - 1 - i)) * int((delta >> i) & 1);
    c.predictor = clip16((nibble >> shift) & 1 ? c.predictor - diff : c.predictor + diff);
    c.index = index;
    return int16_t(c.predictor);
}

struct Ms {
    int coeff1 = 0;
    int coeff2 = 0;
    int delta = 0;
    int sample1 = 0;
    int sample2 = 0;
};

int16_t msExpand(Ms& c, int nibble) {
    int predictor = (c.sample1 * c.coeff1 + c.sample2 * c.coeff2) / 64;
    predictor += ((nibble & 8) ? nibble - 16 : nibble) * c.delta;
    c.sample2 = c.sample1;
    c.sample1 = clip16(predictor);
    c.delta = std::min((kMsAdaptation[nibble] * c.delta) >> 8, INT_MAX / 768);
    if (c.delta < 16) c.delta = 16;
    return int16_t(c.sample1);
}

struct Yamaha {
    int predictor = 0;
    int step = 0;
};

int16_t yamahaExpand(Yamaha& c, int nibble) {
    if (!c.step) {
        c.predictor = 0;
        c.step = 127;
    }
    c.predictor = clip16(c.predictor + (c.step * kYamahaDiff[nibble]) / 8);
    c.step = std::clamp((c.step * kYamahaScale[nibble]) >> 8, 127, 24576);
    return int16_t(c.predictor);
}

struct Float11 {
    uint8_t sign = 0;
    uint8_t exp = 0;
    uint8_t mant = 32;
};

int log2Short(int v) { return 31 - std::countl_zero(uint32_t(v & 0xFFFF) | 1); }

Float11 toFloat11(int i) {
    Float11 f;
    f.sign = i < 0;
    if (f.sign) i = -i;
    f.exp = uint8_t(log2Short(i) + (i != 0));
    f.mant = uint8_t(i ? (i << 6) >> f.exp : 32);
    return f;
}

int multiply(const Float11& a, const Float11& b) {
    const int exp = a.exp + b.exp;
    int res = ((a.mant * b.mant) + 0x30) >> 4;
    res = exp > 19 ? res << (exp - 19) : res >> (19 - exp);
    return (a.sign ^ b.sign) ? -res : res;
}

int sgn(int v) { return v < 0 ? -1 : 1; }

const int16_t kG726Iquant16[4] = {116, 365, 365, 116};
const int16_t kG726W16[4] = {-22, 439, 439, -22};
const uint8_t kG726F16[4] = {0, 7, 7, 0};
const int16_t kG726Iquant24[8] = {INT16_MIN, 135, 273, 373, 373, 273, 135, INT16_MIN};
const int16_t kG726W24[8] = {-4, 30, 137, 582, 582, 137, 30, -4};
const uint8_t kG726F24[8] = {0, 1, 2, 7, 7, 2, 1, 0};
const int16_t kG726Iquant32[16] = {INT16_MIN, 4, 135, 213, 273, 323, 373, 425, 425, 373, 323, 273, 213, 135, 4, INT16_MIN};
const int16_t kG726W32[16] = {-12, 18, 41, 64, 112, 198, 355, 1122, 1122, 355, 198, 112, 64, 41, 18, -12};
const uint8_t kG726F32[16] = {0, 0, 0, 1, 1, 1, 3, 7, 7, 3, 1, 1, 1, 0, 0, 0};
const int16_t kG726Iquant40[32] = {INT16_MIN, -66, 28, 104, 169, 224, 274, 318, 358, 395, 429, 459, 488, 514, 539, 566, 566, 539, 514, 488, 459, 429,
    395, 358, 318, 274, 224, 169, 104, 28, -66, INT16_MIN};
const int16_t kG726W40[32] = {14, 14, 24, 39, 40, 41, 58, 100, 141, 179, 219, 280, 358, 440, 529, 696, 696, 529, 440, 358, 280, 219, 179, 141, 100, 58,
    41, 40, 39, 24, 14, 14};
const uint8_t kG726F40[32] = {0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 2, 3, 4, 5, 6, 6, 6, 6, 5, 4, 3, 2, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
const int16_t* const kG726Iquant[4] = {kG726Iquant16, kG726Iquant24, kG726Iquant32, kG726Iquant40};
const int16_t* const kG726W[4] = {kG726W16, kG726W24, kG726W32, kG726W40};
const uint8_t* const kG726F[4] = {kG726F16, kG726F24, kG726F32, kG726F40};

struct G726 {
    Float11 sr[2];
    Float11 dq[6];
    int a[2] = {0, 0};
    int b[6] = {0, 0, 0, 0, 0, 0};
    int pk[2] = {1, 1};
    int ap = 0;
    int yu = 544;
    int yl = 34816;
    int dms = 0;
    int dml = 0;
    int td = 0;
    int se = 0;
    int sez = 0;
    int y = 544;

    int16_t decode(int code, int size) {
        const int16_t* iquant = kG726Iquant[size - 2];
        const int sign = code >> (size - 1);
        const int dql = iquant[code] + (y >> 2);
        int diff = dql < 0 ? 0 : int16_t((((1 << 7) + (dql & 0x7F)) << ((dql >> 7) & 15)) >> 7);
        const int ylint = yl >> 15;
        const int ylfrac = (yl >> 10) & 0x1F;
        const int thr2 = ylint > 9 ? 0x1F << 10 : (0x20 + ylfrac) << ylint;
        const bool tr = td == 1 && diff > ((3 * thr2) >> 2);
        if (sign) diff = -diff;
        const int signal = int16_t(se + diff);
        const int pk0 = (sez + diff) ? sgn(sez + diff) : 0;
        const int dq0 = diff ? sgn(diff) : 0;
        if (tr) {
            a[0] = a[1] = 0;
            for (int& v : b) v = 0;
        } else {
            const int fa1 = std::clamp((-a[0] * pk[0] * pk0) >> 5, -256, 255);
            a[1] += 128 * pk0 * pk[1] + fa1 - (a[1] >> 7);
            a[1] = std::clamp(a[1], -12288, 12288);
            a[0] += 64 * 3 * pk0 * pk[0] - (a[0] >> 8);
            a[0] = std::clamp(a[0], -(15360 - a[1]), 15360 - a[1]);
            for (int i = 0; i < 6; ++i) b[i] += 128 * dq0 * sgn(-dq[i].sign) - (b[i] >> 8);
        }
        pk[1] = pk[0];
        pk[0] = pk0 ? pk0 : 1;
        sr[1] = sr[0];
        sr[0] = toFloat11(signal);
        for (int i = 5; i > 0; --i) dq[i] = dq[i - 1];
        dq[0] = toFloat11(diff);
        dq[0].sign = uint8_t(sign);
        td = a[1] < -11776;
        const uint8_t f = kG726F[size - 2][code];
        dms += (f << 4) + ((-dms) >> 5);
        dml += (f << 4) + ((-dml) >> 7);
        if (tr) {
            ap = 256;
        } else {
            ap += (-ap) >> 4;
            if (y <= 1535 || td || std::abs((dms << 2) - dml) >= (dml >> 3)) ap += 0x20;
        }
        yu = std::clamp(y + kG726W[size - 2][code] + ((-y) >> 5), 544, 5120);
        yl += yu + ((-yl) >> 6);
        const int al = ap >= 256 ? 1 << 6 : ap >> 2;
        y = (yl + (yu - (yl >> 6)) * al) >> 6;
        se = 0;
        for (int i = 0; i < 6; ++i) se += multiply(toFloat11(b[i] >> 2), dq[i]);
        sez = se >> 1;
        for (int i = 0; i < 2; ++i) se += multiply(toFloat11(a[i] >> 2), sr[i]);
        se >>= 1;
        return int16_t(std::clamp(signal * 4, -0xFFFF, 0xFFFF));
    }
};

const int16_t kG722InvLog2[32] = {2048, 2093, 2139, 2186, 2233, 2282, 2332, 2383, 2435, 2489, 2543, 2599, 2656, 2714, 2774, 2834, 2896, 2960, 3025, 3091,
    3158, 3228, 3298, 3371, 3444, 3520, 3597, 3676, 3756, 3838, 3922, 4008};
const int16_t kG722HighStep[2] = {798, -214};
const int16_t kG722HighInvQuant[4] = {-926, -202, 926, 202};
const int16_t kG722LowStep[16] = {-60, 3042, 1198, 538, 334, 172, 58, -30, 3042, 1198, 538, 334, 172, 58, -30, -60};
const int16_t kG722LowInvQuant4[16] = {0, -2557, -1612, -1121, -786, -530, -323, -150, 2557, 1612, 1121, 786, 530, 323, 150, 0};
const int16_t kG722LowInvQuant5[32] = {-35, -35, -2919, -2195, -1765, -1458, -1219, -1023, -858, -714, -587, -473, -370, -276, -190, -110, 2919, 2195,
    1765, 1458, 1219, 1023, 858, 714, 587, 473, 370, 276, 190, 110, 35, -35};
const int16_t kG722LowInvQuant6[64] = {-17, -17, -17, -17, -3101, -2738, -2376, -2088, -1873, -1689, -1535, -1399, -1279, -1170, -1072, -982, -899,
    -822, -750, -682, -618, -558, -501, -447, -396, -347, -300, -254, -211, -170, -130, -91, 3101, 2738, 2376, 2088, 1873, 1689, 1535, 1399, 1279, 1170,
    1072, 982, 899, 822, 750, 682, 618, 558, 501, 447, 396, 347, 300, 254, 211, 170, 130, 91, 54, 17, -54, -17};
const int16_t* const kG722LowInvQuants[3] = {kG722LowInvQuant6, kG722LowInvQuant5, kG722LowInvQuant4};
const int kG722Qmf[24] = {3, -11, -11, 53, 12, -156, 32, 362, -210, -805, 951, 3876, 3876, 951, -805, -210, 362, 32, -156, 12, 53, -11, -11, 3};

struct G722Band {
    int16_t predictor = 0;
    int32_t zero = 0;
    int8_t reconstructed[2] = {0, 0};
    int16_t previous = 0;
    int16_t pole[2] = {0, 0};
    int32_t differences[6] = {0, 0, 0, 0, 0, 0};
    int16_t zeros[6] = {0, 0, 0, 0, 0, 0};
    int16_t logFactor = 0;
    int16_t scaleFactor = 0;

    void predict(int diff) {
        const int part = zero + diff < 0;
        const int sg0 = part != reconstructed[0] ? 1 : -1;
        const int sg1 = part == reconstructed[1] ? 1 : -1;
        reconstructed[1] = reconstructed[0];
        reconstructed[0] = int8_t(part);
        pole[1] = int16_t(std::clamp((sg0 * std::clamp(int(pole[0]), -8191, 8191) >> 5) + sg1 * 128 + (pole[1] * 127 >> 7), -12288, 12288));
        const int limit = 15360 - pole[1];
        pole[0] = int16_t(std::clamp(-192 * sg0 + (pole[0] * 255 >> 8), -limit, limit));
        int sum = 0;
        const int d = diff ? 1 : 0;
        for (int k = 5; k >= 0; --k) {
            const int value = k ? differences[k - 1] : diff * 2;
            zeros[k] = int16_t(((zeros[k] * 255) >> 8) + d * ((differences[k] ^ diff) < 0 ? -128 : 128));
            differences[k] = value;
            sum += (value * zeros[k]) >> 15;
        }
        zero = sum;
        const int reconstructedNow = clip16((predictor + diff) * 2);
        predictor = clip16(zero + (pole[0] * reconstructedNow >> 15) + (pole[1] * previous >> 15));
        previous = int16_t(reconstructedNow);
    }

    static int16_t linear(int factor) {
        const int wd1 = kG722InvLog2[(factor >> 6) & 31];
        const int shift = factor >> 11;
        return int16_t(shift < 0 ? wd1 >> -shift : wd1 << shift);
    }

    void low(int ilow) {
        predict(scaleFactor * kG722LowInvQuant4[ilow] >> 10);
        logFactor = int16_t(std::clamp((logFactor * 127 >> 7) + kG722LowStep[ilow], 0, 18432));
        scaleFactor = linear(logFactor - (8 << 11));
    }

    void high(int dhigh, int ihigh) {
        predict(dhigh);
        logFactor = int16_t(std::clamp((logFactor * 127 >> 7) + kG722HighStep[ihigh & 1], 0, 22528));
        scaleFactor = linear(logFactor - (10 << 11));
    }
};

struct G722 {
    G722Band band[2];
    int16_t history[24] = {};

    G722() {
        band[0].scaleFactor = 8;
        band[1].scaleFactor = 2;
    }

    void decode(uint8_t code, int skip, int16_t out[2]) {
        const int ihigh = code >> 6;
        const int ilow = (code >> skip) & ((1 << (6 - skip)) - 1);
        const int rlow = std::clamp((band[0].scaleFactor * kG722LowInvQuants[skip][ilow] >> 10) + band[0].predictor, -16384, 16383);
        band[0].low(ilow >> (2 - skip));
        const int dhigh = band[1].scaleFactor * kG722HighInvQuant[ihigh] >> 10;
        const int rhigh = std::clamp(dhigh + band[1].predictor, -16384, 16383);
        band[1].high(dhigh, ihigh);
        memmove(history, history + 2, 22 * sizeof(int16_t));
        history[22] = int16_t(rlow + rhigh);
        history[23] = int16_t(rlow - rhigh);
        int x0 = 0, x1 = 0;
        for (int i = 0; i < 24; i += 2) {
            x1 += history[i] * kG722Qmf[i];
            x0 += history[i + 1] * kG722Qmf[i + 1];
        }
        out[0] = clip16(x0 >> 11);
        out[1] = clip16(x1 >> 11);
    }
};

enum class Method { Alaw, Ulaw, ImaQt, ImaWav, Ms, Yamaha, Dk3, Dk4, G726, G722 };

struct State {
    Ima ima[2];
    Yamaha yamaha[2];
    G726 g726;
    G722 g722;
};

struct WaveDecoder : PacketDecoder {
    Method method = Method::Alaw;
    int align = 0;
    int bits = 4;
    std::array<int16_t, 256> law{};
    State state;
    std::vector<int16_t> pcm;

    void reset() override { state = State(); }

    size_t unit(size_t left) const {
        if (method == Method::ImaQt) return size_t(34 * channels);
        if (method == Method::ImaWav || method == Method::Ms || method == Method::Dk3 || method == Method::Dk4) return align > 0 ? std::min(left, size_t(align)) : left;
        return left;
    }

    int64_t frames(size_t n) const {
        const int64_t ch = channels;
        const int64_t size = int64_t(n);
        switch (method) {
            case Method::Alaw:
            case Method::Ulaw: return size / ch;
            case Method::ImaQt: return size >= 34 * ch ? 64 : 0;
            case Method::ImaWav: return size < 4 * ch ? 0 : 1 + (size - 4 * ch) / (kImaBlockSizes[bits - 2] * ch) * kImaBlockSamples[bits - 2];
            case Method::Ms: return size < 7 * ch ? 0 : (size - 6 * ch) * 2 / ch;
            case Method::Yamaha: return size * 2 / ch;
            case Method::Dk3: return size < 16 ? 0 : ((size - 16) * 2 / 3 * 4) / ch;
            case Method::Dk4: return size < 4 * ch ? 0 : 1 + (size - 4 * ch) * 2 / ch;
            case Method::G726: return size * 8 / bits;
            case Method::G722: return size * 2;
        }
        return 0;
    }

    bool decodeUnit(const uint8_t* p, size_t n, int64_t count, int16_t* out) {
        const int ch = channels;
        size_t at = 0;
        auto byte = [&]() { return at < n ? p[at++] : (at++, uint8_t(0)); };
        auto word = [&]() {
            int low = byte();
            return int(int16_t(uint16_t(low | (byte() << 8))));
        };
        switch (method) {
            case Method::Alaw:
            case Method::Ulaw:
                for (int64_t i = 0; i < count * ch; ++i) out[i] = law[p[i]];
                return true;
            case Method::ImaQt:
                for (int c = 0; c < ch; ++c) {
                    Ima& s = state.ima[c];
                    int high = byte();
                    int predictor = int(int16_t(uint16_t((high << 8) | byte())));
                    const int index = predictor & 0x7F;
                    predictor &= ~0x7F;
                    if (s.index != index || std::abs(predictor - s.predictor) > 0x7F) {
                        s.index = index;
                        s.predictor = predictor;
                    }
                    if (s.index > 88) return false;
                    for (int m = 0; m < 64; m += 2) {
                        const int v = byte();
                        out[m * ch + c] = imaQtExpand(s, v & 15);
                        out[(m + 1) * ch + c] = imaQtExpand(s, v >> 4);
                    }
                }
                return true;
            case Method::ImaWav: {
                for (int c = 0; c < ch; ++c) {
                    Ima& s = state.ima[c];
                    s.predictor = word();
                    out[c] = int16_t(s.predictor);
                    s.index = byte();
                    byte();
                    if (s.index > 88) return false;
                }
                if (bits != 4) {
                    const int perBlock = kImaBlockSamples[bits - 2];
                    const int blockSize = kImaBlockSizes[bits - 2];
                    uint8_t temp[20];
                    for (int64_t b = 0; b < (count - 1) / perBlock; ++b) {
                        for (int c = 0; c < ch; ++c) {
                            for (int j = 0; j < blockSize; ++j) {
                                const size_t from = size_t(4 * ch + blockSize * b * ch + (j % 4) + (j / 4) * (ch * 4) + c * 4);
                                temp[j] = from < n ? p[from] : 0;
                            }
                            uint64_t acc = 0;
                            int have = 0, taken = 0;
                            for (int m = 0; m < perBlock; ++m) {
                                while (have < bits) {
                                    acc |= uint64_t(taken < blockSize ? temp[taken] : 0) << have;
                                    ++taken;
                                    have += 8;
                                }
                                const uint32_t nibble = uint32_t(acc & ((1u << bits) - 1));
                                acc >>= bits;
                                have -= bits;
                                out[(1 + b * perBlock + m) * ch + c] = imaWavExpand(state.ima[c], nibble, bits);
                            }
                        }
                    }
                    return true;
                }
                for (int64_t b = 0; b < (count - 1) / 8; ++b) {
                    for (int c = 0; c < ch; ++c) {
                        for (int m = 0; m < 8; m += 2) {
                            const int v = byte();
                            out[(1 + b * 8 + m) * ch + c] = imaQtExpand(state.ima[c], v & 15);
                            out[(1 + b * 8 + m + 1) * ch + c] = imaQtExpand(state.ima[c], v >> 4);
                        }
                    }
                }
                return true;
            }
            case Method::Ms: {
                Ms s[6];
                if (ch > 2) {
                    for (int c = 0; c < ch; ++c) {
                        const int predictor = byte();
                        if (predictor > 6) return false;
                        s[c].coeff1 = kMsCoeff1[predictor];
                        s[c].coeff2 = kMsCoeff2[predictor];
                        s[c].delta = word();
                        s[c].sample1 = word();
                        s[c].sample2 = word();
                        int64_t k = 0;
                        out[k++ * ch + c] = int16_t(s[c].sample2);
                        out[k++ * ch + c] = int16_t(s[c].sample1);
                        for (int64_t i = (count - 2) >> 1; i > 0; --i) {
                            const int v = byte();
                            out[k++ * ch + c] = msExpand(s[c], v >> 4);
                            out[k++ * ch + c] = msExpand(s[c], v & 15);
                        }
                    }
                    return true;
                }
                const int st = ch == 2;
                for (int c = 0; c <= st; ++c) {
                    const int predictor = byte();
                    if (predictor > 6) return false;
                    s[c].coeff1 = kMsCoeff1[predictor];
                    s[c].coeff2 = kMsCoeff2[predictor];
                }
                for (int c = 0; c <= st; ++c) s[c].delta = word();
                for (int c = 0; c <= st; ++c) s[c].sample1 = word();
                for (int c = 0; c <= st; ++c) s[c].sample2 = word();
                int16_t* o = out;
                for (int c = 0; c <= st; ++c) *o++ = int16_t(s[c].sample2);
                for (int c = 0; c <= st; ++c) *o++ = int16_t(s[c].sample1);
                for (int64_t i = (count - 2) >> (1 - st); i > 0; --i) {
                    const int v = byte();
                    *o++ = msExpand(s[0], v >> 4);
                    *o++ = msExpand(s[st], v & 15);
                }
                return true;
            }
            case Method::Yamaha: {
                const int st = ch == 2;
                int16_t* o = out;
                for (int64_t i = count >> (1 - st); i > 0; --i) {
                    const int v = byte();
                    *o++ = yamahaExpand(state.yamaha[0], v & 15);
                    *o++ = yamahaExpand(state.yamaha[st], v >> 4);
                }
                return true;
            }
            case Method::Dk4: {
                Ima s[2];
                for (int c = 0; c < ch; ++c) {
                    s[c].predictor = word();
                    out[c] = int16_t(s[c].predictor);
                    s[c].index = word();
                    if (s[c].index < 0 || s[c].index > 88) return false;
                }
                const int st = ch == 2;
                int16_t* o = out + ch;
                for (int64_t i = (count - 1) >> (1 - st); i > 0; --i) {
                    const int v = byte();
                    *o++ = imaExpand(s[0], v >> 4, 3);
                    *o++ = imaExpand(s[st], v & 15, 3);
                }
                return true;
            }
            case Method::Dk3: {
                Ima s[2];
                at = 10;
                s[0].predictor = word();
                s[1].predictor = word();
                s[0].index = byte();
                s[1].index = byte();
                if (s[0].index > 88 || s[1].index > 88) return false;
                int last = 0;
                bool top = false;
                auto nibble = [&]() {
                    if (top) {
                        top = false;
                        return last >> 4;
                    }
                    last = byte();
                    top = true;
                    return last & 15;
                };
                int16_t* o = out;
                int16_t* end = out + count * 2;
                while (o < end) {
                    imaExpand(s[0], nibble(), 3);
                    imaExpand(s[1], nibble(), 3);
                    *o++ = int16_t(s[0].predictor + s[1].predictor);
                    *o++ = int16_t(s[0].predictor - s[1].predictor);
                    imaExpand(s[0], nibble(), 3);
                    if (o == end) break;
                    *o++ = int16_t(s[0].predictor + s[1].predictor);
                    *o++ = int16_t(s[0].predictor - s[1].predictor);
                }
                return true;
            }
            case Method::G726: {
                uint64_t acc = 0;
                int have = 0;
                for (int64_t i = 0; i < count; ++i) {
                    while (have < bits) {
                        acc = (acc << 8) | byte();
                        have += 8;
                    }
                    const int code = int((acc >> (have - bits)) & ((1u << bits) - 1));
                    have -= bits;
                    out[i] = state.g726.decode(code, bits);
                }
                return true;
            }
            case Method::G722:
                for (int64_t i = 0; i < count / 2; ++i) state.g722.decode(p[i], 8 - bits, out + 2 * i);
                return true;
        }
        return false;
    }

    size_t used(size_t n, int64_t count, size_t left) const {
        const size_t ch = size_t(channels);
        const size_t k = size_t(count);
        size_t bytes = n;
        switch (method) {
            case Method::Alaw:
            case Method::Ulaw: bytes = k * ch; break;
            case Method::ImaQt: bytes = 34 * ch; break;
            case Method::ImaWav: bytes = bits == 4 ? 4 * ch + (k - 1) / 8 * 4 * ch : size_t(align) >= 4 * ch ? size_t(align) : left; break;
            case Method::Ms: bytes = 7 * ch + (ch > 2 ? ch * ((k - 2) >> 1) : ch == 2 ? k - 2 : (k - 2) >> 1); break;
            case Method::Dk4: bytes = 4 * ch + ((k - 1) >> (ch == 2 ? 0 : 1)); break;
            case Method::Dk3: bytes = 16 + (3 * ((k + 1) / 2) + 1) / 2; bytes += bytes & 1; break;
            default: break;
        }
        return std::clamp<size_t>(bytes, 1, left);
    }

    void feed(const std::vector<uint8_t>& packet, std::vector<float>& out) override {
        size_t at = 0;
        while (at < packet.size()) {
            const size_t left = packet.size() - at;
            const size_t n = std::min(unit(left), left);
            const int64_t count = frames(n);
            if (count <= 0) break;
            pcm.assign(size_t((count + 1) * channels), 0);
            if (!decodeUnit(packet.data() + at, left, count, pcm.data())) break;
            const size_t base = out.size();
            out.resize(base + size_t(count * channels));
            for (size_t i = 0; i < size_t(count * channels); ++i) out[base + i] = float(pcm[i]) * (1.0f / 32768.0f);
            at += used(n, count, left);
        }
    }

    int64_t packetFramesOf(size_t size) const {
        int64_t total = 0;
        size_t at = 0;
        while (at < size) {
            const size_t left = size - at;
            const size_t n = std::min(unit(left), left);
            const int64_t count = frames(n);
            if (count <= 0) break;
            total += count;
            at += used(n, count, left);
        }
        return total;
    }

    bool counts(Reader&, const Track& track, std::vector<int64_t>& out) override {
        out.resize(track.samples.size());
        for (size_t i = 0; i < out.size(); ++i) out[i] = packetFramesOf(track.samples[i].size);
        return true;
    }

    bool save(std::vector<uint8_t>& out) const override {
        if (stateless) return false;
        out.resize(sizeof(State));
        memcpy(out.data(), &state, sizeof(State));
        return true;
    }

    void load(const std::vector<uint8_t>& in) override {
        if (in.size() == sizeof(State)) memcpy(&state, in.data(), sizeof(State));
    }
};

}

bool waveDecoded(uint32_t codec) {
    if (codec == fourcc("alaw") || codec == fourcc("ulaw") || codec == fourcc("ima4")) return true;
    if ((codec >> 16) != 0x6D73) return false;
    switch (codec & 0xFFFF) {
        case 0x0002:
        case 0x0011:
        case 0x0020:
        case 0x0061:
        case 0x0062:
        case 0x0014:
        case 0x0040:
        case 0x0045:
        case 0x0064:
        case 0x028F: return true;
        default: return false;
    }
}

std::unique_ptr<PacketDecoder> openWaveAudio(const Track& track, std::string* error);

int64_t waveFrames(const Track& track, uint64_t bytes) {
    auto decoder = openWaveAudio(track, nullptr);
    return decoder ? static_cast<WaveDecoder&>(*decoder).packetFramesOf(size_t(std::min<uint64_t>(bytes, 1u << 30))) : 0;
}

std::unique_ptr<PacketDecoder> openWaveAudio(const Track& track, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    auto d = std::make_unique<WaveDecoder>();
    d->rate = track.sampleRate;
    d->channels = track.channels;
    const std::vector<uint8_t>& c = track.config;
    const bool described = track.configType == fourcc("wfex") && c.size() >= 16;
    d->align = described ? int(le16(c.data() + 12)) : 0;
    const int coded = described ? int(le16(c.data() + 14)) : 0;
    const uint32_t tag = (track.codec >> 16) == 0x6D73 ? track.codec & 0xFFFF : 0;
    if (track.codec == fourcc("alaw") || track.codec == fourcc("ulaw")) {
        d->method = track.codec == fourcc("alaw") ? Method::Alaw : Method::Ulaw;
        d->law = lawTable(d->method == Method::Alaw);
    } else if (track.codec == fourcc("ima4")) {
        d->method = Method::ImaQt;
    } else if (tag == 0x0011) {
        d->method = Method::ImaWav;
        d->bits = coded;
        if (d->bits < 2 || d->bits > 5) return fail("IMA ADPCM with this number of bits per sample is not supported");
    } else if (tag == 0x0002) {
        d->method = Method::Ms;
    } else if (tag == 0x0020) {
        d->method = Method::Yamaha;
    } else if (tag == 0x0061) {
        d->method = Method::Dk4;
    } else if (tag == 0x0062) {
        d->method = Method::Dk3;
    } else if (tag == 0x0014 || tag == 0x0040 || tag == 0x0045 || tag == 0x0064) {
        d->method = Method::G726;
        d->bits = coded;
        if (d->bits < 2 || d->bits > 5) return fail("G.726 with this number of bits per sample is not supported");
    } else if (tag == 0x028F) {
        d->method = Method::G722;
        d->bits = 8;
    } else {
        return fail("sound codec not supported");
    }
    const int most = d->method == Method::Ms ? 6 : d->method == Method::Alaw || d->method == Method::Ulaw ? 64 : 2;
    const bool mono = d->method == Method::G726 || d->method == Method::G722;
    if (d->channels < 1 || d->channels > (mono ? 1 : most) || (d->method == Method::Dk3 && d->channels != 2)) return fail("this number of channels is not supported for this sound codec");
    d->stateless = d->method == Method::Alaw || d->method == Method::Ulaw || d->method == Method::ImaWav || d->method == Method::Ms ||
                   d->method == Method::Dk3 || d->method == Method::Dk4;
    d->prerollPackets = d->stateless ? 0 : 1;
    return d;
}

}

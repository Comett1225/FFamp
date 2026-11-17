#include "audio_codec.h"
#include "read_common.h"
#include <algorithm>
#include <bit>
#include <climits>
#include <cstring>

namespace absent {

namespace {

const int kMaxTerms = 16;
const int kMaxTerm = 8;
const int kMaxSamples = 150000;
const uint32_t kMono = 0x4, kHybrid = 0x8, kJoint = 0x10, kFloat = 0x80, kHybridBitrate = 0x200, kInitial = 0x800, kFinal = 0x1000,
               kFalseStereo = 0x40000000, kDsd = 0x80000000;
const int kShiftOnes = 0x01, kShiftSame = 0x02, kShiftSent = 0x04, kZeroSent = 0x08, kZeroSign = 0x10;
const int kRates[16] = {6000, 8000, 9600, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 64000, 88200, 96000, 192000, 0};
const uint64_t kDefaultMasks[9] = {0, 0x4, 0x3, 0xB, 0x107, 0x37, 0x3F, 0x70F, 0x63F};

const uint8_t kExp2[256] = {0x00, 0x01, 0x01, 0x02, 0x03, 0x03, 0x04, 0x05, 0x06, 0x06, 0x07, 0x08, 0x08, 0x09, 0x0a, 0x0b, 0x0b, 0x0c, 0x0d, 0x0e, 0x0e, 0x0f,
    0x10, 0x10, 0x11, 0x12, 0x13, 0x13, 0x14, 0x15, 0x16, 0x16, 0x17, 0x18, 0x19, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1d, 0x1e, 0x1f, 0x20, 0x20, 0x21, 0x22, 0x23,
    0x24, 0x24, 0x25, 0x26, 0x27, 0x28, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x3a, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f,
    0x50, 0x51, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5e, 0x5f, 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67,
    0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f, 0x80, 0x81,
    0x82, 0x83, 0x84, 0x85, 0x87, 0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x91, 0x92, 0x93, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d,
    0x9f, 0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xaf, 0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xbc,
    0xbd, 0xbe, 0xbf, 0xc0, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc8, 0xc9, 0xca, 0xcb, 0xcd, 0xce, 0xcf, 0xd0, 0xd2, 0xd3, 0xd4, 0xd6, 0xd7, 0xd8, 0xd9, 0xdb, 0xdc,
    0xdd, 0xde, 0xe0, 0xe1, 0xe2, 0xe4, 0xe5, 0xe6, 0xe8, 0xe9, 0xea, 0xec, 0xed, 0xee, 0xf0, 0xf1, 0xf2, 0xf4, 0xf5, 0xf6, 0xf8, 0xf9, 0xfa, 0xfc, 0xfd, 0xff};
const uint8_t kLog2[256] = {0x00, 0x01, 0x03, 0x04, 0x06, 0x07, 0x09, 0x0a, 0x0b, 0x0d, 0x0e, 0x10, 0x11, 0x12, 0x14, 0x15, 0x16, 0x18, 0x19, 0x1a, 0x1c, 0x1d,
    0x1e, 0x20, 0x21, 0x22, 0x24, 0x25, 0x26, 0x28, 0x29, 0x2a, 0x2c, 0x2d, 0x2e, 0x2f, 0x31, 0x32, 0x33, 0x34, 0x36, 0x37, 0x38, 0x39, 0x3b, 0x3c, 0x3d, 0x3e,
    0x3f, 0x41, 0x42, 0x43, 0x44, 0x45, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4d, 0x4e, 0x4f, 0x50, 0x51, 0x52, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x5c, 0x5d,
    0x5e, 0x5f, 0x60, 0x61, 0x62, 0x63, 0x64, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f, 0x70, 0x71, 0x72, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
    0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f, 0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x91, 0x92, 0x93,
    0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f, 0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xa9, 0xaa, 0xab,
    0xac, 0xad, 0xae, 0xaf, 0xb0, 0xb1, 0xb2, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf, 0xc0, 0xc0, 0xc1, 0xc2,
    0xc3, 0xc4, 0xc5, 0xc6, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xcb, 0xcb, 0xcc, 0xcd, 0xce, 0xcf, 0xd0, 0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8,
    0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdc, 0xdd, 0xde, 0xdf, 0xe0, 0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe4, 0xe5, 0xe6, 0xe7, 0xe7, 0xe8, 0xe9, 0xea, 0xea, 0xeb, 0xec,
    0xed, 0xee, 0xee, 0xef, 0xf0, 0xf1, 0xf1, 0xf2, 0xf3, 0xf4, 0xf4, 0xf5, 0xf6, 0xf7, 0xf7, 0xf8, 0xf9, 0xf9, 0xfa, 0xfb, 0xfc, 0xfc, 0xfd, 0xfe, 0xff, 0xff};

int exp2Of(int16_t value) {
    int v = value;
    bool negative = v < 0;
    if (negative) v = -v;
    int result = kExp2[v & 0xFF] | 0x100;
    v >>= 8;
    if (unsigned(v) > 31) return INT_MIN;
    result = v > 9 ? int(unsigned(result) << (v - 9)) : result >> (9 - v);
    return negative ? -result : result;
}

int log2Of(uint32_t v) {
    if (!v) return 0;
    if (v == 1) return 256;
    v += v >> 9;
    const int bits = 32 - std::countl_zero(v);
    return bits < 9 ? (bits << 8) + kLog2[(v << (9 - bits)) & 0xFF] : (bits << 8) + kLog2[(v >> (bits - 9)) & 0xFF];
}

struct LeBits {
    const uint8_t* p = nullptr;
    int64_t end = 0;
    int64_t at = 0;

    void init(const uint8_t* data, size_t bytes) {
        p = data;
        end = int64_t(bytes) * 8;
        at = 0;
    }
    int64_t left() const { return end - at; }
    uint32_t bit() {
        const uint32_t value = at < end ? (p[at >> 3] >> (at & 7)) & 1 : 0;
        ++at;
        return value;
    }
    uint32_t read(int n) {
        uint32_t value = 0;
        for (int i = 0; i < n; ++i) value |= bit() << i;
        return value;
    }
    int unary() {
        int n = 0;
        while (n < 33 && bit()) ++n;
        return n;
    }
};

struct Decorr {
    int delta, value, weightA, weightB;
    int samplesA[kMaxTerm], samplesB[kMaxTerm];
};

struct Channel {
    int median[3];
    int slowLevel, errorLimit;
    unsigned bitrateAcc, bitrateDelta;
};

int median(const Channel* c, int n) { return (c->median[n] >> 4) + 1; }
void lower(Channel* c, int n) { c->median[n] -= int(unsigned((c->median[n] + (128 >> n) - 2) / (128 >> n)) * 2u); }
void raise(Channel* c, int n) { c->median[n] += int(unsigned((c->median[n] + (128 >> n)) / (128 >> n)) * 5u); }
int levelDecay(int a) { return (a + 0x80) >> 8; }

enum Format { kS16, kS32, kFlt };

struct Block {
    uint32_t flags = 0;
    int stereo = 0, stereoIn = 0, joint = 0, hybrid = 0, hybridBitrate = 0, samples = 0;
    int terms = 0;
    Decorr decorr[kMaxTerms];
    int zero = 0, one = 0, zeroes = 0;
    int extraBits = 0, andMask = 0, orMask = 0, shift = 0, postShift = 0;
    int hybridMaxClip = 0, hybridMinClip = 0;
    int floatFlag = 0, floatShift = 0, floatMaxExp = 0;
    bool gotExtraBits = false;
    LeBits bits, extra;
    Channel ch[2];

    unsigned tail(unsigned k) {
        if (k < 1) return 0;
        const int p = 31 - std::countl_zero(k);
        const int e = int((1LL << (p + 1)) - k - 1);
        int res = int(bits.read(p));
        if (res >= e) res = int(unsigned(res) * 2u - unsigned(e) + bits.bit());
        return unsigned(res);
    }

    bool updateErrorLimit() {
        int br[2], sl[2];
        for (int i = 0; i <= stereoIn; i++) {
            if (ch[i].bitrateAcc > UINT_MAX - ch[i].bitrateDelta) return false;
            ch[i].bitrateAcc += ch[i].bitrateDelta;
            br[i] = int(ch[i].bitrateAcc >> 16);
            sl[i] = levelDecay(ch[i].slowLevel);
        }
        if (stereoIn && hybridBitrate) {
            const int balance = (sl[1] - sl[0] + br[1] + 1) >> 1;
            if (balance > br[0]) {
                br[1] = br[0] * 2;
                br[0] = 0;
            } else if (-balance > br[0]) {
                br[0] *= 2;
                br[1] = 0;
            } else {
                br[1] = br[0] + balance;
                br[0] = br[0] - balance;
            }
        }
        for (int i = 0; i <= stereoIn; i++) {
            if (hybridBitrate) ch[i].errorLimit = sl[i] - br[i] > -0x100 ? exp2Of(int16_t(sl[i] - br[i] + 0x100)) : 0;
            else ch[i].errorLimit = exp2Of(int16_t(br[i]));
        }
        return true;
    }

    int value(int channel, bool& last) {
        Channel* c = &ch[channel];
        last = false;
        int t;
        if (unsigned(ch[0].median[0]) < 2u && unsigned(ch[1].median[0]) < 2u && !zero && !one) {
            if (zeroes) {
                zeroes--;
                if (zeroes) {
                    c->slowLevel -= levelDecay(c->slowLevel);
                    return 0;
                }
            } else {
                t = bits.unary();
                if (t >= 2) {
                    if (t >= 32 || bits.left() < t - 1) return last = true, 0;
                    t = int(bits.read(t - 1) | (1u << (t - 1)));
                } else if (bits.left() < 0) {
                    return last = true, 0;
                }
                zeroes = t;
                if (zeroes) {
                    memset(ch[0].median, 0, sizeof ch[0].median);
                    memset(ch[1].median, 0, sizeof ch[1].median);
                    c->slowLevel -= levelDecay(c->slowLevel);
                    return 0;
                }
            }
        }
        if (zero) {
            t = 0;
            zero = 0;
        } else {
            t = bits.unary();
            if (bits.left() < 0) return last = true, 0;
            if (t == 16) {
                int t2 = bits.unary();
                if (t2 < 2) {
                    if (bits.left() < 0) return last = true, 0;
                    t += t2;
                } else {
                    if (t2 >= 32 || bits.left() < t2 - 1) return last = true, 0;
                    t += int(bits.read(t2 - 1) | (1u << (t2 - 1)));
                }
            }
            if (one) {
                one = t & 1;
                t = (t >> 1) + 1;
            } else {
                one = t & 1;
                t >>= 1;
            }
            zero = !one;
        }
        if (hybrid && !channel && !updateErrorLimit()) return last = true, 0;
        int base, add;
        if (!t) {
            base = 0;
            add = median(c, 0) - 1;
            lower(c, 0);
        } else if (t == 1) {
            base = median(c, 0);
            add = median(c, 1) - 1;
            raise(c, 0);
            lower(c, 1);
        } else if (t == 2) {
            base = median(c, 0) + median(c, 1);
            add = median(c, 2) - 1;
            raise(c, 0);
            raise(c, 1);
            lower(c, 2);
        } else {
            base = int(unsigned(median(c, 0) + median(c, 1)) + unsigned(median(c, 2)) * unsigned(t - 2));
            add = median(c, 2) - 1;
            raise(c, 0);
            raise(c, 1);
            raise(c, 2);
        }
        int ret;
        if (!c->errorLimit) {
            ret = int(unsigned(base) + tail(unsigned(add)));
            if (bits.left() <= 0) return last = true, 0;
        } else {
            int mid = int((unsigned(base) * 2u + unsigned(add) + 1) >> 1);
            while (add > c->errorLimit) {
                if (bits.left() <= 0) return last = true, 0;
                if (bits.bit()) {
                    add -= int(unsigned(mid) - unsigned(base));
                    base = mid;
                } else {
                    add = int(unsigned(mid) - unsigned(base) - 1);
                }
                mid = int((unsigned(base) * 2u + unsigned(add) + 1) >> 1);
            }
            ret = mid;
        }
        const uint32_t sign = bits.bit();
        if (hybridBitrate) c->slowLevel += log2Of(uint32_t(ret)) - levelDecay(c->slowLevel);
        return sign ? ~ret : ret;
    }

    int integer(unsigned s) {
        if (extraBits) {
            s <<= extraBits;
            if (gotExtraBits && extra.left() >= extraBits) s |= extra.read(extraBits);
        }
        unsigned bit = (s & unsigned(andMask)) | unsigned(orMask);
        bit = ((s + bit) << shift) - bit;
        if (hybrid) bit = unsigned(std::clamp(int(bit), hybridMinClip, hybridMaxClip));
        return int(bit << postShift);
    }

    float floating(int s) {
        int exp = floatMaxExp;
        uint32_t sign;
        if (gotExtraBits && extra.left() + 8 * 64 < 1 + 23 + 8 + 1) return 0.0f;
        if (s) {
            s = int(unsigned(s) << floatShift);
            sign = s < 0;
            if (sign) s = int(-unsigned(s));
            if (unsigned(s) >= 0x1000000u) {
                s = gotExtraBits && extra.bit() ? int(extra.read(23)) : 0;
                exp = 255;
            } else if (exp) {
                int shiftBy = 23 - (31 - std::countl_zero(unsigned(s)));
                exp = floatMaxExp;
                if (exp <= shiftBy) shiftBy = --exp;
                exp -= shiftBy;
                if (shiftBy) {
                    s = int(unsigned(s) << shiftBy);
                    if ((floatFlag & kShiftOnes) || (gotExtraBits && (floatFlag & kShiftSame) && extra.bit())) s |= (1 << shiftBy) - 1;
                    else if (gotExtraBits && (floatFlag & kShiftSent)) s |= int(extra.read(shiftBy));
                }
            } else {
                exp = floatMaxExp;
            }
            s &= 0x7fffff;
        } else {
            sign = 0;
            exp = 0;
            if (gotExtraBits && (floatFlag & kZeroSent)) {
                if (extra.bit()) {
                    s = int(extra.read(23));
                    if (floatMaxExp >= 25) exp = int(extra.read(8));
                    sign = extra.bit();
                } else if (floatFlag & kZeroSign) {
                    sign = extra.bit();
                }
            }
        }
        const uint32_t word = (sign << 31) | (uint32_t(exp) << 23) | uint32_t(s);
        float out;
        memcpy(&out, &word, 4);
        return out;
    }

    void put(Format format, float* to, int s) {
        if (format == kFlt) *to = floating(s);
        else if (format == kS16) *to = float(int16_t(integer(unsigned(s)))) / 32768.0f;
        else *to = float(integer(unsigned(s))) * (1.0f / 2147483648.0f);
    }

    int weighted(Format format, int weight, int sample) {
        if (format != kS16) return int((weight * int64_t(sample) + 512) >> 10);
        return int(unsigned(int(unsigned(weight) * unsigned(sample) + 512u) >> 10));
    }

    static void clipWeight(int& weight, int delta, int samples, int in) {
        if (samples && in) {
            if ((samples ^ in) < 0) {
                weight -= delta;
                if (weight < -1024) weight = -1024;
            } else {
                weight += delta;
                if (weight > 1024) weight = 1024;
            }
        }
    }

    bool unpackStereo(Format format, float* left, float* right, size_t step) {
        int count = 0, pos = 0;
        bool last = false;
        one = zero = zeroes = 0;
        do {
            int L = value(0, last);
            if (last) break;
            int R = value(1, last);
            if (last) break;
            for (int i = 0; i < terms; i++) {
                Decorr* d = &decorr[i];
                const int t = d->value;
                int L2, R2;
                if (t > 0) {
                    int A, B, j;
                    if (t > 8) {
                        if (t & 1) {
                            A = int(2u * unsigned(d->samplesA[0]) - unsigned(d->samplesA[1]));
                            B = int(2u * unsigned(d->samplesB[0]) - unsigned(d->samplesB[1]));
                        } else {
                            A = int(3u * unsigned(d->samplesA[0]) - unsigned(d->samplesA[1])) >> 1;
                            B = int(3u * unsigned(d->samplesB[0]) - unsigned(d->samplesB[1])) >> 1;
                        }
                        d->samplesA[1] = d->samplesA[0];
                        d->samplesB[1] = d->samplesB[0];
                        j = 0;
                    } else {
                        A = d->samplesA[pos];
                        B = d->samplesB[pos];
                        j = (pos + t) & 7;
                    }
                    L2 = int(unsigned(L) + unsigned(weighted(format, d->weightA, A)));
                    R2 = int(unsigned(R) + unsigned(weighted(format, d->weightB, B)));
                    if (A && L) d->weightA -= ((((L ^ A) >> 30) & 2) - 1) * d->delta;
                    if (B && R) d->weightB -= ((((R ^ B) >> 30) & 2) - 1) * d->delta;
                    d->samplesA[j] = L = L2;
                    d->samplesB[j] = R = R2;
                } else if (t == -1) {
                    L2 = int(unsigned(L) + unsigned(weighted(format, d->weightA, d->samplesA[0])));
                    clipWeight(d->weightA, d->delta, d->samplesA[0], L);
                    L = L2;
                    R2 = int(unsigned(R) + unsigned(weighted(format, d->weightB, L2)));
                    clipWeight(d->weightB, d->delta, L2, R);
                    R = R2;
                    d->samplesA[0] = R;
                } else {
                    R2 = int(unsigned(R) + unsigned(weighted(format, d->weightB, d->samplesB[0])));
                    clipWeight(d->weightB, d->delta, d->samplesB[0], R);
                    R = R2;
                    if (t == -3) {
                        R2 = d->samplesA[0];
                        d->samplesA[0] = R;
                    }
                    L2 = int(unsigned(L) + unsigned(weighted(format, d->weightA, R2)));
                    clipWeight(d->weightA, d->delta, R2, L);
                    L = L2;
                    d->samplesB[0] = L;
                }
            }
            if (format == kS16 && std::llabs(int64_t(L)) + std::llabs(int64_t(R)) > (1 << 19)) return false;
            pos = (pos + 1) & 7;
            if (joint) {
                R = int(unsigned(R) - unsigned(L >> 1));
                L = int(unsigned(L) + unsigned(R));
            }
            put(format, left + size_t(count) * step, L);
            put(format, right + size_t(count) * step, R);
            count++;
        } while (!last && count < samples);
        return true;
    }

    bool unpackMono(Format format, float* to, size_t step) {
        int count = 0, pos = 0;
        bool last = false;
        one = zero = zeroes = 0;
        do {
            int T = value(0, last);
            int S = 0;
            if (last) break;
            for (int i = 0; i < terms; i++) {
                Decorr* d = &decorr[i];
                const int t = d->value;
                int A, j;
                if (t > 8) {
                    A = t & 1 ? int(2u * unsigned(d->samplesA[0]) - unsigned(d->samplesA[1])) : int(3u * unsigned(d->samplesA[0]) - unsigned(d->samplesA[1])) >> 1;
                    d->samplesA[1] = d->samplesA[0];
                    j = 0;
                } else {
                    A = d->samplesA[pos];
                    j = (pos + t) & 7;
                }
                S = int(unsigned(T) + unsigned(weighted(format, d->weightA, A)));
                if (A && T) d->weightA -= ((((T ^ A) >> 30) & 2) - 1) * d->delta;
                d->samplesA[j] = T = S;
            }
            pos = (pos + 1) & 7;
            put(format, to + size_t(count) * step, S);
            count++;
        } while (!last && count < samples);
        return true;
    }
};

struct WvPacket {
    struct Part {
        uint32_t flags = 0;
        const uint8_t* data = nullptr;
        size_t size = 0;
    };
    int samples = 0;
    std::vector<Part> parts;
};

bool splitPacket(const uint8_t* p, size_t n, WvPacket& out) {
    out.parts.clear();
    if (n < 12) return false;
    out.samples = int(le32(p));
    p += 4;
    n -= 4;
    while (n >= 8) {
        WvPacket::Part part;
        part.flags = le32(p);
        p += 8;
        n -= 8;
        uint32_t size = uint32_t(n);
        if ((part.flags & (kInitial | kFinal)) != (kInitial | kFinal)) {
            if (n < 4) return false;
            size = le32(p);
            p += 4;
            n -= 4;
        }
        if (size > n) return false;
        part.data = p - 8 - ((part.flags & (kInitial | kFinal)) != (kInitial | kFinal) ? 4 : 0);
        part.size = size;
        out.parts.push_back(part);
        p += size;
        n -= size;
    }
    return !out.parts.empty() && out.samples > 0 && out.samples <= kMaxSamples;
}

struct Stream {
    int rate = 0;
    int channels = 0;
    uint64_t mask = 0;
    bool dsd = false;
};

bool describe(const WvPacket& packet, Stream& out) {
    const WvPacket::Part& first = packet.parts[0];
    const uint8_t* data = first.data;
    const size_t head = (first.flags & (kInitial | kFinal)) != (kInitial | kFinal) ? 12 : 8;
    const uint8_t* p = data + head;
    size_t n = first.size;
    int chan = 0, custom = 0;
    uint64_t mask = 0;
    while (n >= 2) {
        const int id = p[0];
        size_t size = p[1];
        size_t skip = 2;
        if (id & 0x80) {
            if (n < 4) break;
            size |= size_t(le16(p + 2)) << 8;
            skip = 4;
        }
        size <<= 1;
        const size_t whole = size;
        if (id & 0x40) size--;
        if (n - skip < whole) break;
        const uint8_t* body = p + skip;
        if ((id & 0x3F) == 0x0D && size >= 2) {
            chan = body[0];
            switch (size - 2) {
                case 0: mask = body[1]; break;
                case 1: mask = le16(body + 1); break;
                case 2: mask = body[1] | uint32_t(body[2]) << 8 | uint32_t(body[3]) << 16; break;
                case 3: mask = le32(body + 1); break;
                case 4:
                    chan = (chan | (body[2] & 0xF) << 8) + 1;
                    mask = body[3] | uint32_t(body[4]) << 8 | uint32_t(body[5]) << 16;
                    break;
                case 5:
                    chan = (chan | (body[2] & 0xF) << 8) + 1;
                    mask = le32(body + 3);
                    break;
            }
        } else if ((id & 0x3F) == 0x27 && size == 3) {
            custom = int(body[0] | uint32_t(body[1]) << 8 | uint32_t(body[2]) << 16);
        }
        p += skip + whole;
        n -= skip + whole;
    }
    const int code = int((first.flags >> 23) & 0xF);
    out.rate = code == 15 ? custom : kRates[code];
    out.dsd = first.flags & kDsd;
    const bool multiblock = (first.flags & (kInitial | kFinal)) != (kInitial | kFinal);
    if (multiblock) {
        out.mask = mask;
        out.channels = mask ? std::popcount(mask) : chan;
        if (mask && chan && out.channels != chan) return false;
    } else {
        out.channels = first.flags & kMono ? 1 : 2;
        out.mask = 0;
    }
    if (!out.mask && out.channels <= 8) out.mask = kDefaultMasks[out.channels];
    return out.rate > 0 && out.channels > 0;
}

struct WavpackAudio : PacketDecoder {
    Block block;

    void reset() override {}

    bool decodeBlock(const WvPacket::Part& part, int samples, float* out, int& offset) {
        Block& s = block;
        const uint8_t* p = part.data;
        const bool multiblock = (part.flags & (kInitial | kFinal)) != (kInitial | kFinal);
        s = Block{};
        s.samples = samples;
        s.flags = part.flags;
        if (s.flags & kDsd) return false;
        const Format format = s.flags & kFloat ? kFlt : (s.flags & 3) <= 1 ? kS16 : kS32;
        const int bytes = format == kS16 ? 2 : 4;
        const int originalBits = int(((s.flags & 3) + 1) << 3);
        s.stereo = !(s.flags & kMono);
        s.stereoIn = s.flags & kFalseStereo ? 0 : s.stereo;
        s.joint = s.flags & kJoint;
        s.hybrid = s.flags & kHybrid;
        s.hybridBitrate = s.flags & kHybridBitrate;
        s.postShift = bytes * 8 - originalBits + int((s.flags >> 13) & 0x1f);
        if (s.postShift < 0 || s.postShift > 31) return false;
        s.hybridMaxClip = int((1LL << (originalBits - 1)) - 1);
        s.hybridMinClip = int(-1L * (1LL << (originalBits - 1)));
        const uint8_t* m = p + (multiblock ? 12 : 8);
        size_t n = part.size;
        bool gotTerms = false, gotWeights = false, gotSamples = false, gotEntropy = false, gotPcm = false, gotFloat = false, gotHybrid = false;
        while (n >= 2) {
            const int id = m[0];
            int size = m[1];
            size_t skip = 2;
            if (id & 0x80) {
                if (n < 4) break;
                size |= int(le16(m + 2)) << 8;
                skip = 4;
            }
            size <<= 1;
            const int whole = size;
            if (id & 0x40) size--;
            if (size < 0 || n - skip < size_t(whole)) break;
            const uint8_t* b = m + skip;
            size_t at = 0;
            auto byte = [&]() -> int { return at < size_t(size) ? b[at++] : (at++, 0); };
            auto word = [&]() -> int {
                int lo = byte();
                return lo | (byte() << 8);
            };
            switch (id & 0x3F) {
                case 2:
                    if (size > kMaxTerms) {
                        s.terms = 0;
                        break;
                    }
                    s.terms = size;
                    for (int i = 0; i < s.terms; i++) {
                        const int v = byte();
                        s.decorr[s.terms - i - 1].value = (v & 0x1F) - 5;
                        s.decorr[s.terms - i - 1].delta = v >> 5;
                    }
                    gotTerms = true;
                    break;
                case 3: {
                    if (!gotTerms) break;
                    const int weights = size >> s.stereoIn;
                    if (weights > kMaxTerms || weights > s.terms) break;
                    for (int i = 0; i < weights; i++) {
                        Decorr& d = s.decorr[s.terms - i - 1];
                        d.weightA = int8_t(byte()) * 8;
                        if (d.weightA > 0) d.weightA += (d.weightA + 64) >> 7;
                        if (s.stereoIn) {
                            d.weightB = int8_t(byte()) * 8;
                            if (d.weightB > 0) d.weightB += (d.weightB + 64) >> 7;
                        }
                    }
                    gotWeights = true;
                    break;
                }
                case 4: {
                    if (!gotTerms) break;
                    int t = 0;
                    for (int i = s.terms - 1; i >= 0 && t < size; i--) {
                        Decorr& d = s.decorr[i];
                        if (d.value > 8) {
                            d.samplesA[0] = exp2Of(int16_t(word()));
                            d.samplesA[1] = exp2Of(int16_t(word()));
                            if (s.stereoIn) {
                                d.samplesB[0] = exp2Of(int16_t(word()));
                                d.samplesB[1] = exp2Of(int16_t(word()));
                                t += 4;
                            }
                            t += 4;
                        } else if (d.value < 0) {
                            d.samplesA[0] = exp2Of(int16_t(word()));
                            d.samplesB[0] = exp2Of(int16_t(word()));
                            t += 4;
                        } else {
                            for (int j = 0; j < d.value; j++) {
                                d.samplesA[j] = exp2Of(int16_t(word()));
                                if (s.stereoIn) d.samplesB[j] = exp2Of(int16_t(word()));
                            }
                            t += d.value * 2 * (s.stereoIn + 1);
                        }
                    }
                    gotSamples = true;
                    break;
                }
                case 5:
                    if (size != 6 * (s.stereoIn + 1)) break;
                    for (int j = 0; j <= s.stereoIn; j++)
                        for (int i = 0; i < 3; i++) s.ch[j].median[i] = exp2Of(int16_t(word()));
                    gotEntropy = true;
                    break;
                case 6: {
                    int left = size;
                    if (s.hybridBitrate) {
                        for (int i = 0; i <= s.stereoIn; i++) {
                            s.ch[i].slowLevel = exp2Of(int16_t(word()));
                            left -= 2;
                        }
                    }
                    for (int i = 0; i < s.stereoIn + 1; i++) {
                        s.ch[i].bitrateAcc = unsigned(word()) << 16;
                        left -= 2;
                    }
                    for (int i = 0; i < s.stereoIn + 1; i++) s.ch[i].bitrateDelta = left > 0 ? unsigned(exp2Of(int16_t(word()))) : 0;
                    gotHybrid = true;
                    break;
                }
                case 9: {
                    if (size != 4) break;
                    const int v0 = byte(), v1 = byte(), v2 = byte(), v3 = byte();
                    if (v0 > 30) break;
                    s.extraBits = v0;
                    if (v1) s.shift = v1;
                    if (v2) {
                        s.andMask = s.orMask = 1;
                        s.shift = v2;
                    }
                    if (v3) {
                        s.andMask = 1;
                        s.shift = v3;
                    }
                    if (s.shift > 31) {
                        s.andMask = s.orMask = s.shift = 0;
                        break;
                    }
                    if (s.hybrid && bytes == 4 && s.postShift < 8 && s.shift > 8) {
                        s.postShift += 8;
                        s.shift -= 8;
                        s.hybridMaxClip >>= 8;
                        s.hybridMinClip >>= 8;
                    }
                    break;
                }
                case 8:
                    if (size != 4) break;
                    s.floatFlag = byte();
                    s.floatShift = byte();
                    s.floatMaxExp = byte();
                    if (s.floatShift > 31) {
                        s.floatShift = 0;
                        break;
                    }
                    gotFloat = true;
                    break;
                case 10:
                    s.bits.init(b, size_t(size));
                    gotPcm = true;
                    break;
                case 12:
                    if (size <= 4) break;
                    s.extra.init(b, size_t(size));
                    s.extra.read(32);
                    s.gotExtraBits = true;
                    break;
            }
            m += skip + size_t(whole);
            n -= skip + size_t(whole);
        }
        if (!gotPcm || !gotTerms || !gotWeights || !gotSamples || !gotEntropy || (s.hybrid && !gotHybrid) || (format == kFlt && !gotFloat)) return false;
        if (s.gotExtraBits && format != kFlt && s.extra.left() < int64_t(s.samples) * s.extraBits << s.stereoIn) s.gotExtraBits = false;
        if (offset + s.stereo >= channels) return false;
        const size_t step = size_t(channels);
        float* left = out + offset;
        float* right = s.stereo ? out + offset + 1 : nullptr;
        offset += 1 + s.stereo;
        if (s.stereoIn) return s.unpackStereo(format, left, right, step);
        if (!s.unpackMono(format, left, step)) return false;
        if (s.stereo)
            for (int i = 0; i < s.samples; ++i) right[size_t(i) * step] = left[size_t(i) * step];
        return true;
    }

    void feed(const std::vector<uint8_t>& data, std::vector<float>& out) override {
        WvPacket packet;
        if (!splitPacket(data.data(), data.size(), packet)) return;
        const size_t at = out.size();
        out.resize(at + size_t(packet.samples) * size_t(channels), 0.0f);
        int offset = 0;
        for (const WvPacket::Part& part : packet.parts) {
            if (!decodeBlock(part, packet.samples, out.data() + at, offset)) return out.resize(at);
        }
        if (offset != channels) out.resize(at);
    }
};

}

std::unique_ptr<PacketDecoder> openWavpackAudio(const Track&, const std::vector<uint8_t>& firstPacket, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    WvPacket packet;
    Stream stream;
    if (!splitPacket(firstPacket.data(), firstPacket.size(), packet) || !describe(packet, stream)) return fail("WavPack block not understood");
    if (stream.dsd) return fail("WavPack DSD streams are not supported");
    if (stream.channels > 64) return fail("WavPack layout not supported");
    auto audio = std::make_unique<WavpackAudio>();
    audio->rate = stream.rate;
    audio->channels = stream.channels;
    audio->layout = std::popcount(stream.mask) == stream.channels ? stream.mask : 0;
    audio->stateless = true;
    return audio;
}

}

#include "ac3_tables.h"
#include "audio_codec.h"
#include "bits.h"
#include "mdct.h"
#include "readers.h"
#include "writers.h"
#include <algorithm>
#include <bit>
#include <climits>
#include <cmath>
#include <cstring>

namespace absent {

namespace {

using namespace ac3;

const int kMaxChannels = 7;
const int kAllChannels = 16;
const int kBlockSize = 256;
const int kMaxBlocks = 6;
const int kBands = 50;
const int kCouplingBands = 18;
const int kSpxBands = 17;
const int kInputSize = 32768;
const int kLfeOn = 8;
const int kCoupling = 0;

const uint64_t kFrontLeft = 0x1, kFrontRight = 0x2, kFrontCenter = 0x4, kLowFrequency = 0x8, kBackLeft = 0x10, kBackRight = 0x20,
               kBackCenter = 0x100, kSideLeft = 0x200, kSideRight = 0x400;

enum { kReuse = 0 };
enum { kDeltaReuse = 0, kDeltaNew, kDeltaNone, kDeltaReserved };
enum { kDualMono = 0, kMono, kStereo, kThreeFront, kTwoOne, kThreeOne, kTwoTwo, kThreeTwo };
enum { kIndependent = 0, kDependent, kConverted, kReservedType };
enum { kGaqNone = 0, kGaq12, kGaq14, kGaq124 };
enum { kParseSync = -1, kParseBsid = -2, kParseRate = -3, kParseSize = -4, kParseType = -5, kParseMap = -7, kHardError = -8 };

struct Shared {
    uint8_t ungroup3In7[128][3];
    int bap1[32][3];
    int bap2[128][3];
    int bap4[128][2];
    int bap3[8];
    int bap5[16];
    float dynamicRange[256];
    float window[kBlockSize];
};

int dequantSymmetric(int code, int levels) { return ((code - (levels >> 1)) * (1 << 24)) / levels; }

const Shared& shared() {
    static const Shared tables = [] {
        Shared s{};
        for (int i = 0; i < 128; ++i) {
            s.ungroup3In7[i][0] = uint8_t(i / 25);
            s.ungroup3In7[i][1] = uint8_t((i % 25) / 5);
            s.ungroup3In7[i][2] = uint8_t((i % 25) % 5);
        }
        for (int i = 0; i < 32; ++i)
            for (int k = 0; k < 3; ++k) s.bap1[i][k] = dequantSymmetric(kUngroup3In5[i][k], 3);
        for (int i = 0; i < 128; ++i) {
            for (int k = 0; k < 3; ++k) s.bap2[i][k] = dequantSymmetric(s.ungroup3In7[i][k], 5);
            s.bap4[i][0] = dequantSymmetric(i / 11, 11);
            s.bap4[i][1] = dequantSymmetric(i % 11, 11);
        }
        for (int i = 0; i < 7; ++i) s.bap3[i] = dequantSymmetric(i, 7);
        for (int i = 0; i < 15; ++i) s.bap5[i] = dequantSymmetric(i, 15);
        for (int i = 0; i < 256; i++) {
            int v = (i >> 5) - ((i >> 7) << 3) - 5;
            s.dynamicRange[i] = std::ldexp(1.0f, v) * float((i & 0x1F) | 0x20);
        }
        kbdWindow(s.window, 5.0f, kBlockSize);
        return s;
    }();
    return tables;
}

struct Header {
    int bitstreamId = 0, channelMode = 0, lfeOn = 0, frameType = 0, substreamId = 0;
    int srCode = 0, srShift = 0, sampleRate = 0, frameSize = 0, numBlocks = 6, channels = 0;
    int compressionExists[2] = {};
    int channelMap = 0;
    int bitstreamMode = 0, bitRateCode = 0, complexity = 0;
};

int signedBits(BitReader& r, int n) { return int32_t(r.read(n) << (32 - n)) >> (32 - n); }

int ac3Info(BitReader& r, Header& h) {
    for (int i = 0; i < (h.channelMode ? 1 : 2); i++) {
        r.skip(5);
        h.compressionExists[i] = r.bit();
        if (h.compressionExists[i]) r.skip(8);
        if (r.bit()) r.skip(8);
        if (r.bit()) r.skip(7);
    }
    r.skip(2);
    if (r.bit()) r.skip(14);
    if (r.bit()) r.skip(14);
    if (r.bit()) {
        int i = int(r.read(6));
        do r.skip(8);
        while (i--);
    }
    return 0;
}

int eac3Info(BitReader& r, Header& h) {
    if (h.frameType == kReservedType) return kParseType;
    if (h.substreamId) return kParseType;
    r.skip(5);
    for (int i = 0; i < (h.channelMode ? 1 : 2); i++) {
        r.skip(5);
        h.compressionExists[i] = r.bit();
        if (h.compressionExists[i]) r.skip(8);
    }
    if (h.frameType == kDependent) {
        if (r.bit()) {
            uint64_t layout = 0;
            int map = int(r.read(16));
            for (int i = 0; i < 16; i++)
                if (map & (1 << (kAllChannels - i - 1))) layout |= kCustomChannels[i][1];
            if (std::popcount(layout) > kAllChannels) return kParseMap;
            h.channelMap = map;
        }
    }
    if (r.bit()) {
        if (h.channelMode > kStereo) {
            r.skip(2);
            if (h.channelMode & 1) r.skip(6);
            if (h.channelMode & 4) r.skip(6);
        }
        if (h.lfeOn && r.bit()) r.skip(5);
        if (h.frameType == kIndependent) {
            for (int i = 0; i < (h.channelMode ? 1 : 2); i++)
                if (r.bit()) r.skip(6);
            if (r.bit()) r.skip(6);
            switch (r.read(2)) {
                case 1: r.skip(5); break;
                case 2: r.skip(12); break;
                case 3: r.skip((int64_t(r.read(5)) + 2) << 3); break;
            }
            if (h.channelMode < kStereo) {
                for (int i = 0; i < (h.channelMode ? 1 : 2); i++)
                    if (r.bit()) r.skip(14);
            }
            if (r.bit()) {
                for (int i = 0; i < h.numBlocks; i++)
                    if (h.numBlocks == 1 || r.bit()) r.skip(5);
            }
        }
    }
    if (r.bit()) {
        h.bitstreamMode = int(r.read(3));
        r.skip(2);
        if (h.channelMode == kStereo) r.skip(4);
        if (h.channelMode >= kTwoTwo) r.skip(2);
        for (int i = 0; i < (h.channelMode ? 1 : 2); i++)
            if (r.bit()) r.skip(8);
        if (h.srCode != 3) r.skip(1);
    }
    if (h.frameType == kIndependent && h.numBlocks != 6) r.skip(1);
    if (h.frameType == kConverted && (h.numBlocks == 6 || r.bit())) r.skip(6);
    if (r.bit()) {
        int length = int(r.read(6));
        for (int i = 0; i < length + 1; i++) {
            if (i == 0) {
                r.skip(7);
                if (r.bit()) {
                    h.complexity = int(r.read(8));
                    i++;
                }
            } else {
                r.skip(8);
            }
        }
    }
    return 0;
}

int parseHeader(BitReader& r, Header& h) {
    static const uint8_t eac3Blocks[4] = {1, 2, 3, 6};
    h = Header{};
    if (r.read(16) != 0x0B77) return kParseSync;
    h.bitstreamId = int(r.peek(29) & 0x1F);
    if (h.bitstreamId > 16) return kParseBsid;
    h.numBlocks = 6;
    if (h.bitstreamId <= 10) {
        r.skip(16);
        h.srCode = int(r.read(2));
        if (h.srCode == 3) return kParseRate;
        int frameSizeCode = int(r.read(6));
        if (frameSizeCode > 37) return kParseSize;
        h.bitRateCode = frameSizeCode >> 1;
        r.skip(5);
        h.bitstreamMode = int(r.read(3));
        h.channelMode = int(r.read(3));
        if (h.channelMode == kStereo) {
            r.skip(2);
        } else {
            if ((h.channelMode & 1) && h.channelMode != kMono) r.skip(2);
            if (h.channelMode & 4) r.skip(2);
        }
        h.lfeOn = r.bit();
        h.srShift = std::max(h.bitstreamId, 8) - 8;
        h.sampleRate = kSampleRates[h.srCode] >> h.srShift;
        h.channels = kChannels[h.channelMode] + h.lfeOn;
        h.frameSize = kFrameSizes[frameSizeCode][h.srCode] * 2;
        h.frameType = kConverted;
        h.substreamId = 0;
        return ac3Info(r, h);
    }
    h.frameType = int(r.read(2));
    if (h.frameType == kReservedType) return kParseType;
    h.substreamId = int(r.read(3));
    h.frameSize = int(r.read(11) + 1) << 1;
    if (h.frameSize < 7) return kParseSize;
    h.srCode = int(r.read(2));
    if (h.srCode == 3) {
        int code = int(r.read(2));
        if (code == 3) return kParseRate;
        h.sampleRate = kSampleRates[code] / 2;
        h.srShift = 1;
    } else {
        h.numBlocks = eac3Blocks[r.read(2)];
        h.sampleRate = kSampleRates[h.srCode];
        h.srShift = 0;
    }
    h.channelMode = int(r.read(3));
    h.lfeOn = r.bit();
    h.channels = kChannels[h.channelMode] + h.lfeOn;
    return eac3Info(r, h);
}

int findSync(const uint8_t* buf, int size) {
    int i;
    for (i = 1; i < size; i += 2) {
        if (buf[i] == 0x77 || buf[i] == 0x0B) {
            if ((buf[i] ^ buf[i - 1]) == (0x77 ^ 0x0B)) {
                i--;
                break;
            } else if (i + 1 < size && (buf[i] ^ buf[i + 1]) == (0x77 ^ 0x0B)) {
                break;
            }
        }
    }
    return i >= size ? -1 : i;
}

int lowcomp1(int a, int b0, int b1, int c) {
    if ((b0 + 256) == b1) a = c;
    else if (b0 > b1) a = std::max(a - 64, 0);
    return a;
}

int lowcomp(int a, int b0, int b1, int bin) {
    if (bin < 7) return lowcomp1(a, b0, b1, 384);
    if (bin < 20) return lowcomp1(a, b0, b1, 320);
    return std::max(a - 128, 0);
}

struct BitAllocation {
    int srCode, srShift;
    int slowGain, slowDecay, fastDecay, dbPerBit, floor;
    int couplingFastLeak, couplingSlowLeak;
};

void powerSpectrum(const int8_t* exp, int start, int end, int16_t* psd, int16_t* bandPsd) {
    int bin, band;
    for (bin = start; bin < end; bin++) psd[bin] = int16_t(3072 - (exp[bin] << 7));
    for (bin = start; bin < std::min(end, 28); bin++) bandPsd[bin] = psd[bin];
    if (bin >= end) return;
    band = kBinToBand[bin];
    do {
        int v = psd[bin++];
        int bandEnd = std::min<int>(kBandStart[band + 1], end);
        for (; bin < bandEnd; bin++) {
            int most = std::max<int>(v, psd[bin]);
            int adr = std::min(most - ((v + psd[bin] + 1) >> 1), 255);
            v = most + kLogAdd[adr];
        }
        bandPsd[band++] = int16_t(v);
    } while (end > kBandStart[band]);
}

bool maskingCurve(const BitAllocation& s, const int16_t* bandPsd, int start, int end, int fastGain, bool lfe, int deltaMode, int deltaSegments,
                  const uint8_t* deltaOffsets, const uint8_t* deltaLengths, const uint8_t* deltaValues, int16_t* mask) {
    int16_t excite[kBands];
    int band, begin, end1, low = 0, fastLeak = 0, slowLeak = 0;
    if (end <= 0) return false;
    const int bandStart = kBinToBand[start];
    const int bandEnd = kBinToBand[end - 1] + 1;
    if (bandStart == 0) {
        low = lowcomp1(low, bandPsd[0], bandPsd[1], 384);
        excite[0] = int16_t(bandPsd[0] - fastGain - low);
        low = lowcomp1(low, bandPsd[1], bandPsd[2], 384);
        excite[1] = int16_t(bandPsd[1] - fastGain - low);
        begin = 7;
        for (band = 2; band < 7; band++) {
            if (!(lfe && band == 6)) low = lowcomp1(low, bandPsd[band], bandPsd[band + 1], 384);
            fastLeak = bandPsd[band] - fastGain;
            slowLeak = bandPsd[band] - s.slowGain;
            excite[band] = int16_t(fastLeak - low);
            if (!(lfe && band == 6)) {
                if (bandPsd[band] <= bandPsd[band + 1]) {
                    begin = band + 1;
                    break;
                }
            }
        }
        end1 = std::min(bandEnd, 22);
        for (band = begin; band < end1; band++) {
            if (!(lfe && band == 6)) low = lowcomp(low, bandPsd[band], bandPsd[band + 1], band);
            fastLeak = std::max(fastLeak - s.fastDecay, bandPsd[band] - fastGain);
            slowLeak = std::max(slowLeak - s.slowDecay, bandPsd[band] - s.slowGain);
            excite[band] = int16_t(std::max(fastLeak - low, slowLeak));
        }
        begin = 22;
    } else {
        begin = bandStart;
        fastLeak = (s.couplingFastLeak << 8) + 768;
        slowLeak = (s.couplingSlowLeak << 8) + 768;
    }
    for (band = begin; band < bandEnd; band++) {
        fastLeak = std::max(fastLeak - s.fastDecay, bandPsd[band] - fastGain);
        slowLeak = std::max(slowLeak - s.slowDecay, bandPsd[band] - s.slowGain);
        excite[band] = int16_t(std::max(fastLeak, slowLeak));
    }
    for (band = bandStart; band < bandEnd; band++) {
        int tmp = s.dbPerBit - bandPsd[band];
        if (tmp > 0) excite[band] = int16_t(excite[band] + (tmp >> 2));
        mask[band] = int16_t(std::max<int>(kHearingThreshold[band >> s.srShift][s.srCode], excite[band]));
    }
    if (deltaMode == kDeltaReuse || deltaMode == kDeltaNew) {
        if (deltaSegments > 8) return false;
        band = bandStart;
        for (int seg = 0; seg < deltaSegments; seg++) {
            band += deltaOffsets[seg];
            if (band >= kBands || deltaLengths[seg] > kBands - band) return false;
            int delta = deltaValues[seg] >= 4 ? (deltaValues[seg] - 3) * 128 : (deltaValues[seg] - 4) * 128;
            for (int i = 0; i < deltaLengths[seg]; i++) {
                mask[band] = int16_t(mask[band] + delta);
                band++;
            }
        }
    }
    return true;
}

void bitPointers(const int16_t* mask, const int16_t* psd, int start, int end, int snrOffset, int floor, const uint8_t* table, uint8_t* bap) {
    int bin, band, bandEnd;
    if (snrOffset == -960) {
        memset(bap, 0, kBlockSize);
        return;
    }
    for (bin = start; bin < std::min(end, 28); bin++) {
        int m = (std::max(mask[bin] - snrOffset - floor, 0) & 0x1FE0) + floor;
        bap[bin] = table[std::clamp((psd[bin] - m) >> 5, 0, 63)];
    }
    if (bin >= end) return;
    band = kBinToBand[bin];
    do {
        int m = (std::max(mask[band] - snrOffset - floor, 0) & 0x1FE0) + floor;
        bandEnd = std::min<int>(kBandStart[++band], end);
        for (; bin < bandEnd; bin++) bap[bin] = table[std::clamp((psd[bin] - m) >> 5, 0, 63)];
    } while (end > bandEnd);
}

void windowed(float* dst, const float* src0, const float* src1, const float* win, int len) {
    dst += len;
    win += len;
    src0 += len;
    for (int i = -len, j = len - 1; i < 0; i++, j--) {
        float s0 = src0[i];
        float s1 = src1[j];
        float wi = win[i];
        float wj = win[j];
        dst[i] = s0 * wj - s1 * wi;
        dst[j] = s0 * wi + s1 * wj;
    }
}

const int64_t kIdct0 = 10273905LL, kIdct1 = 11863283LL, kIdct2 = 3070444LL;

void idct6(int pre[6]) {
    int odd1 = pre[1] - pre[3] - pre[5];
    int even2 = int((pre[2] * kIdct0) >> 23);
    int tmp = int((pre[4] * kIdct1) >> 23);
    int odd0 = int(((int64_t(pre[1]) + pre[5]) * kIdct2) >> 23);
    int even0 = pre[0] + (tmp >> 1);
    int even1 = pre[0] - tmp;
    tmp = even0;
    even0 = tmp + even2;
    even2 = tmp - even2;
    tmp = odd0;
    odd0 = tmp + pre[1] + pre[3];
    int odd2 = tmp + pre[5] - pre[3];
    pre[0] = even0 + odd0;
    pre[1] = even1 + odd1;
    pre[2] = even2 + odd2;
    pre[3] = even2 - odd2;
    pre[4] = even1 - odd1;
    pre[5] = even0 - odd0;
}

typedef float Lanes __attribute__((vector_size(16)));

struct BatchMdct {
    static const int kHalf = 128, kQuarter = 64, kLanes = 4;
    float turnRe[kHalf], turnIm[kHalf];
    float twiddleRe[kHalf], twiddleIm[kHalf];
    uint8_t order[kHalf];
    Lanes re[kHalf], im[kHalf];

    BatchMdct() {
        const double theta = 1.0 / 8.0;
        for (int i = 0; i < kHalf; ++i) {
            const double alpha = M_PI_2 * (i + theta) / kHalf;
            turnRe[i] = float(std::cos(alpha) * 1.0);
            turnIm[i] = float(std::sin(alpha) * 1.0);
            uint32_t r = 0;
            for (int b = 1; b < kHalf; b <<= 1) r = (r << 1) | ((uint32_t(i) & uint32_t(b)) ? 1 : 0);
            order[i] = uint8_t(r);
        }
        int at = 0;
        for (int length = 2; length <= kHalf; length <<= 1) {
            for (int k = 0; k < length / 2; ++k, ++at) {
                const double angle = 2 * M_PI * k / length;
                twiddleRe[at] = float(std::cos(angle));
                twiddleIm[at] = float(std::sin(angle));
            }
        }
    }

    void run(const float* const* source, float* const* target, int lanes) {
        static const float silence[2 * kHalf] = {};
        float spare[2 * kHalf];
        const float* in[kLanes];
        float* out[kLanes];
        for (int l = 0; l < kLanes; ++l) {
            in[l] = l < lanes ? source[l] : silence;
            out[l] = l < lanes ? target[l] : spare;
        }
        const int n = 2 * kHalf;
        for (int i = 0; i < kHalf; ++i) {
            Lanes a, b;
            for (int l = 0; l < kLanes; ++l) {
                a[l] = in[l][n - 1 - 2 * i];
                b[l] = in[l][2 * i];
            }
            const float tr = turnRe[i], ti = turnIm[i];
            re[order[i]] = a * tr - b * ti;
            im[order[i]] = a * ti + b * tr;
        }
        for (int length = 2; length <= kHalf; length <<= 1) {
            const int step = length / 2;
            const float* wr = twiddleRe + step - 1;
            const float* wi = twiddleIm + step - 1;
            for (int start = 0; start < kHalf; start += length) {
                for (int k = 0; k < step; ++k) {
                    const int x = start + k, y = x + step;
                    const float c = wr[k], d = wi[k];
                    const Lanes tr = re[y] * c - im[y] * d;
                    const Lanes ti = re[y] * d + im[y] * c;
                    re[y] = re[x] - tr;
                    im[y] = im[x] - ti;
                    re[x] += tr;
                    im[x] += ti;
                }
            }
        }
        for (int i = 0; i < kQuarter; ++i) {
            const int i0 = kQuarter + i, i1 = kQuarter - i - 1;
            const float c1 = turnRe[i1], s1 = turnIm[i1], c0 = turnRe[i0], s0 = turnIm[i0];
            const Lanes o0 = im[i1] * s1 - re[i1] * c1;
            const Lanes o1 = im[i1] * c1 + re[i1] * s1;
            const Lanes o2 = im[i0] * s0 - re[i0] * c0;
            const Lanes o3 = im[i0] * c0 + re[i0] * s0;
            for (int l = 0; l < kLanes; ++l) {
                out[l][2 * i1] = o0[l];
                out[l][2 * i0 + 1] = o1[l];
                out[l][2 * i0] = o2[l];
                out[l][2 * i1 + 1] = o3[l];
            }
        }
    }
};

struct Dither {
    uint32_t state[64];
    uint32_t index;
    void seed() {
        memcpy(state, kDitherSeed, sizeof state);
        index = 0;
    }
    uint32_t next() {
        uint32_t a = state[index & 63] = state[(index - 24) & 63] + state[(index - 55) & 63];
        index += 1;
        return a;
    }
};

struct Carry {
    Dither dither;
    int32_t downmixed;
    uint8_t couplingBandStruct[kCouplingBands];
    uint8_t spxBandStruct[kSpxBands];
};

struct State {
    int frameType, substreamId, superframeSize, frameSize, bitRate, sampleRate, numBlocks, bitstreamId, channelMode, lfeOn;
    int compressionExists[2];
    int channelMap;
    int eac3;
    int snrOffsetStrategy, blockSwitchSyntax, ditherFlagSyntax, bitAllocationSyntax, fastGainSyntax, dbaSyntax, skipSyntax;
    int couplingInUse[kMaxBlocks], couplingStrategyExists[kMaxBlocks], channelInCoupling[kMaxChannels];
    int phaseFlagsInUse, phaseFlags[kCouplingBands], couplingBandCount;
    uint8_t couplingBandSizes[kCouplingBands];
    int firstCouplingCoords[kMaxChannels], couplingCoords[kMaxChannels][kCouplingBands];
    int spxInUse;
    uint8_t channelUsesSpx[kMaxChannels];
    int8_t spxAttenuation[kMaxChannels];
    int spxSourceStart, spxDestinationEnd, spxDestinationStart, spxBandCount;
    uint8_t spxBandSizes[kSpxBands];
    uint8_t firstSpxCoords[kMaxChannels];
    float spxNoiseBlend[kMaxChannels][kSpxBands], spxSignalBlend[kMaxChannels][kSpxBands];
    int channelUsesAht[kMaxChannels];
    int preMantissa[kMaxChannels][kBlockSize][kMaxBlocks];
    int fullChannels, channels, lfeChannel, outputMode, previousOutputMode, outChannels;
    float dynamicRange[2];
    int startFrequency[kMaxChannels], endFrequency[kMaxChannels];
    int rematrixingBands, rematrixingFlags[4];
    int exponentGroups[kMaxChannels];
    int8_t exponents[kMaxChannels][kBlockSize];
    int exponentStrategy[kMaxBlocks][kMaxChannels];
    BitAllocation allocation;
    int firstCouplingLeak;
    int snrOffset[kMaxChannels], fastGain[kMaxChannels];
    uint8_t bap[kMaxChannels][kBlockSize];
    int16_t psd[kMaxChannels][kBlockSize];
    int16_t bandPsd[kMaxChannels][kBands];
    int16_t mask[kMaxChannels][kBands];
    int deltaMode[kMaxChannels], deltaSegments[kMaxChannels];
    uint8_t deltaOffsets[kMaxChannels][8], deltaLengths[kMaxChannels][8], deltaValues[kMaxChannels][8];
    int ditherFlag[kMaxChannels];
    int blockSwitch[kMaxChannels];
    int32_t coeffs[kMaxChannels][kBlockSize];
    float transformCoeffs[kMaxChannels][kBlockSize];
    float delay[kAllChannels][kBlockSize];
    float tmpOutput[kBlockSize];
    float output[kAllChannels][kBlockSize];
    float outputBuffer[kAllChannels][kBlockSize * kMaxBlocks];
};

struct Mantissas {
    int b1Mantissa[2], b2Mantissa[2], b4Mantissa;
    int b1, b2, b4;
};

struct Ac3Audio : PacketDecoder {
    const Shared& t = shared();
    BatchMdct longTransform;
    InverseMdct shortTransform{7, 1.0};
    float longOutput[kMaxChannels][kBlockSize];
    State s{};
    Carry c{};
    BitReader r;
    float* outptr[kMaxChannels] = {};
    uint8_t swapped[kInputSize + 2] = {};
    uint64_t want[8] = {};
    bool fold = false;
    uint64_t lastMask = 0;
    int lastMap[kAllChannels] = {};
    int lastFrames = 0;
    int produced = 0;

    void reset() override {
        memset(&s, 0, sizeof s);
        memset(&c, 0, sizeof c);
        c.dither.seed();
        c.downmixed = 1;
    }

    bool save(std::vector<uint8_t>& state) const override {
        state.resize(sizeof c);
        memcpy(state.data(), &c, sizeof c);
        return true;
    }

    void load(const std::vector<uint8_t>& state) override {
        if (state.size() == sizeof c) memcpy(&c, state.data(), sizeof c);
    }

    int exponentsOf(int strategy, int groups, uint8_t absolute, int8_t* out) {
        int dexp[256];
        int groupSize = strategy + (strategy == 3);
        int i = 0;
        for (int g = 0; g < groups; g++) {
            int packed = int(r.read(7));
            if (packed >= 125) return -1;
            dexp[i++] = t.ungroup3In7[packed][0];
            dexp[i++] = t.ungroup3In7[packed][1];
            dexp[i++] = t.ungroup3In7[packed][2];
        }
        int previous = absolute;
        for (int k = 0, j = 0; k < groups * 3; k++) {
            previous += dexp[k] - 2;
            if (unsigned(previous) > 24U) return -1;
            switch (groupSize) {
                case 4:
                    out[j++] = int8_t(previous);
                    out[j++] = int8_t(previous);
                    [[fallthrough]];
                case 2: out[j++] = int8_t(previous); [[fallthrough]];
                case 1: out[j++] = int8_t(previous);
            }
        }
        return 0;
    }

    void couplingCoefficients() {
        int bin = s.startFrequency[kCoupling];
        for (int band = 0; band < s.couplingBandCount; band++) {
            int bandStart = bin;
            int bandEnd = bin + s.couplingBandSizes[band];
            for (int ch = 1; ch <= s.fullChannels; ch++) {
                if (s.channelInCoupling[ch]) {
                    const int64_t coord = int64_t(s.couplingCoords[ch][band] << 5);
                    for (bin = bandStart; bin < bandEnd; bin++) s.coeffs[ch][bin] = int32_t((int64_t(s.coeffs[kCoupling][bin] * (1 << 4)) * coord) >> 32);
                    if (ch == 2 && s.phaseFlags[band]) {
                        for (bin = bandStart; bin < bandEnd; bin++) s.coeffs[2][bin] = -s.coeffs[2][bin];
                    }
                }
            }
            bin = bandEnd;
        }
    }

    void plainCoefficients(int ch, Mantissas& m) {
        const int start = s.startFrequency[ch];
        const int end = s.endFrequency[ch];
        const uint8_t* baps = s.bap[ch];
        const int8_t* exps = s.exponents[ch];
        int32_t* coeffs = s.coeffs[ch];
        const bool dither = ch == kCoupling || s.ditherFlag[ch];
        for (int freq = start; freq < end; freq++) {
            int bap = baps[freq];
            int mantissa;
            switch (bap) {
                case 0:
                    mantissa = dither ? int((((c.dither.next() >> 8) * 181) >> 8)) - 5931008 : 0;
                    break;
                case 1:
                    if (m.b1) {
                        m.b1--;
                        mantissa = m.b1Mantissa[m.b1];
                    } else {
                        int bits = int(r.read(5));
                        mantissa = t.bap1[bits][0];
                        m.b1Mantissa[1] = t.bap1[bits][1];
                        m.b1Mantissa[0] = t.bap1[bits][2];
                        m.b1 = 2;
                    }
                    break;
                case 2:
                    if (m.b2) {
                        m.b2--;
                        mantissa = m.b2Mantissa[m.b2];
                    } else {
                        int bits = int(r.read(7));
                        mantissa = t.bap2[bits][0];
                        m.b2Mantissa[1] = t.bap2[bits][1];
                        m.b2Mantissa[0] = t.bap2[bits][2];
                        m.b2 = 2;
                    }
                    break;
                case 3: mantissa = t.bap3[r.read(3)]; break;
                case 4:
                    if (m.b4) {
                        m.b4 = 0;
                        mantissa = m.b4Mantissa;
                    } else {
                        int bits = int(r.read(7));
                        mantissa = t.bap4[bits][0];
                        m.b4Mantissa = t.bap4[bits][1];
                        m.b4 = 1;
                    }
                    break;
                case 5: mantissa = t.bap5[r.read(4)]; break;
                default: {
                    if (bap > 15) bap = 15;
                    const int q = kQuantizationBits[bap];
                    mantissa = int(unsigned(signedBits(r, q)) << (24 - q));
                    break;
                }
            }
            coeffs[freq] = mantissa >> exps[freq];
        }
    }

    void ahtMantissas(int ch) {
        int gaqGain[kBlockSize];
        int gaqMode = int(r.read(2));
        int endBap = gaqMode < 2 ? 12 : 17;
        int gs = 0;
        if (gaqMode == kGaq12 || gaqMode == kGaq14) {
            for (int bin = s.startFrequency[ch]; bin < s.endFrequency[ch]; bin++) {
                if (s.bap[ch][bin] > 7 && s.bap[ch][bin] < endBap) gaqGain[gs++] = int(r.bit()) << (gaqMode - 1);
            }
        } else if (gaqMode == kGaq124) {
            int gc = 2;
            for (int bin = s.startFrequency[ch]; bin < s.endFrequency[ch]; bin++) {
                if (s.bap[ch][bin] > 7 && s.bap[ch][bin] < 17) {
                    if (gc++ == 2) {
                        int group = std::min<int>(int(r.read(5)), 26);
                        gaqGain[gs++] = kUngroup3In5[group][0];
                        gaqGain[gs++] = kUngroup3In5[group][1];
                        gaqGain[gs++] = kUngroup3In5[group][2];
                        gc = 0;
                    }
                }
            }
        }
        gs = 0;
        for (int bin = s.startFrequency[ch]; bin < s.endFrequency[ch]; bin++) {
            int hebap = s.bap[ch][bin];
            int bits = kBitsForHighBap[hebap];
            int* pre = s.preMantissa[ch][bin];
            if (!hebap) {
                for (int blk = 0; blk < 6; blk++) pre[blk] = int(c.dither.next() & 0x7FFFFF) - 0x400000;
            } else if (hebap < 8) {
                int v = int(r.read(bits));
                for (int blk = 0; blk < 6; blk++) pre[blk] = kMantissaVq[hebap][v][blk] * (1 << 8);
            } else {
                int logGain = gaqMode != kGaqNone && hebap < endBap ? gaqGain[gs++] : 0;
                int gbits = bits - logGain;
                for (int blk = 0; blk < 6; blk++) {
                    int mant = signedBits(r, gbits);
                    if (logGain && mant == -(1 << (gbits - 1))) {
                        int b;
                        int mbits = bits - (2 - logGain);
                        mant = signedBits(r, mbits);
                        mant = int(unsigned(mant) << (23 - (mbits - 1)));
                        if (mant >= 0) b = 1 << (23 - logGain);
                        else b = kGaqRemapB[hebap - 8][logGain - 1] * (1 << 8);
                        mant += int((kGaqRemapA[hebap - 8][logGain - 1] * int64_t(mant)) >> 15) + b;
                    } else {
                        mant *= 1 << (24 - bits);
                        if (!logGain) mant += int((kGaqRemap1[hebap - 8] * int64_t(mant)) >> 15);
                    }
                    pre[blk] = mant;
                }
            }
            idct6(pre);
        }
    }

    void channelCoefficients(int blk, int ch, Mantissas& m) {
        if (!s.channelUsesAht[ch]) {
            plainCoefficients(ch, m);
            return;
        }
        if (!blk) ahtMantissas(ch);
        for (int bin = s.startFrequency[ch]; bin < s.endFrequency[ch]; bin++)
            s.coeffs[ch][bin] = s.preMantissa[ch][bin][blk] >> s.exponents[ch][bin];
    }

    void transformCoefficients(int blk) {
        bool gotCoupling = false;
        Mantissas m{};
        for (int ch = 1; ch <= s.channels; ch++) {
            channelCoefficients(blk, ch, m);
            int end;
            if (s.channelInCoupling[ch]) {
                if (!gotCoupling) {
                    channelCoefficients(blk, kCoupling, m);
                    couplingCoefficients();
                    gotCoupling = true;
                }
                end = s.endFrequency[kCoupling];
            } else {
                end = s.endFrequency[ch];
            }
            do s.coeffs[ch][end] = 0;
            while (++end < 256);
        }
        for (int ch = 1; ch <= s.fullChannels; ch++) {
            if (!s.ditherFlag[ch] && s.channelInCoupling[ch]) {
                for (int i = s.startFrequency[kCoupling]; i < s.endFrequency[kCoupling]; i++)
                    if (!s.bap[kCoupling][i]) s.coeffs[ch][i] = 0;
            }
        }
    }

    void rematrix() {
        int end = std::min(s.endFrequency[1], s.endFrequency[2]);
        for (int band = 0; band < s.rematrixingBands; band++) {
            if (s.rematrixingFlags[band]) {
                int bandEnd = std::min<int>(end, kRematrixBands[band + 1]);
                for (int i = kRematrixBands[band]; i < bandEnd; i++) {
                    int32_t first = s.coeffs[1][i];
                    s.coeffs[1][i] += s.coeffs[2][i];
                    s.coeffs[2][i] = first - s.coeffs[2][i];
                }
            }
        }
    }

    void inverseTransform(int count, int offset) {
        const float* in[kMaxChannels];
        float* out[kMaxChannels];
        int lanes = 0;
        for (int ch = 1; ch <= count; ch++) {
            if (s.blockSwitch[ch]) continue;
            in[lanes] = s.transformCoeffs[ch];
            out[lanes] = longOutput[lanes];
            ++lanes;
        }
        for (int first = 0; first < lanes; first += BatchMdct::kLanes) longTransform.run(in + first, out + first, std::min(BatchMdct::kLanes, lanes - first));
        for (int ch = 1, lane = 0; ch <= count; ch++) {
            float* delayed = s.delay[ch - 1 + offset];
            if (s.blockSwitch[ch]) {
                float* x = s.tmpOutput + 128;
                for (int i = 0; i < 128; i++) x[i] = s.transformCoeffs[ch][2 * i];
                shortTransform.half(x, s.tmpOutput);
                windowed(outptr[ch - 1], delayed, s.tmpOutput, t.window, 128);
                for (int i = 0; i < 128; i++) x[i] = s.transformCoeffs[ch][2 * i + 1];
                shortTransform.half(x, delayed);
            } else {
                const float* tmp = longOutput[lane++];
                windowed(outptr[ch - 1], delayed, tmp, t.window, 128);
                memcpy(delayed, tmp + 128, 128 * sizeof(float));
            }
        }
    }

    void upmixDelay() {
        const size_t size = sizeof s.delay[0];
        switch (s.channelMode) {
            case kDualMono:
            case kStereo: memcpy(s.delay[1], s.delay[0], size); break;
            case kTwoTwo: memset(s.delay[3], 0, size); [[fallthrough]];
            case kTwoOne: memset(s.delay[2], 0, size); break;
            case kThreeTwo: memset(s.delay[4], 0, size); [[fallthrough]];
            case kThreeOne: memset(s.delay[3], 0, size); [[fallthrough]];
            case kThreeFront:
                memcpy(s.delay[2], s.delay[1], size);
                memset(s.delay[1], 0, size);
                break;
        }
    }

    void bandStructure(int blk, bool enhanced, int startSubband, int endSubband, const uint8_t* defaults, int* bandCount, uint8_t* bandSizes,
                       uint8_t* bandStruct, int structSize) {
        int subbands = endSubband - startSubband, bands = 0;
        uint8_t sizes[22];
        if (!blk) memcpy(bandStruct, defaults, size_t(structSize));
        bandStruct += startSubband + 1;
        if (!s.eac3 || r.bit()) {
            for (int sub = 0; sub < subbands - 1; sub++) bandStruct[sub] = uint8_t(r.bit());
        }
        bands = subbands;
        sizes[0] = enhanced ? 6 : 12;
        for (int band = 0, sub = 1; sub < subbands; sub++) {
            int size = (enhanced && sub < 4) ? 6 : 12;
            if (bandStruct[sub - 1]) {
                bands--;
                sizes[band] = uint8_t(sizes[band] + size);
            } else {
                sizes[++band] = uint8_t(size);
            }
        }
        *bandCount = bands;
        memcpy(bandSizes, sizes, size_t(bands));
    }

    int spxStrategy(int blk) {
        if (s.channelMode == kMono) {
            s.channelUsesSpx[1] = 1;
        } else {
            unsigned uses = r.read(s.fullChannels);
            for (int ch = s.fullChannels; ch >= 1; --ch) {
                s.channelUsesSpx[ch] = uint8_t(uses & 1);
                uses >>= 1;
            }
        }
        int destinationStart = int(r.read(2));
        int startSubband = int(r.read(3)) + 2;
        if (startSubband > 7) startSubband += startSubband - 7;
        int endSubband = int(r.read(3)) + 5;
        if (endSubband > 7) endSubband += endSubband - 7;
        destinationStart = destinationStart * 12 + 25;
        int sourceStart = startSubband * 12 + 25;
        int destinationEnd = endSubband * 12 + 25;
        if (startSubband >= endSubband) return -1;
        if (destinationStart >= sourceStart) return -1;
        s.spxDestinationStart = destinationStart;
        s.spxSourceStart = sourceStart;
        s.spxDestinationEnd = destinationEnd;
        bandStructure(blk, false, startSubband, endSubband, kDefaultSpxBands, &s.spxBandCount, s.spxBandSizes, c.spxBandStruct, kSpxBands);
        return 0;
    }

    void spxCoordinates() {
        for (int ch = 1; ch <= s.fullChannels; ch++) {
            if (s.channelUsesSpx[ch]) {
                if (s.firstSpxCoords[ch] || r.bit()) {
                    s.firstSpxCoords[ch] = 0;
                    float blend = float(r.read(5)) * (1.0f / 32);
                    int master = int(r.read(2)) * 3;
                    int bin = s.spxSourceStart;
                    for (int band = 0; band < s.spxBandCount; band++) {
                        int bandSize = s.spxBandSizes[band];
                        float ratio = (float(bin + (bandSize >> 1)) / float(s.spxDestinationEnd)) - blend;
                        ratio = std::clamp(ratio, 0.0f, 1.0f);
                        float noiseBlend = std::sqrt(3.0f * ratio);
                        float signalBlend = std::sqrt(1.0f - ratio);
                        bin += bandSize;
                        int exponent = int(r.read(4));
                        int mantissa = int(r.read(2));
                        if (exponent == 15) mantissa <<= 1;
                        else mantissa += 4;
                        mantissa <<= (25 - exponent - master);
                        float coord = float(mantissa) * (1.0f / (1 << 23));
                        s.spxNoiseBlend[ch][band] = noiseBlend * coord;
                        s.spxSignalBlend[ch][band] = signalBlend * coord;
                    }
                }
            } else {
                s.firstSpxCoords[ch] = 1;
            }
        }
    }

    int couplingStrategy(int blk, uint8_t* stages) {
        memset(stages, 3, kMaxChannels);
        if (!s.eac3) s.couplingInUse[blk] = r.bit();
        if (s.couplingInUse[blk]) {
            if (s.channelMode < kStereo) return -1;
            if (s.eac3 && r.bit()) return -1;
            if (s.eac3 && s.channelMode == kStereo) {
                s.channelInCoupling[1] = 1;
                s.channelInCoupling[2] = 1;
            } else {
                for (int ch = 1; ch <= s.fullChannels; ch++) s.channelInCoupling[ch] = r.bit();
            }
            if (s.channelMode == kStereo) s.phaseFlagsInUse = r.bit();
            int startSubband = int(r.read(4));
            int endSubband = s.spxInUse ? (s.spxSourceStart - 37) / 12 : int(r.read(4)) + 3;
            if (startSubband >= endSubband) return -1;
            s.startFrequency[kCoupling] = startSubband * 12 + 37;
            s.endFrequency[kCoupling] = endSubband * 12 + 37;
            bandStructure(blk, false, startSubband, endSubband, kDefaultCouplingBands, &s.couplingBandCount, s.couplingBandSizes,
                          c.couplingBandStruct, kCouplingBands);
        } else {
            for (int ch = 1; ch <= s.fullChannels; ch++) {
                s.channelInCoupling[ch] = 0;
                s.firstCouplingCoords[ch] = 1;
            }
            s.firstCouplingLeak = s.eac3;
            s.phaseFlagsInUse = 0;
        }
        return 0;
    }

    int couplingCoordinates(int blk) {
        bool exist = false;
        for (int ch = 1; ch <= s.fullChannels; ch++) {
            if (s.channelInCoupling[ch]) {
                if ((s.eac3 && s.firstCouplingCoords[ch]) || r.bit()) {
                    s.firstCouplingCoords[ch] = 0;
                    exist = true;
                    int master = 3 * int(r.read(2));
                    for (int band = 0; band < s.couplingBandCount; band++) {
                        int exponent = int(r.read(4));
                        int mantissa = int(r.read(4));
                        if (exponent == 15) s.couplingCoords[ch][band] = mantissa << 22;
                        else s.couplingCoords[ch][band] = (mantissa + 16) << 21;
                        s.couplingCoords[ch][band] >>= (exponent + master);
                    }
                } else if (!blk) {
                    return -1;
                }
            } else {
                s.firstCouplingCoords[ch] = 1;
            }
        }
        if (s.channelMode == kStereo && exist) {
            for (int band = 0; band < s.couplingBandCount; band++) s.phaseFlags[band] = s.phaseFlagsInUse ? r.bit() : 0;
        }
        return 0;
    }

    void spectralExtension() {
        uint8_t wrap[kSpxBands] = {1, 0};
        uint8_t copySizes[kSpxBands];
        float rms[kSpxBands];
        int sections = 0;
        int bin = s.spxDestinationStart;
        for (int band = 0; band < s.spxBandCount; band++) {
            int copySize;
            int bandSize = s.spxBandSizes[band];
            if (bin + bandSize > s.spxSourceStart) {
                copySizes[sections++] = uint8_t(bin - s.spxDestinationStart);
                bin = s.spxDestinationStart;
                wrap[band] = 1;
            }
            for (int i = 0; i < bandSize; i += copySize) {
                if (bin == s.spxSourceStart) {
                    copySizes[sections++] = uint8_t(bin - s.spxDestinationStart);
                    bin = s.spxDestinationStart;
                }
                copySize = std::min(bandSize - i, s.spxSourceStart - bin);
                bin += copySize;
            }
        }
        copySizes[sections++] = uint8_t(bin - s.spxDestinationStart);
        for (int ch = 1; ch <= s.fullChannels; ch++) {
            if (!s.channelUsesSpx[ch]) continue;
            float* coeffs = s.transformCoeffs[ch];
            bin = s.spxSourceStart;
            for (int i = 0; i < sections; i++) {
                memmove(&coeffs[bin], &coeffs[s.spxDestinationStart], copySizes[i] * sizeof(float));
                bin += copySizes[i];
            }
            bin = s.spxSourceStart;
            for (int band = 0; band < s.spxBandCount; band++) {
                int bandSize = s.spxBandSizes[band];
                float accum = 0.0f;
                for (int i = 0; i < bandSize; i++) {
                    float coeff = coeffs[bin++];
                    accum += coeff * coeff;
                }
                rms[band] = std::sqrt(accum / float(bandSize));
            }
            if (s.spxAttenuation[ch] >= 0) {
                const float* atten = kSpxAttenuation[s.spxAttenuation[ch]];
                bin = s.spxSourceStart - 2;
                for (int band = 0; band < s.spxBandCount; band++) {
                    if (wrap[band]) {
                        float* at = &coeffs[bin];
                        at[0] *= atten[0];
                        at[1] *= atten[1];
                        at[2] *= atten[2];
                        at[3] *= atten[1];
                        at[4] *= atten[0];
                    }
                    bin += s.spxBandSizes[band];
                }
            }
            bin = s.spxSourceStart;
            for (int band = 0; band < s.spxBandCount; band++) {
                float noiseScale = s.spxNoiseBlend[ch][band] * rms[band] * (1.0f / float(INT32_MIN));
                float signalScale = s.spxSignalBlend[ch][band];
                for (int i = 0; i < s.spxBandSizes[band]; i++) {
                    float noise = noiseScale * float(int32_t(c.dither.next()));
                    coeffs[bin] *= signalScale;
                    coeffs[bin++] += noise;
                }
            }
        }
    }

    int audioBlock(int blk, int offset) {
        const int fbw = s.fullChannels;
        const int mode = s.channelMode;
        uint8_t stages[kMaxChannels] = {};
        bool different = false;
        if (s.blockSwitchSyntax) {
            for (int ch = 1; ch <= fbw; ch++) {
                s.blockSwitch[ch] = r.bit();
                if (ch > 1 && s.blockSwitch[ch] != s.blockSwitch[1]) different = true;
            }
        }
        if (s.ditherFlagSyntax) {
            for (int ch = 1; ch <= fbw; ch++) s.ditherFlag[ch] = r.bit();
        }
        int i = !s.channelMode;
        do {
            if (r.bit()) s.dynamicRange[i] = t.dynamicRange[r.read(8)];
            else if (blk == 0) s.dynamicRange[i] = 1.0f;
        } while (i--);
        if (s.eac3 && (!blk || r.bit())) {
            s.spxInUse = r.bit();
            if (s.spxInUse && spxStrategy(blk) < 0) return -1;
        }
        if (!s.eac3 || !s.spxInUse) {
            s.spxInUse = 0;
            for (int ch = 1; ch <= fbw; ch++) {
                s.channelUsesSpx[ch] = 0;
                s.firstSpxCoords[ch] = 1;
            }
        }
        if (s.spxInUse) spxCoordinates();
        if (s.eac3 ? s.couplingStrategyExists[blk] : r.bit()) {
            if (couplingStrategy(blk, stages) < 0) return -1;
        } else if (!s.eac3) {
            if (!blk) return -1;
            s.couplingInUse[blk] = s.couplingInUse[blk - 1];
        }
        const int coupled = s.couplingInUse[blk];
        if (coupled && couplingCoordinates(blk) < 0) return -1;
        if (mode == kStereo) {
            if ((s.eac3 && !blk) || r.bit()) {
                s.rematrixingBands = 4;
                if (coupled && s.startFrequency[kCoupling] <= 61) s.rematrixingBands -= 1 + (s.startFrequency[kCoupling] == 37);
                else if (s.spxInUse && s.spxSourceStart <= 61) s.rematrixingBands--;
                for (int band = 0; band < s.rematrixingBands; band++) s.rematrixingFlags[band] = r.bit();
            } else if (!blk) {
                s.rematrixingBands = 0;
            }
        }
        for (int ch = !coupled; ch <= s.channels; ch++) {
            if (!s.eac3) s.exponentStrategy[blk][ch] = int(r.read(2 - (ch == s.lfeChannel)));
            if (s.exponentStrategy[blk][ch] != kReuse) stages[ch] = 3;
        }
        for (int ch = 1; ch <= fbw; ch++) {
            s.startFrequency[ch] = 0;
            if (s.exponentStrategy[blk][ch] != kReuse) {
                int previous = s.endFrequency[ch];
                if (s.channelInCoupling[ch]) {
                    s.endFrequency[ch] = s.startFrequency[kCoupling];
                } else if (s.channelUsesSpx[ch]) {
                    s.endFrequency[ch] = s.spxSourceStart;
                } else {
                    int bandwidth = int(r.read(6));
                    if (bandwidth > 60) return -1;
                    s.endFrequency[ch] = bandwidth * 3 + 73;
                }
                int groupSize = 3 << (s.exponentStrategy[blk][ch] - 1);
                s.exponentGroups[ch] = (s.endFrequency[ch] + groupSize - 4) / groupSize;
                if (blk > 0 && s.endFrequency[ch] != previous) memset(stages, 3, kMaxChannels);
            }
        }
        if (coupled && s.exponentStrategy[blk][kCoupling] != kReuse) {
            s.exponentGroups[kCoupling] = (s.endFrequency[kCoupling] - s.startFrequency[kCoupling]) / (3 << (s.exponentStrategy[blk][kCoupling] - 1));
        }
        for (int ch = !coupled; ch <= s.channels; ch++) {
            if (s.exponentStrategy[blk][ch] != kReuse) {
                s.exponents[ch][0] = int8_t(r.read(4) << !ch);
                if (exponentsOf(s.exponentStrategy[blk][ch], s.exponentGroups[ch], uint8_t(s.exponents[ch][0]),
                                &s.exponents[ch][s.startFrequency[ch] + !!ch]))
                    return -1;
                if (ch != kCoupling && ch != s.lfeChannel) r.skip(2);
            }
        }
        if (s.bitAllocationSyntax) {
            if (r.bit()) {
                s.allocation.slowDecay = kSlowDecay[r.read(2)] >> s.allocation.srShift;
                s.allocation.fastDecay = kFastDecay[r.read(2)] >> s.allocation.srShift;
                s.allocation.slowGain = kSlowGain[r.read(2)];
                s.allocation.dbPerBit = kDbPerBit[r.read(2)];
                s.allocation.floor = kFloors[r.read(3)];
                for (int ch = !coupled; ch <= s.channels; ch++) stages[ch] = std::max<uint8_t>(stages[ch], 2);
            } else if (!blk) {
                return -1;
            }
        }
        if (!s.eac3 || !blk) {
            if (s.snrOffsetStrategy && r.bit()) {
                int snr = 0;
                int coarse = (int(r.read(6)) - 15) << 4;
                int first = !coupled;
                for (int ch = first; ch <= s.channels; ch++) {
                    if (ch == first || s.snrOffsetStrategy == 2) snr = (coarse + int(r.read(4))) << 2;
                    if (blk && s.snrOffset[ch] != snr) stages[ch] = std::max<uint8_t>(stages[ch], 1);
                    s.snrOffset[ch] = snr;
                    if (!s.eac3) {
                        int previous = s.fastGain[ch];
                        s.fastGain[ch] = kFastGain[r.read(3)];
                        if (blk && previous != s.fastGain[ch]) stages[ch] = std::max<uint8_t>(stages[ch], 2);
                    }
                }
            } else if (!s.eac3 && !blk) {
                return -1;
            }
        }
        if (s.fastGainSyntax && r.bit()) {
            for (int ch = !coupled; ch <= s.channels; ch++) {
                int previous = s.fastGain[ch];
                s.fastGain[ch] = kFastGain[r.read(3)];
                if (blk && previous != s.fastGain[ch]) stages[ch] = std::max<uint8_t>(stages[ch], 2);
            }
        } else if (s.eac3 && !blk) {
            for (int ch = !coupled; ch <= s.channels; ch++) s.fastGain[ch] = kFastGain[4];
        }
        if (s.frameType == kIndependent && r.bit()) r.skip(10);
        if (coupled) {
            if (s.firstCouplingLeak || r.bit()) {
                int fast = int(r.read(3));
                int slow = int(r.read(3));
                if (blk && (fast != s.allocation.couplingFastLeak || slow != s.allocation.couplingSlowLeak))
                    stages[kCoupling] = std::max<uint8_t>(stages[kCoupling], 2);
                s.allocation.couplingFastLeak = fast;
                s.allocation.couplingSlowLeak = slow;
            } else if (!s.eac3 && !blk) {
                return -1;
            }
            s.firstCouplingLeak = 0;
        }
        if (s.dbaSyntax && r.bit()) {
            for (int ch = !coupled; ch <= fbw; ch++) {
                s.deltaMode[ch] = int(r.read(2));
                if (s.deltaMode[ch] == kDeltaReserved) return -1;
                stages[ch] = std::max<uint8_t>(stages[ch], 2);
            }
            for (int ch = !coupled; ch <= fbw; ch++) {
                if (s.deltaMode[ch] == kDeltaNew) {
                    s.deltaSegments[ch] = int(r.read(3)) + 1;
                    for (int seg = 0; seg < s.deltaSegments[ch]; seg++) {
                        s.deltaOffsets[ch][seg] = uint8_t(r.read(5));
                        s.deltaLengths[ch][seg] = uint8_t(r.read(4));
                        s.deltaValues[ch][seg] = uint8_t(r.read(3));
                    }
                    stages[ch] = std::max<uint8_t>(stages[ch], 2);
                }
            }
        } else if (blk == 0) {
            for (int ch = 0; ch <= s.channels; ch++) s.deltaMode[ch] = kDeltaNone;
        }
        for (int ch = !coupled; ch <= s.channels; ch++) {
            if (stages[ch] > 2) powerSpectrum(s.exponents[ch], s.startFrequency[ch], s.endFrequency[ch], s.psd[ch], s.bandPsd[ch]);
            if (stages[ch] > 1) {
                if (!maskingCurve(s.allocation, s.bandPsd[ch], s.startFrequency[ch], s.endFrequency[ch], s.fastGain[ch], ch == s.lfeChannel,
                                  s.deltaMode[ch], s.deltaSegments[ch], s.deltaOffsets[ch], s.deltaLengths[ch], s.deltaValues[ch], s.mask[ch]))
                    return -1;
            }
            if (stages[ch] > 0) {
                const uint8_t* table = s.channelUsesAht[ch] ? kHighBapTable : kBapTable;
                bitPointers(s.mask[ch], s.psd[ch], s.startFrequency[ch], s.endFrequency[ch], s.snrOffset[ch], s.allocation.floor, table, s.bap[ch]);
            }
        }
        if (s.skipSyntax && r.bit()) r.skip(int64_t(r.read(9)) * 8);
        transformCoefficients(blk);
        if (s.channelMode == kStereo) rematrix();
        for (int ch = 1; ch <= s.channels; ch++) {
            int audioChannel = 0;
            if (s.channelMode == kDualMono && ch <= 2) audioChannel = 2 - ch;
            float gain = s.dynamicRange[audioChannel];
            gain *= 1.0f / 4194304.0f;
            for (int k = 0; k < kBlockSize; ++k) s.transformCoeffs[ch][k] = float(s.coeffs[ch][k]) * gain;
        }
        if (s.spxInUse) spectralExtension();
        if (different) {
            if (c.downmixed) {
                c.downmixed = 0;
                upmixDelay();
            }
            inverseTransform(s.channels, offset);
        } else {
            inverseTransform(s.outChannels, offset);
        }
        return 0;
    }

    int eac3FrameInfo() {
        if (s.frameType == kReservedType) return kParseType;
        if (s.substreamId) return kParseType;
        if (s.allocation.srCode == 3) return kHardError;
        int ac3Strategy, ahtInfo;
        if (s.numBlocks == 6) {
            ac3Strategy = r.bit();
            ahtInfo = r.bit();
        } else {
            ac3Strategy = 1;
            ahtInfo = 0;
        }
        s.snrOffsetStrategy = int(r.read(2));
        int transientInfo = r.bit();
        s.blockSwitchSyntax = r.bit();
        if (!s.blockSwitchSyntax) memset(s.blockSwitch, 0, sizeof s.blockSwitch);
        s.ditherFlagSyntax = r.bit();
        if (!s.ditherFlagSyntax) {
            for (int ch = 1; ch <= s.fullChannels; ch++) s.ditherFlag[ch] = 1;
        }
        s.ditherFlag[kCoupling] = s.ditherFlag[s.lfeChannel] = 0;
        s.bitAllocationSyntax = r.bit();
        if (!s.bitAllocationSyntax) {
            s.allocation.slowDecay = kSlowDecay[2];
            s.allocation.fastDecay = kFastDecay[1];
            s.allocation.slowGain = kSlowGain[1];
            s.allocation.dbPerBit = kDbPerBit[2];
            s.allocation.floor = kFloors[7];
        }
        s.fastGainSyntax = r.bit();
        s.dbaSyntax = r.bit();
        s.skipSyntax = r.bit();
        int spxAttenuationInfo = r.bit();
        int couplingBlocks = 0;
        if (s.channelMode > 1) {
            for (int blk = 0; blk < s.numBlocks; blk++) {
                s.couplingStrategyExists[blk] = (!blk || r.bit());
                if (s.couplingStrategyExists[blk]) s.couplingInUse[blk] = r.bit();
                else s.couplingInUse[blk] = s.couplingInUse[blk - 1];
                couplingBlocks += s.couplingInUse[blk];
            }
        } else {
            memset(s.couplingInUse, 0, sizeof s.couplingInUse);
        }
        if (ac3Strategy) {
            for (int blk = 0; blk < s.numBlocks; blk++)
                for (int ch = !s.couplingInUse[blk]; ch <= s.fullChannels; ch++) s.exponentStrategy[blk][ch] = int(r.read(2));
        } else {
            for (int ch = !((s.channelMode > 1) && couplingBlocks); ch <= s.fullChannels; ch++) {
                int combined = int(r.read(5));
                for (int blk = 0; blk < 6; blk++) s.exponentStrategy[blk][ch] = kFrameExponentStrategies[combined][blk];
            }
        }
        if (s.lfeOn) {
            for (int blk = 0; blk < s.numBlocks; blk++) s.exponentStrategy[blk][s.lfeChannel] = r.bit();
        }
        if (s.frameType == kIndependent && (s.numBlocks == 6 || r.bit())) r.skip(5 * s.fullChannels);
        if (ahtInfo) {
            s.channelUsesAht[kCoupling] = 0;
            for (int ch = (couplingBlocks != 6); ch <= s.channels; ch++) {
                bool use = true;
                for (int blk = 1; blk < 6; blk++) {
                    if (s.exponentStrategy[blk][ch] != kReuse || (!ch && s.couplingStrategyExists[blk])) {
                        use = false;
                        break;
                    }
                }
                s.channelUsesAht[ch] = use && r.bit();
            }
        } else {
            memset(s.channelUsesAht, 0, sizeof s.channelUsesAht);
        }
        if (!s.snrOffsetStrategy) {
            int coarse = (int(r.read(6)) - 15) << 4;
            int snr = (coarse + int(r.read(4))) << 2;
            for (int ch = 0; ch <= s.channels; ch++) s.snrOffset[ch] = snr;
        }
        if (transientInfo) {
            for (int ch = 1; ch <= s.fullChannels; ch++)
                if (r.bit()) r.skip(18);
        }
        for (int ch = 1; ch <= s.fullChannels; ch++) {
            if (spxAttenuationInfo && r.bit()) s.spxAttenuation[ch] = int8_t(r.read(5));
            else s.spxAttenuation[ch] = -1;
        }
        if (s.numBlocks > 1 && r.bit()) r.skip(int64_t(s.numBlocks - 1) * (4 + std::bit_width(unsigned(std::max(1, s.frameSize - 2))) - 1));
        for (int ch = 1; ch <= s.fullChannels; ch++) {
            s.firstSpxCoords[ch] = 1;
            s.firstCouplingCoords[ch] = 1;
        }
        s.firstCouplingLeak = 1;
        return 0;
    }

    int frameHeader() {
        Header h;
        int err = parseHeader(r, h);
        if (err) return err;
        s.allocation.srCode = h.srCode;
        s.bitstreamId = h.bitstreamId;
        s.channelMode = h.channelMode;
        s.lfeOn = h.lfeOn;
        s.allocation.srShift = h.srShift;
        s.sampleRate = h.sampleRate;
        s.channels = h.channels;
        s.fullChannels = s.channels - s.lfeOn;
        s.lfeChannel = s.fullChannels + 1;
        s.frameSize = h.frameSize;
        s.superframeSize += h.frameSize;
        s.numBlocks = h.numBlocks;
        s.frameType = h.frameType;
        s.substreamId = h.substreamId;
        if (s.lfeOn) {
            s.startFrequency[s.lfeChannel] = 0;
            s.endFrequency[s.lfeChannel] = 7;
            s.exponentGroups[s.lfeChannel] = 2;
            s.channelInCoupling[s.lfeChannel] = 0;
        }
        if (s.bitstreamId <= 10) {
            s.eac3 = 0;
            s.snrOffsetStrategy = 2;
            s.blockSwitchSyntax = 1;
            s.ditherFlagSyntax = 1;
            s.bitAllocationSyntax = 1;
            s.fastGainSyntax = 0;
            s.firstCouplingLeak = 0;
            s.dbaSyntax = 1;
            s.skipSyntax = 1;
            memset(s.channelUsesAht, 0, sizeof s.channelUsesAht);
            for (int i = 0; i < (s.channelMode ? 1 : 2); i++) s.compressionExists[i] = h.compressionExists[i];
            return 0;
        }
        s.eac3 = 1;
        s.channelMap = h.channelMap;
        return eac3FrameInfo();
    }

    int frame(const uint8_t* data, int fullSize, std::vector<float>* out) {
        s.superframeSize = 0;
        int bufSize = fullSize;
        int sync = findSync(data, bufSize);
        if (sync < 0) return -1;
        if (sync > 10) return sync;
        const uint8_t* buf = data + sync;
        bufSize -= sync;
        if (bufSize >= 2 && buf[0] == 0x77 && buf[1] == 0x0B) {
            int words = std::min(bufSize, kInputSize) >> 1;
            for (int k = 0; k < words; ++k) {
                swapped[2 * k] = buf[2 * k + 1];
                swapped[2 * k + 1] = buf[2 * k];
            }
            buf = swapped;
        }
        int skip = 0;
        bool gotIndependent = false;
        int channelMapBits = 0;
        const uint8_t* channelMap = nullptr;
        int offset = 0;
        int err = 0;
        for (;;) {
            r = BitReader(buf, size_t(std::min(bufSize, kInputSize)));
            err = frameHeader();
            if (err) {
                switch (err) {
                    case kParseSync: return -1;
                    case kParseType:
                        if (s.substreamId) return bufSize;
                        break;
                    case kParseMap: return -1;
                    case kParseBsid:
                    case kParseRate:
                    case kParseSize: break;
                    default: return -1;
                }
            } else if (s.frameSize > bufSize) {
                err = kParseSize;
            }
            if (s.frameType == kDependent && !gotIndependent) return std::min(fullSize, s.frameSize);
            if (!err || (s.channels && s.outChannels != s.channels)) {
                s.outChannels = s.channels;
                s.outputMode = s.channelMode;
                if (s.lfeOn) s.outputMode |= kLfeOn;
            } else if (!s.channels) {
                return -1;
            }
            channelMap = kChannelMap[s.outputMode & ~kLfeOn][s.lfeOn];
            channelMapBits = s.channelMap;
            offset = s.frameType == kDependent ? kMaxChannels : 0;
            float* previous[kMaxChannels];
            for (int ch = 0; ch < kMaxChannels; ch++) {
                previous[ch] = s.output[ch + offset];
                outptr[ch] = s.output[ch + offset];
            }
            for (int ch = 0; ch < s.channels; ch++)
                if (ch < s.outChannels) outptr[channelMap[ch]] = s.outputBuffer[ch + offset];
            for (int blk = 0; blk < s.numBlocks; blk++) {
                if (!err && audioBlock(blk, offset)) err = 1;
                if (err)
                    for (int ch = 0; ch < s.outChannels; ch++)
                        memcpy(s.outputBuffer[ch + offset] + kBlockSize * blk, previous[ch], kBlockSize * sizeof(float));
                for (int ch = 0; ch < s.outChannels; ch++) previous[ch] = outptr[channelMap[ch]];
                for (int ch = 0; ch < s.outChannels; ch++)
                    if (!ch || channelMap[ch]) outptr[channelMap[ch]] += kBlockSize;
            }
            for (int ch = 0; ch < s.outChannels; ch++) memcpy(s.output[ch + offset], previous[ch], kBlockSize * sizeof(float));
            if (bufSize > s.frameSize) {
                if (bufSize - s.frameSize <= 16) {
                    skip = bufSize - s.frameSize;
                    break;
                }
                r = BitReader(buf + s.frameSize, size_t(std::min(bufSize - s.frameSize, kInputSize)));
                Header next;
                if (parseHeader(r, next)) return -1;
                if (next.frameType == kDependent && next.numBlocks == s.numBlocks && s.sampleRate == next.sampleRate) {
                    buf += s.frameSize;
                    bufSize -= s.frameSize;
                    s.previousOutputMode = s.outputMode;
                    gotIndependent = true;
                    continue;
                }
            }
            break;
        }
        int extended[kAllChannels];
        for (int ch = 0; ch < kAllChannels; ch++) extended[ch] = ch;
        uint64_t layout = kLayouts[s.outputMode & ~kLfeOn] | ((s.outputMode & kLfeOn) ? kLowFrequency : 0);
        if (s.frameType == kDependent) {
            uint64_t combined = kLayouts[s.previousOutputMode & ~kLfeOn] | ((s.previousOutputMode & kLfeOn) ? kLowFrequency : 0);
            const int mapSize = kChannels[s.outputMode & ~kLfeOn] + s.lfeOn;
            int extend = 0;
            for (int ch = 0; ch < 16; ch++)
                if (channelMapBits & (1 << (kAllChannels - ch - 1))) combined |= kCustomChannels[ch][1];
            if (std::popcount(combined) > kAllChannels) return -1;
            layout = combined;
            auto indexOf = [&](int bit) { return (layout >> bit) & 1 ? std::popcount(layout & ((uint64_t(1) << bit) - 1)) : -1; };
            for (int ch = 0; ch < kAllChannels; ch++) {
                if (!(channelMapBits & (1 << (kAllChannels - ch - 1)))) continue;
                if (kCustomChannels[ch][0]) {
                    int index = indexOf(std::countr_zero(kCustomChannels[ch][1]));
                    if (index < 0) return -1;
                    if (extend >= mapSize) break;
                    extended[index] = offset + channelMap[extend++];
                } else {
                    for (int bit = 0; bit < 64; bit++) {
                        if (!((uint64_t(1) << bit) & kCustomChannels[ch][1])) continue;
                        int index = indexOf(bit);
                        if (index < 0) return -1;
                        if (extend >= mapSize) break;
                        extended[index] = offset + channelMap[extend++];
                    }
                }
            }
        }
        lastMask = layout;
        memcpy(lastMap, extended, sizeof extended);
        lastFrames = s.numBlocks * kBlockSize;
        produced += lastFrames;
        if (out) emit(*out);
        return std::min(fullSize, (s.superframeSize ? s.superframeSize : s.frameSize) + skip);
    }

    const float* sourceOf(uint64_t bit) const {
        if (!(lastMask & bit)) return nullptr;
        int index = std::popcount(lastMask & (bit - 1));
        return s.outputBuffer[lastMap[index]];
    }

    void emit(std::vector<float>& out) {
        const size_t frames = size_t(lastFrames);
        const size_t count = size_t(channels);
        const size_t at = out.size();
        out.resize(at + frames * count);
        float* to = out.data() + at;
        for (size_t slot = 0; slot < count; ++slot) {
            const float* from = sourceOf(want[slot]);
            float gain = 1.0f;
            if (!from && fold && slot >= 4) {
                from = sourceOf(want[slot] == kSideLeft ? kBackLeft : kBackRight);
                if (!from) {
                    from = sourceOf(kBackCenter);
                    gain = float(M_SQRT1_2);
                }
            }
            float* dst = to + slot;
            if (!from) {
                for (size_t i = 0; i < frames; ++i) dst[i * count] = 0.0f;
            } else if (gain == 1.0f) {
                for (size_t i = 0; i < frames; ++i) dst[i * count] = from[i];
            } else {
                for (size_t i = 0; i < frames; ++i) dst[i * count] = from[i] * gain;
            }
        }
    }

    void decode(const uint8_t* data, size_t size, std::vector<float>* out) {
        size_t at = 0;
        while (at < size) {
            int used = frame(data + at, int(std::min<size_t>(size - at, INT_MAX)), out);
            if (used <= 0) break;
            at += size_t(used);
        }
    }

    void feed(const std::vector<uint8_t>& packet, std::vector<float>& out) override { decode(packet.data(), packet.size(), &out); }
    void skim(const std::vector<uint8_t>& packet) override { decode(packet.data(), packet.size(), nullptr); }

    void choose(uint64_t mask) {
        static const uint64_t layouts[8][8] = {
            {kFrontCenter},
            {kFrontLeft, kFrontRight},
            {kFrontLeft, kFrontRight, kFrontCenter},
            {kFrontLeft, kFrontRight, kFrontCenter, kBackCenter},
            {kFrontLeft, kFrontRight, kFrontCenter, kSideLeft, kSideRight},
            {kFrontLeft, kFrontRight, kFrontCenter, kLowFrequency, kSideLeft, kSideRight},
            {kFrontLeft, kFrontRight, kFrontCenter, kLowFrequency, kBackCenter, kSideLeft, kSideRight},
            {kFrontLeft, kFrontRight, kFrontCenter, kLowFrequency, kBackLeft, kBackRight, kSideLeft, kSideRight},
        };
        int count = 6;
        if (mask == kFrontCenter) count = 1;
        else if (mask == (kFrontLeft | kFrontRight)) count = 2;
        else if (mask == (kFrontLeft | kFrontRight | kFrontCenter)) count = 3;
        else if (mask == (kFrontLeft | kFrontRight | kFrontCenter | kBackCenter)) count = 4;
        else if (mask == (kFrontLeft | kFrontRight | kFrontCenter | kSideLeft | kSideRight)) count = 5;
        else if (mask & (kBackLeft | kBackRight)) count = 8;
        else if ((mask & kBackCenter) && (mask & (kSideLeft | kSideRight))) count = 7;
        channels = count;
        fold = count == 6;
        memcpy(want, layouts[count - 1], sizeof want);
    }
};

}

std::unique_ptr<PacketDecoder> openAc3Audio(const Track&, const std::vector<uint8_t>& firstPacket, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    DolbyFrame head;
    if (firstPacket.size() < 8 || !dolbyFrame(firstPacket.data(), head)) return fail("AC-3 frame header not understood");
    if (head.codec == fourcc("ec-3") && head.rate < 32000) return fail("E-AC-3 at a reduced sample rate is not supported");
    auto audio = std::make_unique<Ac3Audio>();
    audio->reset();
    audio->decode(firstPacket.data(), firstPacket.size(), nullptr);
    uint64_t mask = audio->lastMask;
    if (audio->produced) {
        audio->rate = audio->s.sampleRate;
        audio->packetFrames = audio->produced;
    } else {
        mask = kLayouts[head.mode] | (head.lfe ? kLowFrequency : 0);
        audio->rate = head.rate;
        audio->packetFrames = int(head.samples);
    }
    audio->choose(mask);
    audio->reset();
    audio->prerollPackets = 1;
    return audio;
}

std::vector<uint8_t> dolbySpecific(uint32_t codec, const uint8_t* packet, size_t size) {
    std::vector<uint8_t> out;
    uint64_t pending = 0;
    int count = 0;
    auto put = [&](uint32_t value, int bits) {
        pending = (pending << bits) | (value & ((1u << bits) - 1));
        count += bits;
        for (; count >= 8; count -= 8) out.push_back(uint8_t(pending >> (count - 8)));
    };
    Header h;
    BitReader first(packet, size);
    if (size < 8 || parseHeader(first, h) || h.frameSize > int(size)) return {};
    if (codec == fourcc("ac-3")) {
        if (h.bitstreamId > 8) return {};
        put(uint32_t(h.srCode), 2);
        put(uint32_t(h.bitstreamId), 5);
        put(uint32_t(h.bitstreamMode), 3);
        put(uint32_t(h.channelMode), 3);
        put(uint32_t(h.lfeOn), 1);
        put(uint32_t(h.bitRateCode), 5);
        put(0, 5);
        return out;
    }
    if (h.frameType == kDependent) return {};
    const Header main = h;
    int dependents = 0, places = 0;
    for (size_t at = size_t(main.frameSize); at + 8 <= size;) {
        BitReader next(packet + at, size - at);
        if (parseHeader(next, h) || h.frameType != kDependent || h.frameSize > int(size - at)) break;
        ++dependents;
        places |= h.channelMap ? (h.channelMap >> 5) & 0x1F : h.channelMode;
        at += size_t(h.frameSize);
    }
    put(uint32_t(int64_t(main.frameSize) * 8 * main.sampleRate / (main.numBlocks * 256) / 1000), 13);
    put(0, 3);
    put(uint32_t(main.srCode), 2);
    put(uint32_t(main.bitstreamId), 5);
    put(0, 2);
    put(uint32_t(main.bitstreamMode), 3);
    put(uint32_t(main.channelMode), 3);
    put(uint32_t(main.lfeOn), 1);
    put(0, 3);
    put(uint32_t(dependents), 4);
    if (dependents) put(uint32_t(places), 9);
    else put(0, 1);
    if (main.complexity) {
        put(1, 8);
        put(uint32_t(main.complexity), 8);
    }
    if (count) put(0, 8 - count);
    return out;
}

}

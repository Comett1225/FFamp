#include "aac.h"
#include "aac_config.h"
#include "audio_codec.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <mutex>

namespace absent {

namespace {

using namespace aac;

const int kMaxElemId = 16;
const int kMaxChannels = 64;
const int kOutputChannels = 8;

enum WindowSequence { kOnlyLong = 0, kLongStart, kEightShort, kLongStop };
enum BandType { kZeroBt = 0, kFirstPairBt = 5, kEscBt = 11, kReservedBt = 12, kNoiseBt = 13, kIntensityBt2 = 14, kIntensityBt = 15 };
enum ChannelPosition { kChannelOff = 0, kChannelFront, kChannelSide, kChannelBack, kChannelLfe, kChannelCc };
enum OcStatus { kOcNone = 0, kOcTrialPce, kOcTrialFrame, kOcGlobalHdr, kOcLocked };
enum { kAotMain = 1, kAotLc = 2, kAotSsr = 3, kAotLtp = 4, kAotSbr = 5, kAotErLc = 17, kAotPs = 29, kAotEscape = 31 };
enum ExtensionType { kExtFill = 0, kExtFillData = 1, kExtDataElement = 2, kExtDynamicRange = 0xb, kExtSbrData = 0xd, kExtSbrDataCrc = 0xe };

const int kNone = -1;
const int kUnused = 0x200;
const int kFL = 0, kFR = 1, kFC = 2, kLFE = 3, kBL = 4, kBR = 5, kFLC = 6, kFRC = 7, kBC = 8, kSL = 9, kSR = 10, kTC = 11, kTFL = 12, kTFC = 13,
          kTFR = 14, kTBL = 15, kTBC = 16, kTBR = 17, kLFE2 = 35, kTSL = 36, kTSR = 37, kBFC = 38, kBFL = 39, kBFR = 40;

const int16_t kChannelMap[3][4][6] = {
    {{kFC, kFLC, kFRC, kFL, kFR, kNone}, {kUnused, kSL, kSR, kNone, kNone, kNone}, {kUnused, kSL, kSR, kBL, kBR, kBC}, {kLFE, kLFE2, kNone, kNone, kNone, kNone}},
    {{kTFC, kNone, kNone, kTFL, kTFR, kNone}, {kUnused, kTSL, kTSR, kNone, kNone, kTC}, {kUnused, kNone, kNone, kTBL, kTBR, kTBC}, {kNone, kNone, kNone, kNone, kNone, kNone}},
    {{kBFC, kNone, kNone, kBFL, kBFR, kNone}, {kNone, kNone, kNone, kNone, kNone, kNone}, {kNone, kNone, kNone, kNone, kNone, kNone}, {kNone, kNone, kNone, kNone, kNone, kNone}},
};

const uint8_t kLayoutMap[16][16][3] = {
    {{kSce, 0, kChannelFront}},
    {{kCpe, 0, kChannelFront}},
    {{kSce, 0, kChannelFront}, {kCpe, 0, kChannelFront}},
    {{kSce, 0, kChannelFront}, {kCpe, 0, kChannelFront}, {kSce, 1, kChannelBack}},
    {{kSce, 0, kChannelFront}, {kCpe, 0, kChannelFront}, {kCpe, 1, kChannelBack}},
    {{kSce, 0, kChannelFront}, {kCpe, 0, kChannelFront}, {kCpe, 1, kChannelBack}, {kLfe, 0, kChannelLfe}},
    {{kSce, 0, kChannelFront}, {kCpe, 0, kChannelFront}, {kCpe, 1, kChannelFront}, {kCpe, 2, kChannelBack}, {kLfe, 0, kChannelLfe}},
    {},
    {},
    {},
    {{kSce, 0, kChannelFront}, {kCpe, 0, kChannelFront}, {kCpe, 1, kChannelBack}, {kSce, 1, kChannelBack}, {kLfe, 0, kChannelLfe}},
    {{kSce, 0, kChannelFront}, {kCpe, 0, kChannelFront}, {kCpe, 1, kChannelBack}, {kCpe, 2, kChannelBack}, {kLfe, 0, kChannelLfe}},
    {{kSce, 0, kChannelFront},
     {kCpe, 0, kChannelFront},
     {kCpe, 1, kChannelFront},
     {kCpe, 2, kChannelBack},
     {kCpe, 3, kChannelBack},
     {kSce, 1, kChannelBack},
     {kLfe, 0, kChannelLfe},
     {kLfe, 1, kChannelLfe},
     {kSce, 2, kChannelFront},
     {kCpe, 4, kChannelFront},
     {kCpe, 5, kChannelSide},
     {kSce, 3, kChannelSide},
     {kCpe, 6, kChannelBack},
     {kSce, 4, kChannelBack},
     {kSce, 5, kChannelFront},
     {kCpe, 7, kChannelFront}},
    {{kSce, 0, kChannelFront}, {kCpe, 0, kChannelFront}, {kCpe, 1, kChannelBack}, {kLfe, 0, kChannelLfe}, {kCpe, 2, kChannelFront}},
    {},
};

const int kSampleRates[16] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350, 0, 0, 0};

struct Shared {
    Vlc spectral[11];
    Vlc scalefactor;
    uint32_t cbrt[1 << 13];
    float pow2sf[428];
    alignas(32) float kbdLong[1024];
    alignas(32) float kbdShort[128];
};

void cbrtTable(uint32_t* out) {
    const int lut = 1 << 13, tmpSize = lut / 2;
    std::vector<double> tmp(size_t(tmpSize), 1.0);
    for (int idx = 1; idx < 45; ++idx) {
        if (tmp[size_t(idx)] == 1) {
            int i = 2 * idx + 1;
            double v = i * cbrt(double(i));
            for (int k = i; k < lut; k *= i)
                for (int idx2 = k >> 1; idx2 < tmpSize; idx2 += k) tmp[size_t(idx2)] *= v;
        }
    }
    for (int idx = 45; idx < tmpSize; ++idx) {
        if (tmp[size_t(idx)] == 1) {
            int i = 2 * idx + 1;
            double v = i * cbrt(double(i));
            for (int idx2 = idx; idx2 < tmpSize; idx2 += i) tmp[size_t(idx2)] *= v;
        }
    }
    double cbrt2 = 2 * cbrt(2.0);
    for (int idx = tmpSize - 1; idx >= 0; --idx) {
        double v = tmp[size_t(idx)];
        for (int i = 2 * idx + 1; i < lut; i *= 2) {
            out[i] = std::bit_cast<uint32_t>(float(v));
            v *= cbrt2;
        }
    }
    out[0] = 0;
}

void pow2Table(float* pow2sf) {
    static const float exp2Lut[] = {1.00000000000000000000f, 1.04427378242741384032f, 1.09050773266525765921f, 1.13878863475669165370f,
                                    1.18920711500272106672f, 1.24185781207348404859f, 1.29683955465100966593f, 1.35425554693689272830f,
                                    1.41421356237309504880f, 1.47682614593949931139f, 1.54221082540794082361f, 1.61049033194925430818f,
                                    1.68179283050742908606f, 1.75625216037329948311f, 1.83400808640934246349f, 1.91520656139714729387f};
    float t1 = 8.8817841970012523233890533447265625e-16f;
    int t1IncPrev = 0;
    for (int i = 0; i < 428; i++) {
        int t1IncCur = 4 * (i % 4);
        if (t1IncCur < t1IncPrev) t1 *= 2;
        pow2sf[i] = t1 * exp2Lut[t1IncCur];
        t1IncPrev = t1IncCur;
    }
}

const Shared& shared() {
    static Shared* made = nullptr;
    static std::once_flag once;
    std::call_once(once, [] {
        auto* s = new Shared();
        for (int i = 0; i < 11; i++) {
            const int n = kSpectralSizes[i];
            std::vector<uint32_t> codes(kSpectralSizes[i]);
            std::vector<int32_t> symbols(kSpectralSizes[i]);
            for (int j = 0; j < n; j++) {
                codes[size_t(j)] = kSpectralCodes[i][j];
                symbols[size_t(j)] = kVectorIndex[i][j];
            }
            s->spectral[i].build(8, size_t(n), kSpectralBits[i], codes.data(), symbols.data());
        }
        s->scalefactor.build(7, 121, kScalefactorBits, kScalefactorCode);
        cbrtTable(s->cbrt);
        pow2Table(s->pow2sf);
        kbdWindow(s->kbdLong, 4.0f, 1024);
        kbdWindow(s->kbdShort, 6.0f, 128);
        sineWindow(10);
        made = s;
    });
    return *made;
}

struct Ics {
    uint8_t maxSfb;
    int windowSequence[2];
    uint8_t useKbWindow[2];
    int numWindowGroups, prevNumWindowGroups;
    uint8_t groupLen[8];
    const uint16_t* swbOffset;
    int numSwb, numWindows, tnsMaxBands, predictorPresent;
};

struct Tns {
    int present;
    int nFilt[8];
    int length[8][4], direction[8][4], order[8][4];
    float coef[8][4][20];
};

struct Pulse {
    int numPulse, start, pos[4], amp[4];
};

struct Sce {
    Ics ics;
    Tns tns;
    int bandType[128];
    int sfo[128];
    float sf[128];
    alignas(32) float coeffs[1024];
    alignas(32) float saved[1536];
    alignas(32) float out[2048];
};

struct Element {
    int present;
    uint8_t maxSfbSte;
    uint8_t msMask[128];
    Sce ch[2];
    int sbrUsed;
    Sbr sbr;
};

struct OutputConfig {
    StreamConfig m4ac;
    uint8_t layoutMap[kMaxElemId * 4][3];
    int layoutMapTags;
    int channels;
    uint64_t layout;
    int status;
};

float horizontal(const float* x) { return (x[0] + x[2]) + (x[1] + x[3]); }

float scalarProduct(const float* a, const float* b, int size) {
    float x[8] = {};
    int at = 0;
    auto quads = [&](int count) {
        for (; count > 0; count -= 4, at += 4)
            for (int k = 0; k < 4; k++) x[k] += a[at + k] * b[at + k];
    };
    if (!fastFma()) {
        quads(size & ~3);
        return horizontal(x);
    }
    float y[8] = {}, z[8] = {}, w[8] = {};
    auto octets = [&](float* acc, int from) {
        for (int k = 0; k < 8; k++) acc[k] = fmaHardware(a[from + k], b[from + k], acc[k]);
    };
    auto fold = [](float* acc, const float* other) {
        for (int k = 0; k < 8; k++) acc[k] = acc[k] + other[k];
    };
    auto halves = [&] {
        for (int k = 0; k < 4; k++) x[k] = x[k] + x[k + 4];
    };
    int rest = size;
    if (size >= 32) {
        for (int end = size & ~31; at < end; at += 32) {
            octets(x, at);
            octets(y, at + 8);
            octets(z, at + 16);
            octets(w, at + 24);
        }
        fold(x, z);
        fold(y, w);
        rest = size & 31;
        if (rest < 16) {
            fold(x, y);
            if (rest < 8) {
                halves();
                quads(rest & ~3);
                return horizontal(x);
            }
        }
    }
    if (size >= 16 && rest >= 16) {
        for (int end = at + (rest & ~15); at < end; at += 16) {
            octets(x, at);
            octets(y, at + 8);
        }
        fold(x, y);
        rest &= 15;
        if (rest < 8) {
            halves();
            quads(rest & ~3);
            return horizontal(x);
        }
    }
    if (rest >= 8) {
        for (int end = at + (rest & ~7); at < end; at += 8) octets(x, at);
        rest &= 7;
    }
    halves();
    quads(rest & ~3);
    return horizontal(x);
}

uint32_t bitsOf(float f) { return std::bit_cast<uint32_t>(f); }
float floatOf(uint32_t u) { return std::bit_cast<float>(u); }

int lcgRandom(unsigned previous) { return int(previous * 1664525u + 1013904223u); }

int objectTypeOf(BitReader& gb) {
    int type = int(gb.read(5));
    if (type == kAotEscape) type = 32 + int(gb.read(6));
    return type;
}

int sampleRateOf(BitReader& gb, int& index) {
    index = int(gb.read(4));
    return index == 0x0f ? int(gb.read(24)) : kSampleRates[index];
}

int64_t mpeg4Config(StreamConfig& c, BitReader& gb) {
    int64_t start = gb.position();
    c.objectType = objectTypeOf(gb);
    c.sampleRate = sampleRateOf(gb, c.samplingIndex);
    c.channelConfig = int(gb.read(4));
    if (c.channelConfig >= 15) return -1;
    c.sbr = -1;
    c.ps = -1;
    int extObjectType = 0, extIndex = 0;
    if (c.objectType == kAotSbr || (c.objectType == kAotPs && !((gb.peek(3) & 0x03) && !(gb.peek(9) & 0x3F)))) {
        if (c.objectType == kAotPs) c.ps = 1;
        extObjectType = kAotSbr;
        c.sbr = 1;
        c.extSampleRate = sampleRateOf(gb, extIndex);
        c.objectType = objectTypeOf(gb);
        if (c.objectType == 22) gb.skip(4);
    } else {
        c.extSampleRate = 0;
    }
    int64_t specific = gb.position();
    switch (c.objectType) {
        case kAotMain:
        case kAotLc:
        case kAotSsr:
        case kAotLtp:
        case kAotErLc:
            c.frameLengthShort = gb.bit();
            break;
        default:
            break;
    }
    if (extObjectType != kAotSbr) {
        while (gb.left() > 15) {
            if (gb.peek(11) == 0x2b7) {
                gb.skip(11);
                extObjectType = objectTypeOf(gb);
                if (extObjectType == kAotSbr && (c.sbr = gb.bit()) == 1) {
                    c.extSampleRate = sampleRateOf(gb, extIndex);
                    if (c.extSampleRate == c.sampleRate) c.sbr = -1;
                }
                if (gb.left() > 11 && gb.read(11) == 0x548) c.ps = gb.bit();
                break;
            }
            gb.skip(1);
        }
    }
    return specific - start;
}

int countChannels(const uint8_t (*layout)[3], int tags) {
    int sum = 0;
    for (int i = 0; i < tags; i++) {
        int synEle = layout[i][0], pos = layout[i][2];
        sum += (1 + (synEle == kCpe)) * (pos != kChannelOff && pos != kChannelCc);
    }
    return sum;
}

struct ElemToChannel {
    uint64_t avPosition;
    uint8_t synEle, elemId, aacPosition;
};

int assignPair(ElemToChannel* e2c, uint8_t (*layoutMap)[3], int offset, uint64_t left, uint64_t right, int pos, uint64_t* layout) {
    if (layoutMap[offset][0] == kCpe) {
        e2c[offset] = {left | right, kCpe, layoutMap[offset][1], uint8_t(pos)};
        if (e2c[offset].avPosition != UINT64_MAX) *layout |= e2c[offset].avPosition;
        return 1;
    }
    e2c[offset] = {left, kSce, layoutMap[offset][1], uint8_t(pos)};
    e2c[offset + 1] = {right, kSce, layoutMap[offset + 1][1], uint8_t(pos)};
    if (left != UINT64_MAX) *layout |= left;
    if (right != UINT64_MAX) *layout |= right;
    return 2;
}

int countPairedChannels(uint8_t (*layoutMap)[3], int tags, int pos, int current) {
    int num = 0, firstCpe = 0, sceParity = 0;
    for (int i = current; i < tags; i++) {
        if (layoutMap[i][2] != pos) break;
        if (layoutMap[i][0] == kCpe) {
            if (sceParity) {
                if (pos == kChannelFront && !firstCpe) sceParity = 0;
                else return -1;
            }
            num += 2;
            firstCpe = 1;
        } else {
            num++;
            sceParity ^= (pos != kChannelLfe);
        }
    }
    if (sceParity && (pos == kChannelFront && firstCpe)) return -1;
    return num;
}

uint64_t bitAt(int position) { return position >= 0 && position < 64 ? uint64_t(1) << position : 0; }

int assignChannels(ElemToChannel* e2c, uint8_t (*layoutMap)[3], uint64_t* layout, int tags, int layer, int pos, int* current) {
    int i = *current, j = 0;
    int nb = countPairedChannels(layoutMap, tags, pos, i);
    if (nb < 0 || nb > 5) return 0;
    const int16_t* map = kChannelMap[layer][pos - 1];
    if (pos == kChannelLfe) {
        while (nb) {
            if (map[j] == kNone) return -1;
            e2c[i] = {bitAt(map[j]), layoutMap[i][0], layoutMap[i][1], uint8_t(pos)};
            *layout |= e2c[i].avPosition;
            i++;
            j++;
            nb--;
        }
        *current = i;
        return 0;
    }
    while (nb & 1) {
        if (map[0] == kNone) return -1;
        if (map[0] == kUnused) break;
        e2c[i] = {bitAt(map[0]), layoutMap[i][0], layoutMap[i][1], uint8_t(pos)};
        *layout |= e2c[i].avPosition;
        i++;
        nb--;
    }
    j = (pos != kChannelSide) && nb <= 3 ? 3 : 1;
    while (nb >= 2) {
        if (map[j] == kNone || map[j + 1] == kNone) return -1;
        i += assignPair(e2c, layoutMap, i, bitAt(map[j]), bitAt(map[j + 1]), pos, layout);
        j += 2;
        nb -= 2;
    }
    while (nb & 1) {
        if (map[5] == kNone) return -1;
        e2c[i] = {bitAt(map[5]), layoutMap[i][0], layoutMap[i][1], uint8_t(pos)};
        *layout |= e2c[i].avPosition;
        i++;
        nb--;
    }
    if (nb) return -1;
    *current = i;
    return 0;
}

uint64_t sniffChannelOrder(uint8_t (*layoutMap)[3], int tags) {
    ElemToChannel e2c[4 * kMaxElemId] = {};
    uint64_t layout = 0;
    if (4 * kMaxElemId < tags) return 0;
    int i = 0, n;
    for (n = 0; n < 3 && i < tags; n++) {
        if (assignChannels(e2c, layoutMap, &layout, tags, n, kChannelFront, &i) < 0) return 0;
        if (assignChannels(e2c, layoutMap, &layout, tags, n, kChannelSide, &i) < 0) return 0;
        if (assignChannels(e2c, layoutMap, &layout, tags, n, kChannelBack, &i) < 0) return 0;
        if (assignChannels(e2c, layoutMap, &layout, tags, n, kChannelLfe, &i) < 0) return 0;
    }
    const int total = n = i;
    do {
        int next = 0;
        for (i = 1; i < n; i++) {
            if (e2c[i - 1].avPosition > e2c[i].avPosition) {
                std::swap(e2c[i - 1], e2c[i]);
                next = i;
            }
        }
        n = next;
    } while (n > 0);
    for (i = 0; i < total; i++) {
        layoutMap[i][0] = e2c[i].synEle;
        layoutMap[i][1] = e2c[i].elemId;
        layoutMap[i][2] = e2c[i].aacPosition;
    }
    return layout;
}

int defaultChannelConfig(uint8_t (*layoutMap)[3], int* tags, int channelConfig) {
    if (channelConfig < 1 || (channelConfig > 7 && channelConfig < 11) || channelConfig > 14) return -1;
    *tags = kTagsPerConfig[channelConfig];
    memcpy(layoutMap, kLayoutMap[channelConfig - 1], size_t(*tags) * sizeof *layoutMap);
    if (channelConfig == 7) layoutMap[2][2] = kChannelBack;
    return 0;
}

void decodeChannelMap(uint8_t (*layoutMap)[3], int type, BitReader& gb, int n) {
    while (n--) {
        int synEle;
        switch (type) {
            case kChannelFront:
            case kChannelBack:
            case kChannelSide: synEle = gb.bit(); break;
            case kChannelCc:
                gb.skip(1);
                synEle = kCce;
                break;
            default: synEle = kLfe; break;
        }
        layoutMap[0][0] = uint8_t(synEle);
        layoutMap[0][1] = uint8_t(gb.read(4));
        layoutMap[0][2] = uint8_t(type);
        layoutMap++;
    }
}

int decodePce(uint8_t (*layoutMap)[3], BitReader& gb, int64_t byteAlignRef) {
    gb.skip(2);
    gb.skip(4);
    int numFront = int(gb.read(4));
    int numSide = int(gb.read(4));
    int numBack = int(gb.read(4));
    int numLfe = int(gb.read(2));
    int numAssocData = int(gb.read(3));
    int numCc = int(gb.read(4));
    if (gb.bit()) gb.skip(4);
    if (gb.bit()) gb.skip(4);
    if (gb.bit()) gb.skip(3);
    if (gb.left() < 5 * (numFront + numSide + numBack + numCc) + 4 * (numLfe + numAssocData + numCc)) return -1;
    decodeChannelMap(layoutMap, kChannelFront, gb, numFront);
    int tags = numFront;
    decodeChannelMap(layoutMap + tags, kChannelSide, gb, numSide);
    tags += numSide;
    decodeChannelMap(layoutMap + tags, kChannelBack, gb, numBack);
    tags += numBack;
    decodeChannelMap(layoutMap + tags, kChannelLfe, gb, numLfe);
    tags += numLfe;
    gb.skip(4 * numAssocData);
    decodeChannelMap(layoutMap + tags, kChannelCc, gb, numCc);
    tags += numCc;
    int n = int((byteAlignRef - gb.position()) & 7);
    if (n) gb.skip(n);
    int commentLen = int(gb.read(8)) * 8;
    if (gb.left() < commentLen) return -1;
    int heightExt = 0;
    if (commentLen >= 16 + (numFront * 2 + numSide * 2 + numBack * 2)) heightExt = gb.peek(8) == 0xAC;
    if (heightExt) {
        uint8_t heightMap[4][16];
        uint8_t tag[6][16][3];
        int invalid = 0, heightTags = 0;
        gb.skip(8);
        const int counts[3] = {numFront, numSide, numBack};
        const int positions[3] = {kChannelFront, kChannelSide, kChannelBack};
        for (int p = 0; p < 3; p++) {
            for (int i = 0; i < counts[p]; i++) {
                int height = int(gb.read(2));
                invalid |= height > 2;
                heightMap[positions[p]][i] = uint8_t(height);
                memcpy(&tag[positions[p]][i], layoutMap + heightTags, sizeof *layoutMap);
                heightTags++;
            }
        }
        for (int i = 0; i < numLfe; i++) memcpy(&tag[kChannelLfe][i], layoutMap + heightTags++, sizeof *layoutMap);
        for (int i = 0; i < numCc; i++) memcpy(&tag[kChannelCc][i], layoutMap + heightTags++, sizeof *layoutMap);
        if (!invalid) {
            heightTags = 0;
            for (int h = 0; h < 3; h++) {
                for (int p = 0; p < 3; p++) {
                    for (int j = 0; j < counts[p]; j++) {
                        if (heightMap[positions[p]][j] == h) memcpy(layoutMap + heightTags++, &tag[positions[p]][j], sizeof *layoutMap);
                    }
                }
                if (h == 0) {
                    for (int j = 0; j < numLfe; j++) memcpy(layoutMap + heightTags++, &tag[kChannelLfe][j], sizeof *layoutMap);
                    for (int j = 0; j < numCc; j++) memcpy(layoutMap + heightTags++, &tag[kChannelCc][j], sizeof *layoutMap);
                }
            }
        }
        commentLen -= 8 + (numFront * 2 + numSide * 2 + numBack * 2);
    }
    gb.skip(commentLen);
    return tags;
}

struct Core {
    const Shared& t = shared();
    OutputConfig oc[2] = {};
    std::unique_ptr<Element> che[4][kMaxElemId];
    Element* tagCheMap[4][kMaxElemId] = {};
    int tagsMapped = 0;
    Sce* outputElement[kMaxChannels] = {};
    int randomState = 0x1f2e3d4c;
    PreciseMdct mdct1024{10, 1.0 / 1024 / 32768.0};
    PreciseMdct mdct128{7, 1.0 / 128 / 32768.0};
    double coefMdct[1024];
    double outMdct[1024];
    alignas(32) float bufMdct[1024];
    alignas(32) float temp[128];
    std::unique_ptr<SbrTools> sbrTools;
    const float* sine1024 = sineWindow(10);
    const float* sine128 = sineWindow(7);

    SbrTools& tools() {
        if (!sbrTools) sbrTools = std::make_unique<SbrTools>();
        return *sbrTools;
    }

    int cheConfigure(int position, int type, int id, int* channels) {
        if (*channels >= kMaxChannels) return -1;
        if (position) {
            if (!che[type][id]) {
                che[type][id].reset(new Element());
                sbrInit(che[type][id]->sbr, type);
            }
            if (type != kCce) {
                if (*channels >= kMaxChannels - (type == kCpe || (type == kSce && oc[1].m4ac.ps == 1))) return -1;
                outputElement[(*channels)++] = &che[type][id]->ch[0];
                if (type == kCpe || (type == kSce && oc[1].m4ac.ps == 1)) outputElement[(*channels)++] = &che[type][id]->ch[1];
            }
        } else {
            if (che[type][id]) {
                for (auto& row : tagCheMap)
                    for (Element*& e : row)
                        if (e == che[type][id].get()) e = nullptr;
            }
            che[type][id].reset();
            memset(outputElement, 0, sizeof outputElement);
        }
        return 0;
    }

    int outputConfigure(uint8_t (*layoutMap)[3], int tags, int ocType) {
        int channels = 0;
        uint64_t layout = 0;
        uint8_t idMap[kEnd][kMaxElemId] = {};
        uint8_t typeCounts[kEnd] = {};
        if (oc[1].layoutMap != layoutMap) {
            memcpy(oc[1].layoutMap, layoutMap, size_t(tags) * sizeof layoutMap[0]);
            oc[1].layoutMapTags = tags;
        }
        for (int i = 0; i < tags; i++) {
            int type = layoutMap[i][0], id = layoutMap[i][1];
            idMap[type][id] = typeCounts[type]++;
            if (idMap[type][id] >= kMaxElemId) return -1;
        }
        layout = sniffChannelOrder(layoutMap, tags);
        for (int i = 0; i < tags; i++) {
            int type = layoutMap[i][0], id = layoutMap[i][1], iid = idMap[type][id], position = layoutMap[i][2];
            if (cheConfigure(position, type, iid, &channels) < 0) return -1;
            tagCheMap[type][id] = che[type][iid].get();
        }
        if (oc[1].m4ac.ps == 1 && channels == 2) layout = layout == bitAt(kFC) ? (bitAt(kFL) | bitAt(kFR)) : 0;
        oc[1].layout = layout;
        oc[1].channels = channels;
        oc[1].status = ocType;
        return 0;
    }

    int pushOutputConfiguration() {
        int pushed = 0;
        if (oc[1].status == kOcLocked || oc[0].status == kOcNone) {
            oc[0] = oc[1];
            pushed = 1;
        }
        oc[1].status = kOcNone;
        return pushed;
    }

    void popOutputConfiguration() {
        if (oc[1].status != kOcLocked && oc[0].status != kOcNone) {
            oc[1] = oc[0];
            outputConfigure(oc[1].layoutMap, oc[1].layoutMapTags, oc[1].status);
        }
    }

    Element* getChe(int type, int elemId) {
        if (!oc[1].m4ac.channelConfig) return tagCheMap[type][elemId];
        if (!tagsMapped && type == kCpe && oc[1].m4ac.channelConfig == 1) {
            uint8_t layoutMap[kMaxElemId * 4][3];
            int tags;
            pushOutputConfiguration();
            if (defaultChannelConfig(layoutMap, &tags, 2) < 0) return nullptr;
            if (outputConfigure(layoutMap, tags, kOcTrialFrame) < 0) return nullptr;
            oc[1].m4ac.channelConfig = 2;
            oc[1].m4ac.ps = 0;
        }
        if (!tagsMapped && type == kSce && oc[1].m4ac.channelConfig == 2) {
            uint8_t layoutMap[kMaxElemId * 4][3];
            pushOutputConfiguration();
            layoutMap[0][0] = layoutMap[1][0] = kSce;
            layoutMap[0][2] = layoutMap[1][2] = kChannelFront;
            layoutMap[0][1] = 0;
            layoutMap[1][1] = 1;
            if (outputConfigure(layoutMap, 2, kOcTrialFrame) < 0) return nullptr;
            if (oc[1].m4ac.sbr) oc[1].m4ac.ps = -1;
        }
        const int cc = oc[1].m4ac.channelConfig;
        auto map = [&](int t, int id, Element* e) {
            tagsMapped++;
            return tagCheMap[t][id] = e;
        };
        switch (cc) {
            case 14:
                if (tagsMapped > 2 && ((type == kCpe && elemId < 3) || (type == kLfe && elemId < 1))) return map(type, elemId, che[type][elemId].get());
                [[fallthrough]];
            case 13:
                if (tagsMapped > 3 && ((type == kCpe && elemId < 8) || (type == kSce && elemId < 6) || (type == kLfe && elemId < 2)))
                    return map(type, elemId, che[type][elemId].get());
                [[fallthrough]];
            case 12:
            case 7:
                if (tagsMapped == 3 && type == kCpe) return map(kCpe, elemId, che[kCpe][2].get());
                [[fallthrough]];
            case 11:
                if (tagsMapped == 3 && type == kSce) return map(kSce, elemId, che[kSce][1].get());
                [[fallthrough]];
            case 6:
                if (tagsMapped == kTagsPerConfig[cc] - 1 && (type == kLfe || type == kSce)) return map(type, elemId, che[kLfe][0].get());
                [[fallthrough]];
            case 5:
                if (tagsMapped == 2 && type == kCpe) return map(kCpe, elemId, che[kCpe][1].get());
                [[fallthrough]];
            case 4:
                if (tagsMapped == kTagsPerConfig[cc] - 1 && (type == kLfe || type == kSce)) return map(type, elemId, che[kSce][1].get());
                if (tagsMapped == 2 && cc == 4 && type == kSce) return map(kSce, elemId, che[kSce][1].get());
                [[fallthrough]];
            case 3:
            case 2:
                if (tagsMapped == (cc != 2) && type == kCpe) return map(kCpe, elemId, che[kCpe][0].get());
                if (tagsMapped == 1 && cc == 2 && type == kSce) return map(kSce, elemId, che[kSce][1].get());
                [[fallthrough]];
            case 1:
                if (!tagsMapped && type == kSce) return map(kSce, elemId, che[kSce][0].get());
                [[fallthrough]];
            default: return nullptr;
        }
    }

    int decodeGaSpecificConfig(BitReader& gb, StreamConfig& m4ac, int channelConfig) {
        uint8_t layoutMap[kMaxElemId * 4][3];
        int tags = 0;
        m4ac.frameLengthShort = gb.bit();
        if (gb.bit()) gb.skip(14);
        int extensionFlag = gb.bit();
        if (channelConfig == 0) {
            gb.skip(4);
            tags = decodePce(layoutMap, gb, 0);
            if (tags < 0) return tags;
        } else if (defaultChannelConfig(layoutMap, &tags, channelConfig) < 0) {
            return -1;
        }
        if (countChannels(layoutMap, tags) > 1) m4ac.ps = 0;
        else if (m4ac.sbr == 1 && m4ac.ps == -1) m4ac.ps = 1;
        if (outputConfigure(layoutMap, tags, kOcGlobalHdr) < 0) return -1;
        if (extensionFlag) {
            if (m4ac.objectType == kAotErLc) {
                if (gb.read(3)) return -1;
            }
            gb.skip(1);
        }
        if (m4ac.objectType == kAotErLc && gb.read(2)) return -1;
        return 0;
    }

    int decodeAudioSpecificConfig(const uint8_t* data, size_t size) {
        BitReader gb(data, size);
        StreamConfig& m4ac = oc[1].m4ac;
        StreamConfig backup = m4ac;
        BitReader probe = gb;
        int64_t i = mpeg4Config(m4ac, probe);
        if (i < 0 || m4ac.samplingIndex > 12) {
            m4ac = backup;
            return -1;
        }
        gb.skip(i);
        switch (m4ac.objectType) {
            case kAotLc: return decodeGaSpecificConfig(gb, m4ac, m4ac.channelConfig);
            default: return -2;
        }
    }

    int decodeIcsInfo(Ics& ics, BitReader& gb) {
        const StreamConfig& m4ac = oc[1].m4ac;
        const int si = m4ac.samplingIndex;
        gb.skip(1);
        ics.windowSequence[1] = ics.windowSequence[0];
        ics.windowSequence[0] = int(gb.read(2));
        ics.useKbWindow[1] = ics.useKbWindow[0];
        ics.useKbWindow[0] = uint8_t(gb.bit());
        ics.prevNumWindowGroups = std::max(ics.numWindowGroups, 1);
        ics.numWindowGroups = 1;
        ics.groupLen[0] = 1;
        if (ics.windowSequence[0] == kEightShort) {
            ics.maxSfb = uint8_t(gb.read(4));
            for (int i = 0; i < 7; i++) {
                if (gb.bit()) {
                    ics.groupLen[ics.numWindowGroups - 1]++;
                } else {
                    ics.numWindowGroups++;
                    ics.groupLen[ics.numWindowGroups - 1] = 1;
                }
            }
            ics.numWindows = 8;
            ics.swbOffset = kSwbOffset128[si];
            ics.numSwb = kNumSwb128[si];
            ics.tnsMaxBands = kTnsMaxBands128[si];
            ics.predictorPresent = 0;
        } else {
            ics.maxSfb = uint8_t(gb.read(6));
            ics.numWindows = 1;
            ics.numSwb = kNumSwb1024[si];
            ics.swbOffset = kSwbOffset1024[si];
            ics.tnsMaxBands = kTnsMaxBands1024[si];
            ics.predictorPresent = gb.bit();
            if (ics.predictorPresent) {
                ics.maxSfb = 0;
                return -1;
            }
        }
        if (ics.maxSfb > ics.numSwb) {
            ics.maxSfb = 0;
            return -1;
        }
        return 0;
    }

    int decodeBandTypes(Sce& sce, BitReader& gb) {
        Ics& ics = sce.ics;
        const int bits = ics.windowSequence[0] == kEightShort ? 3 : 5;
        for (int g = 0; g < ics.numWindowGroups; g++) {
            int k = 0;
            while (k < ics.maxSfb) {
                int sectEnd = k;
                int sectLenIncr;
                int sectBandType = int(gb.read(4));
                if (sectBandType == 12) return -1;
                do {
                    sectLenIncr = int(gb.read(bits));
                    sectEnd += sectLenIncr;
                    if (gb.left() < 0) return -1;
                    if (sectEnd > ics.maxSfb) return -1;
                } while (sectLenIncr == (1 << bits) - 1);
                for (; k < sectEnd; k++) sce.bandType[g * ics.maxSfb + k] = sectBandType;
            }
        }
        return 0;
    }

    int decodeScalefactors(Sce& sce, BitReader& gb, int globalGain) {
        Ics& ics = sce.ics;
        int offset[3] = {globalGain, globalGain - 90, 0};
        int noiseFlag = 1;
        for (int g = 0; g < ics.numWindowGroups; g++) {
            for (int sfb = 0; sfb < ics.maxSfb; sfb++) {
                const int idx = g * ics.maxSfb + sfb;
                switch (sce.bandType[idx]) {
                    case kZeroBt: sce.sfo[idx] = 0; break;
                    case kIntensityBt:
                    case kIntensityBt2:
                        offset[2] += t.scalefactor.read(gb) - 60;
                        sce.sfo[idx] = std::clamp(offset[2], -155, 100) - 100;
                        break;
                    case kNoiseBt:
                        if (noiseFlag-- > 0) offset[1] += int(gb.read(9)) - 256;
                        else offset[1] += t.scalefactor.read(gb) - 60;
                        sce.sfo[idx] = std::clamp(offset[1], -100, 155);
                        break;
                    default:
                        offset[0] += t.scalefactor.read(gb) - 60;
                        if (unsigned(offset[0]) > 255u) return -1;
                        sce.sfo[idx] = offset[0] - 100;
                        break;
                }
            }
        }
        return 0;
    }

    void dequantScalefactors(Sce& sce) {
        Ics& ics = sce.ics;
        int idx = 0;
        for (int g = 0; g < ics.numWindowGroups; g++) {
            for (int sfb = 0; sfb < ics.maxSfb; sfb++, idx++) {
                switch (sce.bandType[g * ics.maxSfb + sfb]) {
                    case kZeroBt: sce.sf[idx] = 0.f; break;
                    case kIntensityBt:
                    case kIntensityBt2: sce.sf[idx] = t.pow2sf[-sce.sfo[idx] - 100 + 200]; break;
                    default: sce.sf[idx] = -t.pow2sf[sce.sfo[idx] + 200]; break;
                }
            }
        }
    }

    int decodePulses(Pulse& pulse, BitReader& gb, const uint16_t* swbOffset, int numSwb) {
        pulse.numPulse = int(gb.read(2)) + 1;
        int pulseSwb = int(gb.read(6));
        if (pulseSwb >= numSwb) return -1;
        pulse.pos[0] = swbOffset[pulseSwb];
        pulse.pos[0] += int(gb.read(5));
        if (pulse.pos[0] >= swbOffset[numSwb]) return -1;
        pulse.amp[0] = int(gb.read(4));
        for (int i = 1; i < pulse.numPulse; i++) {
            pulse.pos[i] = int(gb.read(5)) + pulse.pos[i - 1];
            if (pulse.pos[i] >= swbOffset[numSwb]) return -1;
            pulse.amp[i] = int(gb.read(4));
        }
        return 0;
    }

    int decodeTns(Tns& tns, BitReader& gb, const Ics& ics) {
        const int is8 = ics.windowSequence[0] == kEightShort;
        const int maxOrder = is8 ? 7 : 12;
        for (int w = 0; w < ics.numWindows; w++) {
            if ((tns.nFilt[w] = int(gb.read(2 - is8)))) {
                int coefRes = gb.bit();
                for (int filt = 0; filt < tns.nFilt[w]; filt++) {
                    tns.length[w][filt] = int(gb.read(6 - 2 * is8));
                    tns.order[w][filt] = int(gb.read(5 - 2 * is8));
                    if (tns.order[w][filt] > maxOrder) {
                        tns.order[w][filt] = 0;
                        return -1;
                    }
                    if (tns.order[w][filt]) {
                        tns.direction[w][filt] = gb.bit();
                        int coefCompress = gb.bit();
                        int coefLen = coefRes + 3 - coefCompress;
                        int tmp2Idx = 2 * coefCompress + coefRes;
                        for (int i = 0; i < tns.order[w][filt]; i++) tns.coef[w][filt][i] = kTnsMap[tmp2Idx][gb.read(coefLen)];
                    }
                }
            }
        }
        return 0;
    }

    void decodeMidSide(Element& cpe, BitReader& gb, int msPresent) {
        int maxIdx = cpe.ch[0].ics.numWindowGroups * cpe.ch[0].ics.maxSfb;
        cpe.maxSfbSte = cpe.ch[0].ics.maxSfb;
        if (msPresent == 1) {
            for (int idx = 0; idx < maxIdx; idx++) cpe.msMask[idx] = uint8_t(gb.bit());
        } else if (msPresent == 2) {
            memset(cpe.msMask, 1, size_t(maxIdx) * sizeof cpe.msMask[0]);
        }
    }

    void decodeGainControl(Sce& sce, BitReader& gb) {
        static const uint8_t gainMode[4][3] = {{1, 0, 5}, {2, 1, 2}, {8, 0, 2}, {2, 1, 5}};
        const int mode = sce.ics.windowSequence[0];
        int maxBand = int(gb.read(2));
        for (int bd = 0; bd < maxBand; bd++) {
            for (int wd = 0; wd < gainMode[mode][0]; wd++) {
                int adjustNum = int(gb.read(3));
                for (int ad = 0; ad < adjustNum; ad++) gb.skip(4 + ((wd == 0 && gainMode[mode][1]) ? 4 : gainMode[mode][2]));
            }
        }
    }

    static float* vmul2(float* dst, const float* v, unsigned idx, float s) {
        *dst++ = v[idx & 15] * s;
        *dst++ = v[idx >> 4 & 15] * s;
        return dst;
    }

    static float* vmul4(float* dst, const float* v, unsigned idx, float s) {
        *dst++ = v[idx & 3] * s;
        *dst++ = v[idx >> 2 & 3] * s;
        *dst++ = v[idx >> 4 & 3] * s;
        *dst++ = v[idx >> 6 & 3] * s;
        return dst;
    }

    static float* vmul2s(float* dst, const float* v, unsigned idx, unsigned sign, float scale) {
        uint32_t s0 = bitsOf(scale), s1 = s0;
        s0 ^= sign >> 1 << 31;
        s1 ^= sign << 31;
        *dst++ = v[idx & 15] * floatOf(s0);
        *dst++ = v[idx >> 4 & 15] * floatOf(s1);
        return dst;
    }

    static float* vmul4s(float* dst, const float* v, unsigned idx, unsigned sign, float scale) {
        unsigned nz = idx >> 12;
        uint32_t s = bitsOf(scale);
        *dst++ = v[idx & 3] * floatOf(s ^ (sign & 1u << 31));
        sign <<= nz & 1;
        nz >>= 1;
        *dst++ = v[idx >> 2 & 3] * floatOf(s ^ (sign & 1u << 31));
        sign <<= nz & 1;
        nz >>= 1;
        *dst++ = v[idx >> 4 & 3] * floatOf(s ^ (sign & 1u << 31));
        sign <<= nz & 1;
        *dst++ = v[idx >> 6 & 3] * floatOf(s ^ (sign & 1u << 31));
        return dst;
    }

    int decodeSpectrum(BitReader& gb, const Pulse* pulse, Sce& sce) {
        float* coef = sce.coeffs;
        Ics& ics = sce.ics;
        const int c = 1024 / ics.numWindows;
        const uint16_t* offsets = ics.swbOffset;
        const float* sf = sce.sf;
        const int* bandType = sce.bandType;
        float* coefBase = coef;
        int idx = 0;
        for (int g = 0; g < ics.numWindows; g++) memset(coef + g * 128 + offsets[ics.maxSfb], 0, sizeof(float) * size_t(c - offsets[ics.maxSfb]));
        for (int g = 0; g < ics.numWindowGroups; g++) {
            unsigned gLen = ics.groupLen[g];
            for (int i = 0; i < ics.maxSfb; i++, idx++) {
                const unsigned cbtM1 = unsigned(bandType[idx] - 1);
                float* cfo = coef + offsets[i];
                int offLen = offsets[i + 1] - offsets[i];
                if (cbtM1 >= kIntensityBt2 - 1) {
                    for (unsigned group = 0; group < gLen; group++, cfo += 128) memset(cfo, 0, size_t(offLen) * sizeof *cfo);
                } else if (cbtM1 == kNoiseBt - 1) {
                    for (unsigned group = 0; group < gLen; group++, cfo += 128) {
                        for (int k = 0; k < offLen; k++) {
                            randomState = lcgRandom(unsigned(randomState));
                            cfo[k] = float(randomState);
                        }
                        float bandEnergy = scalarProduct(cfo, cfo, offLen);
                        float scale = sf[idx] / sqrtf(bandEnergy);
                        for (int k = 0; k < offLen; k++) cfo[k] *= scale;
                    }
                } else {
                    const float* vq = kVectorValues[cbtM1];
                    const Vlc& vlc = t.spectral[cbtM1];
                    switch (cbtM1 >> 1) {
                        case 0:
                            for (unsigned group = 0; group < gLen; group++, cfo += 128) {
                                float* cf = cfo;
                                for (int len = offLen; len; len -= 4) cf = vmul4(cf, vq, unsigned(vlc.read(gb)), sf[idx]);
                            }
                            break;
                        case 1:
                            for (unsigned group = 0; group < gLen; group++, cfo += 128) {
                                float* cf = cfo;
                                for (int len = offLen; len; len -= 4) {
                                    unsigned cbIdx = unsigned(vlc.read(gb));
                                    unsigned nnz = cbIdx >> 8 & 15;
                                    uint32_t bits = nnz ? uint32_t(gb.peek(32)) : 0;
                                    gb.skip(nnz);
                                    cf = vmul4s(cf, vq, cbIdx, bits, sf[idx]);
                                }
                            }
                            break;
                        case 2:
                            for (unsigned group = 0; group < gLen; group++, cfo += 128) {
                                float* cf = cfo;
                                for (int len = offLen; len; len -= 2) cf = vmul2(cf, vq, unsigned(vlc.read(gb)), sf[idx]);
                            }
                            break;
                        case 3:
                        case 4:
                            for (unsigned group = 0; group < gLen; group++, cfo += 128) {
                                float* cf = cfo;
                                for (int len = offLen; len; len -= 2) {
                                    unsigned cbIdx = unsigned(vlc.read(gb));
                                    unsigned nnz = cbIdx >> 8 & 15;
                                    unsigned sign = nnz ? gb.peek(int(nnz)) << (cbIdx >> 12) : 0;
                                    gb.skip(nnz);
                                    cf = vmul2s(cf, vq, cbIdx, sign, sf[idx]);
                                }
                            }
                            break;
                        default:
                            for (unsigned group = 0; group < gLen; group++, cfo += 128) {
                                uint32_t* icf = reinterpret_cast<uint32_t*>(cfo);
                                for (int len = offLen; len; len -= 2) {
                                    unsigned cbIdx = unsigned(vlc.read(gb));
                                    if (cbIdx == 0) {
                                        *icf++ = 0;
                                        *icf++ = 0;
                                        continue;
                                    }
                                    unsigned nnz = cbIdx >> 12;
                                    unsigned nzt = cbIdx >> 8;
                                    uint32_t bits = nnz ? gb.peek(int(nnz)) << (32 - nnz) : 0;
                                    gb.skip(nnz);
                                    for (int j = 0; j < 2; j++) {
                                        if (nzt & 1 << j) {
                                            uint32_t b = uint32_t(gb.peek(32));
                                            int ones = std::countl_one(b);
                                            if (ones > 8) return -1;
                                            gb.skip(ones + 1);
                                            ones += 4;
                                            int n = (1 << ones) + int(gb.read(ones));
                                            *icf++ = t.cbrt[n] | (bits & 1u << 31);
                                            bits <<= 1;
                                        } else {
                                            unsigned v = bitsOf(vq[cbIdx & 15]);
                                            *icf++ = (bits & 1u << 31) | v;
                                            bits <<= !!v;
                                        }
                                        cbIdx >>= 4;
                                    }
                                }
                                for (int k = 0; k < offLen; k++) cfo[k] *= sf[idx];
                            }
                            break;
                    }
                }
            }
            coef += gLen << 7;
        }
        if (pulse) {
            idx = 0;
            for (int i = 0; i < pulse->numPulse; i++) {
                float co = coefBase[pulse->pos[i]];
                while (offsets[idx + 1] <= pulse->pos[i]) idx++;
                if (bandType[idx] != kNoiseBt && sf[idx]) {
                    float ico = float(-pulse->amp[i]);
                    if (co) {
                        co /= sf[idx];
                        ico = co / sqrtf(sqrtf(fabsf(co))) + (co > 0 ? -ico : ico);
                    }
                    coefBase[pulse->pos[i]] = cbrtf(fabsf(ico)) * ico * sf[idx];
                }
            }
        }
        return 0;
    }

    int decodeIcs(Sce& sce, BitReader& gb, int commonWindow) {
        Pulse pulse{};
        Tns& tns = sce.tns;
        Ics& ics = sce.ics;
        int pulsePresent = 0;
        int globalGain = int(gb.read(8));
        auto fail = [&] {
            memset(sce.sfo, 0, sizeof sce.sfo);
            tns.present = 0;
            return -1;
        };
        if (!commonWindow && decodeIcsInfo(ics, gb) < 0) return fail();
        if (decodeBandTypes(sce, gb) < 0) return fail();
        if (decodeScalefactors(sce, gb, globalGain) < 0) return fail();
        dequantScalefactors(sce);
        if ((pulsePresent = gb.bit())) {
            if (ics.windowSequence[0] == kEightShort) return fail();
            if (decodePulses(pulse, gb, ics.swbOffset, ics.numSwb)) return fail();
        }
        tns.present = gb.bit();
        if (tns.present && decodeTns(tns, gb, ics) < 0) return fail();
        if (gb.bit()) decodeGainControl(sce, gb);
        if (decodeSpectrum(gb, pulsePresent ? &pulse : nullptr, sce) < 0) return fail();
        return 0;
    }

    void applyMidSide(Element& cpe) {
        const Ics& ics = cpe.ch[0].ics;
        float* ch0 = cpe.ch[0].coeffs;
        float* ch1 = cpe.ch[1].coeffs;
        const uint16_t* offsets = ics.swbOffset;
        for (int g = 0; g < ics.numWindowGroups; g++) {
            for (int sfb = 0; sfb < cpe.maxSfbSte; sfb++) {
                const int idx = g * cpe.maxSfbSte + sfb;
                if (cpe.msMask[idx] && cpe.ch[0].bandType[idx] < kNoiseBt && cpe.ch[1].bandType[idx] < kNoiseBt) {
                    for (int group = 0; group < ics.groupLen[g]; group++) {
                        float* a = ch0 + group * 128 + offsets[sfb];
                        float* b = ch1 + group * 128 + offsets[sfb];
                        for (int i = 0; i < offsets[sfb + 1] - offsets[sfb]; i++) {
                            float d = a[i] - b[i];
                            a[i] += b[i];
                            b[i] = d;
                        }
                    }
                }
            }
            ch0 += ics.groupLen[g] * 128;
            ch1 += ics.groupLen[g] * 128;
        }
    }

    void applyIntensity(Element& cpe, int msPresent) {
        const Ics& ics = cpe.ch[1].ics;
        Sce& sce1 = cpe.ch[1];
        float* coef0 = cpe.ch[0].coeffs;
        float* coef1 = cpe.ch[1].coeffs;
        const uint16_t* offsets = ics.swbOffset;
        for (int g = 0; g < ics.numWindowGroups; g++) {
            for (int sfb = 0; sfb < ics.maxSfb; sfb++) {
                const int idx = g * ics.maxSfb + sfb;
                if (sce1.bandType[idx] == kIntensityBt || sce1.bandType[idx] == kIntensityBt2) {
                    int c = -1 + 2 * (sce1.bandType[idx] - 14);
                    if (msPresent) c *= 1 - 2 * cpe.msMask[idx];
                    float scale = float(c) * sce1.sf[idx];
                    for (int group = 0; group < ics.groupLen[g]; group++) {
                        float* to = coef1 + group * 128 + offsets[sfb];
                        const float* from = coef0 + group * 128 + offsets[sfb];
                        for (int i = 0; i < offsets[sfb + 1] - offsets[sfb]; i++) to[i] = from[i] * scale;
                    }
                }
            }
            coef0 += ics.groupLen[g] * 128;
            coef1 += ics.groupLen[g] * 128;
        }
    }

    int decodeCpe(BitReader& gb, Element& cpe) {
        int msPresent = 0;
        int commonWindow = gb.bit();
        if (commonWindow) {
            if (decodeIcsInfo(cpe.ch[0].ics, gb)) return -1;
            uint8_t kb = cpe.ch[1].ics.useKbWindow[0];
            cpe.ch[1].ics = cpe.ch[0].ics;
            cpe.ch[1].ics.useKbWindow[1] = kb;
            msPresent = int(gb.read(2));
            if (msPresent == 3) return -1;
            if (msPresent) decodeMidSide(cpe, gb, msPresent);
        }
        if (decodeIcs(cpe.ch[0], gb, commonWindow)) return -1;
        if (decodeIcs(cpe.ch[1], gb, commonWindow)) return -1;
        if (commonWindow && msPresent) applyMidSide(cpe);
        applyIntensity(cpe, msPresent);
        return 0;
    }

    static void lpcCoefs(const float* autoc, int maxOrder, float* lpc) {
        for (int i = 0; i < maxOrder; i++) {
            float r = -autoc[i];
            lpc[i] = r;
            for (int j = 0; j < (i + 1) >> 1; j++) {
                float f = lpc[j];
                float b = lpc[i - 1 - j];
                lpc[j] = f + r * b;
                lpc[i - 1 - j] = b + r * f;
            }
        }
    }

    void applyTns(float* coef, Tns& tns, Ics& ics) {
        const int mmm = std::min(ics.tnsMaxBands, int(ics.maxSfb));
        float lpc[20];
        if (!mmm) return;
        for (int w = 0; w < ics.numWindows; w++) {
            int bottom = ics.numSwb;
            for (int filt = 0; filt < tns.nFilt[w]; filt++) {
                int top = bottom;
                bottom = std::max(0, top - tns.length[w][filt]);
                int order = tns.order[w][filt];
                if (order == 0) continue;
                lpcCoefs(tns.coef[w][filt], order, lpc);
                int start = ics.swbOffset[std::min(bottom, mmm)];
                int end = ics.swbOffset[std::min(top, mmm)];
                int size = end - start;
                if (size <= 0) continue;
                int inc;
                if (tns.direction[w][filt]) {
                    inc = -1;
                    start = end - 1;
                } else {
                    inc = 1;
                }
                start += w * 128;
                for (int m = 0; m < size; m++, start += inc)
                    for (int i = 1; i <= std::min(m, order); i++) coef[start] -= coef[start - i * inc] * lpc[i - 1];
            }
        }
    }

    void imdctAndWindowing(Sce& sce) {
        Ics& ics = sce.ics;
        float* out = sce.out;
        float* saved = sce.saved;
        const float* swindow = ics.useKbWindow[0] ? t.kbdShort : sine128;
        const float* lwindowPrev = ics.useKbWindow[1] ? t.kbdLong : sine1024;
        const float* swindowPrev = ics.useKbWindow[1] ? t.kbdShort : sine128;
        float* buf = bufMdct;
        for (int i = 0; i < 1024; i++) coefMdct[i] = sce.coeffs[i];
        if (ics.windowSequence[0] == kEightShort) {
            for (int i = 0; i < 1024; i += 128) mdct128.half(coefMdct + i, outMdct + i);
        } else {
            mdct1024.half(coefMdct, outMdct);
        }
        for (int i = 0; i < 1024; i++) buf[i] = float(outMdct[i]);
        if ((ics.windowSequence[1] == kOnlyLong || ics.windowSequence[1] == kLongStop) &&
            (ics.windowSequence[0] == kOnlyLong || ics.windowSequence[0] == kLongStart)) {
            vectorFmulWindow(out, saved, buf, lwindowPrev, 512);
        } else {
            memcpy(out, saved, 448 * sizeof *out);
            if (ics.windowSequence[0] == kEightShort) {
                vectorFmulWindow(out + 448 + 0 * 128, saved + 448, buf + 0 * 128, swindowPrev, 64);
                vectorFmulWindow(out + 448 + 1 * 128, buf + 0 * 128 + 64, buf + 1 * 128, swindow, 64);
                vectorFmulWindow(out + 448 + 2 * 128, buf + 1 * 128 + 64, buf + 2 * 128, swindow, 64);
                vectorFmulWindow(out + 448 + 3 * 128, buf + 2 * 128 + 64, buf + 3 * 128, swindow, 64);
                vectorFmulWindow(temp, buf + 3 * 128 + 64, buf + 4 * 128, swindow, 64);
                memcpy(out + 448 + 4 * 128, temp, 64 * sizeof *out);
            } else {
                vectorFmulWindow(out + 448, saved + 448, buf, swindowPrev, 64);
                memcpy(out + 576, buf + 64, 448 * sizeof *out);
            }
        }
        if (ics.windowSequence[0] == kEightShort) {
            memcpy(saved, temp + 64, 64 * sizeof *saved);
            vectorFmulWindow(saved + 64, buf + 4 * 128 + 64, buf + 5 * 128, swindow, 64);
            vectorFmulWindow(saved + 192, buf + 5 * 128 + 64, buf + 6 * 128, swindow, 64);
            vectorFmulWindow(saved + 320, buf + 6 * 128 + 64, buf + 7 * 128, swindow, 64);
            memcpy(saved + 448, buf + 7 * 128 + 64, 64 * sizeof *saved);
        } else if (ics.windowSequence[0] == kLongStart) {
            memcpy(saved, buf + 512, 448 * sizeof *saved);
            memcpy(saved + 448, buf + 7 * 128 + 64, 64 * sizeof *saved);
        } else {
            memcpy(saved, buf + 512, 512 * sizeof *saved);
        }
    }

    int decodeExtensionPayload(BitReader& gb, int cnt, Element* che, int elemType) {
        int crcFlag = 0;
        int res = cnt;
        int type = int(gb.read(4));
        switch (type) {
            case kExtSbrDataCrc:
                crcFlag++;
                [[fallthrough]];
            case kExtSbrData:
                if (!che) return res;
                if (!oc[1].m4ac.sbr || (oc[1].m4ac.sbr == -1 && oc[1].status == kOcLocked)) {
                    gb.skip(8 * int64_t(cnt) - 4);
                    return res;
                }
                if (oc[1].m4ac.ps == -1 && oc[1].status < kOcLocked && oc[1].channels == 1) {
                    oc[1].m4ac.sbr = 1;
                    oc[1].m4ac.ps = 1;
                    outputConfigure(oc[1].layoutMap, oc[1].layoutMapTags, oc[1].status);
                } else {
                    oc[1].m4ac.sbr = 1;
                }
                che->sbrUsed = 1;
                sbrDecodeExtension(che->sbr, oc[1].m4ac, gb, crcFlag, cnt, elemType);
                break;
            case kExtDynamicRange: res = decodeDynamicRange(gb); break;
            default: gb.skip(8 * int64_t(cnt) - 4); break;
        }
        return res;
    }

    int decodeDynamicRange(BitReader& gb) {
        int n = 1, bands = 1;
        if (gb.bit()) {
            gb.skip(8);
            n++;
        }
        if (gb.bit()) {
            int excluded = 0;
            do {
                gb.skip(7);
                excluded += 7;
            } while (excluded < kMaxChannels - 7 && gb.bit());
            n += excluded / 7;
        }
        if (gb.bit()) {
            int incr = int(gb.read(4));
            gb.skip(4);
            n++;
            bands += incr;
            for (int i = 0; i < bands; i++) {
                gb.skip(8);
                n++;
            }
        }
        if (gb.bit()) {
            gb.skip(8);
            n++;
        }
        for (int i = 0; i < bands; i++) {
            gb.skip(8);
            n++;
        }
        return n;
    }

    void spectralToSample() {
        for (int type = 3; type >= 0; type--) {
            for (int i = 0; i < kMaxElemId; i++) {
                Element* e = che[type][i].get();
                if (!e || !e->present) continue;
                if (e->ch[0].tns.present) applyTns(e->ch[0].coeffs, e->ch[0].tns, e->ch[0].ics);
                if (e->ch[1].tns.present) applyTns(e->ch[1].coeffs, e->ch[1].tns, e->ch[1].ics);
                if (type != kCce) {
                    imdctAndWindowing(e->ch[0]);
                    if (type == kCpe) imdctAndWindowing(e->ch[1]);
                    if (oc[1].m4ac.sbr > 0) {
                        e->sbrUsed = 1;
                        sbrApply(e->sbr, tools(), oc[1].m4ac, type, e->ch[0].out, e->ch[1].out);
                    }
                }
                e->present = 0;
            }
        }
    }

    int decodeFrame(const uint8_t* data, size_t size, int& samples) {
        BitReader gb(data, size);
        samples = 0;
        tagsMapped = 0;
        uint8_t presence[4][kMaxElemId] = {};
        Element *cheElem = nullptr, *chePrev = nullptr;
        int chePrevType = kEnd;
        int audioFound = 0, pceFound = 0;
        const int64_t payloadAlignment = gb.position();
        int elemType;
        while ((elemType = int(gb.read(3))) != kEnd) {
            int elemId = int(gb.read(4));
            int err = 0;
            if (!oc[1].channels && elemType != kPce) return -1;
            if (elemType < kDse) {
                if (presence[elemType][elemId]) {
                    if (presence[elemType][elemId] > 1) return -1;
                }
                presence[elemType][elemId]++;
                if (!(cheElem = getChe(elemType, elemId))) return -1;
                samples = 1024;
                cheElem->present = 1;
            }
            switch (elemType) {
                case kSce:
                case kLfe:
                    err = decodeIcs(cheElem->ch[0], gb, 0);
                    audioFound = 1;
                    break;
                case kCpe:
                    err = decodeCpe(gb, *cheElem);
                    audioFound = 1;
                    break;
                case kCce: return -1;
                case kDse: {
                    int byteAlign = gb.bit();
                    int count = int(gb.read(8));
                    if (count == 255) count += int(gb.read(8));
                    if (byteAlign) gb.align();
                    if (gb.left() < 8 * count) return -1;
                    gb.skip(8 * count);
                    break;
                }
                case kPce: {
                    uint8_t layoutMap[kMaxElemId * 4][3] = {};
                    int pushed = pushOutputConfiguration();
                    if (pceFound && !pushed) return -1;
                    int tags = decodePce(layoutMap, gb, payloadAlignment);
                    if (tags < 0) {
                        err = tags;
                        break;
                    }
                    if (pceFound) {
                        popOutputConfiguration();
                    } else {
                        err = outputConfigure(layoutMap, tags, kOcTrialPce);
                        if (!err) oc[1].m4ac.channelConfig = 0;
                        pceFound = 1;
                    }
                    break;
                }
                case kFil: {
                    if (elemId == 15) elemId += int(gb.read(8)) - 1;
                    if (gb.left() < 8 * elemId) return -1;
                    while (elemId > 0) {
                        int ret = decodeExtensionPayload(gb, elemId, chePrev, chePrevType);
                        if (ret < 0) {
                            err = ret;
                            break;
                        }
                        elemId -= ret;
                    }
                    break;
                }
                default: return -1;
            }
            if (elemType < kDse) {
                chePrev = cheElem;
                chePrevType = elemType;
            }
            if (err) return err;
            if (gb.left() < 3) return -1;
        }
        if (!oc[1].channels) return 0;
        int multiplier = oc[1].m4ac.sbr == 1 ? oc[1].m4ac.extSampleRate > oc[1].m4ac.sampleRate : 0;
        samples <<= multiplier;
        spectralToSample();
        if (oc[1].status && audioFound) oc[1].status = kOcLocked;
        return 0;
    }

    int decode(const uint8_t* data, size_t size, int& samples) {
        int err = decodeFrame(data, size, samples);
        if (err < 0) {
            popOutputConfiguration();
            samples = 0;
        }
        return err;
    }

    int outputRate() const {
        int multiplier = oc[1].m4ac.sbr == 1 ? oc[1].m4ac.extSampleRate > oc[1].m4ac.sampleRate : 0;
        return oc[1].m4ac.sampleRate << multiplier;
    }
};

void put(std::vector<uint8_t>& out, const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    out.insert(out.end(), b, b + n);
}

bool take(const std::vector<uint8_t>& in, size_t& at, void* p, size_t n) {
    if (in.size() - at < n || at > in.size()) return false;
    memcpy(p, in.data() + at, n);
    at += n;
    return true;
}

struct AacAudio : PacketDecoder {
    std::vector<uint8_t> specific;
    std::unique_ptr<Core> core;

    bool start() {
        core = std::make_unique<Core>();
        core->oc[1].m4ac.sampleRate = rate;
        return core->decodeAudioSpecificConfig(specific.data(), specific.size()) >= 0;
    }

    void reset() override { start(); }

    void feed(const std::vector<uint8_t>& packet, std::vector<float>& out) override {
        if (!core || packet.empty()) return;
        int samples = 0;
        if (core->decode(packet.data(), packet.size(), samples) < 0 || !samples) return;
        const size_t at = out.size(), wanted = size_t(channels);
        out.resize(at + size_t(samples) * wanted, 0.0f);
        float* to = out.data() + at;
        const int given = std::min(core->oc[1].channels, channels);
        for (int c = 0; c < given; c++) {
            const Sce* from = core->outputElement[c];
            if (!from) continue;
            for (int f = 0; f < samples; f++) to[size_t(f) * wanted + size_t(c)] = from->out[f];
        }
    }

    bool save(std::vector<uint8_t>& state) const override {
        state.clear();
        if (!core) return false;
        put(state, &core->randomState, sizeof core->randomState);
        put(state, core->oc, sizeof core->oc);
        for (int type = 0; type < 4; type++) {
            for (int id = 0; id < kMaxElemId; id++) {
                const Element* e = core->che[type][id].get();
                if (!e) continue;
                uint8_t head[3] = {uint8_t(type), uint8_t(id), uint8_t(e->sbrUsed)};
                put(state, head, sizeof head);
                for (const Sce& s : e->ch) {
                    put(state, &s.ics, sizeof s.ics);
                    put(state, s.saved, sizeof s.saved);
                }
                if (e->sbrUsed) put(state, &e->sbr, sizeof e->sbr);
            }
        }
        return true;
    }

    void load(const std::vector<uint8_t>& state) override {
        if (!core) return;
        size_t at = 0;
        if (!take(state, at, &core->randomState, sizeof core->randomState) || !take(state, at, core->oc, sizeof core->oc)) return;
        core->outputConfigure(core->oc[1].layoutMap, core->oc[1].layoutMapTags, core->oc[1].status);
        uint8_t head[3];
        while (take(state, at, head, sizeof head)) {
            if (head[0] >= 4 || head[1] >= kMaxElemId) return;
            auto& slot = core->che[head[0]][head[1]];
            if (!slot) {
                slot.reset(new Element());
                sbrInit(slot->sbr, head[0]);
            }
            for (Sce& s : slot->ch) {
                if (!take(state, at, &s.ics, sizeof s.ics) || !take(state, at, s.saved, sizeof s.saved)) return;
            }
            slot->sbrUsed = head[2];
            if (head[2] && !take(state, at, &slot->sbr, sizeof slot->sbr)) return;
        }
    }
};

}

std::unique_ptr<PacketDecoder> openAacAudio(const Track& track, const std::vector<uint8_t>& firstPacket, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    auto audio = std::make_unique<AacAudio>();
    if (track.configType == fourcc("ASC ")) audio->specific = track.config;
    else if (track.configType == fourcc("esds") && track.config.size() > 4) {
        ElementaryStream stream;
        if (parseElementaryStream(View{track.config.data(), track.config.size()}, stream) && stream.specific.p)
            audio->specific.assign(stream.specific.p, stream.specific.p + stream.specific.n);
    }
    if (audio->specific.empty()) return fail("AAC track without a decoder configuration");
    StreamConfig probe;
    BitReader gb(audio->specific.data(), audio->specific.size());
    if (mpeg4Config(probe, gb) < 0) return fail("AAC configuration not understood");
    if (probe.objectType != kAotLc) return fail("only AAC-LC, HE-AAC and HE-AAC v2 are decoded here");
    if (probe.frameLengthShort) return fail("AAC with 960-sample frames is not supported");
    audio->rate = track.sampleRate;
    if (!audio->start()) return fail("AAC configuration not understood");
    int samples = 0;
    if (!firstPacket.empty() && audio->core->decode(firstPacket.data(), firstPacket.size(), samples) >= 0 && samples) {
        audio->rate = audio->core->outputRate();
        audio->channels = audio->core->oc[1].channels;
        audio->packetFrames = samples;
    } else {
        const StreamConfig& m = audio->core->oc[1].m4ac;
        const bool doubled = m.sbr == 1 && m.extSampleRate > m.sampleRate;
        audio->rate = doubled ? m.extSampleRate : m.sampleRate;
        audio->channels = audio->core->oc[1].channels;
        audio->packetFrames = doubled ? 2048 : 1024;
    }
    if (audio->channels < 1 || audio->channels > kOutputChannels) return fail("audio layout not supported");
    const uint64_t layout = audio->core->oc[1].layout;
    audio->layout = std::popcount(layout) == audio->channels ? layout : 0;
    audio->reset();
    audio->prerollPackets = 2;
    return audio;
}

}

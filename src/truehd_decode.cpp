#include "audio_codec.h"
#include "bits.h"
#include "readers.h"
#include <algorithm>
#include <bit>
#include <cstring>

namespace absent {

namespace {

const int kMaxChannels = 8;
const int kMaxMatrices = 8;
const int kMaxSubstreams = 4;
const int kMaxBlock = 160;
const int kMaxBlockPow2 = 256;
const int kMaxFir = 8;
const int kMaxIir = 4;
const int kFir = 0, kIir = 1;
const int kParamBlocksize = 1 << 7, kParamMatrix = 1 << 6, kParamOutshift = 1 << 5, kParamQuantstep = 1 << 4, kParamFir = 1 << 3, kParamIir = 1 << 2,
          kParamHuffoffset = 1 << 1, kParamPresence = 1 << 0;

const uint8_t kHuffman[3][18][2] = {
    {{0x01, 9}, {0x01, 8}, {0x01, 7}, {0x01, 6}, {0x01, 5}, {0x01, 4}, {0x01, 3}, {0x04, 3}, {0x05, 3}, {0x06, 3}, {0x07, 3}, {0x03, 3}, {0x05, 4}, {0x09, 5},
        {0x11, 6}, {0x21, 7}, {0x41, 8}, {0x81, 9}},
    {{0x01, 9}, {0x01, 8}, {0x01, 7}, {0x01, 6}, {0x01, 5}, {0x01, 4}, {0x01, 3}, {0x02, 2}, {0x03, 2}, {0x03, 3}, {0x05, 4}, {0x09, 5}, {0x11, 6}, {0x21, 7},
        {0x41, 8}, {0x81, 9}},
    {{0x01, 9}, {0x01, 8}, {0x01, 7}, {0x01, 6}, {0x01, 5}, {0x01, 4}, {0x01, 3}, {0x01, 1}, {0x03, 3}, {0x05, 4}, {0x09, 5}, {0x11, 6}, {0x21, 7}, {0x41, 8},
        {0x81, 9}}};

const int8_t kNoise[256] = {30, 51, 22, 54, 3, 7, -4, 38, 14, 55, 46, 81, 22, 58, -3, 2, 52, 31, -7, 51, 15, 44, 74, 30, 85, -17, 10, 33, 18, 80, 28, 62, 10, 32,
    23, 69, 72, 26, 35, 17, 73, 60, 8, 56, 2, 6, -2, -5, 51, 4, 11, 50, 66, 76, 21, 44, 33, 47, 1, 26, 64, 48, 57, 40, 38, 16, -10, -28, 92, 22, -18, 29, -10, 5,
    -13, 49, 19, 24, 70, 34, 61, 48, 30, 14, -6, 25, 58, 33, 42, 60, 67, 17, 54, 17, 22, 30, 67, 44, -9, 50, -11, 43, 40, 32, 59, 82, 13, 49, -14, 55, 60, 36, 48,
    49, 31, 47, 15, 12, 4, 65, 1, 23, 29, 39, 45, -2, 84, 69, 0, 72, 37, 57, 27, 41, -15, -16, 35, 31, 14, 61, 24, 0, 27, 24, 16, 41, 55, 34, 53, 9, 56, 12, 25,
    29, 53, 5, 20, -20, -8, 20, 13, 28, -3, 78, 38, 16, 11, 62, 46, 29, 21, 24, 46, 65, 43, -23, 89, 18, 74, 21, 38, -12, 19, 12, -19, 8, 15, 33, 4, 57, 9, -8,
    36, 35, 26, 28, 7, 83, 63, 79, 75, 11, 3, 87, 37, 47, 34, 40, 39, 19, 20, 42, 27, 34, 39, 77, 13, 42, 59, 64, 45, -1, 32, 37, 45, -5, 53, -6, 7, 36, 50, 23,
    6, 32, 9, -21, 18, 71, 27, 52, -25, 31, 35, 42, -1, 68, 63, 52, 26, 43, 66, 37, 41, 25, 40, 70};

const uint8_t kThdCount[13] = {2, 1, 1, 2, 2, 2, 2, 1, 1, 2, 2, 1, 1};
const uint64_t kThdLayout[13] = {0x3, 0x4, 0x8, 0x600, 0x5000, 0xC0, 0x30, 0x100, 0x800, 0x600000000ull, 0x180000000ull, 0x2000, 0x800000000ull};
const int kThdOrder[20] = {0, 1, 2, 3, 9, 10, 12, 14, 6, 7, 4, 5, 8, 11, 33, 34, 31, 32, 13, 35};
const uint8_t kMlpQuants[16] = {16, 20, 24};
const uint64_t kMlpLayout[32] = {0x4, 0x3, 0x103, 0x33, 0xB, 0x10B, 0x3B, 0x7, 0x107, 0x37, 0xF, 0x10F, 0x3F, 0x107, 0x37, 0xF, 0x10F, 0x3F, 0x3B, 0x37, 0x3F};

struct Lut {
    int16_t symbol[512];
    uint8_t length[512];
};

const Lut* luts() {
    static const Lut* tables = [] {
        static Lut t[3];
        for (int b = 0; b < 3; ++b) {
            std::fill(std::begin(t[b].symbol), std::end(t[b].symbol), int16_t(-1));
            std::fill(std::begin(t[b].length), std::end(t[b].length), uint8_t(9));
            for (int s = 0; s < 18; ++s) {
                const int code = kHuffman[b][s][0], length = kHuffman[b][s][1];
                if (!length) continue;
                for (int k = code << (9 - length); k < (code + 1) << (9 - length); ++k) {
                    t[b].symbol[k] = int16_t(s);
                    t[b].length[k] = uint8_t(length);
                }
            }
        }
        return t;
    }();
    return tables;
}

int signedRead(BitReader& r, int n) { return n ? int32_t(r.read(n) << (32 - n)) >> (32 - n) : 0; }

uint8_t xorBytes(const uint8_t* p, size_t n) {
    uint8_t x = 0;
    for (size_t i = 0; i < n; ++i) x ^= p[i];
    return x;
}

uint16_t crc16(const uint8_t* p, size_t n) {
    uint16_t crc = 0;
    for (size_t i = 0; i < n; ++i) {
        crc ^= uint16_t(p[i] << 8);
        for (int k = 0; k < 8; ++k) crc = uint16_t(crc & 0x8000 ? (crc << 1) ^ 0x002D : crc << 1);
    }
    return crc;
}

int samplerateOf(int bits) { return bits == 0xF ? 0 : (bits & 8 ? 44100 : 48000) << (bits & 7); }

uint64_t thdLayout(int map) {
    uint64_t layout = 0;
    for (int i = 0; i < 13; i++) if (map >> i & 1) layout |= kThdLayout[i];
    return layout;
}

int thdChannels(int map) {
    int n = 0;
    for (int i = 0; i < 13; i++) n += kThdCount[i] * (map >> i & 1);
    return n;
}

struct Major {
    int type = 0, size = 0, bits = 0, rate = 0, unit = 0, unitPow2 = 0, substreams = 0, info = 0, arrangement = 0;
    uint64_t layout1 = 0, layout2 = 0, layoutMlp = 0;
    int channels1 = 0, channels2 = 0;
};

bool readMajor(const uint8_t* p, size_t n, Major& m) {
    if (n < 28 || (uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]) >> 1 != 0xf8726fba >> 1) return false;
    int size = 28;
    if (p[3] == 0xba && (p[25] & 1)) size += 2 + (p[26] >> 4) * 2;
    if (int(n) < size) return false;
    const uint16_t sum = uint16_t(crc16(p, size_t(size) - 4) ^ (p[size - 4] << 8 | p[size - 3]));
    if (sum != (p[size - 2] << 8 | p[size - 1])) return false;
    BitReader r(p, size_t(size));
    r.skip(24);
    m.type = int(r.read(8));
    m.size = size;
    int ratebits;
    if (m.type == 0xbb) {
        m.bits = kMlpQuants[r.read(4)];
        const int second = kMlpQuants[r.read(4)];
        ratebits = int(r.read(4));
        m.rate = samplerateOf(ratebits);
        const int rate2 = samplerateOf(int(r.read(4)));
        r.skip(11);
        m.arrangement = int(r.read(5));
        m.layoutMlp = kMlpLayout[m.arrangement];
        if (second > m.bits || (rate2 && rate2 != m.rate)) return false;
    } else if (m.type == 0xba) {
        m.bits = 24;
        ratebits = int(r.read(4));
        m.rate = samplerateOf(ratebits);
        r.skip(8);
        m.arrangement = int(r.read(5));
        m.channels1 = thdChannels(m.arrangement);
        m.layout1 = thdLayout(m.arrangement);
        r.skip(2);
        const int map2 = int(r.read(13));
        m.channels2 = thdChannels(map2);
        m.layout2 = thdLayout(map2);
    } else {
        return false;
    }
    m.unit = 40 << (ratebits & 7);
    m.unitPow2 = 64 << (ratebits & 7);
    r.skip(48);
    r.skip(16);
    m.substreams = int(r.read(4));
    r.skip(4);
    m.info = int(r.read(8));
    return m.bits && m.rate && m.rate <= 192000 && m.unit <= kMaxBlock && m.substreams && m.substreams <= kMaxSubstreams && (m.type != 0xbb || m.substreams <= 2);
}

struct FilterParams {
    uint8_t order = 0, shift = 0;
    int32_t state[kMaxFir] = {};
};

struct ChannelParams {
    FilterParams filter[2];
    int32_t coeff[2][kMaxFir] = {};
    int16_t huffOffset = 0;
    int32_t signHuffOffset = 0;
    uint8_t codebook = 0, huffLsbs = 0;
};

struct Substream {
    bool restartSeen = false, endOfStream = false;
    uint16_t noiseType = 0;
    uint8_t minChannel = 0, maxChannel = 0, maxMatrixChannel = 0;
    uint64_t codedChannels = 0;
    uint8_t assign[kMaxChannels] = {};
    uint64_t mask = 0;
    ChannelParams params[kMaxChannels];
    uint8_t noiseShift = 0;
    uint32_t seed = 0;
    bool dataCheck = false;
    uint8_t presence = 0;
    uint8_t matrices = 0;
    uint8_t matrixOut[kMaxMatrices] = {};
    uint8_t lsbBypass[kMaxMatrices] = {};
    int32_t matrixCoeff[kMaxMatrices][kMaxChannels + 2] = {};
    uint8_t matrixNoiseShift[kMaxMatrices] = {};
    uint8_t quantStep[kMaxChannels] = {};
    uint16_t blocksize = 0, blockpos = 0;
    int8_t outputShift[kMaxChannels] = {};
};

struct TruehdAudio : PacketDecoder {
    bool truehd = true;
    bool valid = false;
    int majorSize = 0, unit = 0, unitPow2 = 0, substreams = 0, info = 0, last = 0;
    bool reorder = false;
    Substream sub[kMaxSubstreams];
    int matrixChanged = 0;
    int filterChanged[kMaxChannels][2] = {};
    int8_t noise[kMaxBlockPow2] = {};
    uint8_t bypassed[kMaxBlock][kMaxChannels] = {};
    int32_t samples[kMaxBlock][kMaxChannels + 2] = {};

    void reset() override {
        valid = false;
        for (Substream& s : sub) s = Substream{};
    }

    bool major(const uint8_t* p, size_t n) {
        Major m;
        if (!readMajor(p, n, m) || (m.type == 0xba) != truehd) return false;
        apply(m);
        return true;
    }

    void apply(const Major& m) {
        majorSize = m.size;
        unit = m.unit;
        unitPow2 = m.unitPow2;
        substreams = m.substreams;
        info = m.info;
        last = std::min(substreams - 1, 2);
        valid = true;
        for (Substream& s : sub) s.restartSeen = false;
        if (!truehd) {
            int s = substreams > 1;
            if (s) sub[0].mask = 0x3;
            sub[s].mask = m.layoutMlp;
        } else {
            sub[1].mask = m.layout1;
            if (m.channels1 == 2 && m.channels2 == 2) sub[0].mask = 0x3;
            if (substreams > 1) sub[0].mask = 0x3;
            if (substreams == 1 && m.channels1 == 1 && m.channels2 == 1) sub[0].mask = 0x4;
            if (substreams > 2) sub[2].mask = m.layout2 ? m.layout2 : m.layout1;
            if (substreams == 2) sub[1].mask = m.layout2;
        }
        reorder = m.arrangement >= 18 && m.arrangement <= 20;
    }

    int32_t signHuff(Substream& s, int ch) {
        ChannelParams& cp = s.params[ch];
        const int lsbBits = cp.huffLsbs - s.quantStep[ch];
        const int signShift = lsbBits + (cp.codebook ? 2 - cp.codebook : -1);
        int32_t offset = cp.huffOffset;
        if (cp.codebook > 0) offset -= 7 << lsbBits;
        if (signShift >= 0) offset -= 1 << signShift;
        return offset;
    }

    bool restart(BitReader& r, const uint8_t*, Substream& s, int index) {
        if (r.read(13) != (0x31ea >> 1)) return false;
        const int noiseType = int(r.read(1));
        if (!truehd && noiseType) return false;
        r.skip(16);
        const int minChannel = int(r.read(4)), maxChannel = int(r.read(4)), maxMatrix = int(r.read(4));
        if (maxMatrix > (truehd ? 7 : 5)) return false;
        if (maxMatrix > 5 && !noiseType) return false;
        if (maxChannel + 1 > kMaxChannels || maxChannel + 1 < minChannel) return false;
        s.minChannel = uint8_t(minChannel);
        s.maxChannel = uint8_t(maxChannel);
        s.codedChannels = ((1ull << (maxChannel - minChannel + 1)) - 1) << minChannel;
        s.maxMatrixChannel = uint8_t(maxMatrix);
        s.noiseType = uint16_t(noiseType);
        s.noiseShift = uint8_t(r.read(4));
        s.seed = r.read(23);
        r.skip(19);
        s.dataCheck = r.read(1);
        r.skip(8);
        r.skip(16);
        memset(s.assign, 0, sizeof s.assign);
        for (int ch = 0; ch <= s.maxMatrixChannel; ch++) {
            int target = int(r.read(6));
            if (truehd) {
                int channel = -1, k = target;
                if (std::popcount(s.mask) > k)
                    for (int position : kThdOrder)
                        if (s.mask >> position & 1 && !k--) {
                            channel = position;
                            break;
                        }
                target = channel < 0 ? -1 : std::popcount(s.mask & ((1ull << channel) - 1));
            }
            if (target < 0 || target > s.maxMatrixChannel) return false;
            s.assign[target] = uint8_t(ch);
        }
        r.skip(8);
        s.presence = 0xff;
        s.matrices = 0;
        s.blocksize = 8;
        memset(s.outputShift, 0, sizeof s.outputShift);
        memset(s.quantStep, 0, sizeof s.quantStep);
        for (int ch = s.minChannel; ch <= s.maxChannel; ch++) {
            ChannelParams& cp = s.params[ch];
            cp.filter[kFir].order = cp.filter[kIir].order = 0;
            cp.filter[kFir].shift = cp.filter[kIir].shift = 0;
            cp.huffOffset = 0;
            cp.signHuffOffset = -(1 << 23);
            cp.codebook = 0;
            cp.huffLsbs = 24;
        }
        if (index == last && !truehd && reorder) {
            if (s.mask == 0x3B || s.mask == 0x37) {
                const uint8_t i = s.assign[4];
                s.assign[4] = s.assign[3];
                s.assign[3] = s.assign[2];
                s.assign[2] = i;
            } else if (s.mask == 0x3F) {
                std::swap(s.assign[2], s.assign[4]);
                std::swap(s.assign[3], s.assign[5]);
            }
        }
        return true;
    }

    bool filterParams(BitReader& r, Substream& s, int ch, int which) {
        FilterParams& fp = s.params[ch].filter[which];
        if (filterChanged[ch][which]++ > 1) return false;
        const int order = int(r.read(4));
        if (order > (which ? kMaxIir : kMaxFir)) return false;
        fp.order = uint8_t(order);
        if (order > 0) {
            int32_t* coeff = s.params[ch].coeff[which];
            fp.shift = uint8_t(r.read(4));
            const int bits = int(r.read(5)), shift = int(r.read(3));
            if (bits < 1 || bits > 16 || bits + shift > 16) return false;
            for (int i = 0; i < order; i++) coeff[i] = signedRead(r, bits) * (1 << shift);
            if (r.read(1)) {
                if (which == kFir) return false;
                const int stateBits = int(r.read(4)), stateShift = int(r.read(4));
                for (int i = 0; i < order; i++) fp.state[i] = stateBits ? signedRead(r, stateBits) * (1 << stateShift) : 0;
            }
        }
        return true;
    }

    bool matrixParams(BitReader& r, Substream& s) {
        if (matrixChanged++ > 1) return false;
        s.matrices = uint8_t(r.read(4));
        auto fail = [&] {
            s.matrices = 0;
            memset(s.matrixOut, 0, sizeof s.matrixOut);
            return false;
        };
        if (s.matrices > (truehd ? 8 : 6)) return fail();
        for (int mat = 0; mat < s.matrices; mat++) {
            s.matrixOut[mat] = uint8_t(r.read(4));
            const int fracBits = int(r.read(4));
            s.lsbBypass[mat] = uint8_t(r.read(1));
            if (s.matrixOut[mat] > s.maxMatrixChannel || fracBits > 14) return fail();
            const int maxChan = s.maxMatrixChannel + (s.noiseType ? 0 : 2);
            for (int ch = 0; ch <= maxChan; ch++) {
                int value = 0;
                if (r.read(1)) value = signedRead(r, fracBits + 2);
                s.matrixCoeff[mat][ch] = value * (1 << (14 - fracBits));
            }
            s.matrixNoiseShift[mat] = s.noiseType ? uint8_t(r.read(4)) : 0;
        }
        return true;
    }

    bool channelParams(BitReader& r, Substream& s, int ch) {
        ChannelParams& cp = s.params[ch];
        FilterParams& fir = cp.filter[kFir];
        FilterParams& iir = cp.filter[kIir];
        if ((s.presence & kParamFir) && r.read(1) && !filterParams(r, s, ch, kFir)) return false;
        if ((s.presence & kParamIir) && r.read(1) && !filterParams(r, s, ch, kIir)) return false;
        if (fir.order + iir.order > 8) return false;
        if (fir.order && iir.order && fir.shift != iir.shift) return false;
        if (!fir.order && iir.order) fir.shift = iir.shift;
        if ((s.presence & kParamHuffoffset) && r.read(1)) cp.huffOffset = int16_t(signedRead(r, 15));
        cp.codebook = uint8_t(r.read(2));
        cp.huffLsbs = uint8_t(r.read(5));
        if (cp.codebook > 0 && cp.huffLsbs > 24) {
            cp.huffLsbs = 0;
            return false;
        }
        return true;
    }

    bool decodingParams(BitReader& r, Substream& s, int index) {
        unsigned recompute = 0;
        bool ok = true;
        (void)index;
        if ((s.presence & kParamPresence) && r.read(1)) s.presence = uint8_t(r.read(8));
        if ((s.presence & kParamBlocksize) && r.read(1)) {
            s.blocksize = uint16_t(r.read(9));
            if (s.blocksize < 8 || s.blocksize > unit) {
                s.blocksize = 0;
                return false;
            }
        }
        if ((s.presence & kParamMatrix) && r.read(1) && !matrixParams(r, s)) return false;
        if ((s.presence & kParamOutshift) && r.read(1)) {
            for (int ch = 0; ch <= s.maxMatrixChannel; ch++) {
                s.outputShift[ch] = int8_t(signedRead(r, 4));
                if (s.outputShift[ch] < 0) s.outputShift[ch] = 0;
            }
        }
        if ((s.presence & kParamQuantstep) && r.read(1)) {
            for (int ch = 0; ch <= s.maxChannel; ch++) {
                s.quantStep[ch] = uint8_t(r.read(4));
                recompute |= 1u << ch;
            }
        }
        for (int ch = s.minChannel; ch <= s.maxChannel && ok; ch++) {
            if (r.read(1)) {
                recompute |= 1u << ch;
                ok = channelParams(r, s, ch);
            }
        }
        for (int ch = 0; ch <= s.maxChannel; ch++) {
            if (!(recompute & (1u << ch))) continue;
            ChannelParams& cp = s.params[ch];
            if (cp.codebook > 0 && cp.huffLsbs < s.quantStep[ch]) {
                ok = false;
                s.quantStep[ch] = 0;
            }
            cp.signHuffOffset = signHuff(s, ch);
        }
        return ok;
    }

    bool blockData(BitReader& r, Substream& s) {
        if (s.dataCheck) r.skip(16);
        if (s.blockpos + s.blocksize > unit) return false;
        memset(&bypassed[s.blockpos][0], 0, size_t(s.blocksize) * sizeof bypassed[0]);
        const Lut* tables = luts();
        for (int i = 0; i < s.blocksize; i++) {
            const int pos = i + s.blockpos;
            for (int mat = 0; mat < s.matrices; mat++)
                if (s.lsbBypass[mat]) bypassed[pos][mat] = uint8_t(r.read(1));
            for (int ch = s.minChannel; ch <= s.maxChannel; ch++) {
                ChannelParams& cp = s.params[ch];
                const int step = s.quantStep[ch];
                const int lsbBits = cp.huffLsbs - step;
                int result = 0;
                if (cp.codebook > 0) {
                    const Lut& t = tables[cp.codebook - 1];
                    const uint32_t look = r.peek(9);
                    result = t.symbol[look];
                    r.skip(t.length[look]);
                }
                if (result < 0) return false;
                if (lsbBits > 0) result = int(unsigned(result) << lsbBits) + int(r.read(lsbBits));
                result += cp.signHuffOffset;
                samples[pos][ch] = int32_t(unsigned(result) << step);
            }
        }
        for (int ch = s.minChannel; ch <= s.maxChannel; ch++) filter(s, ch);
        s.blockpos = uint16_t(s.blockpos + s.blocksize);
        if (s.dataCheck) r.skip(8);
        return true;
    }

    void filter(Substream& s, int ch) {
        ChannelParams& cp = s.params[ch];
        FilterParams& fir = cp.filter[kFir];
        FilterParams& iir = cp.filter[kIir];
        int32_t state[2][kMaxBlock + kMaxFir];
        int32_t* firBuf = state[0] + kMaxBlock;
        int32_t* iirBuf = state[1] + kMaxBlock;
        memcpy(firBuf, fir.state, sizeof fir.state);
        memcpy(iirBuf, iir.state, kMaxIir * sizeof(int32_t));
        const int32_t mask = int32_t(~0u << s.quantStep[ch]);
        const int32_t* firCoeff = cp.coeff[kFir];
        const int32_t* iirCoeff = cp.coeff[kIir];
        for (int i = 0; i < s.blocksize; i++) {
            int32_t& sample = samples[s.blockpos + i][ch];
            int64_t accum = 0;
            for (int o = 0; o < fir.order; o++) accum += int64_t(firBuf[o]) * firCoeff[o];
            for (int o = 0; o < iir.order; o++) accum += int64_t(iirBuf[o]) * iirCoeff[o];
            accum >>= fir.shift;
            const int32_t result = int32_t((accum + sample) & mask);
            *--firBuf = result;
            *--iirBuf = int32_t(result - accum);
            sample = result;
        }
        memcpy(fir.state, firBuf, sizeof fir.state);
        memcpy(iir.state, iirBuf, kMaxIir * sizeof(int32_t));
    }

    void twoNoiseChannels(Substream& s) {
        uint32_t seed = s.seed;
        const int maxChan = s.maxMatrixChannel;
        for (int i = 0; i < s.blockpos; i++) {
            const uint16_t shr7 = uint16_t(seed >> 7);
            samples[i][maxChan + 1] = int8_t(seed >> 15) * (1 << s.noiseShift);
            samples[i][maxChan + 2] = int8_t(shr7) * (1 << s.noiseShift);
            seed = (seed << 16) ^ shr7 ^ (uint32_t(shr7) << 5);
        }
        s.seed = seed;
    }

    void fillNoise(Substream& s) {
        uint32_t seed = s.seed;
        for (int i = 0; i < unitPow2; i++) {
            const uint8_t shr15 = uint8_t(seed >> 15);
            noise[i] = kNoise[shr15];
            seed = (seed << 8) ^ shr15 ^ (uint32_t(shr15) << 5);
        }
        s.seed = seed;
    }

    bool output(Substream& s, std::vector<float>& out) {
        if (channels != s.maxMatrixChannel + 1 || !s.blockpos) return false;
        int maxChan = s.maxMatrixChannel;
        if (!s.noiseType) {
            twoNoiseChannels(s);
            maxChan += 2;
        } else {
            fillNoise(s);
        }
        for (int mat = 0; mat < s.matrices; mat++) {
            const int dest = s.matrixOut[mat];
            const int32_t* coeffs = s.matrixCoeff[mat];
            const int32_t mask = int32_t(~0u << s.quantStep[dest]);
            int index = s.matrices - mat;
            const int index2 = 2 * index + 1;
            for (int i = 0; i < s.blockpos; i++) {
                int64_t accum = 0;
                for (int src = 0; src <= maxChan; src++) accum += int64_t(samples[i][src]) * coeffs[src];
                if (s.matrixNoiseShift[mat]) {
                    index &= unitPow2 - 1;
                    accum += noise[index] * (1 << (s.matrixNoiseShift[mat] + 7));
                    index += index2;
                }
                samples[i][dest] = int32_t(((accum >> 14) & mask) + bypassed[i][mat]);
            }
        }
        const size_t at = out.size();
        out.resize(at + size_t(s.blockpos) * size_t(channels));
        float* to = out.data() + at;
        for (int i = 0; i < s.blockpos; i++) {
            for (int ch = 0; ch < channels; ch++) {
                const int mat = s.assign[ch];
                const int32_t sample = int32_t(uint32_t(samples[i][mat]) << s.outputShift[mat]);
                *to++ = wide ? float(int32_t(uint32_t(sample) * 256u)) * (1.0f / 2147483648.0f) : float(int16_t(sample >> 8)) / 32768.0f;
            }
        }
        return true;
    }

    bool wide = true;

    int unitOf(const uint8_t* buf, size_t size, std::vector<float>& out) {
        if (size < 4) return -1;
        const size_t length = size_t((buf[0] << 8 | buf[1]) & 0xfff) * 2;
        if (length < 4 || length > size) return -1;
        BitReader r(buf + 4, length - 4);
        size_t header = 4, subHeader = 0;
        bool majorUnit = false;
        if (r.peek(31) == (0xf8726fbau >> 1)) {
            if (!major(buf + 4, length - 4)) {
                valid = false;
                return int(length);
            }
            r.skip(int64_t(majorSize) * 8);
            majorUnit = true;
            header += size_t(majorSize);
        }
        if (!valid) return int(length);
        uint16_t dataLength[kMaxSubstreams] = {};
        unsigned start = 0;
        for (int i = 0; i < substreams; i++) {
            const bool extraword = r.read(1), nonrestart = r.read(1);
            r.skip(2);
            unsigned end = r.read(12) * 2;
            subHeader += 2;
            if (extraword) {
                if (!truehd) return valid = false, int(length);
                r.skip(16);
                subHeader += 2;
            }
            if (length < header + subHeader || !(nonrestart ^ majorUnit)) return valid = false, int(length);
            if (end + header + subHeader > length) end = unsigned(length - header - subHeader);
            if (end < start) return valid = false, int(length);
            if (i > last) continue;
            dataLength[i] = uint16_t(end - start);
            start = end;
        }
        uint8_t parity = uint8_t(xorBytes(buf, 4) ^ xorBytes(buf + header, subHeader));
        if ((((parity >> 4) ^ parity) & 0xF) != 0xF) return valid = false, int(length);
        const uint8_t* p = buf + header + subHeader;
        for (int i = 0; i <= last; i++) {
            Substream& s = sub[i];
            BitReader b(p, dataLength[i]);
            matrixChanged = 0;
            memset(filterChanged, 0, sizeof filterChanged);
            s.blockpos = 0;
            bool skipRest = false;
            do {
                if (b.read(1)) {
                    if (b.read(1)) {
                        if (!restart(b, p, s, i)) {
                            skipRest = true;
                            break;
                        }
                        s.restartSeen = true;
                    }
                    if (!s.restartSeen || !decodingParams(b, s, i)) {
                        skipRest = true;
                        break;
                    }
                }
                if (!s.restartSeen) {
                    skipRest = true;
                    break;
                }
                const int nb = channels;
                if (((nb == 6 && ((info >> 2) & 0x3) != 0x3) || (nb == 8 && ((info >> 4) & 0x7) != 0x7 && ((info >> 4) & 0x7) != 0x6 && ((info >> 4) & 0x7) != 0x3)) &&
                    i > 0 && i < last && s.minChannel <= sub[i - 1].maxChannel) {
                    skipRest = true;
                    break;
                }
                if (i != last && (s.codedChannels & sub[last].codedChannels)) {
                    skipRest = true;
                    break;
                }
                if (!blockData(b, s)) return -1;
                if (b.position() >= int64_t(dataLength[i]) * 8) return -1;
            } while (!b.read(1));
            if (!skipRest) {
                b.skip((-b.position()) & 15);
                if (int64_t(dataLength[i]) * 8 - b.position() >= 32) {
                    if (b.read(16) != 0xD234) return -1;
                    const unsigned shorten = b.read(16);
                    if (truehd && (shorten & 0x2000)) s.blockpos = uint16_t(s.blockpos - std::min<unsigned>(shorten & 0x1FFF, s.blockpos));
                    else if (!truehd && shorten != 0xD234) return -1;
                    s.endOfStream = true;
                }
            }
            p += dataLength[i];
        }
        if (!output(sub[last], out)) return -1;
        for (int i = 0; i <= last; i++) {
            if (sub[i].endOfStream) {
                sub[i].endOfStream = false;
                valid = false;
            }
        }
        return int(length);
    }

    int64_t endTrim(Reader& file, const Track& track) override {
        const Sample& tail = track.samples.back();
        std::vector<uint8_t> data(tail.size);
        if (data.size() < 4 || !file.read(track, uint32_t(track.samples.size() - 1), data.data())) return 0;
        size_t at = 0, unitAt = 0, unitLength = 0;
        while (at + 4 <= data.size()) {
            const size_t length = size_t((data[at] << 8 | data[at + 1]) & 0xfff) * 2;
            if (length < 4 || length > data.size() - at) break;
            unitAt = at;
            unitLength = length;
            at += length;
        }
        if (!unitLength) return 0;
        const uint8_t* unit = data.data() + unitAt;
        BitReader r(unit + 4, unitLength - 4);
        size_t header = 4;
        if (r.peek(31) == (0xf8726fbau >> 1)) {
            Major m;
            if (!readMajor(unit + 4, unitLength - 4, m)) return 0;
            r.skip(int64_t(m.size) * 8);
            header += size_t(m.size);
        }
        size_t directory = 0, start = 0, end = 0;
        bool check = false;
        for (int i = 0; i <= last && i < substreams; i++) {
            const bool extraword = r.read(1);
            r.skip(1);
            const bool parity = r.read(1);
            r.skip(1);
            const size_t stop = r.read(12) * 2;
            directory += extraword ? 4 : 2;
            if (extraword) r.skip(16);
            start = end;
            end = stop;
            check = parity;
        }
        for (int i = last + 1; i < substreams; i++) {
            directory += r.read(1) ? 4 : 2;
            r.skip(15);
        }
        const size_t base = unitAt + header + directory;
        const size_t words = end - start;
        const size_t tailBytes = check ? 6 : 4;
        if (words < tailBytes || base + end > data.size()) return 0;
        const uint8_t* mark = data.data() + base + end - tailBytes;
        if ((mark[0] << 8 | mark[1]) != 0xD234) return 0;
        const unsigned shorten = unsigned(mark[2] << 8 | mark[3]);
        return truehd && (shorten & 0x2000) ? int64_t(shorten & 0x1FFF) : 0;
    }

    void feed(const std::vector<uint8_t>& packet, std::vector<float>& out) override {
        size_t at = 0;
        while (at + 4 <= packet.size()) {
            const int used = unitOf(packet.data() + at, packet.size() - at, out);
            if (used <= 0) {
                const size_t length = size_t((packet[at] << 8 | packet[at + 1]) & 0xfff) * 2;
                if (length < 4) break;
                at += length;
                continue;
            }
            at += size_t(used);
        }
    }
};

bool firstMajor(const uint8_t* p, size_t n, Major& m) {
    for (size_t at = 0; at + 4 <= n;) {
        const size_t length = size_t((p[at] << 8 | p[at + 1]) & 0xfff) * 2;
        if (length < 4 || length > n - at) return false;
        if (readMajor(p + at + 4, length - 4, m)) return true;
        at += length;
    }
    return false;
}

}

uint32_t mlpUnitSamples(const uint8_t* p, size_t have, size_t size, int rate) {
    if (have < 2 || rate <= 0) return 0;
    const size_t length = size_t((p[0] << 8 | p[1]) & 0xfff) * 2;
    if (length != size) return 0;
    const int base = rate % 44100 == 0 ? 44100 : 48000;
    const int factor = rate / base;
    return factor == 1 ? 40 : factor == 2 ? 80 : factor == 4 ? 160 : 0;
}

std::unique_ptr<PacketDecoder> openTruehdAudio(const Track&, const std::vector<uint8_t>& firstPacket, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    Major m;
    if (!firstMajor(firstPacket.data(), firstPacket.size(), m)) return fail("TrueHD stream does not start with a major sync");
    auto audio = std::make_unique<TruehdAudio>();
    audio->truehd = m.type == 0xba;
    audio->apply(m);
    const int last = std::min(m.substreams - 1, 2);
    const uint64_t mask = audio->sub[last].mask;
    audio->rate = m.rate;
    audio->channels = std::popcount(mask);
    audio->layout = mask;
    audio->wide = m.bits > 16;
    audio->prerollPackets = 2;
    audio->prerollFrames = 128 * m.unit;
    audio->longestPacket = 0;
    audio->reset();
    if (audio->channels < 1 || audio->channels > kMaxChannels) return fail("TrueHD layout not supported");
    return audio;
}

}

#include "soft_video.h"
#include "bits.h"
#include "parallel.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace absent {

namespace {

constexpr uint8_t kProgressiveOrder[64] = {
    0, 1, 8, 9, 2, 3, 10, 11, 16, 17, 24, 25, 18, 19, 26, 27, 4, 5, 12, 20, 13, 6, 7, 14, 21, 28, 29, 22, 15, 23, 30, 31,
    32, 33, 40, 48, 41, 34, 35, 42, 49, 56, 57, 50, 43, 36, 37, 44, 51, 58, 59, 52, 45, 38, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
constexpr uint8_t kInterlacedOrder[64] = {
    0, 8, 1, 9, 16, 24, 17, 25, 2, 10, 3, 11, 18, 26, 19, 27, 32, 40, 33, 34, 41, 48, 56, 49, 42, 35, 43, 50, 57, 58, 51, 59,
    4, 12, 5, 6, 13, 20, 28, 21, 14, 7, 15, 22, 29, 36, 44, 37, 30, 23, 31, 38, 45, 52, 60, 53, 46, 39, 47, 54, 61, 62, 55, 63};

constexpr std::array<uint8_t, 64> transposed(const uint8_t (&order)[64]) {
    std::array<uint8_t, 64> out{};
    for (int i = 0; i < 64; i++) out[size_t(i)] = uint8_t((order[i] & 7) << 3 | order[i] >> 3);
    return out;
}
constexpr std::array<uint8_t, 64> kProgressiveScan = transposed(kProgressiveOrder), kInterlacedScan = transposed(kInterlacedOrder);
constexpr uint8_t kDcCodebook[7] = {0x04, 0x28, 0x28, 0x4D, 0x4D, 0x70, 0x70};
constexpr uint8_t kRunToCodebook[16] = {0x06, 0x06, 0x05, 0x05, 0x04, 0x29, 0x29, 0x29, 0x29, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x4C};
constexpr uint8_t kLevelToCodebook[10] = {0x04, 0x0A, 0x05, 0x06, 0x04, 0x28, 0x28, 0x28, 0x28, 0x4C};

inline unsigned be16(const uint8_t* p) { return unsigned(p[0]) << 8 | p[1]; }
inline uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline int16_t saturate16(int value) { return int16_t(std::clamp(value, -32768, 32767)); }

bool codeword(BitReader& in, unsigned codebook, unsigned& value) {
    const uint32_t cache = in.peek(32);
    const unsigned switchBits = codebook & 3, riceOrder = codebook >> 5, expOrder = (codebook >> 2) & 7;
    const unsigned q = cache ? unsigned(__builtin_clz(cache)) : 31;
    if (q > switchBits) {
        const unsigned bits = expOrder - switchBits + (q << 1);
        if (bits > 31) return false;
        value = in.peek(int(bits)) - (1u << expOrder) + ((switchBits + 1) << riceOrder);
        in.skip(bits);
    } else if (riceOrder) {
        in.skip(q + 1);
        value = (q << riceOrder) + in.peek(int(riceOrder));
        in.skip(riceOrder);
    } else {
        value = q;
        in.skip(q + 1);
    }
    return true;
}

bool dcCoefficients(BitReader& in, int16_t* out, int blocks) {
    unsigned code;
    if (!codeword(in, 0xB8, code)) return false;
    int16_t previous = int16_t(int(code >> 1) ^ -int(code & 1));
    out[0] = previous;
    code = 5;
    int sign = 0;
    for (int i = 1; i < blocks; i++) {
        if (!codeword(in, kDcCodebook[std::min(code, 6u)], code)) return false;
        if (code) sign ^= -int(code & 1);
        else sign = 0;
        previous = int16_t(previous + ((int((code + 1) >> 1) ^ sign) - sign));
        out[i * 64] = previous;
    }
    return true;
}

bool acCoefficients(BitReader& in, int16_t* out, int blocks, const uint8_t* scan) {
    const int log2Blocks = 31 - __builtin_clz(unsigned(blocks));
    const unsigned most = 64u << log2Blocks, mask = unsigned(blocks) - 1;
    unsigned run = 4, level = 2;
    for (unsigned pos = mask;;) {
        const int64_t left = in.left();
        if (left <= 0 || (left < 32 && !in.peek(int(left)))) break;
        if (!codeword(in, kRunToCodebook[std::min(run, 15u)], run)) return false;
        pos += run + 1;
        if (pos >= most) return false;
        if (!codeword(in, kLevelToCodebook[std::min(level, 9u)], level)) return false;
        level += 1;
        const int sign = in.read(1) ? -1 : 0;
        out[((pos & mask) << 6) + scan[pos >> log2Blocks]] = int16_t((int(level) ^ sign) - sign);
    }
    return true;
}

template <int Bits, int Bias, int Offset, unsigned Round, int Shift, bool Saturate, bool Shortcut>
void idctPass(int16_t* b) {
    constexpr int w1 = Bits == 10 ? 22725 : 45451, w2 = Bits == 10 ? 21407 : 42813, w3 = Bits == 10 ? 19265 : 38531, w4 = Bits == 10 ? 16384 : 32767;
    constexpr int w5 = Bits == 10 ? 12873 : 25746, w6 = Bits == 10 ? 8867 : 17734, w7 = Bits == 10 ? 4520 : 9041;
    for (int i = 0; i < 8; i++) {
        const int r0 = int16_t(b[i] + Bias) + Offset, r1 = b[8 + i], r2 = b[16 + i], r3 = b[24 + i], r4 = b[32 + i], r5 = b[40 + i], r6 = b[48 + i], r7 = b[56 + i];
        const unsigned a0 = unsigned(w4 * r0 + w2 * r2) + unsigned(w4 * r4 + w6 * r6) + Round;
        const unsigned a1 = unsigned(w4 * r0 + w6 * r2) - unsigned(w4 * r4 + w2 * r6) + Round;
        const unsigned a2 = unsigned(w4 * r0 - w6 * r2) - unsigned(w4 * r4 - w2 * r6) + Round;
        const unsigned a3 = unsigned(w4 * r0 - w2 * r2) + unsigned(w4 * r4 - w6 * r6) + Round;
        const unsigned b0 = unsigned(w1 * r1 + w3 * r3) + unsigned(w5 * r5 + w7 * r7);
        const unsigned b1 = unsigned(w3 * r1 - w7 * r3) - unsigned(w1 * r5 + w5 * r7);
        const unsigned b2 = unsigned(w5 * r1 - w1 * r3) + unsigned(w7 * r5 + w3 * r7);
        const unsigned b3 = unsigned(w7 * r1 - w5 * r3) + unsigned(w3 * r5 - w1 * r7);
        const unsigned sums[8] = {a0 + b0, a1 + b1, a2 + b2, a3 + b3, a3 - b3, a2 - b2, a1 - b1, a0 - b0};
        const bool flat = Shortcut && !(r1 | r2 | r3 | r4 | r5 | r6 | r7);
        for (int k = 0; k < 8; k++) {
            const int16_t value = Saturate ? saturate16(int(sums[k]) >> Shift) : int16_t(int(sums[k]) >> Shift);
            b[k * 8 + i] = flat ? int16_t((r0 + 1) >> 1) : value;
        }
    }
}

void transpose(int16_t* b) {
    for (int i = 0; i < 8; i++)
        for (int j = i + 1; j < 8; j++) std::swap(b[i * 8 + j], b[j * 8 + i]);
}

template <int Bits>
void idctPut(uint16_t* out, size_t stride, int16_t* block, const int16_t* qmat) {
    for (int i = 0; i < 64; i++) block[i] = int16_t(block[i] * qmat[i]);
    if constexpr (Bits == 10) idctPass<10, 1, 0, 0, 15, true, false>(block);
    else idctPass<12, 0, 0, 1u << 15, 16, false, true>(block);
    transpose(block);
    if constexpr (Bits == 10) idctPass<10, 0x2008, 0, 0, 18, true, false>(block);
    else idctPass<12, 8192, 2, 0, 17, false, false>(block);
    for (int y = 0; y < 8; y++, out += stride)
        for (int x = 0; x < 8; x++) out[x] = uint16_t(std::clamp<int>(block[y * 8 + x], 4, (1 << Bits) - 5));
}

void unpackAlpha(BitReader& in, uint16_t* out, int count, int bits, int precision) {
    const int mask = (1 << bits) - 1;
    auto scaled = [&](int a) { return uint16_t(bits == 16 ? a >> (precision == 10 ? 6 : 4) : precision == 10 ? (a << 2) | (a >> 6) : (a << 4) | (a >> 4)); };
    int at = 0, alpha = mask;
    do {
        do {
            int value;
            if (in.read(1)) {
                value = int(in.read(bits));
            } else {
                value = int(in.read(bits == 16 ? 7 : 4));
                const int sign = value & 1;
                value = (value + 2) >> 1;
                if (sign) value = -value;
            }
            alpha = (alpha + value) & mask;
            out[at++] = scaled(alpha);
            if (at >= count) break;
        } while (in.left() > 0 && in.read(1));
        int run = int(in.read(4));
        if (!run) run = int(in.read(11));
        run = std::min(run, count - at);
        std::fill(out + at, out + at + run, scaled(alpha));
        at += run;
    } while (at < count);
}

struct Slice {
    const uint8_t* data = nullptr;
    unsigned x = 0, y = 0, count = 0, size = 0;
};

class ProResDecoder : public SoftDecoder {
public:
    explicit ProResDecoder(int bits) : bits(bits) {}

    const char* name() const override { return "prores"; }
    void reset() override {}

    bool decode(const uint8_t* data, size_t size, int64_t stamp, const PictureSink& shown) override {
        if (size < 28 || size > (1u << 30) || memcmp(data + 4, "icpf", 4) != 0) return false;
        const uint8_t* buf = data + 8;
        const int available = int(size - 8);
        const int header = frameHeader(buf, available);
        if (header < 0) return false;
        const uint8_t* at = buf + header;
        int left = available - header;
        firstField = true;
        for (;;) {
            const int picture = pictureHeader(at, left);
            if (picture < 0 || !decodeSlices()) return false;
            at += picture;
            left -= picture;
            if (!frameType || left <= 0 || !firstField) break;
            firstField = false;
        }
        emit(stamp, shown);
        return true;
    }

private:
    int bits;
    int width = 0, height = 0, frameType = 0, alphaInfo = 0;
    bool full444 = false, firstField = true;
    Color color;
    uint8_t qmatLuma[64] = {}, qmatChroma[64] = {};
    const uint8_t* scan = kProgressiveScan.data();
    unsigned mbWidth = 0, mbHeight = 0, sliceMbWidth = 1;
    std::vector<Slice> slices;
    std::vector<uint16_t> planes[4];
    size_t stride[4] = {};
    int rows = 0;
    std::vector<uint8_t> veil;

    int frameHeader(const uint8_t* buf, int available) {
        const int header = int(be16(buf));
        if (header > available || be16(buf + 2) > 1) return -1;
        const int w = int(be16(buf + 8)), h = int(be16(buf + 10));
        if (w <= 0 || h <= 0) return -1;
        const int type = (buf[12] >> 2) & 3;
        const int alpha = buf[17] & 0xF;
        if (alpha > 2) return -1;
        width = w;
        height = h;
        frameType = type;
        alphaInfo = alpha;
        full444 = (buf[12] & 0xC0) == 0xC0;
        scan = frameType ? kInterlacedScan.data() : kProgressiveScan.data();
        const int primaries = buf[14], transfer = buf[15], matrix = buf[16];
        auto told = [](int v) { return v != 0 && v != 2; };
        color = Color();
        color.known = told(primaries) || told(transfer) || told(matrix);
        if (color.known) {
            color.primaries = primaries;
            color.transfer = transfer;
            color.matrix = matrix;
        }
        const uint8_t* ptr = buf + 20;
        const int flags = buf[19];
        if (flags & 2) {
            if (buf + available - ptr < 64) return -1;
            memcpy(qmatLuma, ptr, 64);
            ptr += 64;
        } else {
            memset(qmatLuma, 4, 64);
        }
        if (flags & 1) {
            if (buf + available - ptr < 64) return -1;
            memcpy(qmatChroma, ptr, 64);
        } else {
            memcpy(qmatChroma, qmatLuma, 64);
        }
        return header;
    }

    int pictureHeader(const uint8_t* buf, int available) {
        if (available < 8) return -1;
        const int header = buf[0] >> 3;
        if (header < 8 || header > available) return -1;
        const uint32_t pictureSize = be32(buf + 1);
        if (pictureSize > uint32_t(available)) return -1;
        const int log2Width = buf[7] >> 4, log2Height = buf[7] & 0xF;
        if (log2Width > 3 || log2Height) return -1;
        sliceMbWidth = 1u << log2Width;
        mbWidth = unsigned(width + 15) >> 4;
        mbHeight = frameType ? unsigned(height + 31) >> 5 : unsigned(height + 15) >> 4;
        const unsigned count = mbHeight * ((mbWidth >> log2Width) + unsigned(__builtin_popcount(mbWidth & (sliceMbWidth - 1))));
        if (!count || int64_t(header) + int64_t(count) * 2 > available) return -1;
        slices.resize(count);
        const uint8_t* index = buf + header;
        const uint8_t* data = index + size_t(count) * 2;
        unsigned sliceMbs = sliceMbWidth, x = 0, y = 0;
        for (unsigned i = 0; i < count; i++) {
            Slice& s = slices[i];
            s.data = data;
            data += be16(index + 2 * i);
            while (mbWidth - x < sliceMbs) sliceMbs >>= 1;
            s.x = x;
            s.y = y;
            s.count = sliceMbs;
            s.size = unsigned(data - s.data);
            if (s.size < 6) return -1;
            x += sliceMbs;
            if (x == mbWidth) {
                sliceMbs = sliceMbWidth;
                x = 0;
                y++;
            }
            if (data > buf + available) return -1;
        }
        if (x || y != mbHeight) return -1;
        shapePlanes();
        return int(pictureSize);
    }

    void shapePlanes() {
        stride[0] = stride[3] = size_t(mbWidth) * 16;
        stride[1] = stride[2] = full444 ? stride[0] : stride[0] / 2;
        rows = int(mbHeight) * 16 * (frameType ? 2 : 1);
        for (int i = 0; i < 4; i++) {
            const size_t wanted = stride[i] * size_t(rows);
            if ((i < 3 || alphaInfo) && planes[i].size() != wanted) planes[i].assign(wanted, 0);
        }
    }

    bool decodeSlices() {
        std::vector<char> failed(slices.size(), 0);
        parallelFor(slices.size(), [&](size_t i) { failed[i] = !decodeSlice(slices[i]); });
        return std::count(failed.begin(), failed.end(), 1) < std::ptrdiff_t(slices.size());
    }

    void idct(uint16_t* out, size_t pitch, int16_t* block, const int16_t* qmat) const {
        if (bits == 10) idctPut<10>(out, pitch, block, qmat);
        else idctPut<12>(out, pitch, block, qmat);
    }

    bool decodeSlice(const Slice& slice) {
        const uint8_t* buf = slice.data;
        const int header = buf[0] >> 3;
        if (header > int(slice.size)) return false;
        int qscale = std::clamp<int>(buf[1], 1, 224);
        qscale = qscale > 128 ? (qscale - 96) << 2 : qscale;
        const int ySize = int(be16(buf + 2)), uSize = int(be16(buf + 4));
        int vSize = int(slice.size) - ySize - uSize - header;
        if (header > 7) vSize = int(be16(buf + 6));
        const int aSize = int(slice.size) - ySize - uSize - vSize - header;
        if (vSize < 0 || header + ySize + uSize + vSize > int(slice.size)) return false;
        buf += header;
        int16_t lumaScaled[64], chromaScaled[64];
        for (int i = 0; i < 64; i++) {
            lumaScaled[(i & 7) << 3 | i >> 3] = int16_t(qmatLuma[i] * qscale);
            chromaScaled[(i & 7) << 3 | i >> 3] = int16_t(qmatChroma[i] * qscale);
        }
        const bool field = frameType != 0;
        const bool lower = field && (firstField != (frameType == 1));
        const size_t lumaPitch = stride[0] << (field ? 1 : 0), chromaPitch = stride[1] << (field ? 1 : 0);
        const size_t firstRow = size_t(slice.y) << 4;
        uint16_t* y = planes[0].data() + firstRow * lumaPitch + (size_t(slice.x) << 4) + (lower ? stride[0] : 0);
        const size_t chromaX = size_t(slice.x) << (full444 ? 4 : 3);
        uint16_t* u = planes[1].data() + firstRow * chromaPitch + chromaX + (lower ? stride[1] : 0);
        uint16_t* v = planes[2].data() + firstRow * chromaPitch + chromaX + (lower ? stride[1] : 0);
        alignas(32) int16_t blocks[8 * 4 * 64];
        const int lumaBlocks = int(slice.count) << 2;
        memset(blocks, 0, sizeof(int16_t) * 64 * size_t(lumaBlocks));
        BitReader lumaBits(buf, size_t(ySize));
        if (!dcCoefficients(lumaBits, blocks, lumaBlocks) || !acCoefficients(lumaBits, blocks, lumaBlocks, scan)) return false;
        int16_t* block = blocks;
        for (unsigned i = 0; i < slice.count; i++, block += 256, y += 16) {
            idct(y, lumaPitch, block, lumaScaled);
            idct(y + 8, lumaPitch, block + 64, lumaScaled);
            idct(y + 8 * lumaPitch, lumaPitch, block + 128, lumaScaled);
            idct(y + 8 * lumaPitch + 8, lumaPitch, block + 192, lumaScaled);
        }
        const int log2PerMb = full444 ? 2 : 1;
        const int chromaBlocks = int(slice.count) << log2PerMb;
        const uint8_t* chromaData[2] = {buf + ySize, buf + ySize + uSize};
        const int chromaSize[2] = {uSize, vSize};
        uint16_t* chromaOut[2] = {u, v};
        for (int plane = 0; plane < 2; plane++) {
            memset(blocks, 0, sizeof(int16_t) * 64 * size_t(chromaBlocks));
            if (chromaSize[plane]) {
                BitReader in(chromaData[plane], size_t(chromaSize[plane]));
                if (!dcCoefficients(in, blocks, chromaBlocks) || !acCoefficients(in, blocks, chromaBlocks, scan)) return false;
            }
            block = blocks;
            uint16_t* out = chromaOut[plane];
            for (unsigned i = 0; i < slice.count; i++) {
                for (int j = 0; j < log2PerMb; j++, block += 128, out += 8) {
                    idct(out, chromaPitch, block, chromaScaled);
                    idct(out + 8 * chromaPitch, chromaPitch, block + 64, chromaScaled);
                }
            }
        }
        if (alphaInfo && aSize > 0) {
            uint16_t* a = planes[3].data() + firstRow * lumaPitch + (size_t(slice.x) << 4) + (lower ? stride[3] : 0);
            const int count = int(slice.count) * 256;
            alignas(32) uint16_t values[8 * 4 * 64];
            BitReader in(buf + ySize + uSize + vSize, size_t(aSize));
            unpackAlpha(in, values, count, alphaInfo == 2 ? 16 : 8, bits);
            const size_t across = size_t(slice.count) * 16;
            for (int row = 0; row < 16; row++) memcpy(a + size_t(row) * lumaPitch, values + size_t(row) * across, across * 2);
        }
        return true;
    }

    void emit(int64_t stamp, const PictureSink& shown) {
        Picture p;
        p.stamp = stamp;
        p.width = width;
        p.height = height;
        p.chromaWidth = full444 ? width : (width + 1) / 2;
        p.chromaHeight = height;
        p.bits = bits;
        p.color = color;
        for (int i = 0; i < 3; i++) {
            p.plane[i] = reinterpret_cast<const uint8_t*>(planes[i].data());
            p.stride[i] = stride[i] * 2;
        }
        if (alphaInfo) {
            veil.resize(size_t(width) * size_t(height));
            const int drop = bits - 8;
            for (int row = 0; row < height; row++) {
                const uint16_t* from = planes[3].data() + size_t(row) * stride[3];
                uint8_t* to = veil.data() + size_t(row) * size_t(width);
                for (int x = 0; x < width; x++) to[x] = uint8_t(std::min((from[x] + (1 << (drop - 1))) >> drop, 255));
            }
            p.alpha = veil.data();
            p.alphaStride = size_t(width);
        }
        shown(p);
    }
};

}

std::unique_ptr<SoftDecoder> openProRes(uint32_t codec, std::string* error) {
    const bool twelve = codec == fourcc("ap4h") || codec == fourcc("ap4x");
    if (!twelve && codec != fourcc("apch") && codec != fourcc("apcn") && codec != fourcc("apcs") && codec != fourcc("apco")) {
        if (error) *error = "not a ProRes track";
        return nullptr;
    }
    return std::make_unique<ProResDecoder>(twelve ? 12 : 10);
}

}

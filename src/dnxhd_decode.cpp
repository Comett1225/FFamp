#include "own_video.h"
#include "dnxhd_tables.h"
#include "h263.h"
#include "read_common.h"
#include "vlc.h"
#include <atomic>

namespace absent {

namespace {

const uint8_t kZigzag[64] = {0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

template <int Depth>
struct SimpleIdct {
    static constexpr int W1 = Depth == 12 ? 45451 : 22725, W2 = Depth == 12 ? 42813 : 21407, W3 = Depth == 12 ? 38531 : Depth == 10 ? 19265 : 19266;
    static constexpr int W4 = Depth == 12 ? 32767 : Depth == 10 ? 16384 : 16383, W5 = Depth == 12 ? 25746 : 12873, W6 = Depth == 12 ? 17734 : 8867;
    static constexpr int W7 = Depth == 12 ? 9041 : 4520;
    static constexpr int RowShift = Depth == 12 ? 16 : Depth == 10 ? 12 : 11, ColShift = Depth == 12 ? 17 : Depth == 10 ? 19 : 20;
    static constexpr int DcShift = Depth == 12 ? -1 : Depth == 10 ? 2 : 3;

    static void row(int16_t* r) {
        if (!(r[1] | r[2] | r[3] | r[4] | r[5] | r[6] | r[7])) {
            const int16_t dc = DcShift >= 0 ? int16_t(unsigned(r[0]) << (DcShift > 0 ? DcShift : 0)) : int16_t((r[0] + 1) >> 1);
            for (int i = 0; i < 8; ++i) r[i] = dc;
            return;
        }
        unsigned a0 = unsigned(W4) * unsigned(r[0]) + (1u << (RowShift - 1)), a1 = a0, a2 = a0, a3 = a0;
        a0 += unsigned(W2) * unsigned(r[2]);
        a1 += unsigned(W6) * unsigned(r[2]);
        a2 -= unsigned(W6) * unsigned(r[2]);
        a3 -= unsigned(W2) * unsigned(r[2]);
        unsigned b0 = unsigned(W1) * unsigned(r[1]) + unsigned(W3) * unsigned(r[3]);
        unsigned b1 = unsigned(W3) * unsigned(r[1]) - unsigned(W7) * unsigned(r[3]);
        unsigned b2 = unsigned(W5) * unsigned(r[1]) - unsigned(W1) * unsigned(r[3]);
        unsigned b3 = unsigned(W7) * unsigned(r[1]) - unsigned(W5) * unsigned(r[3]);
        if (r[4] | r[5] | r[6] | r[7]) {
            a0 += unsigned(W4) * unsigned(r[4]) + unsigned(W6) * unsigned(r[6]);
            a1 += unsigned(-W4) * unsigned(r[4]) - unsigned(W2) * unsigned(r[6]);
            a2 += unsigned(-W4) * unsigned(r[4]) + unsigned(W2) * unsigned(r[6]);
            a3 += unsigned(W4) * unsigned(r[4]) - unsigned(W6) * unsigned(r[6]);
            b0 += unsigned(W5) * unsigned(r[5]) + unsigned(W7) * unsigned(r[7]);
            b1 += unsigned(-W1) * unsigned(r[5]) + unsigned(-W5) * unsigned(r[7]);
            b2 += unsigned(W7) * unsigned(r[5]) + unsigned(W3) * unsigned(r[7]);
            b3 += unsigned(W3) * unsigned(r[5]) + unsigned(-W1) * unsigned(r[7]);
        }
        r[0] = int16_t(int(a0 + b0) >> RowShift);
        r[7] = int16_t(int(a0 - b0) >> RowShift);
        r[1] = int16_t(int(a1 + b1) >> RowShift);
        r[6] = int16_t(int(a1 - b1) >> RowShift);
        r[2] = int16_t(int(a2 + b2) >> RowShift);
        r[5] = int16_t(int(a2 - b2) >> RowShift);
        r[3] = int16_t(int(a3 + b3) >> RowShift);
        r[4] = int16_t(int(a3 - b3) >> RowShift);
    }

    template <typename Pixel>
    static void column(Pixel* dest, ptrdiff_t stride, const int16_t* c) {
        unsigned a0 = unsigned(W4) * unsigned(c[0] + ((1 << (ColShift - 1)) / W4)), a1 = a0, a2 = a0, a3 = a0;
        a0 += unsigned(W2) * unsigned(c[16]);
        a1 += unsigned(W6) * unsigned(c[16]);
        a2 += unsigned(-W6) * unsigned(c[16]);
        a3 += unsigned(-W2) * unsigned(c[16]);
        unsigned b0 = unsigned(W1) * unsigned(c[8]), b1 = unsigned(W3) * unsigned(c[8]), b2 = unsigned(W5) * unsigned(c[8]), b3 = unsigned(W7) * unsigned(c[8]);
        b0 += unsigned(W3) * unsigned(c[24]);
        b1 += unsigned(-W7) * unsigned(c[24]);
        b2 += unsigned(-W1) * unsigned(c[24]);
        b3 += unsigned(-W5) * unsigned(c[24]);
        if (c[32]) {
            a0 += unsigned(W4) * unsigned(c[32]);
            a1 += unsigned(-W4) * unsigned(c[32]);
            a2 += unsigned(-W4) * unsigned(c[32]);
            a3 += unsigned(W4) * unsigned(c[32]);
        }
        if (c[40]) {
            b0 += unsigned(W5) * unsigned(c[40]);
            b1 += unsigned(-W1) * unsigned(c[40]);
            b2 += unsigned(W7) * unsigned(c[40]);
            b3 += unsigned(W3) * unsigned(c[40]);
        }
        if (c[48]) {
            a0 += unsigned(W6) * unsigned(c[48]);
            a1 += unsigned(-W2) * unsigned(c[48]);
            a2 += unsigned(W2) * unsigned(c[48]);
            a3 += unsigned(-W6) * unsigned(c[48]);
        }
        if (c[56]) {
            b0 += unsigned(W7) * unsigned(c[56]);
            b1 += unsigned(-W5) * unsigned(c[56]);
            b2 += unsigned(W3) * unsigned(c[56]);
            b3 += unsigned(-W1) * unsigned(c[56]);
        }
        const int sums[8] = {int(a0 + b0), int(a1 + b1), int(a2 + b2), int(a3 + b3), int(a3 - b3), int(a2 - b2), int(a1 - b1), int(a0 - b0)};
        for (int k = 0; k < 8; ++k) dest[k * stride] = Pixel(std::clamp(sums[k] >> ColShift, 0, (1 << Depth) - 1));
    }

    static void put(uint8_t* dest, ptrdiff_t stride, int16_t* block) {
        for (int i = 0; i < 8; ++i) row(block + 8 * i);
        if constexpr (Depth == 8) {
            for (int i = 0; i < 8; ++i) column(dest + i, stride, block + i);
        } else {
            uint16_t* wide = reinterpret_cast<uint16_t*>(dest);
            for (int i = 0; i < 8; ++i) column(wide + i, stride / 2, block + i);
        }
    }
};

void put8(uint8_t* dest, ptrdiff_t stride, int16_t* block) {
#if defined(__ARM_NEON)
    vc1::neonSimpleIdctPut(dest, stride, block);
#else
    SimpleIdct<8>::put(dest, stride, block);
#endif
}

struct DnxhdDecoder : SoftDecoder {
    const dnxhd::CidEntry* table = nullptr;
    int cid = -1;
    int depth = 0;
    Vlc ac, dc, run;
    Planes out;

    const char* name() const override { return "dnxhd"; }
    void reset() override {}

    bool tables(int id, int bits) {
        if (id == cid) return true;
        const dnxhd::CidEntry* entry = dnxhd::cidEntry(id);
        if (!entry || (entry->bitDepth != bits && entry->bitDepth != 0)) return false;
        std::vector<uint32_t> codes(257);
        for (int i = 0; i < 257; ++i) codes[size_t(i)] = entry->acCodes[i];
        if (!ac.build(9, 257, entry->acBits, codes.data())) return false;
        const int dcCount = bits > 8 ? 14 : 12;
        for (int i = 0; i < dcCount; ++i) codes[size_t(i)] = entry->dcCodes[i];
        if (!dc.build(7, size_t(dcCount), entry->dcBits, codes.data())) return false;
        std::vector<int32_t> symbols(62);
        for (int i = 0; i < 62; ++i) {
            codes[size_t(i)] = entry->runCodes[i];
            symbols[size_t(i)] = entry->run[i];
        }
        if (!run.build(9, 62, entry->runBits, codes.data(), symbols.data())) return false;
        table = entry;
        cid = id;
        return true;
    }

    struct Row {
        alignas(32) int16_t blocks[12][64];
        int lumaScale[64], chromaScale[64];
        int lastDc[3];
        int lastQscale = -1;
    };

    struct Mode {
        int indexBits, levelBias, levelShift, dcShift;
        bool four;
        bool mbaff;
    };

    bool block(const Mode& f, Row& r, BitReader& in, int n) const {
        int16_t* b = r.blocks[n];
        memset(b, 0, sizeof r.blocks[n]);
        int component;
        const int* scale;
        const uint8_t* weight;
        if (!f.four) component = n & 2 ? 1 + (n & 1) : 0;
        else component = (n >> 1) % 3;
        if (component) {
            scale = r.chromaScale;
            weight = table->chromaWeight;
        } else {
            scale = r.lumaScale;
            weight = table->lumaWeight;
        }
        const int length = dc.read(in);
        if (length < 0) return false;
        if (length) {
            int level = int(in.read(length));
            if (!(level >> (length - 1))) level -= (1 << length) - 1;
            r.lastDc[component] += level * (1 << f.dcShift);
        }
        b[0] = int16_t(r.lastDc[component]);
        int i = 0;
        for (int index = ac.read(in); index != table->eobIndex; index = ac.read(in)) {
            if (index < 0) return false;
            int level = table->acInfo[2 * index];
            const int flags = table->acInfo[2 * index + 1];
            const int sign = in.read(1) ? -1 : 0;
            if (flags & 1) level += int(in.read(f.indexBits)) << 7;
            if (flags & 2) {
                const int steps = run.read(in);
                if (steps < 0) return false;
                i += steps;
            }
            if (++i > 63) return false;
            level *= scale[i];
            level += scale[i] >> 1;
            if (f.levelBias < 32 || weight[i] != f.levelBias) level += f.levelBias;
            level >>= f.levelShift;
            b[kZigzag[i]] = int16_t((level ^ sign) - sign);
        }
        return true;
    }

    bool decode(const uint8_t* data, size_t size, int64_t stamp, const PictureSink& sink) override {
        bool first = true, ok = true, firstFour = false;
        int field = 0, firstWidth = 0, firstHeight = 0;
        size_t offset = 0;
        for (;;) {
            const uint8_t* buf = data + offset;
            const size_t left = size - offset;
            if (left < 0x280) return false;
            const uint64_t prefix = uint64_t(be32(buf)) << 16 | uint64_t(buf[4]) << 8;
            const uint64_t place = prefix >> 16;
            const bool hr = (prefix & 0xFFFF0000FFFFull) == 0x0300 && place >= 0x0280 && place <= 0x2170 && !(place & 3);
            if (prefix != 0x000002800100ull && prefix != 0x000002800200ull && !hr) return false;
            const bool interlaced = buf[5] & 2;
            field = interlaced ? (first ? buf[5] & 1 : !field) : 0;
            const bool mbaff = (buf[6] >> 5) & 1;
            int height = int(be16(buf + 0x18)), width = int(be16(buf + 0x1a));
            const int code = buf[0x21] >> 5;
            const int bits = code == 1 ? 8 : code == 2 ? 10 : code == 3 ? 12 : 0;
            if (!bits || !tables(int(be32(buf + 0x28)), bits)) return false;
            const int matrix = (buf[0x2C] >> 1) & 3;
            const bool act = buf[0x2C] & 1, four = (buf[0x2C] >> 6) & 1;
            if (four && bits == 8) return false;
            if (width != int(table->width) && table->width) width = int(table->width);
            if (left < table->codingUnitSize) return false;
            const int mbWidth = (width + 15) >> 4, mbHeight = int(be16(buf + 0x16c));
            if (interlaced && (height + 15) >> 4 == mbHeight) height <<= 1;
            size_t dataOffset = 0x280;
            if (mbHeight > 68 && hr) dataOffset = 0x170 + size_t(mbHeight) * 4;
            else if (mbHeight > 68) return false;
            if ((mbHeight << (interlaced ? 1 : 0)) > (height + 15) >> 4 || left < dataOffset || mbHeight > 512 || width <= 0 || height <= 0) return false;
            if (int64_t(width + 128) * int64_t(height + 128) >= (int64_t(1) << 28)) return false;
            if (first) {
                depth = bits;
                out.shape(((width + 15) & ~15), ((height + 15) & ~15), four ? ((width + 15) & ~15) : ((width + 15) & ~15) / 2, ((height + 15) & ~15), bits, false);
                out.clear();
                out.color = Color();
                out.color.known = true;
                out.color.matrix = four && !act ? 0 : matrix == 1 || matrix == 2 ? 9 : 1;
                out.color.primaries = out.color.matrix == 9 ? 9 : 1;
                out.color.transfer = 1;
                out.color.full = four && !act;
                firstWidth = width;
                firstHeight = height;
                firstFour = four;
            } else if (bits != depth || width != firstWidth || height != firstHeight || four != firstFour || !interlaced) {
                return false;
            }
            std::vector<uint32_t> rows(static_cast<size_t>(mbHeight), 0);
            for (int i = 0; i < mbHeight; ++i) {
                rows[size_t(i)] = be32(buf + 0x170 + size_t(i) * 4);
                if (left - dataOffset < rows[size_t(i)]) return false;
            }
            Mode f;
            f.four = four;
            f.mbaff = mbaff;
            f.indexBits = bits == 8 ? 4 : 6;
            f.levelBias = bits == 8 ? 32 : bits == 10 ? (four || table->cid == 1271 ? 32 : 8) : (four ? 32 : 8);
            f.levelShift = bits == 8 ? 6 : bits == 10 ? (four || table->cid == 1271 ? 6 : 4) : 4;
            f.dcShift = bits == 12 ? 2 : 0;
            const uint8_t* coded = buf + dataOffset;
            const size_t codedSize = left - dataOffset;
            std::atomic<int> errors{0};
            inParallel(mbHeight, [&](int top, int bottom) {
                Row r;
                for (int y = top; y < bottom; ++y) {
                    r.lastDc[0] = r.lastDc[1] = r.lastDc[2] = 1 << (bits + 2);
                    r.lastQscale = -1;
                    BitReader in(coded + rows[size_t(y)], codedSize - rows[size_t(y)]);
                    for (int x = 0; x < mbWidth; ++x) {
                        if (!macroblock(f, r, in, x, y, interlaced, field)) {
                            ++errors;
                            break;
                        }
                    }
                }
            });
            ok = ok && !errors;
            if (first && interlaced) {
                offset += table->codingUnitSize;
                first = false;
                continue;
            }
            Picture p = out.picture(stamp);
            p.width = width;
            p.height = height;
            p.chromaWidth = four ? width : (width + 1) / 2;
            p.chromaHeight = height;
            if (!ok) return false;
            sink(p);
            return true;
        }
    }

    bool macroblock(const Mode& f, Row& r, BitReader& in, int x, int y, bool interlaced, int field) {
        bool fieldMb = false;
        int qscale;
        if (f.mbaff) {
            fieldMb = in.read(1);
            qscale = int(in.read(10));
        } else {
            qscale = int(in.read(11));
        }
        in.read(1);
        if (qscale != r.lastQscale) {
            for (int i = 0; i < 64; ++i) {
                r.lumaScale[i] = qscale * table->lumaWeight[i];
                r.chromaScale[i] = qscale * table->chromaWeight[i];
            }
            r.lastQscale = qscale;
        }
        for (int i = 0; i < 8 + 4 * f.four; ++i)
            if (!block(f, r, in, i)) return false;
        const int unit = depth > 8 ? 2 : 1;
        ptrdiff_t lumaLine = ptrdiff_t(out.stride(0)), chromaLine = ptrdiff_t(out.stride(1));
        if (interlaced) {
            lumaLine <<= 1;
            chromaLine <<= 1;
        }
        uint8_t* dy = out.data[0].data() + (y * lumaLine << 4) + (x << 4) * unit;
        uint8_t* du = out.data[1].data() + (y * chromaLine << 4) + (x << (3 + f.four)) * unit;
        uint8_t* dv = out.data[2].data() + (y * chromaLine << 4) + (x << (3 + f.four)) * unit;
        if (interlaced && field) {
            dy += out.stride(0);
            du += out.stride(1);
            dv += out.stride(2);
        }
        if (fieldMb) {
            lumaLine <<= 1;
            chromaLine <<= 1;
        }
        ptrdiff_t down = fieldMb ? ptrdiff_t(out.stride(0)) : lumaLine << 3;
        const ptrdiff_t across = 8 * unit;
        auto put = [&](uint8_t* dest, ptrdiff_t line, int16_t* b) {
            if (depth == 8) put8(dest, line, b);
            else if (depth == 10) SimpleIdct<10>::put(dest, line, b);
            else SimpleIdct<12>::put(dest, line, b);
        };
        put(dy, lumaLine, r.blocks[0]);
        put(dy + across, lumaLine, r.blocks[1]);
        put(dy + down, lumaLine, r.blocks[f.four ? 6 : 4]);
        put(dy + down + across, lumaLine, r.blocks[f.four ? 7 : 5]);
        down = fieldMb ? ptrdiff_t(out.stride(1)) : chromaLine << 3;
        if (!f.four) {
            put(du, chromaLine, r.blocks[2]);
            put(dv, chromaLine, r.blocks[3]);
            put(du + down, chromaLine, r.blocks[6]);
            put(dv + down, chromaLine, r.blocks[7]);
        } else {
            put(du, chromaLine, r.blocks[2]);
            put(du + across, chromaLine, r.blocks[3]);
            put(du + down, chromaLine, r.blocks[8]);
            put(du + down + across, chromaLine, r.blocks[9]);
            put(dv, chromaLine, r.blocks[4]);
            put(dv + across, chromaLine, r.blocks[5]);
            put(dv + down, chromaLine, r.blocks[10]);
            put(dv + down + across, chromaLine, r.blocks[11]);
        }
        return true;
    }
};

}

std::unique_ptr<SoftDecoder> openDnxhd(std::string*) { return std::make_unique<DnxhdDecoder>(); }

}

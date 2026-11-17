#include "own_video.h"
#include "read_common.h"

namespace absent {

bool inflateZlib(const uint8_t* data, size_t size, std::vector<uint8_t>& out, size_t expected);

namespace {

struct Bits {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t at = 0;
    uint64_t hold = 0;
    int count = 0;
    bool over = false;

    void fill() {
        while (count <= 56) {
            uint64_t next = 0;
            if (at < size) next = data[at];
            else if (at > size + 8) over = true;
            ++at;
            hold |= next << count;
            count += 8;
        }
    }
    uint32_t peek(int n) {
        if (count < n) fill();
        return uint32_t(hold & ((uint64_t(1) << n) - 1));
    }
    void drop(int n) {
        hold >>= n;
        count -= n;
    }
    uint32_t take(int n) {
        if (!n) return 0;
        const uint32_t v = peek(n);
        drop(n);
        return v;
    }
    void align() { drop(count & 7); }
};

struct Huffman {
    std::vector<uint16_t> table;
    int longest = 0;

    bool build(const uint8_t* lengths, int count) {
        int counts[16] = {};
        for (int i = 0; i < count; ++i) counts[lengths[i]]++;
        counts[0] = 0;
        longest = 0;
        for (int n = 1; n < 16; ++n) if (counts[n]) longest = n;
        if (!longest) longest = 1;
        int left = 1;
        for (int n = 1; n < 16; ++n) {
            left = (left << 1) - counts[n];
            if (left < 0) return false;
        }
        int next[16] = {}, code = 0;
        for (int n = 1; n < 16; ++n) {
            code = (code + counts[n - 1]) << 1;
            next[n] = code;
        }
        table.assign(size_t(1) << longest, 0);
        for (int symbol = 0; symbol < count; ++symbol) {
            const int length = lengths[symbol];
            if (!length) continue;
            int c = next[length]++, reversed = 0;
            for (int k = 0; k < length; ++k) reversed |= ((c >> k) & 1) << (length - 1 - k);
            for (int index = reversed; index < (1 << longest); index += 1 << length) table[size_t(index)] = uint16_t(symbol << 4 | length);
        }
        return true;
    }

    int read(Bits& in) const {
        const uint16_t entry = table[in.peek(longest)];
        if (!(entry & 15)) return -1;
        in.drop(entry & 15);
        return entry >> 4;
    }
};

const uint16_t kLengthBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistanceBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistanceExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

}

bool inflateZlib(const uint8_t* data, size_t size, std::vector<uint8_t>& out, size_t expected) {
    out.clear();
    out.reserve(std::min<size_t>(expected, size_t(64) << 20));
    if (size < 2 || (data[0] & 15) != 8 || ((data[0] << 8) | data[1]) % 31 || (data[1] & 0x20)) return false;
    Bits in{data + 2, size - 2};
    Huffman literals, distances;
    for (bool last = false; !last;) {
        last = in.take(1);
        const int kind = int(in.take(2));
        if (in.over) return false;
        if (kind == 0) {
            in.align();
            const uint32_t length = in.take(16), check = in.take(16);
            if ((length ^ 0xFFFF) != check) return false;
            if (out.size() + length > expected) return false;
            for (uint32_t i = 0; i < length; ++i) {
                if (in.over) return false;
                out.push_back(uint8_t(in.take(8)));
            }
            continue;
        }
        if (kind == 3) return false;
        uint8_t lengths[320] = {};
        int literalCount = 288, distanceCount = 30;
        if (kind == 1) {
            for (int i = 0; i < 144; ++i) lengths[i] = 8;
            for (int i = 144; i < 256; ++i) lengths[i] = 9;
            for (int i = 256; i < 280; ++i) lengths[i] = 7;
            for (int i = 280; i < 288; ++i) lengths[i] = 8;
            for (int i = 0; i < 30; ++i) lengths[288 + i] = 5;
        } else {
            literalCount = int(in.take(5)) + 257;
            distanceCount = int(in.take(5)) + 1;
            const int codeCount = int(in.take(4)) + 4;
            static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t codeLengths[19] = {};
            for (int i = 0; i < codeCount; ++i) codeLengths[order[i]] = uint8_t(in.take(3));
            Huffman codes;
            if (!codes.build(codeLengths, 19)) return false;
            uint8_t all[320] = {};
            for (int i = 0; i < literalCount + distanceCount;) {
                const int symbol = codes.read(in);
                if (symbol < 0 || in.over) return false;
                if (symbol < 16) {
                    all[i++] = uint8_t(symbol);
                    continue;
                }
                int repeat = 0, value = 0;
                if (symbol == 16) {
                    if (!i) return false;
                    value = all[i - 1];
                    repeat = 3 + int(in.take(2));
                } else {
                    repeat = symbol == 17 ? 3 + int(in.take(3)) : 11 + int(in.take(7));
                }
                if (i + repeat > literalCount + distanceCount) return false;
                while (repeat--) all[i++] = uint8_t(value);
            }
            memcpy(lengths, all, size_t(literalCount));
            memcpy(lengths + 288, all + literalCount, size_t(distanceCount));
        }
        if (!literals.build(lengths, literalCount) || !distances.build(lengths + 288, distanceCount)) return false;
        for (;;) {
            const int symbol = literals.read(in);
            if (symbol < 0 || in.over) return false;
            if (out.size() > expected) return false;
            if (symbol < 256) {
                out.push_back(uint8_t(symbol));
                continue;
            }
            if (symbol == 256) break;
            if (symbol > 285) return false;
            const size_t length = kLengthBase[symbol - 257] + in.take(kLengthExtra[symbol - 257]);
            const int code = distances.read(in);
            if (code < 0 || code > 29) return false;
            const size_t distance = kDistanceBase[code] + in.take(kDistanceExtra[code]);
            if (distance > out.size()) return false;
            const size_t from = out.size() - distance;
            for (size_t k = 0; k < length; ++k) out.push_back(out[from + k]);
        }
    }
    return true;
}

namespace {

int paeth(int a, int b, int c) {
    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

bool unfilter(uint8_t* rows, size_t rowBytes, int count, int pixelBytes) {
    std::vector<uint8_t> zero(rowBytes, 0);
    const uint8_t* above = zero.data();
    for (int y = 0; y < count; ++y) {
        uint8_t* line = rows + size_t(y) * (rowBytes + 1);
        const int kind = line[0];
        uint8_t* p = line + 1;
        for (size_t x = 0; x < rowBytes; ++x) {
            const int left = x >= size_t(pixelBytes) ? p[x - size_t(pixelBytes)] : 0, up = above[x], corner = x >= size_t(pixelBytes) ? above[x - size_t(pixelBytes)] : 0;
            switch (kind) {
                case 0: break;
                case 1: p[x] = uint8_t(p[x] + left); break;
                case 2: p[x] = uint8_t(p[x] + up); break;
                case 3: p[x] = uint8_t(p[x] + ((left + up) >> 1)); break;
                case 4: p[x] = uint8_t(p[x] + paeth(left, up, corner)); break;
                default: return false;
            }
        }
        above = p;
    }
    return true;
}

struct PngDecoder : SoftDecoder {
    Planes out;
    std::vector<uint8_t> packed;
    std::vector<uint8_t> raw;

    const char* name() const override { return "png"; }
    void reset() override {}

    bool decode(const uint8_t* data, size_t size, int64_t stamp, const PictureSink& sink) override {
        static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
        if (size < 8 || memcmp(data, signature, 8)) return false;
        uint32_t width = 0, height = 0;
        int depth = 0, type = -1, interlace = 0;
        uint8_t palette[256][4];
        for (auto& entry : palette) entry[0] = entry[1] = entry[2] = 0, entry[3] = 255;
        int transparentGray = -1, transparent[3] = {-1, -1, -1};
        packed.clear();
        for (size_t at = 8; at + 12 <= size;) {
            const uint32_t length = be32(data + at), kind = be32(data + at + 4);
            if (length > size - at - 12) return false;
            const uint8_t* body = data + at + 8;
            if (kind == fourcc("IHDR") && length >= 13) {
                width = be32(body);
                height = be32(body + 4);
                depth = body[8];
                type = body[9];
                interlace = body[12];
            } else if (kind == fourcc("PLTE")) {
                for (uint32_t i = 0; i < length / 3 && i < 256; ++i) memcpy(palette[i], body + 3 * i, 3);
            } else if (kind == fourcc("tRNS")) {
                if (type == 3) for (uint32_t i = 0; i < length && i < 256; ++i) palette[i][3] = body[i];
                else if (type == 0 && length >= 2) transparentGray = int(be16(body));
                else if (type == 2 && length >= 6) for (int c = 0; c < 3; ++c) transparent[c] = int(be16(body + 2 * c));
            } else if (kind == fourcc("IDAT")) {
                packed.insert(packed.end(), body, body + length);
            } else if (kind == fourcc("IEND")) {
                break;
            }
            at += size_t(length) + 12;
        }
        const int channels = type == 0 ? 1 : type == 2 ? 3 : type == 3 ? 1 : type == 4 ? 2 : type == 6 ? 4 : 0;
        if (!channels || !width || !height || uint64_t(width + 128) * uint64_t(height + 128) >= (uint64_t(1) << 28) || interlace > 1) return false;
        if (depth != 8 && depth != 16 && !(depth < 8 && (type == 0 || type == 3) && (depth == 1 || depth == 2 || depth == 4))) return false;
        const int bitsPerPixel = channels * depth, pixelBytes = std::max(1, bitsPerPixel / 8);
        static const int startX[7] = {0, 4, 0, 2, 0, 1, 0}, startY[7] = {0, 0, 4, 0, 2, 0, 1}, stepX[7] = {8, 8, 4, 4, 2, 2, 1}, stepY[7] = {8, 8, 8, 4, 4, 2, 2};
        const int passes = interlace ? 7 : 1;
        size_t expected = 0;
        for (int pass = 0; pass < passes; ++pass) {
            const uint32_t w = interlace ? (width - uint32_t(startX[pass]) + uint32_t(stepX[pass]) - 1) / uint32_t(stepX[pass]) : width;
            const uint32_t h = interlace ? (height - uint32_t(startY[pass]) + uint32_t(stepY[pass]) - 1) / uint32_t(stepY[pass]) : height;
            if (w && h) expected += (size_t(w) * size_t(bitsPerPixel) + 7) / 8 * h + h;
        }
        if (!inflateZlib(packed.data(), packed.size(), raw, expected) || raw.size() < expected) return false;
        const bool alpha = type == 4 || type == 6 || (type == 3 && [&] {
            for (const auto& entry : palette) if (entry[3] != 255) return true;
            return false;
        }()) || transparentGray >= 0 || transparent[0] >= 0;
        const int bits = depth == 16 ? 16 : 8;
        out.rgb(int(width), int(height), bits, alpha);
        size_t offset = 0;
        for (int pass = 0; pass < passes; ++pass) {
            const int x0 = interlace ? startX[pass] : 0, y0 = interlace ? startY[pass] : 0, dx = interlace ? stepX[pass] : 1, dy = interlace ? stepY[pass] : 1;
            const uint32_t w = (width - uint32_t(x0) + uint32_t(dx) - 1) / uint32_t(dx), h = (height - uint32_t(y0) + uint32_t(dy) - 1) / uint32_t(dy);
            if (!w || !h) continue;
            const size_t rowBytes = (size_t(w) * size_t(bitsPerPixel) + 7) / 8;
            uint8_t* rows = raw.data() + offset;
            if (!unfilter(rows, rowBytes, int(h), pixelBytes)) return false;
            offset += (rowBytes + 1) * h;
            for (uint32_t row = 0; row < h; ++row) {
                const uint8_t* line = rows + size_t(row) * (rowBytes + 1) + 1;
                const int y = y0 + int(row) * dy;
                for (uint32_t column = 0; column < w; ++column) {
                    const int x = x0 + int(column) * dx;
                    unsigned v[4] = {0, 0, 0, 65535};
                    if (depth < 8) {
                        const size_t bit = size_t(column) * size_t(depth);
                        const unsigned value = (line[bit / 8] >> (8 - depth - int(bit % 8))) & ((1u << depth) - 1);
                        if (type == 3) {
                            for (int c = 0; c < 4; ++c) v[c] = palette[value][c];
                        } else {
                            v[0] = v[1] = v[2] = value * 255 / ((1u << depth) - 1);
                            v[3] = transparentGray == int(value) ? 0 : 255;
                        }
                    } else {
                        unsigned s[4];
                        for (int c = 0; c < channels; ++c) s[c] = depth == 16 ? be16(line + (size_t(column) * size_t(channels) + size_t(c)) * 2) : line[size_t(column) * size_t(channels) + size_t(c)];
                        const unsigned top = depth == 16 ? 65535 : 255;
                        if (type == 3) {
                            for (int c = 0; c < 4; ++c) v[c] = palette[s[0]][c];
                        } else if (type == 0 || type == 4) {
                            v[0] = v[1] = v[2] = s[0];
                            v[3] = type == 4 ? s[1] : transparentGray == int(s[0]) ? 0 : top;
                        } else {
                            v[0] = s[0];
                            v[1] = s[1];
                            v[2] = s[2];
                            v[3] = type == 6 ? s[3] : (int(s[0]) == transparent[0] && int(s[1]) == transparent[1] && int(s[2]) == transparent[2]) ? 0 : top;
                        }
                    }
                    const int order[3] = {1, 2, 0};
                    for (int plane = 0; plane < 3; ++plane) {
                        if (bits == 16) out.wide(plane, y)[x] = uint16_t(v[order[plane]]);
                        else out.row(plane, y)[x] = uint8_t(v[order[plane]]);
                    }
                    if (alpha) {
                        if (bits == 16) out.wide(3, y)[x] = uint16_t(v[3]);
                        else out.row(3, y)[x] = uint8_t(v[3]);
                    }
                }
            }
        }
        sink(out.picture(stamp));
        return true;
    }
};

}

std::unique_ptr<SoftDecoder> openPng(std::string*) { return std::make_unique<PngDecoder>(); }

}

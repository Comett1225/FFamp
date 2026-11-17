#include "own_video.h"
#include "read_common.h"

namespace absent {

namespace {

bool unsnappy(const uint8_t* in, size_t size, uint8_t* out, size_t room, size_t& made) {
    size_t at = 0;
    uint64_t length = 0;
    for (int shift = 0;; shift += 7) {
        if (at >= size || shift > 28) return false;
        const uint8_t b = in[at++];
        length |= uint64_t(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
    }
    if (length > room) return false;
    size_t o = 0;
    while (at < size) {
        const uint8_t tag = in[at++];
        size_t count = 0, offset = 0;
        if ((tag & 3) == 0) {
            count = tag >> 2;
            if (count >= 60) {
                const size_t bytes = count - 59;
                if (at + bytes > size) return false;
                count = 0;
                for (size_t k = 0; k < bytes; ++k) count |= size_t(in[at + k]) << (8 * k);
                at += bytes;
            }
            ++count;
            if (at + count > size || o + count > length) return false;
            memcpy(out + o, in + at, count);
            at += count;
            o += count;
            continue;
        }
        if ((tag & 3) == 1) {
            if (at >= size) return false;
            count = ((tag >> 2) & 7) + 4;
            offset = size_t(tag >> 5) << 8 | in[at++];
        } else if ((tag & 3) == 2) {
            if (at + 2 > size) return false;
            count = (tag >> 2) + 1;
            offset = le16(in + at);
            at += 2;
        } else {
            if (at + 4 > size) return false;
            count = (tag >> 2) + 1;
            offset = le32(in + at);
            at += 4;
        }
        if (!offset || offset > o || o + count > length) return false;
        for (size_t k = 0; k < count; ++k, ++o) out[o] = out[o - offset];
    }
    made = o;
    return o == length;
}

int64_t snappyLength(const uint8_t* in, size_t size) {
    uint64_t length = 0;
    for (size_t at = 0, shift = 0; at < size && shift <= 28; ++at, shift += 7) {
        length |= uint64_t(in[at] & 0x7F) << shift;
        if (!(in[at] & 0x80)) return int64_t(length);
    }
    return -1;
}

void colors565(uint32_t colors[4], unsigned c0, unsigned c1, bool fourColors, uint8_t lastAlpha) {
    auto expand = [](unsigned v, unsigned bits) {
        const int top = (1 << bits) - 1, half = 1 << (bits - 1);
        const int t = int(v) * 255 + half;
        return uint8_t((t / (top + 1) + t) / (top + 1));
    };
    const uint8_t r0 = expand(c0 >> 11, 5), g0 = expand((c0 >> 5) & 63, 6), b0 = expand(c0 & 31, 5);
    const uint8_t r1 = expand(c1 >> 11, 5), g1 = expand((c1 >> 5) & 63, 6), b1 = expand(c1 & 31, 5);
    auto pack = [](int r, int g, int b, unsigned a) { return uint32_t(uint8_t(r)) | uint32_t(uint8_t(g)) << 8 | uint32_t(uint8_t(b)) << 16 | uint32_t(uint8_t(a)) << 24; };
    const unsigned a = fourColors && lastAlpha == 0 ? 0 : 255;
    colors[0] = pack(r0, g0, b0, a);
    colors[1] = pack(r1, g1, b1, a);
    if (fourColors || c0 > c1) {
        colors[2] = pack((2 * r0 + r1) / 3, (2 * g0 + g1) / 3, (2 * b0 + b1) / 3, a);
        colors[3] = pack((2 * r1 + r0) / 3, (2 * g1 + g0) / 3, (2 * b1 + b0) / 3, a);
    } else {
        colors[2] = pack((r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, a);
        colors[3] = pack(0, 0, 0, lastAlpha);
    }
}

void indices3(uint8_t out[16], const uint8_t* in) {
    for (int half = 0; half < 2; ++half) {
        const uint32_t bits = uint32_t(in[3 * half]) | uint32_t(in[3 * half + 1]) << 8 | uint32_t(in[3 * half + 2]) << 16;
        for (int i = 0; i < 8; ++i) out[8 * half + i] = uint8_t((bits >> (3 * i)) & 7);
    }
}

void dxt1(uint32_t px[16], const uint8_t* block) {
    uint32_t colors[4];
    colors565(colors, le16(block), le16(block + 2), false, 255);
    uint32_t code = le32(block + 4);
    for (int i = 0; i < 16; ++i, code >>= 2) px[i] = colors[code & 3];
}

void dxt5(uint32_t px[16], const uint8_t* block) {
    uint32_t colors[4];
    colors565(colors, le16(block + 8), le16(block + 10), true, 0);
    uint8_t codes[16];
    indices3(codes, block + 2);
    const int a0 = block[0], a1 = block[1];
    uint32_t code = le32(block + 12);
    for (int i = 0; i < 16; ++i, code >>= 2) {
        const int k = codes[i];
        int alpha = k == 0 ? a0 : k == 1 ? a1 : a0 > a1 ? ((8 - k) * a0 + (k - 1) * a1) / 7 : k == 6 ? 0 : k == 7 ? 255 : ((6 - k) * a0 + (k - 1) * a1) / 5;
        px[i] = (colors[code & 3] & 0xFFFFFF) | uint32_t(uint8_t(alpha)) << 24;
    }
}

void ycocgScaled(uint32_t px[16]) {
    for (int i = 0; i < 16; ++i) {
        const int r = px[i] & 255, g = (px[i] >> 8) & 255, b = (px[i] >> 16) & 255, a = px[i] >> 24;
        const int s = (b >> 3) + 1, co = (r - 128) / s, cg = (g - 128) / s;
        auto clip = [](int v) { return uint32_t(std::clamp(v, 0, 255)); };
        px[i] = clip(a + co - cg) | clip(a + cg) << 8 | clip(a - co - cg) << 16 | 255u << 24;
    }
}

void rgtc1(uint8_t out[16], const uint8_t* block) {
    const int r0 = block[0], r1 = block[1];
    int table[8] = {r0, r1};
    if (r0 > r1) {
        for (int k = 2; k < 8; ++k) table[k] = ((8 - k) * r0 + (k - 1) * r1) / 7;
    } else {
        for (int k = 2; k < 6; ++k) table[k] = ((6 - k) * r0 + (k - 1) * r1) / 5;
        table[6] = 0;
        table[7] = 255;
    }
    uint8_t codes[16];
    indices3(codes, block + 2);
    for (int i = 0; i < 16; ++i) out[i] = uint8_t(table[codes[i]]);
}

struct Section {
    size_t size = 0;
    int type = 0;
};

bool section(const uint8_t* data, size_t size, size_t& at, Section& s) {
    if (size - at < 4) return false;
    s.size = data[at] | size_t(data[at + 1]) << 8 | size_t(data[at + 2]) << 16;
    s.type = data[at + 3];
    at += 4;
    if (!s.size) {
        if (size - at < 4) return false;
        s.size = le32(data + at);
        at += 4;
    }
    return s.size <= size - at;
}

struct HapDecoder : SoftDecoder {
    uint32_t tag = 0;
    int width = 0;
    int height = 0;
    Planes out;
    std::vector<uint8_t> texture;

    const char* name() const override { return "hap"; }
    void reset() override {}

    bool unpack(const uint8_t* data, size_t size, size_t at, int ratio, int wantFormat, size_t& sectionSize) {
        Section head;
        if (!section(data, size, at, head)) return false;
        sectionSize = head.size;
        if (wantFormat >= 0 && (head.type & 15) != wantFormat) return false;
        struct Chunk {
            int compressor = 0;
            size_t offset = 0, size = 0;
        };
        std::vector<Chunk> chunks;
        const size_t body = at;
        const int kind = head.type & 0xF0;
        if (kind == 0xA0 || kind == 0xB0) {
            chunks.push_back({kind, 0, head.size});
        } else if (kind == 0xC0) {
            Section instructions;
            if (!section(data, size, at, instructions) || instructions.type != 0x01) return false;
            const size_t end = at + instructions.size;
            bool sized = false, compressed = false, placed = false;
            while (at < end) {
                Section part;
                if (!section(data, end, at, part)) return false;
                const size_t count = part.type == 0x02 ? part.size : part.size / 4;
                if (part.type == 0x02 || part.type == 0x03 || part.type == 0x04) {
                    if (chunks.empty()) chunks.resize(count);
                    else if (chunks.size() != count) return false;
                }
                for (size_t i = 0; i < count && (part.type == 0x02 || part.type == 0x03 || part.type == 0x04); ++i) {
                    if (part.type == 0x02) chunks[i].compressor = data[at + i] << 4;
                    else if (part.type == 0x03) chunks[i].size = le32(data + at + 4 * i);
                    else chunks[i].offset = le32(data + at + 4 * i);
                }
                sized |= part.type == 0x03;
                compressed |= part.type == 0x02;
                placed |= part.type == 0x04;
                at += part.size;
            }
            if (!sized || !compressed) return false;
            if (!placed) {
                size_t running = 0;
                for (Chunk& c : chunks) {
                    c.offset = running;
                    running += c.size;
                }
            }
        } else {
            return false;
        }
        const size_t blocksWide = size_t((width + 3) / 4), blocksHigh = size_t((height + 3) / 4);
        const size_t expected = blocksWide * blocksHigh * size_t(ratio);
        const size_t left = size - at;
        texture.assign(expected, 0);
        size_t total = 0;
        for (const Chunk& c : chunks) {
            if (c.offset + c.size > left) return false;
            const uint8_t* source = data + at + c.offset;
            if (c.compressor == 0xB0) {
                const int64_t length = snappyLength(source, c.size);
                size_t made = 0;
                if (length < 0 || total + size_t(length) > expected || !unsnappy(source, c.size, texture.data() + total, expected - total, made)) return false;
                total += made;
            } else if (c.compressor == 0xA0) {
                if (total + c.size > expected) return false;
                memcpy(texture.data() + total, source, c.size);
                total += c.size;
            } else {
                return false;
            }
        }
        (void)body;
        return total == expected;
    }

    bool decode(const uint8_t* data, size_t size, int64_t stamp, const PictureSink& sink) override {
        const bool alphaOnly = tag == fourcc("HapA"), twoTextures = tag == fourcc("HapM");
        const bool withAlpha = tag == fourcc("Hap5") || twoTextures;
        size_t start = 0;
        if (twoTextures) {
            Section outer;
            if (!section(data, size, start, outer) || (outer.type & 15) != 0x0D) return false;
            start = 4;
        }
        const int format = tag == fourcc("Hap1") ? 0xB : tag == fourcc("Hap5") ? 0xE : tag == fourcc("HapY") ? 0xF : tag == fourcc("HapA") ? 0x1 : -1;
        const int ratio = tag == fourcc("Hap1") || alphaOnly ? 8 : 16;
        size_t first = 0;
        if (!unpack(data, size, start, ratio, format, first)) return false;
        const size_t blocksWide = size_t((width + 3) / 4), blocksHigh = size_t((height + 3) / 4);
        out.rgb(width, height, 8, withAlpha);
        std::vector<uint8_t> second;
        if (twoTextures) {
            std::vector<uint8_t> colour = std::move(texture);
            size_t ignored = 0;
            if (!unpack(data, size, start + first + 4, 8, 0x1, ignored)) return false;
            second = std::move(texture);
            texture = std::move(colour);
        }
        for (size_t by = 0; by < blocksHigh; ++by) {
            for (size_t bx = 0; bx < blocksWide; ++bx) {
                const size_t index = by * blocksWide + bx;
                uint32_t px[16];
                if (alphaOnly) {
                    uint8_t grey[16];
                    rgtc1(grey, texture.data() + index * 8);
                    for (int i = 0; i < 16; ++i) px[i] = grey[i] * 0x010101u | 0xFF000000u;
                } else if (tag == fourcc("Hap1")) {
                    dxt1(px, texture.data() + index * 8);
                } else {
                    dxt5(px, texture.data() + index * 16);
                    if (tag != fourcc("Hap5")) ycocgScaled(px);
                }
                uint8_t veil[16];
                if (twoTextures) rgtc1(veil, second.data() + index * 8);
                for (int i = 0; i < 16; ++i) {
                    const int x = int(bx * 4) + (i & 3), y = int(by * 4) + (i >> 2);
                    if (x >= width || y >= height) continue;
                    out.row(2, y)[x] = uint8_t(px[i]);
                    out.row(0, y)[x] = uint8_t(px[i] >> 8);
                    out.row(1, y)[x] = uint8_t(px[i] >> 16);
                    if (withAlpha) out.row(3, y)[x] = twoTextures ? veil[i] : uint8_t(px[i] >> 24);
                }
            }
        }
        sink(out.picture(stamp));
        return true;
    }
};

}

std::unique_ptr<SoftDecoder> openHap(const Track& track, std::string* error) {
    if (track.codec != fourcc("Hap1") && track.codec != fourcc("Hap5") && track.codec != fourcc("HapY") && track.codec != fourcc("HapA") && track.codec != fourcc("HapM")) {
        if (error) *error = "this HAP variant is not decoded";
        return nullptr;
    }
    auto decoder = std::make_unique<HapDecoder>();
    decoder->tag = track.codec;
    decoder->width = track.width;
    decoder->height = track.height;
    return decoder;
}

}

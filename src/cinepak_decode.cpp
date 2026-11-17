#include "read_common.h"
#include "soft_video.h"
#include <algorithm>
#include <cstring>

namespace absent {

namespace {

using Codebook = uint8_t[12];

struct Strip {
    uint16_t id = 0;
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    Codebook v4[256] = {};
    Codebook v1[256] = {};
};

uint8_t clip(int v) { return uint8_t(std::clamp(v, 0, 255)); }

class CinepakDecoder : public SoftDecoder {
public:
    CinepakDecoder(int width, int height, bool paletted, const std::vector<uint32_t>& palette)
        : width(width), height(height), alignedWidth((width + 3) & ~3), alignedHeight((height + 3) & ~3), paletted(paletted), palette(palette) {
        unit = paletted ? 1 : 3;
        pitch = size_t(alignedWidth) * size_t(unit);
        image.assign(pitch * size_t(alignedHeight), 0);
    }

    const char* name() const override { return "cinepak"; }
    void reset() override { std::fill(image.begin(), image.end(), 0); }

    bool decode(const uint8_t* data, size_t size, int64_t stamp, const PictureSink& shown) override {
        if (size < 10) return false;
        const int strips = int(be16(data + 8));
        if (!strips) return false;
        const size_t encoded = be24(data + 1);
        if (size * 100 < encoded * 5) return false;
        if (skip < 0) {
            if (!encoded) return false;
            if (encoded != size && size % encoded != 0)
                skip = size >= 16 && data[10] == 0xFE && data[11] == 0 && data[12] == 0 && data[13] == 6 && data[14] == 0 && data[15] == 0 ? 6 : 2;
            else
                skip = 0;
        }
        if (size < size_t(10 + skip + strips * 12)) return false;
        const size_t firstStrip = be24(data + 10 + skip + 1);
        if (firstStrip < 12 || firstStrip > encoded) return false;
        frame(data, size);
        output(stamp, shown);
        return true;
    }

private:
    int width, height, alignedWidth, alignedHeight;
    bool paletted;
    std::vector<uint32_t> palette;
    int unit = 3;
    size_t pitch = 0;
    std::vector<uint8_t> image;
    std::vector<uint8_t> planes[3];
    Strip strips[32];
    int skip = -1;

    static void codebook(Codebook* book, int id, size_t size, const uint8_t* data) {
        const uint8_t* end = data + size;
        const int n = (id & 4) ? 4 : 6;
        uint32_t flag = 0, mask = 0;
        uint8_t* p = book[0];
        for (int i = 0; i < 256; ++i) {
            if ((id & 1) && !(mask >>= 1)) {
                if (end - data < 4) break;
                flag = be32(data);
                data += 4;
                mask = 0x80000000u;
            }
            if (!(id & 1) || (flag & mask)) {
                if (end - data < n) break;
                for (int k = 0; k < 4; ++k) {
                    const uint8_t r = *data++;
                    *p++ = r;
                    *p++ = r;
                    *p++ = r;
                }
                if (n == 6) {
                    const int u = int8_t(*data++), v = int8_t(*data++);
                    p -= 12;
                    for (int k = 0; k < 4; ++k, p += 3) {
                        const int y = p[0];
                        p[0] = clip(y + v * 2);
                        p[1] = clip(p[1] - (u / 2) - v);
                        p[2] = clip(p[2] + u * 2);
                    }
                }
            } else {
                p += 12;
            }
        }
    }

    void put(uint8_t* row, int x, const uint8_t* colour) {
        if (paletted) row[x] = colour[0];
        else memcpy(row + size_t(x) * 3, colour, 3);
    }

    bool vectors(Strip& s, int id, size_t size, const uint8_t* data) {
        const uint8_t* end = data + size;
        uint32_t flag = 0, mask = 0;
        for (int y = s.y1; y < s.y2; y += 4) {
            uint8_t* rows[4];
            for (int k = 0; k < 4; ++k) rows[k] = image.data() + size_t(std::min(y + k, alignedHeight - 1)) * pitch;
            for (int x = s.x1; x < s.x2; x += 4) {
                if ((id & 1) && !(mask >>= 1)) {
                    if (end - data < 4) return false;
                    flag = be32(data);
                    data += 4;
                    mask = 0x80000000u;
                }
                if (!(id & 1) || (flag & mask)) {
                    if (!(id & 2) && !(mask >>= 1)) {
                        if (end - data < 4) return false;
                        flag = be32(data);
                        data += 4;
                        mask = 0x80000000u;
                    }
                    if ((id & 2) || (~flag & mask)) {
                        if (data >= end) return false;
                        const uint8_t* p = s.v1[*data++];
                        const int corner[4] = {0, 3, 6, 9};
                        for (int k = 3; k >= 0; --k) {
                            const uint8_t* top = p + corner[(k / 2) * 2];
                            const uint8_t* right = p + corner[(k / 2) * 2 + 1];
                            put(rows[k], x, top);
                            put(rows[k], x + 1, top);
                            put(rows[k], x + 2, right);
                            put(rows[k], x + 3, right);
                        }
                    } else if (flag & mask) {
                        if (end - data < 4) return false;
                        const uint8_t* cb[4] = {s.v4[data[0]], s.v4[data[1]], s.v4[data[2]], s.v4[data[3]]};
                        data += 4;
                        for (int k = 3; k >= 0; --k) {
                            const uint8_t* left = cb[(k / 2) * 2];
                            const uint8_t* right = cb[(k / 2) * 2 + 1];
                            const int half = (k & 1) * 6;
                            put(rows[k], x, left + half);
                            put(rows[k], x + 1, left + half + 3);
                            put(rows[k], x + 2, right + half);
                            put(rows[k], x + 3, right + half + 3);
                        }
                    }
                }
            }
        }
        return true;
    }

    bool strip(Strip& s, const uint8_t* data, size_t size) {
        if (s.x2 > alignedWidth || s.y2 > alignedHeight || s.x1 >= s.x2 || s.y1 >= s.y2) return false;
        const uint8_t* end = data + size;
        while (end - data >= 4) {
            const int id = data[0];
            int64_t length = int64_t(be24(data + 1)) - 4;
            if (length < 0) return false;
            data += 4;
            if (length > end - data) length = end - data;
            switch (id) {
                case 0x20:
                case 0x21:
                case 0x24:
                case 0x25: codebook(s.v4, id, size_t(length), data); break;
                case 0x22:
                case 0x23:
                case 0x26:
                case 0x27: codebook(s.v1, id, size_t(length), data); break;
                case 0x30:
                case 0x31:
                case 0x32: return vectors(s, id, size_t(length), data);
                default: break;
            }
            data += length;
        }
        return false;
    }

    void frame(const uint8_t* data, size_t size) {
        const uint8_t* end = data + size;
        const int flags = data[0];
        const int count = std::min(int(be16(data + 8)), 32);
        data += 10 + skip;
        int y0 = 0;
        for (int i = 0; i < count; ++i) {
            if (end - data < 12) return;
            Strip& s = strips[i];
            s.id = data[0];
            s.y1 = int(be16(data + 4));
            if (!s.y1) {
                s.y1 = y0;
                s.y2 = y0 + int(be16(data + 8));
            } else {
                s.y2 = int(be16(data + 8));
            }
            s.x1 = int(be16(data + 6));
            s.x2 = int(be16(data + 10));
            int64_t length = int64_t(be24(data + 1)) - 12;
            if (length < 0) return;
            data += 12;
            if (length > end - data) length = end - data;
            if (i > 0 && !(flags & 1)) {
                memcpy(s.v4, strips[i - 1].v4, sizeof(s.v4));
                memcpy(s.v1, strips[i - 1].v1, sizeof(s.v1));
            }
            if (!strip(s, data, size_t(length))) return;
            data += length;
            y0 = s.y2;
        }
    }

    void output(int64_t stamp, const PictureSink& shown) {
        const size_t w = size_t(width), h = size_t(height);
        for (auto& plane : planes) plane.resize(w * h);
        for (size_t y = 0; y < h; ++y) {
            const uint8_t* row = image.data() + y * pitch;
            for (size_t x = 0; x < w; ++x) {
                uint32_t r, g, b;
                if (paletted) {
                    const uint32_t c = palette[row[x]];
                    r = (c >> 16) & 255;
                    g = (c >> 8) & 255;
                    b = c & 255;
                } else {
                    r = row[3 * x];
                    g = row[3 * x + 1];
                    b = row[3 * x + 2];
                }
                planes[0][y * w + x] = uint8_t(g);
                planes[1][y * w + x] = uint8_t(b);
                planes[2][y * w + x] = uint8_t(r);
            }
        }
        Picture p;
        p.stamp = stamp;
        p.width = p.chromaWidth = width;
        p.height = p.chromaHeight = height;
        for (int i = 0; i < 3; ++i) {
            p.plane[i] = planes[i].data();
            p.stride[i] = w;
        }
        p.color.known = true;
        p.color.matrix = 0;
        p.color.full = true;
        shown(p);
    }
};

}

std::unique_ptr<SoftDecoder> openCinepak(const Track& track, std::string* error) {
    if (track.width <= 0 || track.height <= 0 || track.width > 32768 || track.height > 32768) {
        if (error) *error = "Cinepak needs a picture size";
        return nullptr;
    }
    const std::vector<uint8_t>& head = track.config;
    const int bits = head.size() >= 16 ? int(le16(head.data() + 14)) : track.bits;
    std::vector<uint32_t> palette(256, 0);
    if (bits == 8 && head.size() > 40) {
        const size_t count = std::min<size_t>(head.size() - 40, 1024) / 4;
        const uint8_t* from = head.data() + head.size() - count * 4;
        for (size_t i = 0; i < count; ++i) palette[i] = 0xFF000000u | le32(from + 4 * i);
    }
    return std::make_unique<CinepakDecoder>(track.width, track.height, bits == 8, palette);
}

}

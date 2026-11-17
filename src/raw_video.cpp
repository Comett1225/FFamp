#include "read_common.h"
#include "readers.h"
#include "soft_video.h"
#include <algorithm>
#include <cstring>

namespace absent {

namespace {

enum class Layout { None, Palette, Rgb555, Bgr24, Bgra, Yuyv, Yvyu, Uyvy, Planar, Gray, Nv12, Nv21 };

struct RawFormat {
    Layout layout = Layout::None;
    int shiftX = 0;
    int shiftY = 0;
    bool swapped = false;
    bool flip = false;
};

RawFormat formatOf(uint32_t codec) {
    struct Entry {
        char name[5];
        Layout layout;
        int shiftX, shiftY;
        bool swapped, flip;
    };
    static const Entry table[] = {
        {"I420", Layout::Planar, 1, 1, false, false}, {"IYUV", Layout::Planar, 1, 1, false, false}, {"yv12", Layout::Planar, 1, 1, false, false},
        {"YV12", Layout::Planar, 1, 1, true, false}, {"YUV9", Layout::Planar, 2, 2, false, false}, {"YVU9", Layout::Planar, 2, 2, true, false},
        {"Y41B", Layout::Planar, 2, 0, false, false}, {"Y42B", Layout::Planar, 1, 0, false, false}, {"P422", Layout::Planar, 1, 0, false, false},
        {"YV16", Layout::Planar, 1, 0, true, false}, {"Y800", Layout::Gray, 0, 0, false, false}, {"Y8  ", Layout::Gray, 0, 0, false, false},
        {"GREY", Layout::Gray, 0, 0, false, false}, {"YUY2", Layout::Yuyv, 1, 0, false, false}, {"Y422", Layout::Yuyv, 1, 0, false, false},
        {"V422", Layout::Yuyv, 1, 0, false, false}, {"VYUY", Layout::Yuyv, 1, 0, false, false}, {"YUNV", Layout::Yuyv, 1, 0, false, false},
        {"YUYV", Layout::Yuyv, 1, 0, false, false}, {"YVYU", Layout::Yvyu, 1, 0, false, false}, {"UYVY", Layout::Uyvy, 1, 0, false, false},
        {"HDYC", Layout::Uyvy, 1, 0, false, false}, {"UYNV", Layout::Uyvy, 1, 0, false, false}, {"UYNY", Layout::Uyvy, 1, 0, false, false},
        {"uyv1", Layout::Uyvy, 1, 0, false, false}, {"2Vu1", Layout::Uyvy, 1, 0, false, false}, {"VDTZ", Layout::Uyvy, 1, 0, false, false},
        {"auv2", Layout::Uyvy, 1, 0, false, false}, {"cyuv", Layout::Uyvy, 1, 0, false, true}, {"NV12", Layout::Nv12, 1, 1, false, false},
        {"NV21", Layout::Nv21, 1, 1, false, false},
    };
    for (const Entry& e : table) {
        if (fourcc(e.name) == codec) return {e.layout, e.shiftX, e.shiftY, e.swapped, e.flip};
    }
    return {};
}

class RawDecoder : public SoftDecoder {
public:
    RawDecoder(const RawFormat& format, int width, int height, int bits, const std::vector<uint32_t>& palette)
        : format(format), width(width), height(height), bits(bits), palette(palette) {}

    const char* name() const override { return "rawvideo"; }
    void reset() override {}

    bool decode(const uint8_t* data, size_t size, int64_t stamp, const PictureSink& shown) override {
        const size_t w = size_t(width), h = size_t(height);
        Picture p;
        p.stamp = stamp;
        p.width = width;
        p.height = height;
        const Layout layout = format.layout;
        if (layout == Layout::Palette || layout == Layout::Rgb555 || layout == Layout::Bgr24 || layout == Layout::Bgra) {
            if (!rgb(data, size)) return false;
            for (int i = 0; i < 3; ++i) {
                p.plane[i] = planes[i].data();
                p.stride[i] = w;
            }
            p.chromaWidth = width;
            p.chromaHeight = height;
            p.color.known = true;
            p.color.matrix = 0;
            p.color.full = true;
            shown(p);
            return true;
        }
        const size_t cw = (w + (size_t(1) << format.shiftX) - 1) >> format.shiftX;
        const size_t ch = (h + (size_t(1) << format.shiftY) - 1) >> format.shiftY;
        p.chromaWidth = int(cw);
        p.chromaHeight = int(ch);
        if (layout == Layout::Planar || layout == Layout::Gray) {
            const size_t need = w * h + (layout == Layout::Planar ? 2 * cw * ch : 0);
            if (size < need) return false;
            if (layout == Layout::Gray) {
                fillGray(cw, ch);
                p.color.full = true;
                p.plane[0] = data;
                p.stride[0] = w;
                for (int i = 1; i < 3; ++i) {
                    p.plane[i] = planes[i].data();
                    p.stride[i] = cw;
                }
            } else {
                p.plane[0] = data;
                p.plane[format.swapped ? 2 : 1] = data + w * h;
                p.plane[format.swapped ? 1 : 2] = data + w * h + cw * ch;
                p.stride[0] = w;
                p.stride[1] = p.stride[2] = cw;
            }
            shown(p);
            return true;
        }
        if (layout == Layout::Nv12 || layout == Layout::Nv21) {
            if (size < w * h + 2 * cw * ch) return false;
            for (int i = 1; i < 3; ++i) planes[i].resize(cw * ch);
            const uint8_t* pairs = data + w * h;
            const int first = layout == Layout::Nv12 ? 1 : 2;
            for (size_t k = 0; k < cw * ch; ++k) {
                planes[first][k] = pairs[2 * k];
                planes[3 - first][k] = pairs[2 * k + 1];
            }
            p.plane[0] = data;
            p.stride[0] = w;
            for (int i = 1; i < 3; ++i) {
                p.plane[i] = planes[i].data();
                p.stride[i] = cw;
            }
            shown(p);
            return true;
        }
        const size_t line = cw * 4;
        if (size < line * h) return false;
        for (int i = 0; i < 3; ++i) planes[i].resize(i ? cw * h : w * h);
        const int lumaAt = layout == Layout::Uyvy ? 1 : 0;
        const int uAt = layout == Layout::Uyvy ? 0 : layout == Layout::Yvyu ? 3 : 1;
        const int vAt = layout == Layout::Uyvy ? 2 : layout == Layout::Yvyu ? 1 : 3;
        for (size_t y = 0; y < h; ++y) {
            const uint8_t* row = data + (format.flip ? h - 1 - y : y) * line;
            uint8_t* luma = planes[0].data() + y * w;
            uint8_t* u = planes[1].data() + y * cw;
            uint8_t* v = planes[2].data() + y * cw;
            for (size_t x = 0; x < cw; ++x) {
                const uint8_t* q = row + 4 * x;
                luma[2 * x] = q[lumaAt];
                if (2 * x + 1 < w) luma[2 * x + 1] = q[lumaAt + 2];
                u[x] = q[uAt];
                v[x] = q[vAt];
            }
        }
        for (int i = 0; i < 3; ++i) {
            p.plane[i] = planes[i].data();
            p.stride[i] = i ? cw : w;
        }
        shown(p);
        return true;
    }

private:
    RawFormat format;
    int width;
    int height;
    int bits;
    std::vector<uint32_t> palette;
    std::vector<uint8_t> planes[3];
    bool grayReady = false;

    void fillGray(size_t cw, size_t ch) {
        if (grayReady) return;
        for (int i = 1; i < 3; ++i) planes[i].assign(cw * ch, 128);
        grayReady = true;
    }

    bool rgb(const uint8_t* data, size_t size) {
        const size_t w = size_t(width), h = size_t(height);
        for (int i = 0; i < 3; ++i) planes[i].resize(w * h);
        auto put = [&](size_t y, size_t x, uint32_t r, uint32_t g, uint32_t b) {
            const size_t at = (format.flip ? h - 1 - y : y) * w + x;
            planes[0][at] = uint8_t(g);
            planes[1][at] = uint8_t(b);
            planes[2][at] = uint8_t(r);
        };
        if (format.layout == Layout::Palette) {
            const size_t stride = size / h;
            const size_t rowBytes = (w * size_t(bits) + 7) / 8;
            if (!stride || stride < rowBytes || size < stride * h) return false;
            const int perByte = 8 / bits, mask = (1 << bits) - 1;
            for (size_t y = 0; y < h; ++y) {
                const uint8_t* row = data + y * stride;
                for (size_t x = 0; x < w; ++x) {
                    const int shift = 8 - bits * (int(x % size_t(perByte)) + 1);
                    const uint32_t index = (row[x / size_t(perByte)] >> shift) & uint32_t(mask);
                    const uint32_t c = index < palette.size() ? palette[index] : 0;
                    put(y, x, (c >> 16) & 255, (c >> 8) & 255, c & 255);
                }
            }
            return true;
        }
        const size_t unit = format.layout == Layout::Rgb555 ? 2 : format.layout == Layout::Bgr24 ? 3 : 4;
        size_t stride = w * unit;
        if (unit != 4 && ((stride + 3) & ~size_t(3)) * h <= size) stride = (stride + 3) & ~size_t(3);
        if (size < stride * h) return false;
        for (size_t y = 0; y < h; ++y) {
            const uint8_t* row = data + y * stride;
            for (size_t x = 0; x < w; ++x) {
                const uint8_t* q = row + x * unit;
                if (unit == 2) {
                    const uint32_t v = le16(q), r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
                    put(y, x, (r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2));
                } else {
                    put(y, x, q[2], q[1], q[0]);
                }
            }
        }
        return true;
    }
};

}

bool rawVideoCodec(uint32_t codec) { return codec == fourcc("raw ") || formatOf(codec).layout != Layout::None; }

std::unique_ptr<SoftDecoder> openRawVideo(const Track& track, std::string* error) {
    auto fail = [&](const char* text) -> std::unique_ptr<SoftDecoder> {
        if (error) *error = text;
        return nullptr;
    };
    if (track.width <= 0 || track.height <= 0) return fail("raw video needs a picture size");
    RawFormat format = formatOf(track.codec);
    std::vector<uint32_t> palette;
    int bits = track.bits;
    if (track.codec == fourcc("raw ")) {
        const std::vector<uint8_t>& head = track.config;
        if (head.size() < 40) return fail("raw video without its BITMAPINFOHEADER");
        if (be32(head.data() + 16) != 0) return fail("this raw video layout (bit fields) is not supported");
        bits = int(le16(head.data() + 14));
        format.layout = bits == 1 || bits == 2 || bits == 4 || bits == 8 ? Layout::Palette : bits == 15 || bits == 16 ? Layout::Rgb555 : bits == 24 ? Layout::Bgr24 : bits == 32 ? Layout::Bgra : Layout::None;
        format.flip = int32_t(le32(head.data() + 8)) > 0;
        if (format.layout == Layout::Palette) {
            palette.assign(size_t(1) << bits, 0);
            if (bits == 1) palette[0] = 0xFFFFFFFFu;
            const size_t extra = head.size() - 40;
            const size_t count = std::min(extra, size_t(4) << bits) / 4;
            const uint8_t* from = head.data() + head.size() - count * 4;
            for (size_t i = 0; i < count; ++i) palette[i] = 0xFF000000u | le32(from + 4 * i);
        }
    }
    if (format.layout == Layout::None) return fail("this raw video layout is not supported");
    return std::make_unique<RawDecoder>(format, track.width, track.height, bits, palette);
}

}

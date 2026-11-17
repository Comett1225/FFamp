#include "own_video.h"
#include "read_common.h"

namespace absent {

namespace {

enum class Raw { None, I420, Yv12, Nv12, Nv21, Yuyv, Uyvy, Yuv422, Gray, Bgr24, Bgra, Rgb555Le, Rgb24, Argb, Rgb555Be, V210 };

Raw rawKind(uint32_t c, int bits) {
    if (c == fourcc("I420") || c == fourcc("IYUV")) return Raw::I420;
    if (c == fourcc("YV12")) return Raw::Yv12;
    if (c == fourcc("NV12")) return Raw::Nv12;
    if (c == fourcc("NV21")) return Raw::Nv21;
    if (c == fourcc("YUY2") || c == fourcc("YUYV") || c == fourcc("yuvs")) return Raw::Yuyv;
    if (c == fourcc("UYVY") || c == fourcc("2vuy") || c == fourcc("HDYC")) return Raw::Uyvy;
    if (c == fourcc("Y42B")) return Raw::Yuv422;
    if (c == fourcc("Y800") || c == fourcc("GREY") || c == fourcc("Y8  ")) return Raw::Gray;
    if (c == fourcc("v210")) return Raw::V210;
    if (c == fourcc("DIB ")) return bits == 24 ? Raw::Bgr24 : bits == 32 ? Raw::Bgra : bits == 16 ? Raw::Rgb555Le : Raw::None;
    if (c == fourcc("raw ")) return bits == 24 ? Raw::Rgb24 : bits == 32 ? Raw::Argb : bits == 16 ? Raw::Rgb555Be : Raw::None;
    return Raw::None;
}

bool utCodec(uint32_t c) {
    static const char* const names[] = {"ULY0", "ULY2", "ULY4", "ULRG", "ULRA", "ULH0", "ULH2", "ULH4", "UQY0", "UQY2", "UQRG", "UQRA"};
    for (const char* name : names) if (c == be32(reinterpret_cast<const uint8_t*>(name))) return true;
    return false;
}

int bitmapBits(const Track& t) { return t.configType == fourcc("bmih") && t.config.size() >= 40 ? int(le16(t.config.data() + 14)) : t.bits; }

struct RawDecoder : SoftDecoder {
    Raw kind = Raw::None;
    int width = 0;
    int height = 0;
    bool bottomUp = false;
    Color color;
    Planes out;

    const char* name() const override { return kind == Raw::V210 ? "v210" : "rawvideo"; }
    void reset() override {}

    size_t minimumRow() const {
        switch (kind) {
            case Raw::Yuyv: case Raw::Uyvy: return size_t((width + 1) / 2) * 4;
            case Raw::Bgr24: case Raw::Rgb24: return size_t(width) * 3;
            case Raw::Bgra: case Raw::Argb: return size_t(width) * 4;
            case Raw::Rgb555Le: case Raw::Rgb555Be: return size_t(width) * 2;
            default: return 0;
        }
    }

    bool planar(const uint8_t* data, size_t size, int64_t stamp, const PictureSink& sink) {
        const int cw = (width + 1) / 2, ch = kind == Raw::Yuv422 ? height : (height + 1) / 2;
        const size_t luma = size_t(width) * size_t(height), chroma = size_t(cw) * size_t(ch);
        const bool pairs = kind == Raw::Nv12 || kind == Raw::Nv21;
        if (size < (kind == Raw::Gray ? luma : luma + 2 * chroma)) return false;
        out.shape(width, height, cw, kind == Raw::Gray ? (height + 1) / 2 : ch, 8, false);
        out.color = color;
        for (int y = 0; y < height; ++y) memcpy(out.row(0, y), data + size_t(y) * size_t(width), size_t(width));
        if (kind == Raw::Gray) {
            std::fill(out.data[1].begin(), out.data[1].end(), 128);
            std::fill(out.data[2].begin(), out.data[2].end(), 128);
        } else {
            const uint8_t* second = data + luma;
            for (int y = 0; y < ch; ++y) {
                if (pairs) {
                    const uint8_t* line = second + size_t(y) * size_t(cw) * 2;
                    uint8_t* u = out.row(kind == Raw::Nv12 ? 1 : 2, y);
                    uint8_t* v = out.row(kind == Raw::Nv12 ? 2 : 1, y);
                    for (int x = 0; x < cw; ++x) {
                        u[x] = line[2 * x];
                        v[x] = line[2 * x + 1];
                    }
                } else {
                    const int first = kind == Raw::Yv12 ? 2 : 1;
                    memcpy(out.row(first, y), second + size_t(y) * size_t(cw), size_t(cw));
                    memcpy(out.row(3 - first, y), second + chroma + size_t(y) * size_t(cw), size_t(cw));
                }
            }
        }
        sink(out.picture(stamp));
        return true;
    }

    bool v210(const uint8_t* data, size_t size, int64_t stamp, const PictureSink& sink) {
        size_t stride = size_t((width + 47) / 48) * 48 * 8 / 3;
        if (size < stride * size_t(height)) {
            const size_t small = size_t((width + 23) / 24) * 24 * 8 / 3;
            if (small * size_t(height) != size) return false;
            stride = small;
        }
        const int cw = (width + 1) / 2;
        out.shape(width, height, cw, height, 10, false);
        out.color = color;
        for (int y = 0; y < height; ++y) {
            const uint8_t* line = data + size_t(y) * stride;
            uint16_t* lumaRow = out.wide(0, y);
            uint16_t* u = out.wide(1, y);
            uint16_t* v = out.wide(2, y);
            int yi = 0, ci = 0, step = 0;
            for (size_t at = 0; at + 4 <= stride && (yi < width || ci < cw); at += 4) {
                const uint32_t word = le32(line + at);
                for (int k = 0; k < 3; ++k, ++step) {
                    const uint16_t value = uint16_t((word >> (10 * k)) & 0x3FF);
                    switch (step & 3) {
                        case 0: if (ci < cw) u[ci] = value; break;
                        case 2: if (ci < cw) v[ci++] = value; break;
                        default: if (yi < width) lumaRow[yi++] = value; break;
                    }
                }
            }
        }
        sink(out.picture(stamp));
        return true;
    }

    bool packed(const uint8_t* data, size_t size, int64_t stamp, const PictureSink& sink) {
        const size_t least = minimumRow();
        size_t stride = least;
        if (kind == Raw::Bgr24 || kind == Raw::Bgra || kind == Raw::Rgb555Le) stride = (least + 3) & ~size_t(3);
        if (size >= least * size_t(height) && size % size_t(height) == 0 && size / size_t(height) >= least) stride = size / size_t(height);
        if (size < stride * size_t(height - 1) + least) return false;
        const bool yuv = kind == Raw::Yuyv || kind == Raw::Uyvy;
        const bool alpha = kind == Raw::Bgra || kind == Raw::Argb;
        if (yuv) {
            out.shape(width, height, (width + 1) / 2, height, 8, false);
            out.color = color;
        } else {
            out.rgb(width, height, 8, alpha);
        }
        for (int y = 0; y < height; ++y) {
            const uint8_t* line = data + size_t(bottomUp ? height - 1 - y : y) * stride;
            if (yuv) {
                const int lumaAt = kind == Raw::Yuyv ? 0 : 1, uAt = kind == Raw::Yuyv ? 1 : 0;
                uint8_t* l = out.row(0, y);
                uint8_t* u = out.row(1, y);
                uint8_t* v = out.row(2, y);
                for (int x = 0; x < width; ++x) l[x] = line[2 * x + lumaAt];
                for (int x = 0; x < (width + 1) / 2; ++x) {
                    u[x] = line[4 * x + uAt];
                    v[x] = line[4 * x + uAt + 2];
                }
                continue;
            }
            uint8_t* g = out.row(0, y);
            uint8_t* b = out.row(1, y);
            uint8_t* r = out.row(2, y);
            uint8_t* a = alpha ? out.row(3, y) : nullptr;
            for (int x = 0; x < width; ++x) {
                const uint8_t* px = line + size_t(x) * (least / size_t(width));
                switch (kind) {
                    case Raw::Bgr24: b[x] = px[0]; g[x] = px[1]; r[x] = px[2]; break;
                    case Raw::Bgra: b[x] = px[0]; g[x] = px[1]; r[x] = px[2]; a[x] = px[3]; break;
                    case Raw::Rgb24: r[x] = px[0]; g[x] = px[1]; b[x] = px[2]; break;
                    case Raw::Argb: a[x] = px[0]; r[x] = px[1]; g[x] = px[2]; b[x] = px[3]; break;
                    default: {
                        const unsigned value = kind == Raw::Rgb555Le ? le16(px) : be16(px);
                        const unsigned red = (value >> 10) & 31, green = (value >> 5) & 31, blue = value & 31;
                        r[x] = uint8_t(red << 3 | red >> 2);
                        g[x] = uint8_t(green << 3 | green >> 2);
                        b[x] = uint8_t(blue << 3 | blue >> 2);
                        break;
                    }
                }
            }
        }
        sink(out.picture(stamp));
        return true;
    }

    bool decode(const uint8_t* data, size_t size, int64_t stamp, const PictureSink& sink) override {
        switch (kind) {
            case Raw::V210: return v210(data, size, stamp, sink);
            case Raw::I420: case Raw::Yv12: case Raw::Nv12: case Raw::Nv21: case Raw::Yuv422: case Raw::Gray: return planar(data, size, stamp, sink);
            default: return packed(data, size, stamp, sink);
        }
    }
};

}

std::unique_ptr<SoftDecoder> openRawVideo(const Track& track, std::string* error) {
    auto decoder = std::make_unique<RawDecoder>();
    decoder->kind = rawKind(track.codec, bitmapBits(track));
    decoder->width = track.width;
    decoder->height = track.height;
    decoder->bottomUp = track.codec == fourcc("DIB ") && track.configType == fourcc("bmih") && track.config.size() >= 40 && int32_t(le32(track.config.data() + 8)) > 0;
    decoder->color = track.color;
    if (decoder->kind == Raw::None) {
        if (error) *error = "this uncompressed picture layout is not decoded";
        return nullptr;
    }
    return decoder;
}

const char* ownVideoName(uint32_t c) {
    if (c == fourcc("AVdn") || c == fourcc("AVdh")) return "dnxhd";
    if (c == fourcc("rle ")) return "qtrle";
    if (c == fourcc("png ")) return "png";
    if (c == fourcc("v210")) return "v210";
    if (c == fourcc("FFV1")) return "ffv1";
    if ((c >> 8) == 0x486170) return "hap";
    if (utCodec(c)) return "utvideo";
    if (rawKind(c, 24) != Raw::None) return "rawvideo";
    return nullptr;
}

std::unique_ptr<SoftDecoder> openOwnVideo(const Track& track, std::string* error) {
    if (int64_t(track.width + 128) * int64_t(track.height + 128) >= (int64_t(1) << 28)) {
        if (error) *error = "the picture is too large";
        return nullptr;
    }
    const std::string kind = ownVideoName(track.codec) ? ownVideoName(track.codec) : "";
    if (kind == "rawvideo" || kind == "v210") return openRawVideo(track, error);
    if (kind == "qtrle") return openQtrle(track, error);
    if (kind == "png") return openPng(error);
    if (kind == "dnxhd") return openDnxhd(error);
    if (kind == "hap") return openHap(track, error);
    if (kind == "utvideo") return openUtVideo(track, error);
    if (kind == "ffv1") return openFfv1(track, error);
    if (error) *error = "video codec not supported by this decoder";
    return nullptr;
}

}

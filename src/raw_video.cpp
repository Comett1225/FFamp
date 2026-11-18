#include "own_video.h"
#include "read_common.h"

namespace absent {

namespace {

enum class Raw { None, Planar, Nv12, Nv21, Gray, Yuyv, Yvyu, Uyvy, Palette, Bgr24, Bgra, Rgb555Le, Rgb24, Argb, Rgb555Be, V210 };

struct RawFormat {
    Raw kind = Raw::None;
    int shiftX = 0;
    int shiftY = 0;
    bool swapped = false;
    bool flip = false;
};

RawFormat rawFormat(uint32_t c, int bits) {
    struct Entry {
        char name[5];
        Raw kind;
        int shiftX, shiftY;
        bool swapped, flip;
    };
    static const Entry table[] = {
        {"I420", Raw::Planar, 1, 1, false, false}, {"IYUV", Raw::Planar, 1, 1, false, false}, {"yv12", Raw::Planar, 1, 1, false, false},
        {"YV12", Raw::Planar, 1, 1, true, false}, {"YUV9", Raw::Planar, 2, 2, false, false}, {"YVU9", Raw::Planar, 2, 2, true, false},
        {"Y41B", Raw::Planar, 2, 0, false, false}, {"Y42B", Raw::Planar, 1, 0, false, false}, {"P422", Raw::Planar, 1, 0, false, false},
        {"YV16", Raw::Planar, 1, 0, true, false}, {"Y800", Raw::Gray, 1, 1, false, false}, {"Y8  ", Raw::Gray, 1, 1, false, false},
        {"GREY", Raw::Gray, 1, 1, false, false}, {"YUY2", Raw::Yuyv, 1, 0, false, false}, {"Y422", Raw::Yuyv, 1, 0, false, false},
        {"V422", Raw::Yuyv, 1, 0, false, false}, {"VYUY", Raw::Yuyv, 1, 0, false, false}, {"YUNV", Raw::Yuyv, 1, 0, false, false},
        {"YUYV", Raw::Yuyv, 1, 0, false, false}, {"yuvs", Raw::Yuyv, 1, 0, false, false}, {"YVYU", Raw::Yvyu, 1, 0, false, false},
        {"UYVY", Raw::Uyvy, 1, 0, false, false}, {"2vuy", Raw::Uyvy, 1, 0, false, false}, {"HDYC", Raw::Uyvy, 1, 0, false, false},
        {"UYNV", Raw::Uyvy, 1, 0, false, false}, {"UYNY", Raw::Uyvy, 1, 0, false, false}, {"uyv1", Raw::Uyvy, 1, 0, false, false},
        {"2Vu1", Raw::Uyvy, 1, 0, false, false}, {"VDTZ", Raw::Uyvy, 1, 0, false, false}, {"auv2", Raw::Uyvy, 1, 0, false, false},
        {"cyuv", Raw::Uyvy, 1, 0, false, true}, {"NV12", Raw::Nv12, 1, 1, false, false}, {"NV21", Raw::Nv21, 1, 1, false, false},
        {"v210", Raw::V210, 1, 0, false, false},
    };
    for (const Entry& e : table) {
        if (fourcc(e.name) == c) return {e.kind, e.shiftX, e.shiftY, e.swapped, e.flip};
    }
    RawFormat f;
    if (c == fourcc("DIB ")) {
        f.kind = bits == 1 || bits == 2 || bits == 4 || bits == 8 ? Raw::Palette : bits == 15 || bits == 16 ? Raw::Rgb555Le : bits == 24 ? Raw::Bgr24 : bits == 32 ? Raw::Bgra : Raw::None;
    } else if (c == fourcc("raw ")) {
        f.kind = bits == 24 ? Raw::Rgb24 : bits == 32 ? Raw::Argb : bits == 16 ? Raw::Rgb555Be : Raw::None;
    }
    return f;
}

bool utCodec(uint32_t c) {
    static const char* const names[] = {"ULY0", "ULY2", "ULY4", "ULRG", "ULRA", "ULH0", "ULH2", "ULH4", "UQY0", "UQY2", "UQRG", "UQRA"};
    for (const char* name : names) if (c == be32(reinterpret_cast<const uint8_t*>(name))) return true;
    return false;
}

int bitmapBits(const Track& t) { return t.configType == fourcc("bmih") && t.config.size() >= 40 ? int(le16(t.config.data() + 14)) : t.bits; }

struct RawDecoder : SoftDecoder {
    RawFormat format;
    int width = 0;
    int height = 0;
    int bits = 8;
    Color color;
    std::vector<uint32_t> palette;
    Planes out;

    const char* name() const override { return format.kind == Raw::V210 ? "v210" : "rawvideo"; }
    void reset() override {}

    size_t minimumRow() const {
        switch (format.kind) {
            case Raw::Yuyv: case Raw::Yvyu: case Raw::Uyvy: return size_t((width + 1) / 2) * 4;
            case Raw::Bgr24: case Raw::Rgb24: return size_t(width) * 3;
            case Raw::Bgra: case Raw::Argb: return size_t(width) * 4;
            case Raw::Rgb555Le: case Raw::Rgb555Be: return size_t(width) * 2;
            default: return 0;
        }
    }

    bool planar(const uint8_t* data, size_t size, int64_t stamp, const PictureSink& sink) {
        const Raw kind = format.kind;
        const int cw = (width + (1 << format.shiftX) - 1) >> format.shiftX, ch = (height + (1 << format.shiftY) - 1) >> format.shiftY;
        const size_t luma = size_t(width) * size_t(height), chroma = size_t(cw) * size_t(ch);
        if (size < (kind == Raw::Gray ? luma : luma + 2 * chroma)) return false;
        out.shape(width, height, cw, ch, 8, false);
        out.color = color;
        if (kind == Raw::Gray) out.color.full = true;
        for (int y = 0; y < height; ++y) memcpy(out.row(0, y), data + size_t(y) * size_t(width), size_t(width));
        if (kind == Raw::Gray) {
            std::fill(out.data[1].begin(), out.data[1].end(), 128);
            std::fill(out.data[2].begin(), out.data[2].end(), 128);
        } else {
            const uint8_t* second = data + luma;
            for (int y = 0; y < ch; ++y) {
                if (kind == Raw::Nv12 || kind == Raw::Nv21) {
                    const uint8_t* line = second + size_t(y) * size_t(cw) * 2;
                    uint8_t* u = out.row(kind == Raw::Nv12 ? 1 : 2, y);
                    uint8_t* v = out.row(kind == Raw::Nv12 ? 2 : 1, y);
                    for (int x = 0; x < cw; ++x) {
                        u[x] = line[2 * x];
                        v[x] = line[2 * x + 1];
                    }
                } else {
                    const int first = format.swapped ? 2 : 1;
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

    bool paletted(const uint8_t* data, size_t size, int64_t stamp, const PictureSink& sink) {
        const size_t stride = size / size_t(height), rowBytes = (size_t(width) * size_t(bits) + 7) / 8;
        if (!stride || stride < rowBytes) return false;
        out.rgb(width, height, 8, false);
        const int perByte = 8 / bits, mask = (1 << bits) - 1;
        for (int y = 0; y < height; ++y) {
            const uint8_t* line = data + size_t(format.flip ? height - 1 - y : y) * stride;
            uint8_t* g = out.row(0, y);
            uint8_t* b = out.row(1, y);
            uint8_t* r = out.row(2, y);
            for (int x = 0; x < width; ++x) {
                const int shift = 8 - bits * (x % perByte + 1);
                const uint32_t c = palette[size_t((line[x / perByte] >> shift) & mask)];
                r[x] = uint8_t(c >> 16);
                g[x] = uint8_t(c >> 8);
                b[x] = uint8_t(c);
            }
        }
        sink(out.picture(stamp));
        return true;
    }

    bool packed(const uint8_t* data, size_t size, int64_t stamp, const PictureSink& sink) {
        const Raw kind = format.kind;
        const size_t least = minimumRow();
        size_t stride = least;
        if (kind == Raw::Bgr24 || kind == Raw::Bgra || kind == Raw::Rgb555Le) stride = (least + 3) & ~size_t(3);
        if (size >= least * size_t(height) && size % size_t(height) == 0 && size / size_t(height) >= least) stride = size / size_t(height);
        if (size < stride * size_t(height - 1) + least) return false;
        const bool yuv = kind == Raw::Yuyv || kind == Raw::Yvyu || kind == Raw::Uyvy;
        const bool alpha = kind == Raw::Bgra || kind == Raw::Argb;
        if (yuv) {
            out.shape(width, height, (width + 1) / 2, height, 8, false);
            out.color = color;
        } else {
            out.rgb(width, height, 8, alpha);
        }
        for (int y = 0; y < height; ++y) {
            const uint8_t* line = data + size_t(format.flip ? height - 1 - y : y) * stride;
            if (yuv) {
                const int lumaAt = kind == Raw::Uyvy ? 1 : 0;
                const int uAt = kind == Raw::Uyvy ? 0 : kind == Raw::Yvyu ? 3 : 1, vAt = kind == Raw::Uyvy ? 2 : kind == Raw::Yvyu ? 1 : 3;
                uint8_t* l = out.row(0, y);
                uint8_t* u = out.row(1, y);
                uint8_t* v = out.row(2, y);
                for (int x = 0; x < width; ++x) l[x] = line[2 * x + lumaAt];
                for (int x = 0; x < (width + 1) / 2; ++x) {
                    u[x] = line[4 * x + uAt];
                    v[x] = line[4 * x + vAt];
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
        switch (format.kind) {
            case Raw::V210: return v210(data, size, stamp, sink);
            case Raw::Palette: return paletted(data, size, stamp, sink);
            case Raw::Planar: case Raw::Nv12: case Raw::Nv21: case Raw::Gray: return planar(data, size, stamp, sink);
            default: return packed(data, size, stamp, sink);
        }
    }
};

}

bool rawVideoCodec(uint32_t codec) { return rawFormat(codec, 24).kind != Raw::None; }

std::unique_ptr<SoftDecoder> openRawVideo(const Track& track, std::string* error) {
    auto fail = [&](const char* text) -> std::unique_ptr<SoftDecoder> {
        if (error) *error = text;
        return nullptr;
    };
    if (track.width <= 0 || track.height <= 0) return fail("raw video needs a picture size");
    auto decoder = std::make_unique<RawDecoder>();
    decoder->bits = bitmapBits(track);
    decoder->format = rawFormat(track.codec, decoder->bits);
    decoder->width = track.width;
    decoder->height = track.height;
    decoder->color = track.color;
    const std::vector<uint8_t>& head = track.config;
    if (track.codec == fourcc("DIB ") && track.configType == fourcc("bmih") && head.size() >= 40) {
        if (be32(head.data() + 16) != 0) return fail("this raw video layout (bit fields) is not supported");
        decoder->format.flip = int32_t(le32(head.data() + 8)) > 0;
        if (decoder->format.kind == Raw::Palette) {
            const int bits = decoder->bits;
            decoder->palette.assign(size_t(1) << bits, 0);
            if (bits == 1) decoder->palette[0] = 0xFFFFFFFFu;
            const size_t count = std::min(head.size() - 40, size_t(4) << bits) / 4;
            const uint8_t* from = head.data() + head.size() - count * 4;
            for (size_t i = 0; i < count; ++i) decoder->palette[i] = 0xFF000000u | le32(from + 4 * i);
        }
    }
    if (decoder->format.kind == Raw::Palette && decoder->palette.empty()) return fail("paletted raw video without its palette");
    if (decoder->format.kind == Raw::None) return fail("this uncompressed picture layout is not decoded");
    return decoder;
}

const char* ownVideoName(uint32_t c) {
    if (c == fourcc("AVdn") || c == fourcc("AVdh")) return "dnxhd";
    if (c == fourcc("rle ")) return "qtrle";
    if (c == fourcc("png ")) return "png";
    if (c == fourcc("v210")) return "v210";
    if (c == fourcc("FFV1")) return "ffv1";
    if (c == fourcc("cvid")) return "cinepak";
    if ((c >> 8) == 0x486170) return "hap";
    if (utCodec(c)) return "utvideo";
    if (rawVideoCodec(c)) return "rawvideo";
    return nullptr;
}

std::unique_ptr<SoftDecoder> openOwnVideo(const Track& track, std::string* error) {
    const std::string kind = ownVideoName(track.codec) ? ownVideoName(track.codec) : "";
    if (kind != "png" && kind != "dnxhd" && (track.width <= 0 || track.height <= 0)) {
        if (error) *error = "the track has no picture size";
        return nullptr;
    }
    if ((int64_t(track.width) + 128) * (int64_t(track.height) + 128) >= (int64_t(1) << 28)) {
        if (error) *error = "the picture is too large";
        return nullptr;
    }
    if (kind == "rawvideo" || kind == "v210") return openRawVideo(track, error);
    if (kind == "cinepak") return openCinepak(track, error);
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

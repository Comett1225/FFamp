#include "own_video.h"
#include "read_common.h"

namespace absent {

namespace {

struct QtrleDecoder : SoftDecoder {
    int width = 0;
    int height = 0;
    int depth = 0;
    int unit = 0;
    std::vector<uint8_t> canvas;
    bool started = false;
    Planes out;

    const char* name() const override { return "qtrle"; }
    void reset() override {}

    void lines(const uint8_t* data, size_t size, size_t at, int64_t rowAt, int count) {
        const int64_t stride = int64_t(width) * unit, limit = stride * height;
        auto byte = [&]() -> int { return at < size ? data[at++] : 0; };
        auto pixels = [&](int64_t from, int n, const uint8_t* source) {
            for (int k = 0; k < n; ++k) memcpy(&canvas[size_t(from + int64_t(k) * unit)], source, size_t(unit));
        };
        while (count-- > 0) {
            int64_t pixel = rowAt + int64_t(byte() - 1) * unit;
            if (pixel < 0 || pixel > limit) return;
            for (;;) {
                if (at >= size) return;
                const int code = int8_t(byte());
                if (code == -1) break;
                if (at >= size) return;
                if (code == 0) {
                    pixel += int64_t(byte() - 1) * unit;
                    if (pixel < 0 || pixel > limit) return;
                } else if (code < 0) {
                    if (pixel - code * unit > limit || at + size_t(unit) > size) return;
                    pixels(pixel, -code, data + at);
                    at += size_t(unit);
                    pixel -= int64_t(code) * unit;
                } else {
                    const size_t bytes = size_t(code) * size_t(unit);
                    if (pixel + int64_t(bytes) > limit || at + bytes > size) return;
                    memcpy(&canvas[size_t(pixel)], data + at, bytes);
                    at += bytes;
                    pixel += int64_t(bytes);
                }
            }
            rowAt += stride;
        }
    }

    void emit(int64_t stamp, const PictureSink& sink) {
        out.rgb(width, height, 8, depth == 32);
        for (int y = 0; y < height; ++y) {
            const uint8_t* line = canvas.data() + size_t(y) * size_t(width) * size_t(unit);
            uint8_t* g = out.row(0, y);
            uint8_t* b = out.row(1, y);
            uint8_t* r = out.row(2, y);
            uint8_t* a = depth == 32 ? out.row(3, y) : nullptr;
            for (int x = 0; x < width; ++x) {
                const uint8_t* px = line + size_t(x) * size_t(unit);
                if (depth == 32) {
                    a[x] = px[0];
                    r[x] = px[1];
                    g[x] = px[2];
                    b[x] = px[3];
                } else if (depth == 24) {
                    r[x] = px[0];
                    g[x] = px[1];
                    b[x] = px[2];
                } else {
                    const unsigned value = be16(px), red = (value >> 10) & 31, green = (value >> 5) & 31, blue = value & 31;
                    r[x] = uint8_t(red << 3 | red >> 2);
                    g[x] = uint8_t(green << 3 | green >> 2);
                    b[x] = uint8_t(blue << 3 | blue >> 2);
                }
            }
        }
        sink(out.picture(stamp));
    }

    bool decode(const uint8_t* data, size_t size, int64_t stamp, const PictureSink& sink) override {
        if (size >= 8) {
            int start = 0, count = height;
            size_t at = 6;
            if (be16(data + 4) & 0x0008) {
                if (size < 14) {
                    count = -1;
                } else {
                    start = int(be16(data + 6));
                    count = int(be16(data + 10));
                    at = 14;
                    if (count > height - start) count = -1;
                }
            }
            if (count >= 0) {
                started = true;
                lines(data, size, at, int64_t(start) * width * unit, count);
            }
        }
        if (!started) return false;
        emit(stamp, sink);
        return true;
    }
};

}

std::unique_ptr<SoftDecoder> openQtrle(const Track& track, std::string* error) {
    if (track.bits != 16 && track.bits != 24 && track.bits != 32) {
        if (error) *error = "QuickTime Animation with " + std::to_string(track.bits) + " bits per pixel (a palette) is not decoded";
        return nullptr;
    }
    auto decoder = std::make_unique<QtrleDecoder>();
    decoder->width = track.width;
    decoder->height = track.height;
    decoder->depth = track.bits;
    decoder->unit = track.bits / 8;
    decoder->canvas.assign(size_t(track.width) * size_t(track.height) * size_t(decoder->unit), 0);
    return decoder;
}

}

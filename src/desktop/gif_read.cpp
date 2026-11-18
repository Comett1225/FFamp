#include "../tools_common.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "../portable.h"

namespace absent {

namespace {

const int64_t kMostPixels = 2048 * 2048;
const size_t kMostFrames = 3000;
const int64_t kMostWork = 600000000;
const int64_t kLongest = 10 * 60 * 1000;

bool fail(std::string* error, const char* text) {
    if (error) *error = text;
    return false;
}

struct GifFrame {
    int x = 0, y = 0, width = 0, height = 0;
    bool interlaced = false;
    int dispose = 0;
    int clear = -1;
    int delay = 0;
    std::vector<uint8_t> palette;
    std::vector<uint8_t> indexes;
};

struct Cursor {
    const std::vector<uint8_t>& d;
    size_t at = 0;
    bool ok = true;
    int u8() {
        if (at >= d.size()) {
            ok = false;
            return 0;
        }
        return d[at++];
    }
    int u16() {
        const int low = u8();
        return low | (u8() << 8);
    }
    void skip(size_t n) {
        if (n > d.size() - std::min(at, d.size())) ok = false;
        at = std::min(d.size(), at + n);
    }
    bool blocks(std::vector<uint8_t>* out) {
        for (;;) {
            const int size = u8();
            if (!ok) return false;
            if (size == 0) return true;
            if (out && at + size_t(size) <= d.size()) out->insert(out->end(), d.begin() + long(at), d.begin() + long(at + size_t(size)));
            skip(size_t(size));
            if (!ok) return false;
        }
    }
};

bool unpack(int minimum, const std::vector<uint8_t>& data, size_t count, std::vector<uint8_t>& out) {
    if (minimum < 2 || minimum > 11) return false;
    out.assign(count, 0);
    const int clearCode = 1 << minimum, endCode = clearCode + 1;
    std::vector<uint16_t> prefix(4096);
    std::vector<uint8_t> suffix(4096), stack(4097);
    for (int i = 0; i < clearCode; ++i) suffix[size_t(i)] = uint8_t(i);
    int size = minimum + 1, next = endCode + 1, old = -1;
    uint8_t first = 0;
    size_t written = 0;
    uint32_t bits = 0;
    int held = 0;
    for (size_t at = 0; written < count;) {
        while (held < size) {
            if (at >= data.size()) return written > 0;
            bits |= uint32_t(data[at++]) << held;
            held += 8;
        }
        int code = int(bits & ((1u << size) - 1));
        bits >>= size;
        held -= size;
        if (code == clearCode) {
            size = minimum + 1;
            next = endCode + 1;
            old = -1;
            continue;
        }
        if (code == endCode) break;
        if (old < 0) {
            if (code >= clearCode) return written > 0;
            out[written++] = first = uint8_t(code);
            old = code;
            continue;
        }
        const int incoming = code;
        size_t depth = 0;
        if (code >= next) {
            if (code > next) return written > 0;
            stack[depth++] = first;
            code = old;
        }
        while (code >= clearCode && depth < 4096) {
            stack[depth++] = suffix[size_t(code)];
            code = prefix[size_t(code)];
        }
        first = uint8_t(code);
        stack[depth++] = first;
        while (depth && written < count) out[written++] = stack[--depth];
        if (next < 4096) {
            prefix[size_t(next)] = uint16_t(old);
            suffix[size_t(next)] = first;
            ++next;
            if (next == (1 << size) && size < 12) ++size;
        }
        old = incoming;
    }
    return true;
}

}

bool gifRead(const std::string& path, GifInfo& info, const std::function<bool(uint32_t, const uint8_t*)>& frame, std::string* error) {
    info = GifInfo();
    FILE* file = openFile(path, "rb");
    if (!file) return fail(error, "cannot open file");
    std::vector<uint8_t> bytes;
    uint8_t chunk[65536];
    for (size_t got; (got = fread(chunk, 1, sizeof chunk, file)) > 0;) {
        bytes.insert(bytes.end(), chunk, chunk + got);
        if (bytes.size() > (size_t(1) << 30)) break;
    }
    fclose(file);
    if (bytes.size() < 13 || (memcmp(bytes.data(), "GIF87a", 6) != 0 && memcmp(bytes.data(), "GIF89a", 6) != 0)) return fail(error, "not a GIF file");
    Cursor c{bytes, 6};
    info.width = c.u16();
    info.height = c.u16();
    if (info.width <= 0 || info.height <= 0) return fail(error, "the GIF is damaged");
    if (int64_t(info.width) * info.height > kMostPixels) return fail(error, "the GIF is too large");
    const int packed = c.u8();
    c.skip(2);
    std::vector<uint8_t> global;
    if (packed & 0x80) {
        const size_t size = size_t(3) << ((packed & 7) + 1);
        if (c.at + size > bytes.size()) return fail(error, "the GIF is damaged");
        global.assign(bytes.begin() + long(c.at), bytes.begin() + long(c.at + size));
        c.skip(size);
    }
    std::vector<GifFrame> frames;
    GifFrame next;
    int64_t decodedPixels = 0;
    while (c.ok && frames.size() <= kMostFrames) {
        const int kind = c.u8();
        if (!c.ok || kind == 0x3B) break;
        if (kind == 0x21) {
            const int label = c.u8();
            std::vector<uint8_t> body;
            if (!c.blocks(&body)) break;
            if (label == 0xF9 && body.size() >= 4) {
                next.dispose = (body[0] >> 2) & 7;
                next.delay = body[1] | (body[2] << 8);
                next.clear = (body[0] & 1) ? body[3] : -1;
            }
        } else if (kind == 0x2C) {
            GifFrame f = next;
            next = GifFrame();
            f.x = c.u16();
            f.y = c.u16();
            f.width = c.u16();
            f.height = c.u16();
            const int flags = c.u8();
            f.interlaced = flags & 0x40;
            if (flags & 0x80) {
                const size_t size = size_t(3) << ((flags & 7) + 1);
                if (c.at + size > bytes.size()) break;
                f.palette.assign(bytes.begin() + long(c.at), bytes.begin() + long(c.at + size));
                c.skip(size);
            } else {
                f.palette = global;
            }
            const int minimum = c.u8();
            std::vector<uint8_t> data;
            if (!c.blocks(&data) || f.width <= 0 || f.height <= 0 || f.palette.empty() || int64_t(f.width) * f.height > kMostPixels) {
                if (frames.empty()) return fail(error, "the GIF is damaged");
                break;
            }
            decodedPixels += int64_t(f.width) * f.height;
            if (decodedPixels > kMostWork || int64_t(frames.size() + 1) * info.width * info.height > kMostWork) return fail(error, "the GIF is too large");
            if (!unpack(minimum, data, size_t(f.width) * size_t(f.height), f.indexes)) {
                if (frames.empty()) return fail(error, "the GIF is damaged");
                break;
            }
            frames.push_back(std::move(f));
        } else if (kind != 0) {
            break;
        }
    }
    if (frames.empty()) return fail(error, "the GIF is damaged");
    if (frames.size() > kMostFrames || int64_t(frames.size()) * info.width * info.height > kMostWork) return fail(error, "the GIF is too large");
    for (const GifFrame& f : frames) {
        info.delaysMs.push_back(gifDelayMs(f.delay / 100.0));
        info.durationMs += info.delaysMs.back();
    }
    if (info.durationMs > kLongest) return fail(error, "the GIF is too large");
    info.fps = int(std::clamp(std::floor(double(frames.size()) * 1000.0 / double(std::max<int64_t>(info.durationMs, 1)) + 0.5), 1.0, 60.0));
    if (!gifVideoSize(info.width, info.height, info.videoWidth, info.videoHeight)) return fail(error, "the GIF is too large");

    const size_t stride = size_t(info.width) * 4;
    std::vector<uint8_t> canvas(stride * size_t(info.height), 0), kept;
    for (size_t i = 0; i < frames.size(); ++i) {
        const GifFrame& f = frames[i];
        if (f.dispose == 3) kept = canvas;
        const int passes[4][2] = {{0, 8}, {4, 8}, {2, 4}, {1, 2}};
        int row = 0;
        for (int pass = 0; pass < (f.interlaced ? 4 : 1); ++pass) {
            const int start = f.interlaced ? passes[pass][0] : 0, step = f.interlaced ? passes[pass][1] : 1;
            for (int y = start; y < f.height; y += step, ++row) {
                const int canvasY = f.y + y;
                if (canvasY < 0 || canvasY >= info.height) continue;
                const uint8_t* line = f.indexes.data() + size_t(row) * size_t(f.width);
                for (int x = 0; x < f.width; ++x) {
                    const int canvasX = f.x + x;
                    if (canvasX >= info.width) break;
                    const int index = line[x];
                    if (index == f.clear || size_t(index) * 3 + 2 >= f.palette.size()) continue;
                    uint8_t* to = &canvas[size_t(canvasY) * stride + size_t(canvasX) * 4];
                    to[0] = f.palette[size_t(index) * 3 + 2];
                    to[1] = f.palette[size_t(index) * 3 + 1];
                    to[2] = f.palette[size_t(index) * 3];
                    to[3] = 255;
                }
            }
        }
        if (!info.transparent) {
            for (size_t at = 3; at < canvas.size(); at += 4) {
                if (canvas[at] != 255) {
                    info.transparent = true;
                    break;
                }
            }
        }
        if (frame && !frame(uint32_t(i), canvas.data())) return fail(error, "cancelled");
        if (f.dispose == 2) {
            for (int y = std::max(f.y, 0); y < std::min(f.y + f.height, info.height); ++y)
                for (int x = std::max(f.x, 0); x < std::min(f.x + f.width, info.width); ++x) memset(&canvas[size_t(y) * stride + size_t(x) * 4], 0, 4);
        } else if (f.dispose == 3 && kept.size() == canvas.size()) {
            canvas = kept;
        }
    }
    return true;
}

}

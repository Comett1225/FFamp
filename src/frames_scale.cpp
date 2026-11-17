#include "frames.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace absent {

namespace {

struct Taps {
    int count = 0;
    int stride = 0;
    std::vector<int> first;
    std::vector<float> weight;
};

double cubic(double x) {
    constexpr double a = -0.6;
    x = std::fabs(x);
    if (x < 1) return ((a + 2) * x - (a + 3)) * x * x + 1;
    if (x < 2) return ((a * x - 5 * a) * x + 8 * a) * x - 4 * a;
    return 0;
}

Taps makeTaps(int in, int out, double step, double origin) {
    const double widen = std::max(1.0, step), radius = 2 * widen;
    Taps taps;
    taps.count = std::min(in, int(std::ceil(radius * 2)));
    taps.stride = (taps.count + 3) & ~3;
    taps.first.resize(size_t(out));
    taps.weight.assign(size_t(out) * size_t(taps.stride), 0.0f);
    std::vector<double> row(size_t(taps.count));
    for (int o = 0; o < out; ++o) {
        const double centre = origin + o * step;
        const int left = int(std::floor(centre - radius)) + 1, right = int(std::floor(centre + radius));
        const int first = std::clamp(left, 0, in - taps.count);
        std::fill(row.begin(), row.end(), 0.0);
        double sum = 0;
        for (int index = left; index <= right; ++index) {
            const double w = cubic((index - centre) / widen);
            const int slot = std::clamp(std::clamp(index, 0, in - 1) - first, 0, taps.count - 1);
            row[size_t(slot)] += w;
            sum += w;
        }
        if (std::fabs(sum) < 1e-9) {
            row[size_t(std::clamp(int(std::lround(centre)) - first, 0, taps.count - 1))] = 1;
            sum = 1;
        }
        taps.first[size_t(o)] = first;
        for (int k = 0; k < taps.count; ++k) taps.weight[size_t(o) * size_t(taps.stride) + size_t(k)] = float(row[size_t(k)] / sum);
    }
    return taps;
}

void across4(const float* __restrict line, const int* __restrict first, const float* __restrict weight, float* __restrict target, int count, int taps) {
    for (int x = 0; x < count; ++x, weight += taps) {
        const float* samples = line + first[x];
#if defined(__aarch64__)
        float32x4_t sum = vmulq_f32(vld1q_f32(samples), vld1q_f32(weight));
        for (int k = 4; k < taps; k += 4) sum = vfmaq_f32(sum, vld1q_f32(samples + k), vld1q_f32(weight + k));
        target[x] = vaddvq_f32(sum);
#else
        float a = 0, b = 0, c = 0, d = 0;
        for (int k = 0; k < taps; k += 4) {
            a += samples[k] * weight[k];
            b += samples[k + 1] * weight[k + 1];
            c += samples[k + 2] * weight[k + 2];
            d += samples[k + 3] * weight[k + 3];
        }
        target[x] = (a + b) + (c + d);
#endif
    }
}

void add(const float* __restrict source, float* __restrict line, int count, float w) {
    for (int x = 0; x < count; ++x) line[x] += source[x] * w;
}

template <typename T>
void settle(const float* __restrict line, T* __restrict target, int count, float top) {
    for (int x = 0; x < count; ++x) target[x] = T(std::min(std::max(line[x], 0.0f), top));
}

std::shared_ptr<const Taps> tapsFor(int in, int out) {
    static std::mutex lock;
    static std::map<std::pair<int, int>, std::shared_ptr<const Taps>> known;
    std::lock_guard<std::mutex> hold(lock);
    if (known.size() > 64) known.clear();
    std::shared_ptr<const Taps>& slot = known[{in, out}];
    if (!slot) {
        const double step = double(in) / out;
        slot = std::make_shared<const Taps>(makeTaps(in, out, step, 0.5 * step - 0.5));
    }
    return slot;
}

void scaleRows(const Picture& p, int plane, uint8_t* out, int outWidth, int bytes, int top, const Taps& across, const Taps& down, int from, int to) {
    if (from >= to) return;
    static thread_local std::vector<float> middle, line;
    const int inWidth = plane ? p.chromaWidth : p.width;
    const int first = down.first[size_t(from)], rows = down.first[size_t(to) - 1] + down.count - first;
    if (middle.size() < size_t(rows) * size_t(outWidth)) middle.resize(size_t(rows) * size_t(outWidth));
    if (line.size() < size_t(std::max(inWidth, outWidth)) + 4) line.resize(size_t(std::max(inWidth, outWidth)) + 4);
    std::fill(line.begin() + inWidth, line.begin() + inWidth + 4, 0.0f);
    for (int y = 0; y < rows; ++y) {
        pictureRow(p, plane, first + y, line.data());
        across4(line.data(), across.first.data(), across.weight.data(), middle.data() + size_t(y) * size_t(outWidth), outWidth, across.stride);
    }
    for (int y = from; y < to; ++y) {
        std::fill(line.begin(), line.begin() + outWidth, 0.5f);
        const float* weight = down.weight.data() + size_t(y) * size_t(down.stride);
        for (int k = 0; k < down.count; ++k) {
            if (weight[k] == 0) continue;
            add(middle.data() + size_t(down.first[size_t(y)] - first + k) * size_t(outWidth), line.data(), outWidth, weight[k]);
        }
        uint8_t* target = out + size_t(y) * size_t(outWidth) * size_t(bytes);
        if (bytes == 2) settle(line.data(), reinterpret_cast<uint16_t*>(target), outWidth, float(top));
        else settle(line.data(), target, outWidth, float(top));
    }
}

void packed(const uint8_t* __restrict bgra, size_t pixels, uint8_t* __restrict out) {
    for (size_t i = 0; i < pixels; ++i, bgra += 4, out += 3) {
        out[0] = bgra[2];
        out[1] = bgra[1];
        out[2] = bgra[0];
    }
}

int unitBytes(PixelLayout layout) {
    return layout == PixelLayout::Rgb24 ? 3 : layout == PixelLayout::Gray ? 1 : 4;
}

void turnPlane(const uint8_t* in, int width, int height, int unit, int rotation, uint8_t* out) {
    const size_t u = size_t(unit);
    if (rotation == 180) {
        for (int y = 0; y < height; ++y) {
            const uint8_t* from = in + (size_t(height - 1 - y) * size_t(width) + size_t(width - 1)) * u;
            uint8_t* to = out + size_t(y) * size_t(width) * u;
            for (int x = 0; x < width; ++x, from -= u, to += u) memcpy(to, from, u);
        }
        return;
    }
    const bool clockwise = rotation == 90;
    inParallel(width, [&](int first, int last) {
        for (int y = first; y < last; ++y) {
            uint8_t* to = out + size_t(y) * size_t(height) * u;
            for (int x = 0; x < height; ++x, to += u) {
                const int sx = clockwise ? y : width - 1 - y, sy = clockwise ? height - 1 - x : x;
                memcpy(to, in + (size_t(sy) * size_t(width) + size_t(sx)) * u, u);
            }
        }
    });
}

}

bool pixelLayoutNamed(const std::string& name, PixelLayout& layout) {
    if (name == "rgba") layout = PixelLayout::Rgba;
    else if (name == "rgb24" || name == "rgb") layout = PixelLayout::Rgb24;
    else if (name == "i420" || name == "yuv420p") layout = PixelLayout::I420;
    else if (name == "bgra") layout = PixelLayout::Bgra;
    else if (name == "gray" || name == "grey") layout = PixelLayout::Gray;
    else return false;
    return true;
}

size_t pixelBytes(PixelLayout layout, int width, int height) {
    if (width <= 0 || height <= 0) return 0;
    const size_t area = size_t(width) * size_t(height);
    if (layout == PixelLayout::I420) return area + 2 * size_t((width + 1) / 2) * size_t((height + 1) / 2);
    return area * size_t(unitBytes(layout));
}

bool pictureToLayout(const Picture& p, int width, int height, PixelLayout layout, std::vector<uint8_t>& out) {
    if (width <= 0 || height <= 0 || p.width <= 0 || p.height <= 0 || p.chromaWidth <= 0 || p.chromaHeight <= 0 || p.bits < 8 || p.bits > 16 || layout == PixelLayout::Gray) return false;
    const bool planar = layout == PixelLayout::I420, same = width == p.width && height == p.height;
    if (planar && !same && rgbPlanes(p)) {
        std::vector<uint8_t> bgra;
        if (!pictureToLayout(p, width, height, PixelLayout::Bgra, bgra)) return false;
        out.resize(pixelBytes(layout, width, height));
        bgraToI420(bgra.data(), size_t(width) * 4, width, height, out.data());
        return true;
    }
    out.resize(pixelBytes(layout, width, height));
    std::vector<uint8_t> bgra;
    auto finish = [&](const Picture& ready) {
        if (layout != PixelLayout::Rgb24) return pictureToBGRA(ready, out.data(), size_t(width) * 4, layout == PixelLayout::Rgba);
        bgra.resize(size_t(width) * size_t(height) * 4);
        if (!pictureToBGRA(ready, bgra.data(), size_t(width) * 4)) return false;
        packed(bgra.data(), size_t(width) * size_t(height), out.data());
        return true;
    };
    if (same) {
        if (!planar) return finish(p);
        pictureToI420(p, out);
        return true;
    }
    const int bytes = p.bits > 8 ? 2 : 1, top = (1 << p.bits) - 1;
    const int chromaWidth = planar ? (width + 1) / 2 : width, chromaHeight = planar ? (height + 1) / 2 : height;
    const size_t luma = size_t(width) * size_t(height) * size_t(bytes), chroma = size_t(chromaWidth) * size_t(chromaHeight) * size_t(bytes);
    static thread_local std::vector<uint8_t> wide;
    std::vector<uint8_t>& scaled = planar && bytes == 1 ? out : wide;
    scaled.resize(luma + 2 * chroma);
    Picture ready;
    ready.plane[0] = scaled.data();
    ready.plane[1] = scaled.data() + luma;
    ready.plane[2] = scaled.data() + luma + chroma;
    ready.stride[0] = size_t(width) * size_t(bytes);
    ready.stride[1] = ready.stride[2] = size_t(chromaWidth) * size_t(bytes);
    ready.width = width;
    ready.height = height;
    ready.chromaWidth = chromaWidth;
    ready.chromaHeight = chromaHeight;
    ready.bits = p.bits;
    ready.color = p.color;
    const int matrix = p.color.known ? p.color.matrix : 2;
    if (matrix != 0 && matrix != 1 && matrix != 5 && matrix != 6 && matrix != 9 && matrix != 10 && p.color.transfer != 16 && p.color.transfer != 18) {
        ready.color.known = true;
        ready.color.matrix = p.height >= 720 ? 1 : 6;
    }
    const auto lumaAcross = tapsFor(p.width, width), lumaDown = tapsFor(p.height, height);
    const auto chromaAcross = tapsFor(p.chromaWidth, chromaWidth), chromaDown = tapsFor(p.chromaHeight, chromaHeight);
    const bool direct = layout == PixelLayout::Bgra || layout == PixelLayout::Rgba;
    static thread_local std::vector<uint8_t> mask;
    Picture cover;
    if (p.alpha && !planar) {
        mask.resize(size_t(width) * size_t(height));
        cover.plane[0] = p.alpha;
        cover.stride[0] = p.alphaStride;
        cover.width = p.width;
        cover.height = p.height;
        ready.alpha = mask.data();
        ready.alphaStride = size_t(width);
    }
    uint8_t* const veil = ready.alpha ? mask.data() : nullptr;
    inParallel(height, [&](int from, int to) {
        if (veil) scaleRows(cover, 0, veil, width, 1, 255, *lumaAcross, *lumaDown, from, to);
        scaleRows(p, 0, scaled.data(), width, bytes, top, *lumaAcross, *lumaDown, from, to);
        const int chromaFrom = int(int64_t(from) * chromaHeight / height), chromaTo = int(int64_t(to) * chromaHeight / height);
        for (int plane = 1; plane <= 2; ++plane) {
            scaleRows(p, plane, scaled.data() + luma + size_t(plane - 1) * chroma, chromaWidth, bytes, top, *chromaAcross, *chromaDown, chromaFrom, chromaTo);
        }
        if (direct) rowsToBGRA(ready, out.data(), size_t(width) * 4, layout == PixelLayout::Rgba, from, to);
    });
    if (direct) return true;
    if (!planar) return finish(ready);
    if (bytes == 2) pictureToPlanar(ready, out, true);
    return true;
}

void turnPixels(const std::vector<uint8_t>& in, int width, int height, PixelLayout layout, int rotation, std::vector<uint8_t>& out) {
    out.resize(in.size());
    if (layout != PixelLayout::I420) {
        turnPlane(in.data(), width, height, unitBytes(layout), rotation, out.data());
        return;
    }
    const int cw = (width + 1) / 2, ch = (height + 1) / 2;
    const size_t luma = size_t(width) * size_t(height), chroma = size_t(cw) * size_t(ch);
    turnPlane(in.data(), width, height, 1, rotation, out.data());
    turnPlane(in.data() + luma, cw, ch, 1, rotation, out.data() + luma);
    turnPlane(in.data() + luma + chroma, cw, ch, 1, rotation, out.data() + luma + chroma);
}

void layoutToI420(const uint8_t* pixels, int width, int height, PixelLayout layout, std::vector<uint8_t>& out) {
    const size_t area = size_t(width) * size_t(height);
    out.resize(pixelBytes(PixelLayout::I420, width, height));
    if (layout == PixelLayout::I420) {
        memcpy(out.data(), pixels, out.size());
    } else if (layout == PixelLayout::Gray) {
        for (size_t i = 0; i < area; ++i) out[i] = uint8_t(16 + (pixels[i] * 219 + 127) / 255);
        memset(out.data() + area, 128, out.size() - area);
    } else {
        bgraToI420(pixels, size_t(width) * 4, width, height, out.data(), layout == PixelLayout::Rgba);
    }
}

}

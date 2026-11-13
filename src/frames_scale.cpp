#include "frames.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

namespace absent {

namespace {

struct Taps {
    int count = 0;
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
    taps.count = std::min(in, int(std::ceil(radius * 2)) + 1);
    taps.first.resize(size_t(out));
    taps.weight.assign(size_t(out) * size_t(taps.count), 0.0f);
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
        for (int k = 0; k < taps.count; ++k) taps.weight[size_t(o) * size_t(taps.count) + size_t(k)] = float(row[size_t(k)] / sum);
    }
    return taps;
}

void inRows(int rows, const std::function<void(int, int)>& work) {
    const int workers = std::clamp(int(std::thread::hardware_concurrency()), 1, 8);
    if (workers == 1 || rows < 64) {
        work(0, rows);
        return;
    }
    std::vector<std::thread> pool;
    for (int k = 1; k < workers; ++k) pool.emplace_back(work, rows * k / workers, rows * (k + 1) / workers);
    work(0, rows / workers);
    for (std::thread& worker : pool) worker.join();
}

template <typename T>
void scaleSamples(const uint8_t* in, size_t inStride, int inWidth, int inHeight, uint8_t* out, int outWidth, int outHeight, int top, const Taps& across, const Taps& down) {
    std::vector<float> middle(size_t(inHeight) * size_t(outWidth));
    inRows(inHeight, [&](int from, int to) {
        std::vector<float> line(static_cast<size_t>(inWidth));
        for (int y = from; y < to; ++y) {
            const T* source = reinterpret_cast<const T*>(in + size_t(y) * inStride);
            for (int x = 0; x < inWidth; ++x) line[size_t(x)] = float(source[x]);
            float* target = middle.data() + size_t(y) * size_t(outWidth);
            const float* weight = across.weight.data();
            for (int x = 0; x < outWidth; ++x, weight += across.count) {
                const float* samples = line.data() + across.first[size_t(x)];
                float sum = 0;
                for (int k = 0; k < across.count; ++k) sum += samples[k] * weight[k];
                target[x] = sum;
            }
        }
    });
    inRows(outHeight, [&](int from, int to) {
        std::vector<float> line(static_cast<size_t>(outWidth));
        for (int y = from; y < to; ++y) {
            std::fill(line.begin(), line.end(), 0.5f);
            const float* weight = down.weight.data() + size_t(y) * size_t(down.count);
            for (int k = 0; k < down.count; ++k) {
                const float w = weight[k];
                if (w == 0) continue;
                const float* source = middle.data() + size_t(down.first[size_t(y)] + k) * size_t(outWidth);
                for (int x = 0; x < outWidth; ++x) line[size_t(x)] += source[x] * w;
            }
            T* target = reinterpret_cast<T*>(out + size_t(y) * size_t(outWidth) * sizeof(T));
            for (int x = 0; x < outWidth; ++x) target[x] = T(std::clamp(line[size_t(x)], 0.0f, float(top)));
        }
    });
}

void packed(const uint8_t* bgra, size_t pixels, PixelLayout layout, uint8_t* out) {
    if (layout == PixelLayout::Rgba) {
        for (size_t i = 0; i < pixels; ++i, bgra += 4, out += 4) {
            out[0] = bgra[2];
            out[1] = bgra[1];
            out[2] = bgra[0];
            out[3] = 255;
        }
    } else {
        for (size_t i = 0; i < pixels; ++i, bgra += 4, out += 3) {
            out[0] = bgra[2];
            out[1] = bgra[1];
            out[2] = bgra[0];
        }
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
    inRows(width, [&](int first, int last) {
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

void scalePlane(const uint8_t* in, size_t inStride, int inWidth, int inHeight, uint8_t* out, int outWidth, int outHeight, int bytes, int top, double stepX, double originX, double stepY, double originY) {
    const Taps across = makeTaps(inWidth, outWidth, stepX, originX), down = makeTaps(inHeight, outHeight, stepY, originY);
    if (bytes == 2) scaleSamples<uint16_t>(in, inStride, inWidth, inHeight, out, outWidth, outHeight, top, across, down);
    else scaleSamples<uint8_t>(in, inStride, inWidth, inHeight, out, outWidth, outHeight, top, across, down);
}

bool pictureToLayout(const Picture& p, int width, int height, PixelLayout layout, std::vector<uint8_t>& out) {
    if (width <= 0 || height <= 0 || p.width <= 0 || p.height <= 0 || p.chromaWidth <= 0 || p.chromaHeight <= 0 || layout == PixelLayout::Gray) return false;
    const bool planar = layout == PixelLayout::I420, same = width == p.width && height == p.height;
    out.resize(pixelBytes(layout, width, height));
    std::vector<uint8_t> bgra;
    auto finish = [&](const Picture& ready) {
        if (layout == PixelLayout::Bgra) return pictureToBGRA(ready, out.data(), size_t(width) * 4);
        bgra.resize(size_t(width) * size_t(height) * 4);
        if (!pictureToBGRA(ready, bgra.data(), size_t(width) * 4)) return false;
        packed(bgra.data(), size_t(width) * size_t(height), layout, out.data());
        return true;
    };
    if (same) {
        if (!planar) return finish(p);
        pictureToI420(p, out);
        return true;
    }
    std::vector<uint8_t> flat;
    pictureToPlanar(p, flat);
    const int bytes = p.bits > 8 ? 2 : 1, top = (1 << p.bits) - 1;
    const int chromaWidth = planar ? (width + 1) / 2 : width, chromaHeight = planar ? (height + 1) / 2 : height;
    const size_t luma = size_t(width) * size_t(height) * size_t(bytes), chroma = size_t(chromaWidth) * size_t(chromaHeight) * size_t(bytes);
    std::vector<uint8_t> wide;
    std::vector<uint8_t>& scaled = planar && bytes == 1 ? out : wide;
    scaled.resize(luma + 2 * chroma);
    const double ratioX = double(p.width) / width, ratioY = double(p.height) / height;
    scalePlane(flat.data(), size_t(p.width) * size_t(bytes), p.width, p.height, scaled.data(), width, height, bytes, top, ratioX, 0.5 * ratioX - 0.5, ratioY, 0.5 * ratioY - 0.5);
    const double stepX = double(p.chromaWidth) / chromaWidth, stepY = double(p.chromaHeight) / chromaHeight;
    const double originX = 0.5 * stepX - 0.5, originY = 0.5 * stepY - 0.5;
    const size_t sourceLuma = size_t(p.width) * size_t(p.height) * size_t(bytes), sourceChroma = size_t(p.chromaWidth) * size_t(p.chromaHeight) * size_t(bytes);
    for (size_t plane = 0; plane < 2; ++plane) {
        scalePlane(flat.data() + sourceLuma + plane * sourceChroma, size_t(p.chromaWidth) * size_t(bytes), p.chromaWidth, p.chromaHeight, scaled.data() + luma + plane * chroma, chromaWidth, chromaHeight,
            bytes, top, stepX, originX, stepY, originY);
    }
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

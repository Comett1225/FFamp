#include "soft_video.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

namespace absent {

namespace {

template <typename In, typename Out>
void spread(const In* __restrict in, Out* __restrict out, int count, bool pairs, int shift) {
    if (pairs) for (int x = 0; x < count; ++x) out[x] = Out(in[2 * x] >> shift);
    else for (int x = 0; x < count; ++x) out[x] = Out(in[x] >> shift);
}

template <typename Out>
void rowOf(const Picture& p, int plane, int y, Out* out) {
    const int count = plane == 0 ? p.width : p.chromaWidth;
    const bool second = plane == 2 && p.interleaved;
    const uint8_t* line = p.plane[second ? 1 : plane] + size_t(y) * p.stride[second ? 1 : plane];
    const bool pairs = plane != 0 && p.interleaved;
    if (p.bits == 8) spread(line + (second ? 1 : 0), out, count, pairs, 0);
    else spread(reinterpret_cast<const uint16_t*>(line) + (second ? 1 : 0), out, count, pairs, p.shift);
}

void inParallel(int height, const std::function<void(int, int)>& rows) {
    const int workers = std::clamp(int(std::thread::hardware_concurrency()), 1, 8);
    if (workers == 1 || height < 64) {
        rows(0, height);
        return;
    }
    std::vector<std::thread> pool;
    for (int k = 1; k < workers; ++k) pool.emplace_back(rows, height * k / workers, height * (k + 1) / workers);
    rows(0, height / workers);
    for (std::thread& worker : pool) worker.join();
}

struct Levels {
    std::vector<float> hlg, pq, encode;
    Levels() : hlg(4096), pq(4096), encode(4096) {
        for (int i = 0; i < 4096; ++i) {
            const float n = float(i) / 4095.0f;
            hlg[size_t(i)] = n <= 0.5f ? n * n / 3 : (std::exp((n - 0.55991073f) / 0.17883277f) + 0.28466892f) / 12;
            const float p = std::pow(n, 1 / 78.84375f);
            pq[size_t(i)] = std::pow(std::max(p - 0.8359375f, 0.0f) / (18.8515625f - 18.6875f * p), 1 / 0.1593017578125f) * 10000 / 203;
            encode[size_t(i)] = (n <= 0.0031308f ? 12.92f * n : 1.055f * std::pow(n, 1 / 2.4f) - 0.055f) * 255.0f + 0.5f;
        }
    }
};

float softKnee(float m) {
    constexpr float k = 0.8f;
    return m <= k ? m : k + (1 - k) * (m - k) / ((m - k) + (1 - k));
}

}

void pictureToPlanar(const Picture& p, std::vector<uint8_t>& out, bool narrow) {
    const size_t bytes = p.bits > 8 && !narrow ? 2 : 1;
    const int drop = p.bits - 8;
    const size_t luma = size_t(p.width) * size_t(p.height), chroma = size_t(p.chromaWidth) * size_t(p.chromaHeight);
    out.resize((luma + 2 * chroma) * bytes);
    std::vector<uint16_t> line(size_t(std::max(p.width, p.chromaWidth)));
    uint8_t* to = out.data();
    for (int plane = 0; plane < 3; ++plane) {
        const int count = plane == 0 ? p.width : p.chromaWidth, rows = plane == 0 ? p.height : p.chromaHeight;
        for (int y = 0; y < rows; ++y) {
            if (p.bits == 8 && !(plane != 0 && p.interleaved)) {
                memcpy(to, p.plane[plane] + size_t(y) * p.stride[plane], size_t(count));
            } else {
                if (bytes == 2) {
                    rowOf(p, plane, y, reinterpret_cast<uint16_t*>(to));
                    to += size_t(count) * bytes;
                    continue;
                }
                if (drop == 0) {
                    rowOf(p, plane, y, to);
                    to += size_t(count);
                    continue;
                }
                rowOf(p, plane, y, line.data());
                if (drop == 0) for (int x = 0; x < count; ++x) to[x] = uint8_t(line[size_t(x)]);
                else for (int x = 0; x < count; ++x) to[x] = uint8_t(std::min((line[size_t(x)] + (1 << (drop - 1))) >> drop, 255));
            }
            to += size_t(count) * bytes;
        }
    }
}

void pictureToI420(const Picture& p, std::vector<uint8_t>& out) {
    const int cw = (p.width + 1) / 2, ch = (p.height + 1) / 2;
    if (p.chromaWidth == cw && p.chromaHeight == ch) {
        pictureToPlanar(p, out, true);
        return;
    }
    std::vector<uint8_t> whole;
    pictureToPlanar(p, whole, true);
    const size_t luma = size_t(p.width) * size_t(p.height), from = size_t(p.chromaWidth) * size_t(p.chromaHeight);
    out.resize(luma + 2 * size_t(cw) * size_t(ch));
    memcpy(out.data(), whole.data(), luma);
    const int stepX = p.chromaWidth > cw ? 2 : 1, stepY = p.chromaHeight > ch ? 2 : 1;
    for (int plane = 0; plane < 2; ++plane) {
        const uint8_t* source = whole.data() + luma + size_t(plane) * from;
        uint8_t* to = out.data() + luma + size_t(plane) * size_t(cw) * size_t(ch);
        for (int y = 0; y < ch; ++y) {
            const uint8_t* a = source + size_t(std::min(y * stepY, p.chromaHeight - 1)) * size_t(p.chromaWidth);
            const uint8_t* b = source + size_t(std::min(y * stepY + stepY - 1, p.chromaHeight - 1)) * size_t(p.chromaWidth);
            for (int x = 0; x < cw; ++x) {
                const int x0 = std::min(x * stepX, p.chromaWidth - 1), x1 = std::min(x * stepX + stepX - 1, p.chromaWidth - 1);
                to[size_t(y) * size_t(cw) + size_t(x)] = uint8_t((a[x0] + a[x1] + b[x0] + b[x1] + 2) >> 2);
            }
        }
    }
}

bool pictureToBGRA(const Picture& p, uint8_t* out, size_t stride) {
    if (p.width <= 0 || p.height <= 0 || p.chromaWidth <= 0 || p.chromaHeight <= 0 || p.bits < 8 || p.bits > 16) return false;
    static const Levels levels;
    const bool hlg = p.color.transfer == 18, hdr = hlg || p.color.transfer == 16;
    int matrix = p.color.known ? p.color.matrix : 2;
    if (matrix != 1 && matrix != 5 && matrix != 6 && matrix != 9 && matrix != 10) matrix = hdr ? 9 : p.height >= 720 ? 1 : 6;
    const float kr = matrix == 1 ? 0.2126f : matrix >= 9 ? 0.2627f : 0.299f;
    const float kb = matrix == 1 ? 0.0722f : matrix >= 9 ? 0.0593f : 0.114f;
    const float kg = 1 - kr - kb;
    const float top = float((1 << p.bits) - 1), unit = float(1 << (p.bits - 8));
    const float lumaBase = p.color.full ? 0 : 16 * unit, lumaScale = p.color.full ? 1 / top : 1 / (219 * unit);
    const float chromaBase = 128 * unit, chromaScale = p.color.full ? 1 / top : 1 / (224 * unit);
    const bool halfRows = p.chromaHeight < p.height, halfColumns = p.chromaWidth < p.width;
    const std::vector<float>& linear = hlg ? levels.hlg : levels.pq;

    inParallel(p.height, [&](int from, int to) {
        const size_t cw = size_t(p.chromaWidth);
        std::vector<uint16_t> luma(size_t(p.width)), a(cw), b(cw);
        std::vector<float> u(cw + 1), v(cw + 1);
        for (int y = from; y < to; ++y) {
            rowOf(p, 0, y, luma.data());
            int near = y, far = y;
            float nearWeight = 1;
            if (halfRows) {
                near = y / 2;
                far = std::clamp(y % 2 ? near + 1 : near - 1, 0, p.chromaHeight - 1);
                nearWeight = 0.75f;
            }
            near = std::min(near, p.chromaHeight - 1);
            for (int plane = 1; plane <= 2; ++plane) {
                std::vector<float>& mixed = plane == 1 ? u : v;
                rowOf(p, plane, near, a.data());
                rowOf(p, plane, far, b.data());
                for (size_t x = 0; x < cw; ++x) mixed[x] = ((float(a[x]) * nearWeight + float(b[x]) * (1 - nearWeight)) - chromaBase) * chromaScale;
                mixed[cw] = mixed[cw - 1];
            }
            uint8_t* row = out + size_t(y) * stride;
            for (int x = 0; x < p.width; ++x, row += 4) {
                float cb, cr;
                if (halfColumns) {
                    const size_t c = std::min(size_t(x / 2), cw - 1);
                    const size_t beside = x % 2 ? c + 1 : (c > 0 ? c - 1 : 0);
                    cb = 0.75f * u[c] + 0.25f * u[beside];
                    cr = 0.75f * v[c] + 0.25f * v[beside];
                } else {
                    cb = u[size_t(x)];
                    cr = v[size_t(x)];
                }
                const float l = (float(luma[size_t(x)]) - lumaBase) * lumaScale;
                float r = l + 2 * (1 - kr) * cr, bl = l + 2 * (1 - kb) * cb;
                float g = (l - kr * r - kb * bl) / kg;
                r = std::clamp(r, 0.0f, 1.0f);
                g = std::clamp(g, 0.0f, 1.0f);
                bl = std::clamp(bl, 0.0f, 1.0f);
                if (!hdr) {
                    row[0] = uint8_t(bl * 255 + 0.5f);
                    row[1] = uint8_t(g * 255 + 0.5f);
                    row[2] = uint8_t(r * 255 + 0.5f);
                    row[3] = 255;
                    continue;
                }
                r = linear[size_t(r * 65535 + 0.5f) >> 4];
                g = linear[size_t(g * 65535 + 0.5f) >> 4];
                bl = linear[size_t(bl * 65535 + 0.5f) >> 4];
                if (hlg) {
                    const float gain = 4.926f * std::pow(std::max(0.2627f * r + 0.678f * g + 0.0593f * bl, 1e-6f), 0.2f);
                    r *= gain;
                    g *= gain;
                    bl *= gain;
                }
                const float c[3] = {-0.0182f * r - 0.1006f * g + 1.1187f * bl, -0.1246f * r + 1.1329f * g - 0.0083f * bl, 1.6605f * r - 0.5876f * g - 0.0728f * bl};
                const float peak = std::max({c[0], c[1], c[2], 1e-6f});
                const float scale = softKnee(peak) / peak;
                for (int k = 0; k < 3; ++k) row[k] = uint8_t(levels.encode[size_t(std::clamp(c[k] * scale, 0.0f, 1.0f) * 4095.0f + 0.5f)]);
                row[3] = 255;
            }
        }
    });
    return true;
}

void bgraToI420(const uint8_t* pixels, size_t stride, int width, int height, uint8_t* out, bool redFirst) {
    const int cw = (width + 1) / 2, ch = (height + 1) / 2;
    const int ri = redFirst ? 0 : 2, bi = redFirst ? 2 : 0;
    uint8_t* lumaOut = out;
    uint8_t* uOut = out + size_t(width) * size_t(height);
    uint8_t* vOut = uOut + size_t(cw) * size_t(ch);
    inParallel(ch, [&](int from, int to) {
        for (int cy = from; cy < to; ++cy) {
            const int y0 = cy * 2, y1 = std::min(y0 + 1, height - 1);
            for (int y = y0; y <= y1; ++y) {
                const uint8_t* line = pixels + size_t(y) * stride;
                uint8_t* luma = lumaOut + size_t(y) * size_t(width);
                for (int x = 0; x < width; ++x, line += 4) luma[x] = uint8_t((11966 * line[ri] + 40254 * line[1] + 4064 * line[bi] + (16 << 16) + 32768) >> 16);
            }
            const uint8_t* top = pixels + size_t(y0) * stride;
            for (int cx = 0; cx < cw; ++cx, top += 8) {
                const int r = top[ri], g = top[1], b = top[bi];
                uOut[size_t(cy) * size_t(cw) + size_t(cx)] = uint8_t((-6596 * r - 22188 * g + 28784 * b + (128 << 16) + 32768) >> 16);
                vOut[size_t(cy) * size_t(cw) + size_t(cx)] = uint8_t((28784 * r - 26145 * g - 2639 * b + (128 << 16) + 32768) >> 16);
            }
        }
    });
}

}

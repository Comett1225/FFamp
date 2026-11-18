#include "soft_video.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
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

struct Crew {
    const int hands = std::clamp(int(std::thread::hardware_concurrency()), 1, 16);
    int parts = 1;
    std::mutex turn, lock;
    std::condition_variable wake, done;
    const std::function<void(int, int)>* work = nullptr;
    int rows = 0;
    int left = 0;
    int busy = 0;
    uint64_t round = 0;
    std::atomic<int> next{1 << 30};

    Crew() {
        for (int k = 1; k < hands; ++k) {
            std::thread([this] {
                uint64_t seen = 0;
                for (;;) {
                    {
                        std::unique_lock<std::mutex> hold(lock);
                        wake.wait(hold, [&] { return round != seen; });
                        seen = round;
                        ++busy;
                    }
                    help();
                    std::lock_guard<std::mutex> hold(lock);
                    if (--busy == 0) done.notify_all();
                }
            }).detach();
        }
    }

    void help() {
        for (;;) {
            const int part = next.fetch_add(1);
            if (part >= parts) return;
            (*work)(int(int64_t(rows) * part / parts), int(int64_t(rows) * (part + 1) / parts));
            std::lock_guard<std::mutex> hold(lock);
            if (--left == 0) done.notify_all();
        }
    }

    void run(int count, const std::function<void(int, int)>& job) {
        std::lock_guard<std::mutex> mine(turn);
        {
            std::unique_lock<std::mutex> hold(lock);
            done.wait(hold, [&] { return busy == 0; });
            work = &job;
            rows = count;
            parts = std::min(hands * 4, count / 16);
            left = parts;
            ++round;
            next = 0;
        }
        wake.notify_all();
        help();
        std::unique_lock<std::mutex> hold(lock);
        done.wait(hold, [&] { return left == 0; });
        next = 1 << 30;
    }
};

template <int R, int B>
void sdrRow(const float* __restrict l, const float* __restrict cbs, const float* __restrict crs, uint8_t* __restrict row, int width, float kr, float kb, float kg) {
    const float toRed = 2 * (1 - kr), toBlue = 2 * (1 - kb);
    for (int x = 0; x < width; ++x) {
        float r = l[x] + toRed * crs[x], bl = l[x] + toBlue * cbs[x];
        float g = (l[x] - kr * r - kb * bl) / kg;
        r = std::min(std::max(r, 0.0f), 1.0f);
        g = std::min(std::max(g, 0.0f), 1.0f);
        bl = std::min(std::max(bl, 0.0f), 1.0f);
        row[4 * x + B] = uint8_t(bl * 255 + 0.5f);
        row[4 * x + 1] = uint8_t(g * 255 + 0.5f);
        row[4 * x + R] = uint8_t(r * 255 + 0.5f);
        row[4 * x + 3] = 255;
    }
}

void widen(const float* __restrict u, float* __restrict out, int count) {
    for (int c = 0; c < count; ++c) {
        out[2 * c] = 0.75f * u[c] + 0.25f * u[c - 1];
        out[2 * c + 1] = 0.75f * u[c] + 0.25f * u[c + 1];
    }
}

void mix(const uint16_t* __restrict a, const uint16_t* __restrict b, float* __restrict out, int count, float nearWeight, float base, float scale) {
    for (int x = 0; x < count; ++x) out[x] = ((float(a[x]) * nearWeight + float(b[x]) * (1 - nearWeight)) - base) * scale;
}

void level(const uint16_t* __restrict in, float* __restrict out, int count, float base, float scale) {
    for (int x = 0; x < count; ++x) out[x] = (float(in[x]) - base) * scale;
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

void inParallel(int rows, const std::function<void(int, int)>& work) {
    static Crew& crew = *new Crew;
    if (crew.hands == 1 || rows < 64) work(0, rows);
    else crew.run(rows, work);
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

bool rgbPlanes(const Picture& p) { return p.color.known && p.color.matrix == 0 && p.chromaWidth == p.width && p.chromaHeight == p.height; }

void pictureToI420(const Picture& p, std::vector<uint8_t>& out) {
    const int cw = (p.width + 1) / 2, ch = (p.height + 1) / 2;
    if (rgbPlanes(p)) {
        std::vector<uint8_t> bgra(size_t(p.width) * size_t(p.height) * 4);
        pictureToBGRA(p, bgra.data(), size_t(p.width) * 4);
        out.resize(size_t(p.width) * size_t(p.height) + 2 * size_t(cw) * size_t(ch));
        bgraToI420(bgra.data(), size_t(p.width) * 4, p.width, p.height, out.data());
        return;
    }
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
            const int rowA = p.chromaHeight < ch ? y * p.chromaHeight / ch : y * stepY, rowB = p.chromaHeight < ch ? rowA : rowA + stepY - 1;
            const uint8_t* a = source + size_t(std::min(rowA, p.chromaHeight - 1)) * size_t(p.chromaWidth);
            const uint8_t* b = source + size_t(std::min(rowB, p.chromaHeight - 1)) * size_t(p.chromaWidth);
            for (int x = 0; x < cw; ++x) {
                if (p.chromaWidth < cw) {
                    const int c = std::min(x / 2, p.chromaWidth - 1), side = x % 2 ? std::min(c + 1, p.chromaWidth - 1) : std::max(c - 1, 0);
                    to[size_t(y) * size_t(cw) + size_t(x)] = uint8_t((3 * (a[c] + b[c]) + a[side] + b[side] + 4) >> 3);
                    continue;
                }
                const int x0 = std::min(x * stepX, p.chromaWidth - 1), x1 = std::min(x * stepX + stepX - 1, p.chromaWidth - 1);
                to[size_t(y) * size_t(cw) + size_t(x)] = uint8_t((a[x0] + a[x1] + b[x0] + b[x1] + 2) >> 2);
            }
        }
    }
}

void pictureRow(const Picture& p, int plane, int y, float* out) {
    rowOf(p, plane, y, out);
}

void rowsToBGRA(const Picture& p, uint8_t* out, size_t stride, bool redFirst, int from, int to) {
    if (rgbPlanes(p)) {
        std::vector<uint16_t> line(size_t(p.width));
        const int drop = p.bits - 8, place[3] = {1, redFirst ? 2 : 0, redFirst ? 0 : 2};
        for (int y = from; y < to; ++y) {
            uint8_t* row = out + size_t(y) * stride;
            for (int plane = 0; plane < 3; ++plane) {
                rowOf(p, plane, y, line.data());
                for (int x = 0; x < p.width; ++x) row[4 * x + place[plane]] = uint8_t(drop ? std::min((line[size_t(x)] + (1 << (drop - 1))) >> drop, 255) : line[size_t(x)]);
            }
            const uint8_t* mask = p.alpha ? p.alpha + size_t(y) * p.alphaStride : nullptr;
            for (int x = 0; x < p.width; ++x) row[4 * x + 3] = mask ? mask[x] : 255;
        }
        return;
    }
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
    const bool halfRows = p.chromaHeight < p.height, quarterRows = p.chromaHeight * 2 < p.height, halfColumns = p.chromaWidth < p.width, quarterColumns = p.chromaWidth * 2 < p.width;
    const std::vector<float>& linear = hlg ? levels.hlg : levels.pq;


    const size_t cw = size_t(p.chromaWidth);
    std::vector<uint16_t> luma(size_t(p.width)), a(cw), b(cw);
    std::vector<float> u(cw + 1), v(cw + 1);
    const bool quick = !hdr && (!halfColumns || cw * 2 >= size_t(p.width));
    std::vector<float> lumaLevel, padded[2], wide[2];
    if (quick) {
        lumaLevel.resize(size_t(p.width));
        for (auto& line : padded) line.resize(cw + 2);
        if (halfColumns) for (auto& line : wide) line.resize(cw * 2);
    }
    for (int y = from; y < to; ++y) {
        rowOf(p, 0, y, luma.data());
        int near = y, far = y;
        float nearWeight = 1;
        if (quarterRows) {
            near = y / 4;
            far = std::clamp(y % 4 >= 2 ? near + 1 : near - 1, 0, p.chromaHeight - 1);
            nearWeight = y % 4 == 0 || y % 4 == 3 ? 0.625f : 0.875f;
        } else if (halfRows) {
            near = y / 2;
            far = std::clamp(y % 2 ? near + 1 : near - 1, 0, p.chromaHeight - 1);
            nearWeight = 0.75f;
        }
        near = std::min(near, p.chromaHeight - 1);
        for (int plane = 1; plane <= 2; ++plane) {
            std::vector<float>& mixed = plane == 1 ? u : v;
            rowOf(p, plane, near, a.data());
            rowOf(p, plane, far, b.data());
            if (quick) {
                float* line = padded[plane - 1].data() + 1;
                mix(a.data(), b.data(), line, int(cw), nearWeight, chromaBase, chromaScale);
                line[-1] = line[0];
                line[cw] = line[cw - 1];
                if (halfColumns) widen(line, wide[plane - 1].data(), int(cw));
                continue;
            }
            for (size_t x = 0; x < cw; ++x) mixed[x] = ((float(a[x]) * nearWeight + float(b[x]) * (1 - nearWeight)) - chromaBase) * chromaScale;
            mixed[cw] = mixed[cw - 1];
        }
        uint8_t* row = out + size_t(y) * stride;
        if (quick) {
            level(luma.data(), lumaLevel.data(), p.width, lumaBase, lumaScale);
            const float* cbs = halfColumns ? wide[0].data() : padded[0].data() + 1;
            const float* crs = halfColumns ? wide[1].data() : padded[1].data() + 1;
            if (redFirst) sdrRow<0, 2>(lumaLevel.data(), cbs, crs, row, p.width, kr, kb, kg);
            else sdrRow<2, 0>(lumaLevel.data(), cbs, crs, row, p.width, kr, kb, kg);
            continue;
        }
        const int ri = redFirst ? 0 : 2, bi = redFirst ? 2 : 0;
        for (int x = 0; x < p.width; ++x, row += 4) {
            float cb, cr;
            if (quarterColumns) {
                const size_t c = std::min(size_t(x / 4), cw - 1);
                const size_t beside = x % 4 >= 2 ? c + 1 : (c > 0 ? c - 1 : 0);
                const float t = x % 4 == 0 || x % 4 == 3 ? 0.375f : 0.125f;
                cb = u[c] + t * (u[beside] - u[c]);
                cr = v[c] + t * (v[beside] - v[c]);
            } else if (halfColumns) {
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
                row[bi] = uint8_t(bl * 255 + 0.5f);
                row[1] = uint8_t(g * 255 + 0.5f);
                row[ri] = uint8_t(r * 255 + 0.5f);
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
            const int place[3] = {bi, 1, ri};
            for (int k = 0; k < 3; ++k) row[place[k]] = uint8_t(levels.encode[size_t(std::clamp(c[k] * scale, 0.0f, 1.0f) * 4095.0f + 0.5f)]);
            row[3] = 255;
        }
    }
    for (int y = from; p.alpha && y < to; ++y) {
        const uint8_t* mask = p.alpha + size_t(y) * p.alphaStride;
        uint8_t* row = out + size_t(y) * stride;
        for (int x = 0; x < p.width; ++x) row[4 * x + 3] = mask[x];
    }
}

bool pictureToBGRA(const Picture& p, uint8_t* out, size_t stride, bool redFirst) {
    if (p.width <= 0 || p.height <= 0 || p.chromaWidth <= 0 || p.chromaHeight <= 0 || p.bits < 8 || p.bits > 16) return false;
    inParallel(p.height, [&](int from, int to) { rowsToBGRA(p, out, stride, redFirst, from, to); });
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
            const uint8_t* bottom = pixels + size_t(y1) * stride;
            for (int cx = 0; cx < cw; ++cx) {
                const size_t left = size_t(cx) * 8, right = size_t(std::min(2 * cx + 1, width - 1)) * 4;
                auto sum = [&](int c) { return top[left + c] + top[right + c] + bottom[left + c] + bottom[right + c]; };
                const int r = sum(ri), g = sum(1), b = sum(bi);
                uOut[size_t(cy) * size_t(cw) + size_t(cx)] = uint8_t((-6596 * r - 22188 * g + 28784 * b + (128 << 18) + (1 << 17)) >> 18);
                vOut[size_t(cy) * size_t(cw) + size_t(cx)] = uint8_t((28784 * r - 26145 * g - 2639 * b + (128 << 18) + (1 << 17)) >> 18);
            }
        }
    });
}

}

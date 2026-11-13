#include "mdct.h"
#include <cmath>
#include <mutex>

namespace absent {

InverseMdct::InverseMdct(int bits, double scale) : n(1 << bits) {
    const int half = n / 2;
    const double theta = (scale < 0 ? half : 0) + 1.0 / 8.0;
    const double root = std::sqrt(std::fabs(scale));
    turnRe.resize(size_t(half));
    turnIm.resize(size_t(half));
    for (int i = 0; i < half; ++i) {
        const double alpha = M_PI_2 * (i + theta) / half;
        turnRe[size_t(i)] = float(std::cos(alpha) * root);
        turnIm[size_t(i)] = float(std::sin(alpha) * root);
    }
    order.resize(size_t(half));
    for (int i = 0; i < half; ++i) {
        uint32_t r = 0;
        for (int b = 1; b < half; b <<= 1) r = (r << 1) | ((uint32_t(i) & uint32_t(b)) ? 1 : 0);
        order[size_t(i)] = r;
    }
    for (int length = 2; length <= half; length <<= 1) {
        for (int k = 0; k < length / 2; ++k) {
            const double angle = 2 * M_PI * k / length;
            twiddleRe.push_back(float(std::cos(angle)));
            twiddleIm.push_back(float(std::sin(angle)));
        }
    }
    re.resize(size_t(half));
    im.resize(size_t(half));
}

void InverseMdct::transform() {
    const int count = n / 2;
    float* xr = re.data();
    float* xi = im.data();
    for (int length = 2; length <= count; length <<= 1) {
        const int step = length / 2;
        const float* wr = twiddleRe.data() + step - 1;
        const float* wi = twiddleIm.data() + step - 1;
        for (int start = 0; start < count; start += length) {
            float* ar = xr + start;
            float* ai = xi + start;
            float* br = ar + step;
            float* bi = ai + step;
            for (int k = 0; k < step; ++k) {
                const float tr = br[k] * wr[k] - bi[k] * wi[k];
                const float ti = br[k] * wi[k] + bi[k] * wr[k];
                br[k] = ar[k] - tr;
                bi[k] = ai[k] - ti;
                ar[k] += tr;
                ai[k] += ti;
            }
        }
    }
}

void InverseMdct::half(const float* in, float* out) {
    const int half = n / 2, quarter = n / 4;
    for (int i = 0; i < half; ++i) {
        const float a = in[n - 1 - 2 * i], b = in[2 * i];
        const uint32_t to = order[size_t(i)];
        re[to] = a * turnRe[size_t(i)] - b * turnIm[size_t(i)];
        im[to] = a * turnIm[size_t(i)] + b * turnRe[size_t(i)];
    }
    transform();
    for (int i = 0; i < quarter; ++i) {
        const int i0 = quarter + i, i1 = quarter - i - 1;
        const float ar = re[size_t(i1)], ai = im[size_t(i1)], br = re[size_t(i0)], bi = im[size_t(i0)];
        const float c1 = turnRe[size_t(i1)], s1 = turnIm[size_t(i1)], c0 = turnRe[size_t(i0)], s0 = turnIm[size_t(i0)];
        out[2 * i1] = ai * s1 - ar * c1;
        out[2 * i0 + 1] = ai * c1 + ar * s1;
        out[2 * i0] = bi * s0 - br * c0;
        out[2 * i1 + 1] = bi * c0 + br * s0;
    }
}

void InverseMdct::full(const float* in, float* out) {
    half(in, out + n / 2);
    for (int i = 0; i < n / 2; ++i) {
        out[i] = -out[n - i - 1];
        out[2 * n - i - 1] = out[n + i];
    }
}

const float* sineWindow(int bits) {
    static std::vector<float> windows[14];
    static std::once_flag made;
    std::call_once(made, [] {
        for (int b = 5; b <= 13; ++b) {
            const int size = 1 << b;
            windows[b].resize(size_t(size));
            for (int i = 0; i < size; ++i) windows[b][size_t(i)] = sinf(float((i + 0.5) * (M_PI / (2.0 * size))));
        }
    });
    return bits >= 5 && bits <= 13 ? windows[bits].data() : nullptr;
}

}

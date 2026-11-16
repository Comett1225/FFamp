#include "mdct.h"
#include <cmath>
#include <mutex>

namespace absent {

template <class T>
InverseMdctOf<T>::InverseMdctOf(int bits, double scale) : n(1 << bits) {
    const int half = n / 2;
    const double theta = (scale < 0 ? half : 0) + 1.0 / 8.0;
    const double root = std::sqrt(std::fabs(scale));
    turnRe.resize(size_t(half));
    turnIm.resize(size_t(half));
    for (int i = 0; i < half; ++i) {
        const double alpha = M_PI_2 * (i + theta) / half;
        turnRe[size_t(i)] = T(std::cos(alpha) * root);
        turnIm[size_t(i)] = T(std::sin(alpha) * root);
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
            twiddleRe.push_back(T(std::cos(angle)));
            twiddleIm.push_back(T(std::sin(angle)));
        }
    }
    re.resize(size_t(half));
    im.resize(size_t(half));
}

template <class T>
void InverseMdctOf<T>::transform() {
    const int count = n / 2;
    T* xr = re.data();
    T* xi = im.data();
    for (int length = 2; length <= count; length <<= 1) {
        const int step = length / 2;
        const T* wr = twiddleRe.data() + step - 1;
        const T* wi = twiddleIm.data() + step - 1;
        for (int start = 0; start < count; start += length) {
            T* __restrict ar = xr + start;
            T* __restrict ai = xi + start;
            T* __restrict br = ar + step;
            T* __restrict bi = ai + step;
            for (int k = 0; k < step; ++k) {
                const T tr = br[k] * wr[k] - bi[k] * wi[k];
                const T ti = br[k] * wi[k] + bi[k] * wr[k];
                br[k] = ar[k] - tr;
                bi[k] = ai[k] - ti;
                ar[k] += tr;
                ai[k] += ti;
            }
        }
    }
}

template <class T>
void InverseMdctOf<T>::half(const T* in, T* out) {
    const int half = n / 2, quarter = n / 4;
    for (int i = 0; i < half; ++i) {
        const T a = in[n - 1 - 2 * i], b = in[2 * i];
        const uint32_t to = order[size_t(i)];
        re[to] = a * turnRe[size_t(i)] - b * turnIm[size_t(i)];
        im[to] = a * turnIm[size_t(i)] + b * turnRe[size_t(i)];
    }
    transform();
    for (int i = 0; i < quarter; ++i) {
        const int i0 = quarter + i, i1 = quarter - i - 1;
        const T ar = re[size_t(i1)], ai = im[size_t(i1)], br = re[size_t(i0)], bi = im[size_t(i0)];
        const T c1 = turnRe[size_t(i1)], s1 = turnIm[size_t(i1)], c0 = turnRe[size_t(i0)], s0 = turnIm[size_t(i0)];
        out[2 * i1] = ai * s1 - ar * c1;
        out[2 * i0 + 1] = ai * c1 + ar * s1;
        out[2 * i0] = bi * s0 - br * c0;
        out[2 * i1 + 1] = bi * c0 + br * s0;
    }
}

template <class T>
void InverseMdctOf<T>::full(const T* in, T* out) {
    half(in, out + n / 2);
    for (int i = 0; i < n / 2; ++i) {
        out[i] = -out[n - i - 1];
        out[2 * n - i - 1] = out[n + i];
    }
}

template class InverseMdctOf<float>;
template class InverseMdctOf<double>;

namespace {

double evalPoly(const double* coeff, int size, double x) {
    double sum = coeff[size - 1];
    for (int i = size - 2; i >= 0; --i) {
        sum *= x;
        sum += coeff[i];
    }
    return sum;
}

double besselI0(double x) {
    static const double p1[] = {-2.2335582639474375249e+15, -5.5050369673018427753e+14, -3.2940087627407749166e+13, -8.4925101247114157499e+11,
                                -1.1912746104985237192e+10, -1.0313066708737980747e+08, -5.9545626019847898221e+05, -2.4125195876041896775e+03,
                                -7.0935347449210549190e+00, -1.5453977791786851041e-02, -2.5172644670688975051e-05, -3.0517226450451067446e-08,
                                -2.6843448573468483278e-11, -1.5982226675653184646e-14, -5.2487866627945699800e-18};
    static const double q1[] = {-2.2335582639474375245e+15, 7.8858692566751002988e+12, -1.2207067397808979846e+10, 1.0377081058062166144e+07,
                                -4.8527560179962773045e+03, 1.0};
    static const double p2[] = {-2.2210262233306573296e-04, 1.3067392038106924055e-02, -4.4700805721174453923e-01, 5.5674518371240761397e+00,
                                -2.3517945679239481621e+01, 3.1611322818701131207e+01, -9.6090021968656180000e+00};
    static const double q2[] = {-5.5194330231005480228e-04, 3.2547697594819615062e-02, -1.1151759188741312645e+00, 1.3982595353892851542e+01,
                                -6.0228002066743340583e+01, 8.5539563258012929600e+01, -3.1446690275135491500e+01, 1.0};
    if (x == 0) return 1.0;
    x = std::fabs(x);
    if (x <= 15) {
        double y = x * x;
        return evalPoly(p1, 15, y) / evalPoly(q1, 6, y);
    }
    double y = 1 / x - 1.0 / 15;
    double r = evalPoly(p2, 7, y) / evalPoly(q2, 8, y);
    double factor = std::exp(x) / std::sqrt(x);
    return factor * r;
}

}

void kbdWindow(float* window, float alpha, int n) {
    double sum = 0.0, scale = 0.0;
    double temp[4096 / 2 + 1];
    double alpha2 = 4 * (alpha * M_PI / n) * (alpha * M_PI / n);
    int i;
    for (i = 0; i <= n / 2; i++) {
        double tmp = i * (n - i) * alpha2;
        temp[i] = besselI0(std::sqrt(tmp));
        scale += temp[i] * (1 + (i && i < n / 2));
    }
    scale = 1.0 / (scale + 1);
    for (i = 0; i <= n / 2; i++) {
        sum += temp[i];
        window[i] = float(std::sqrt(sum * scale));
    }
    for (; i < n; i++) {
        sum += temp[n - i];
        window[i] = float(std::sqrt(sum * scale));
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

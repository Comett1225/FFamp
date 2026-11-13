#pragma once
#include <cstddef>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace absent {

#if defined(__ARM_NEON)

inline float foldLanes(float32x4_t a, float32x4_t b, float32x4_t c, float32x4_t d) { return vaddvq_f32(vaddq_f32(vaddq_f32(a, b), vaddq_f32(c, d))); }

inline float dotOne(const float* x, const float* h, size_t taps) {
    float32x4_t s[8];
    for (auto& lane : s) lane = vdupq_n_f32(0);
    size_t k = 0;
    for (; k + 32 <= taps; k += 32) {
        for (size_t j = 0; j < 8; ++j) s[j] = vfmaq_f32(s[j], vld1q_f32(x + k + 4 * j), vld1q_f32(h + k + 4 * j));
    }
    for (size_t j = 0; k + 4 <= taps; k += 4, ++j) s[j] = vfmaq_f32(s[j], vld1q_f32(x + k), vld1q_f32(h + k));
    return foldLanes(vaddq_f32(s[0], s[4]), vaddq_f32(s[1], s[5]), vaddq_f32(s[2], s[6]), vaddq_f32(s[3], s[7]));
}

inline void dotPair(const float* x, const float* y, const float* h, size_t taps, float* out) {
    float32x4_t s[4], t[4];
    for (auto& lane : s) lane = vdupq_n_f32(0);
    for (auto& lane : t) lane = vdupq_n_f32(0);
    size_t k = 0;
    for (; k + 16 <= taps; k += 16) {
        for (size_t j = 0; j < 4; ++j) {
            float32x4_t weight = vld1q_f32(h + k + 4 * j);
            s[j] = vfmaq_f32(s[j], vld1q_f32(x + k + 4 * j), weight);
            t[j] = vfmaq_f32(t[j], vld1q_f32(y + k + 4 * j), weight);
        }
    }
    for (size_t j = 0; k + 4 <= taps; k += 4, ++j) {
        float32x4_t weight = vld1q_f32(h + k);
        s[j] = vfmaq_f32(s[j], vld1q_f32(x + k), weight);
        t[j] = vfmaq_f32(t[j], vld1q_f32(y + k), weight);
    }
    out[0] = foldLanes(s[0], s[1], s[2], s[3]);
    out[1] = foldLanes(t[0], t[1], t[2], t[3]);
}

inline double dotWide(const double* x, const double* y, size_t count) {
    float64x2_t s[8];
    for (auto& lane : s) lane = vdupq_n_f64(0);
    size_t k = 0;
    for (; k + 16 <= count; k += 16) {
        for (size_t j = 0; j < 8; ++j) s[j] = vfmaq_f64(s[j], vld1q_f64(x + k + 2 * j), vld1q_f64(y + k + 2 * j));
    }
    double sum = vaddvq_f64(vaddq_f64(vaddq_f64(vaddq_f64(s[0], s[1]), vaddq_f64(s[2], s[3])), vaddq_f64(vaddq_f64(s[4], s[5]), vaddq_f64(s[6], s[7]))));
    for (; k < count; ++k) sum += x[k] * y[k];
    return sum;
}

#else

inline float dotOne(const float* x, const float* h, size_t taps) {
    float sum[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (size_t k = 0; k < taps; k += 8) {
        for (size_t j = 0; j < 8; ++j) sum[j] += x[k + j] * h[k + j];
    }
    return ((sum[0] + sum[4]) + (sum[1] + sum[5])) + ((sum[2] + sum[6]) + (sum[3] + sum[7]));
}

inline void dotPair(const float* x, const float* y, const float* h, size_t taps, float* out) {
    out[0] = dotOne(x, h, taps);
    out[1] = dotOne(y, h, taps);
}

inline double dotWide(const double* x, const double* y, size_t count) {
    double sum[4] = {0, 0, 0, 0};
    size_t k = 0;
    for (; k + 4 <= count; k += 4) {
        for (size_t j = 0; j < 4; ++j) sum[j] += x[k + j] * y[k + j];
    }
    double total = (sum[0] + sum[2]) + (sum[1] + sum[3]);
    for (; k < count; ++k) total += x[k] * y[k];
    return total;
}

#endif

}

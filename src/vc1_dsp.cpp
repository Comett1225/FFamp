#include "vc1.h"
#include <cstring>
#include <mutex>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace absent::vc1 {

namespace {

void vOverlap(uint8_t* src, ptrdiff_t stride) {
    int rnd = 1;
    for (int i = 0; i < 8; i++) {
        const int a = src[-2 * stride], b = src[-stride], c = src[0], d = src[stride];
        const int d1 = (a - d + 3 + rnd) >> 3;
        const int d2 = (a - d + b - c + 4 - rnd) >> 3;
        src[-2 * stride] = uint8_t(a - d1);
        src[-stride] = clipU8(b - d2);
        src[0] = clipU8(c + d2);
        src[stride] = uint8_t(d + d1);
        src++;
        rnd = !rnd;
    }
}

void hOverlap(uint8_t* src, ptrdiff_t stride) {
    int rnd = 1;
    for (int i = 0; i < 8; i++) {
        const int a = src[-2], b = src[-1], c = src[0], d = src[1];
        const int d1 = (a - d + 3 + rnd) >> 3;
        const int d2 = (a - d + b - c + 4 - rnd) >> 3;
        src[-2] = uint8_t(a - d1);
        src[-1] = clipU8(b - d2);
        src[0] = clipU8(c + d2);
        src[1] = uint8_t(d + d1);
        src += stride;
        rnd = !rnd;
    }
}

void vSOverlap(int16_t* top, int16_t* bottom) {
    int rnd1 = 4, rnd2 = 3;
    for (int i = 0; i < 8; i++) {
        const int a = top[48], b = top[56], c = bottom[0], d = bottom[8];
        const int d1 = a - d;
        const int d2 = a - d + b - c;
        top[48] = int16_t(((a * 8) - d1 + rnd1) >> 3);
        top[56] = int16_t(((b * 8) - d2 + rnd2) >> 3);
        bottom[0] = int16_t(((c * 8) + d2 + rnd1) >> 3);
        bottom[8] = int16_t(((d * 8) + d1 + rnd2) >> 3);
        bottom++;
        top++;
        rnd2 = 7 - rnd2;
        rnd1 = 7 - rnd1;
    }
}

void hSOverlap(int16_t* left, int16_t* right, ptrdiff_t leftStride, ptrdiff_t rightStride, int flags) {
    int rnd1 = flags & 2 ? 3 : 4;
    int rnd2 = 7 - rnd1;
    for (int i = 0; i < 8; i++) {
        const int a = left[6], b = left[7], c = right[0], d = right[1];
        const int d1 = a - d;
        const int d2 = a - d + b - c;
        left[6] = int16_t(((a * 8) - d1 + rnd1) >> 3);
        left[7] = int16_t(((b * 8) - d2 + rnd2) >> 3);
        right[0] = int16_t(((c * 8) + d2 + rnd1) >> 3);
        right[1] = int16_t(((d * 8) + d1 + rnd2) >> 3);
        right += rightStride;
        left += leftStride;
        if (flags & 1) {
            rnd2 = 7 - rnd2;
            rnd1 = 7 - rnd1;
        }
    }
}

inline int filterLine(uint8_t* src, ptrdiff_t stride, int pq) {
    int a0 = (2 * (src[-2 * stride] - src[1 * stride]) - 5 * (src[-1 * stride] - src[0 * stride]) + 4) >> 3;
    const int a0Sign = a0 >> 31;
    a0 = (a0 ^ a0Sign) - a0Sign;
    if (a0 < pq) {
        const int a1 = std::abs((2 * (src[-4 * stride] - src[-1 * stride]) - 5 * (src[-3 * stride] - src[-2 * stride]) + 4) >> 3);
        const int a2 = std::abs((2 * (src[0 * stride] - src[3 * stride]) - 5 * (src[1 * stride] - src[2 * stride]) + 4) >> 3);
        if (a1 < a0 || a2 < a0) {
            int limit = src[-1 * stride] - src[0 * stride];
            const int limitSign = limit >> 31;
            limit = ((limit ^ limitSign) - limitSign) >> 1;
            if (limit) {
                const int a3 = std::min(a1, a2);
                int d = (5 * (a0 - a3)) >> 3;
                if (a0Sign ^ limitSign) {
                    d = std::min(d, limit);
                    d = (d ^ limitSign) - limitSign;
                    src[-1 * stride] = clipU8(src[-1 * stride] - d);
                    src[0 * stride] = clipU8(src[0 * stride] + d);
                }
                return 1;
            }
        }
    }
    return 0;
}

inline void loopFilter(uint8_t* src, ptrdiff_t step, ptrdiff_t stride, int length, int pq) {
    for (int i = 0; i < length; i += 4) {
        if (filterLine(src + 2 * step, stride, pq)) {
            filterLine(src + 0 * step, stride, pq);
            filterLine(src + 1 * step, stride, pq);
            filterLine(src + 3 * step, stride, pq);
        }
        src += step * 4;
    }
}

void vLoopFilter4(uint8_t* src, ptrdiff_t stride, int pq) { loopFilter(src, 1, stride, 4, pq); }
void hLoopFilter4(uint8_t* src, ptrdiff_t stride, int pq) { loopFilter(src, stride, 1, 4, pq); }
void vLoopFilter8(uint8_t* src, ptrdiff_t stride, int pq) { loopFilter(src, 1, stride, 8, pq); }
void hLoopFilter8(uint8_t* src, ptrdiff_t stride, int pq) { loopFilter(src, stride, 1, 8, pq); }
void vLoopFilter16(uint8_t* src, ptrdiff_t stride, int pq) { loopFilter(src, 1, stride, 16, pq); }
void hLoopFilter16(uint8_t* src, ptrdiff_t stride, int pq) { loopFilter(src, stride, 1, 16, pq); }

void addDc(uint8_t* dest, ptrdiff_t stride, int dc, int columns, int rows) {
    for (int i = 0; i < rows; i++) {
        for (int j = 0; j < columns; j++) dest[j] = clipU8(dest[j] + dc);
        dest += stride;
    }
}

void invTrans8x8Dc(uint8_t* dest, ptrdiff_t stride, int16_t* block) {
    int dc = block[0];
    dc = (3 * dc + 1) >> 1;
    dc = (3 * dc + 16) >> 5;
    addDc(dest, stride, dc, 8, 8);
}

void invTrans8x4Dc(uint8_t* dest, ptrdiff_t stride, int16_t* block) {
    int dc = block[0];
    dc = (3 * dc + 1) >> 1;
    dc = (17 * dc + 64) >> 7;
    addDc(dest, stride, dc, 8, 4);
}

void invTrans4x8Dc(uint8_t* dest, ptrdiff_t stride, int16_t* block) {
    int dc = block[0];
    dc = (17 * dc + 4) >> 3;
    dc = (12 * dc + 64) >> 7;
    addDc(dest, stride, dc, 4, 8);
}

void invTrans4x4Dc(uint8_t* dest, ptrdiff_t stride, int16_t* block) {
    int dc = block[0];
    dc = (17 * dc + 4) >> 3;
    dc = (17 * dc + 64) >> 7;
    addDc(dest, stride, dc, 4, 4);
}

inline void rows8(const int16_t* src, int16_t* dst, int rounding) {
    int t1 = 12 * (src[0] + src[4]) + rounding;
    int t2 = 12 * (src[0] - src[4]) + rounding;
    int t3 = 16 * src[2] + 6 * src[6];
    int t4 = 6 * src[2] - 16 * src[6];
    const int t5 = t1 + t3, t6 = t2 + t4, t7 = t2 - t4, t8 = t1 - t3;
    t1 = 16 * src[1] + 15 * src[3] + 9 * src[5] + 4 * src[7];
    t2 = 15 * src[1] - 4 * src[3] - 16 * src[5] - 9 * src[7];
    t3 = 9 * src[1] - 16 * src[3] + 4 * src[5] + 15 * src[7];
    t4 = 4 * src[1] - 9 * src[3] + 15 * src[5] - 16 * src[7];
    dst[0] = int16_t((t5 + t1) >> 3);
    dst[1] = int16_t((t6 + t2) >> 3);
    dst[2] = int16_t((t7 + t3) >> 3);
    dst[3] = int16_t((t8 + t4) >> 3);
    dst[4] = int16_t((t8 - t4) >> 3);
    dst[5] = int16_t((t7 - t3) >> 3);
    dst[6] = int16_t((t6 - t2) >> 3);
    dst[7] = int16_t((t5 - t1) >> 3);
}

inline void rows4(const int16_t* src, int16_t* dst) {
    const int t1 = 17 * (src[0] + src[2]) + 4;
    const int t2 = 17 * (src[0] - src[2]) + 4;
    const int t3 = 22 * src[1] + 10 * src[3];
    const int t4 = 22 * src[3] - 10 * src[1];
    dst[0] = int16_t((t1 + t3) >> 3);
    dst[1] = int16_t((t2 - t4) >> 3);
    dst[2] = int16_t((t2 + t4) >> 3);
    dst[3] = int16_t((t1 - t3) >> 3);
}

inline void columns8(const int16_t* src, int out[8]) {
    int t1 = 12 * (src[0] + src[32]) + 64;
    int t2 = 12 * (src[0] - src[32]) + 64;
    int t3 = 16 * src[16] + 6 * src[48];
    int t4 = 6 * src[16] - 16 * src[48];
    const int t5 = t1 + t3, t6 = t2 + t4, t7 = t2 - t4, t8 = t1 - t3;
    t1 = 16 * src[8] + 15 * src[24] + 9 * src[40] + 4 * src[56];
    t2 = 15 * src[8] - 4 * src[24] - 16 * src[40] - 9 * src[56];
    t3 = 9 * src[8] - 16 * src[24] + 4 * src[40] + 15 * src[56];
    t4 = 4 * src[8] - 9 * src[24] + 15 * src[40] - 16 * src[56];
    out[0] = (t5 + t1) >> 7;
    out[1] = (t6 + t2) >> 7;
    out[2] = (t7 + t3) >> 7;
    out[3] = (t8 + t4) >> 7;
    out[4] = (t8 - t4 + 1) >> 7;
    out[5] = (t7 - t3 + 1) >> 7;
    out[6] = (t6 - t2 + 1) >> 7;
    out[7] = (t5 - t1 + 1) >> 7;
}

inline void columns4(const int16_t* src, int out[4]) {
    const int t1 = 17 * (src[0] + src[16]) + 64;
    const int t2 = 17 * (src[0] - src[16]) + 64;
    const int t3 = 22 * src[8] + 10 * src[24];
    const int t4 = 22 * src[24] - 10 * src[8];
    out[0] = (t1 + t3) >> 7;
    out[1] = (t2 - t4) >> 7;
    out[2] = (t2 + t4) >> 7;
    out[3] = (t1 - t3) >> 7;
}

void invTrans8x8(int16_t block[64]) {
    int16_t temp[64];
    for (int i = 0; i < 8; i++) {
        int16_t column[8];
        for (int k = 0; k < 8; k++) column[k] = block[i + 8 * k];
        rows8(column, temp + 8 * i, 4);
    }
    for (int i = 0; i < 8; i++) {
        int out[8];
        columns8(temp + i, out);
        for (int k = 0; k < 8; k++) block[i + 8 * k] = int16_t(out[k]);
    }
}

void invTrans8x4(uint8_t* dest, ptrdiff_t stride, int16_t* block) {
    for (int i = 0; i < 4; i++) rows8(block + 8 * i, block + 8 * i, 4);
    for (int i = 0; i < 8; i++) {
        int out[4];
        columns4(block + i, out);
        for (int k = 0; k < 4; k++) dest[k * stride + i] = clipU8(dest[k * stride + i] + out[k]);
    }
}

void invTrans4x8(uint8_t* dest, ptrdiff_t stride, int16_t* block) {
    for (int i = 0; i < 8; i++) rows4(block + 8 * i, block + 8 * i);
    for (int i = 0; i < 4; i++) {
        int out[8];
        columns8(block + i, out);
        for (int k = 0; k < 8; k++) dest[k * stride + i] = clipU8(dest[k * stride + i] + out[k]);
    }
}

void invTrans4x4(uint8_t* dest, ptrdiff_t stride, int16_t* block) {
    for (int i = 0; i < 4; i++) rows4(block + 8 * i, block + 8 * i);
    for (int i = 0; i < 4; i++) {
        int out[4];
        columns4(block + i, out);
        for (int k = 0; k < 4; k++) dest[k * stride + i] = clipU8(dest[k * stride + i] + out[k]);
    }
}

template <int Mode, typename T>
inline int taps(const T* src, ptrdiff_t stride) {
    if constexpr (Mode == 1) return -4 * src[-stride] + 53 * src[0] + 18 * src[stride] - 3 * src[stride * 2];
    else if constexpr (Mode == 2) return -1 * src[-stride] + 9 * src[0] + 9 * src[stride] - 1 * src[stride * 2];
    else if constexpr (Mode == 3) return -3 * src[-stride] + 18 * src[0] + 53 * src[stride] - 4 * src[stride * 2];
    else return src[0];
}

template <int Mode>
inline int single(const uint8_t* src, ptrdiff_t stride, int r) {
    if constexpr (Mode == 2) return (taps<2>(src, stride) + 8 - r) >> 4;
    else return (taps<Mode>(src, stride) + 32 - r) >> 6;
}

template <bool Average>
inline void store(uint8_t& dst, int value) {
    if constexpr (Average) dst = uint8_t((dst + clipU8(value) + 1) >> 1);
    else dst = clipU8(value);
}

template <int Size, bool Average, int H, int V>
void mspel(uint8_t* dst, const uint8_t* src, ptrdiff_t stride, int rnd) {
    if constexpr (H == 0 && V == 0) {
        for (int j = 0; j < Size; j++) {
            if constexpr (Average) {
                for (int i = 0; i < Size; i++) dst[i] = uint8_t((dst[i] + src[i] + 1) >> 1);
            } else {
                memcpy(dst, src, Size);
            }
            dst += stride;
            src += stride;
        }
    } else if constexpr (H && V) {
        constexpr int shifts[] = {0, 5, 1, 5};
        constexpr int shift = (shifts[H] + shifts[V]) >> 1;
        constexpr int span = Size + 3;
        int16_t tmp[span * Size];
        const int r = (1 << (shift - 1)) + rnd - 1;
        src -= 1;
        for (int j = 0; j < Size; j++) {
            for (int i = 0; i < span; i++) tmp[j * span + i] = int16_t((taps<V>(src + i, stride) + r) >> shift);
            src += stride;
        }
        const int r2 = 64 - rnd;
        for (int j = 0; j < Size; j++) {
            const int16_t* row = tmp + j * span + 1;
            for (int i = 0; i < Size; i++) store<Average>(dst[i], (taps<H>(row + i, 1) + r2) >> 7);
            dst += stride;
        }
    } else if constexpr (V) {
        const int r = 1 - rnd;
        for (int j = 0; j < Size; j++) {
            for (int i = 0; i < Size; i++) store<Average>(dst[i], single<V>(src + i, stride, r));
            src += stride;
            dst += stride;
        }
    } else {
        for (int j = 0; j < Size; j++) {
            for (int i = 0; i < Size; i++) store<Average>(dst[i], single<H>(src + i, 1, rnd));
            dst += stride;
            src += stride;
        }
    }
}

template <int Size, bool Average>
void fillMspel(MspelMc table[16]) {
    table[0] = mspel<Size, Average, 0, 0>;
    table[1] = mspel<Size, Average, 1, 0>;
    table[2] = mspel<Size, Average, 2, 0>;
    table[3] = mspel<Size, Average, 3, 0>;
    table[4] = mspel<Size, Average, 0, 1>;
    table[5] = mspel<Size, Average, 1, 1>;
    table[6] = mspel<Size, Average, 2, 1>;
    table[7] = mspel<Size, Average, 3, 1>;
    table[8] = mspel<Size, Average, 0, 2>;
    table[9] = mspel<Size, Average, 1, 2>;
    table[10] = mspel<Size, Average, 2, 2>;
    table[11] = mspel<Size, Average, 3, 2>;
    table[12] = mspel<Size, Average, 0, 3>;
    table[13] = mspel<Size, Average, 1, 3>;
    table[14] = mspel<Size, Average, 2, 3>;
    table[15] = mspel<Size, Average, 3, 3>;
}

template <int Width, bool Average, int Bias>
void chromaMc(uint8_t* dst, const uint8_t* src, ptrdiff_t stride, int h, int x, int y) {
    const int a = (8 - x) * (8 - y), b = x * (8 - y), c = (8 - x) * y, d = x * y;
#if defined(__ARM_NEON)
    const uint8x8_t wa = vdup_n_u8(uint8_t(a)), wb = vdup_n_u8(uint8_t(b)), wc = vdup_n_u8(uint8_t(c)), wd = vdup_n_u8(uint8_t(d));
    const uint16x8_t bias = vdupq_n_u16(Bias);
    for (int i = 0; i < h; i++) {
        uint16x8_t sum = vmull_u8(vld1_u8(src), wa);
        sum = vmlal_u8(sum, vld1_u8(src + 1), wb);
        sum = vmlal_u8(sum, vld1_u8(src + stride), wc);
        sum = vmlal_u8(sum, vld1_u8(src + stride + 1), wd);
        uint8x8_t value = vshrn_n_u16(vaddq_u16(sum, bias), 6);
        if constexpr (Width == 8) {
            if constexpr (Average) value = vrhadd_u8(value, vld1_u8(dst));
            vst1_u8(dst, value);
        } else {
            if constexpr (Average) {
                uint32_t old;
                memcpy(&old, dst, 4);
                value = vrhadd_u8(value, vreinterpret_u8_u32(vdup_n_u32(old)));
            }
            const uint32_t out = vget_lane_u32(vreinterpret_u32_u8(value), 0);
            memcpy(dst, &out, 4);
        }
        dst += stride;
        src += stride;
    }
#else
    for (int i = 0; i < h; i++) {
        for (int k = 0; k < Width; k++) {
            const int value = (a * src[k] + b * src[k + 1] + c * src[stride + k] + d * src[stride + k + 1] + Bias) >> 6;
            dst[k] = Average ? uint8_t((dst[k] + value + 1) >> 1) : uint8_t(value);
        }
        dst += stride;
        src += stride;
    }
#endif
}

template <int Size, bool Rounding, bool Average, int Dxy>
void hpel(uint8_t* dst, const uint8_t* src, ptrdiff_t stride, int h) {
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < Size; i++) {
            int value;
            if (Dxy == 0) value = src[i];
            else if (Dxy == 1) value = (src[i] + src[i + 1] + Rounding) >> 1;
            else if (Dxy == 2) value = (src[i] + src[i + stride] + Rounding) >> 1;
            else value = (src[i] + src[i + 1] + src[i + stride] + src[i + stride + 1] + 1 + Rounding) >> 2;
            dst[i] = Average ? uint8_t((dst[i] + value + 1) >> 1) : uint8_t(value);
        }
        dst += stride;
        src += stride;
    }
}

template <int Size, bool Rounding, bool Average>
void fillHpel(PixelsOp table[4]) {
    table[0] = hpel<Size, Rounding, Average, 0>;
    table[1] = hpel<Size, Rounding, Average, 1>;
    table[2] = hpel<Size, Rounding, Average, 2>;
    table[3] = hpel<Size, Rounding, Average, 3>;
}


constexpr int kW1 = 22725, kW2 = 21407, kW3 = 19266, kW4 = 16383, kW5 = 12873, kW6 = 8867, kW7 = 4520;

void idctRow(int16_t* row) {
    if (!(row[1] | row[2] | row[3] | row[4] | row[5] | row[6] | row[7])) {
        const int16_t value = int16_t(uint16_t((row[0] * 8) & 0xffff));
        for (int i = 0; i < 8; i++) row[i] = value;
        return;
    }
    unsigned a0 = unsigned(kW4) * unsigned(row[0]) + (1u << 10);
    unsigned a1 = a0, a2 = a0, a3 = a0;
    a0 += unsigned(kW2) * unsigned(row[2]);
    a1 += unsigned(kW6) * unsigned(row[2]);
    a2 -= unsigned(kW6) * unsigned(row[2]);
    a3 -= unsigned(kW2) * unsigned(row[2]);
    unsigned b0 = unsigned(kW1 * row[1]) + unsigned(kW3 * row[3]);
    unsigned b1 = unsigned(kW3 * row[1]) + unsigned(-kW7 * row[3]);
    unsigned b2 = unsigned(kW5 * row[1]) + unsigned(-kW1 * row[3]);
    unsigned b3 = unsigned(kW7 * row[1]) + unsigned(-kW5 * row[3]);
    if (row[4] | row[5] | row[6] | row[7]) {
        a0 += unsigned(kW4) * unsigned(row[4]) + unsigned(kW6) * unsigned(row[6]);
        a1 += unsigned(-kW4) * unsigned(row[4]) - unsigned(kW2) * unsigned(row[6]);
        a2 += unsigned(-kW4) * unsigned(row[4]) + unsigned(kW2) * unsigned(row[6]);
        a3 += unsigned(kW4) * unsigned(row[4]) - unsigned(kW6) * unsigned(row[6]);
        b0 += unsigned(kW5 * row[5]) + unsigned(kW7 * row[7]);
        b1 += unsigned(-kW1 * row[5]) + unsigned(-kW5 * row[7]);
        b2 += unsigned(kW7 * row[5]) + unsigned(kW3 * row[7]);
        b3 += unsigned(kW3 * row[5]) + unsigned(-kW1 * row[7]);
    }
    row[0] = int16_t(int(a0 + b0) >> 11);
    row[7] = int16_t(int(a0 - b0) >> 11);
    row[1] = int16_t(int(a1 + b1) >> 11);
    row[6] = int16_t(int(a1 - b1) >> 11);
    row[2] = int16_t(int(a2 + b2) >> 11);
    row[5] = int16_t(int(a2 - b2) >> 11);
    row[3] = int16_t(int(a3 + b3) >> 11);
    row[4] = int16_t(int(a3 - b3) >> 11);
}

void idctColumn(const int16_t* col, int out[8]) {
    unsigned a0 = unsigned(kW4) * unsigned(col[0] + ((1 << 19) / kW4));
    unsigned a1 = a0, a2 = a0, a3 = a0;
    a0 += unsigned(kW2) * unsigned(col[16]);
    a1 += unsigned(kW6) * unsigned(col[16]);
    a2 += unsigned(-kW6) * unsigned(col[16]);
    a3 += unsigned(-kW2) * unsigned(col[16]);
    unsigned b0 = unsigned(kW1 * col[8]), b1 = unsigned(kW3 * col[8]), b2 = unsigned(kW5 * col[8]), b3 = unsigned(kW7 * col[8]);
    b0 += unsigned(kW3 * col[24]);
    b1 += unsigned(-kW7 * col[24]);
    b2 += unsigned(-kW1 * col[24]);
    b3 += unsigned(-kW5 * col[24]);
    if (col[32]) {
        a0 += unsigned(kW4) * unsigned(col[32]);
        a1 += unsigned(-kW4) * unsigned(col[32]);
        a2 += unsigned(-kW4) * unsigned(col[32]);
        a3 += unsigned(kW4) * unsigned(col[32]);
    }
    if (col[40]) {
        b0 += unsigned(kW5 * col[40]);
        b1 += unsigned(-kW1 * col[40]);
        b2 += unsigned(kW7 * col[40]);
        b3 += unsigned(kW3 * col[40]);
    }
    if (col[48]) {
        a0 += unsigned(kW6) * unsigned(col[48]);
        a1 += unsigned(-kW2) * unsigned(col[48]);
        a2 += unsigned(kW2) * unsigned(col[48]);
        a3 += unsigned(-kW6) * unsigned(col[48]);
    }
    if (col[56]) {
        b0 += unsigned(kW7 * col[56]);
        b1 += unsigned(-kW5 * col[56]);
        b2 += unsigned(kW3 * col[56]);
        b3 += unsigned(-kW1 * col[56]);
    }
    out[0] = int(a0 + b0) >> 20;
    out[1] = int(a1 + b1) >> 20;
    out[2] = int(a2 + b2) >> 20;
    out[3] = int(a3 + b3) >> 20;
    out[4] = int(a3 - b3) >> 20;
    out[5] = int(a2 - b2) >> 20;
    out[6] = int(a1 - b1) >> 20;
    out[7] = int(a0 - b0) >> 20;
}

void simpleIdct8x8(int16_t* block) {
    for (int i = 0; i < 8; i++) idctRow(block + i * 8);
    for (int i = 0; i < 8; i++) {
        int out[8];
        idctColumn(block + i, out);
        for (int k = 0; k < 8; k++) block[i + 8 * k] = int16_t(out[k]);
    }
}

void simpleIdct8x8Add(uint8_t* dest, ptrdiff_t stride, int16_t* block) {
    for (int i = 0; i < 8; i++) idctRow(block + i * 8);
    for (int i = 0; i < 8; i++) {
        int out[8];
        idctColumn(block + i, out);
        for (int k = 0; k < 8; k++) dest[i + k * stride] = clipU8(dest[i + k * stride] + out[k]);
    }
}

constexpr int kC1 = int(0.6532814824 * 1.4142135623730951 * 4096 + 0.5), kC2 = int(0.2705980501 * 1.4142135623730951 * 4096 + 0.5), kC3 = int(0.5 * 1.4142135623730951 * 4096 + 0.5);
constexpr int kR1 = int(0.6532814824 * 1.4142135623730951 * 32768 + 0.5), kR2 = int(0.2705980501 * 1.4142135623730951 * 32768 + 0.5), kR3 = int(0.5 * 1.4142135623730951 * 32768 + 0.5);

void idct4ColumnAdd(uint8_t* dest, ptrdiff_t stride, const int16_t* col) {
    const int a0 = col[0], a1 = col[8], a2 = col[16], a3 = col[24];
    const int c0 = (a0 + a2) * kC3 + (1 << 16);
    const int c2 = (a0 - a2) * kC3 + (1 << 16);
    const int c1 = a1 * kC1 + a3 * kC2;
    const int c3 = a1 * kC2 - a3 * kC1;
    dest[0] = clipU8(dest[0] + ((c0 + c1) >> 17));
    dest += stride;
    dest[0] = clipU8(dest[0] + ((c2 + c3) >> 17));
    dest += stride;
    dest[0] = clipU8(dest[0] + ((c2 - c3) >> 17));
    dest += stride;
    dest[0] = clipU8(dest[0] + ((c0 - c1) >> 17));
}

void idct4Row(int16_t* row) {
    const int a0 = row[0], a1 = row[1], a2 = row[2], a3 = row[3];
    const unsigned c0 = unsigned((a0 + a2) * kR3 + (1 << 10));
    const unsigned c2 = unsigned((a0 - a2) * kR3 + (1 << 10));
    const unsigned c1 = unsigned(a1 * kR1 + a3 * kR2);
    const unsigned c3 = unsigned(a1 * kR2 - a3 * kR1);
    row[0] = int16_t((c0 + c1) >> 11);
    row[1] = int16_t((c2 + c3) >> 11);
    row[2] = int16_t((c2 - c3) >> 11);
    row[3] = int16_t((c0 - c1) >> 11);
}

void simpleIdct84Add(uint8_t* dest, ptrdiff_t stride, int16_t* block) {
    for (int i = 0; i < 4; i++) idctRow(block + i * 8);
    for (int i = 0; i < 8; i++) idct4ColumnAdd(dest + i, stride, block + i);
}

void simpleIdct48Add(uint8_t* dest, ptrdiff_t stride, int16_t* block) {
    for (int i = 0; i < 8; i++) idct4Row(block + i * 8);
    for (int i = 0; i < 4; i++) {
        int out[8];
        idctColumn(block + i, out);
        for (int k = 0; k < 8; k++) dest[i + k * stride] = clipU8(dest[i + k * stride] + out[k]);
    }
}

void simpleIdct44Add(uint8_t* dest, ptrdiff_t stride, int16_t* block) {
    for (int i = 0; i < 4; i++) idct4Row(block + i * 8);
    for (int i = 0; i < 4; i++) idct4ColumnAdd(dest + i, stride, block + i);
}

Dsp made() {
    Dsp d;
    d.invTrans8x8 = invTrans8x8;
    d.invTrans8x4 = invTrans8x4;
    d.invTrans4x8 = invTrans4x8;
    d.invTrans4x4 = invTrans4x4;
    d.invTrans8x8Dc = invTrans8x8Dc;
    d.invTrans8x4Dc = invTrans8x4Dc;
    d.invTrans4x8Dc = invTrans4x8Dc;
    d.invTrans4x4Dc = invTrans4x4Dc;
    d.vOverlap = vOverlap;
    d.hOverlap = hOverlap;
    d.vSOverlap = vSOverlap;
    d.hSOverlap = hSOverlap;
    d.vLoopFilter4 = vLoopFilter4;
    d.hLoopFilter4 = hLoopFilter4;
    d.vLoopFilter8 = vLoopFilter8;
    d.hLoopFilter8 = hLoopFilter8;
    d.vLoopFilter16 = vLoopFilter16;
    d.hLoopFilter16 = hLoopFilter16;
    fillMspel<16, false>(d.putVc1MspelPixelsTab[0]);
    fillMspel<8, false>(d.putVc1MspelPixelsTab[1]);
    fillMspel<16, true>(d.avgVc1MspelPixelsTab[0]);
    fillMspel<8, true>(d.avgVc1MspelPixelsTab[1]);
    d.putNoRndVc1ChromaPixelsTab[0] = chromaMc<8, false, 28>;
    d.putNoRndVc1ChromaPixelsTab[1] = chromaMc<4, false, 28>;
    d.avgNoRndVc1ChromaPixelsTab[0] = chromaMc<8, true, 28>;
    d.avgNoRndVc1ChromaPixelsTab[1] = chromaMc<4, true, 28>;
    d.putH264ChromaPixelsTab[0] = chromaMc<8, false, 32>;
    d.putH264ChromaPixelsTab[1] = chromaMc<4, false, 32>;
    d.avgH264ChromaPixelsTab[0] = chromaMc<8, true, 32>;
    d.avgH264ChromaPixelsTab[1] = chromaMc<4, true, 32>;
    fillHpel<16, true, false>(d.putPixelsTab[0]);
    fillHpel<8, true, false>(d.putPixelsTab[1]);
    fillHpel<16, false, false>(d.putNoRndPixelsTab[0]);
    fillHpel<8, false, false>(d.putNoRndPixelsTab[1]);
    fillHpel<16, true, true>(d.avgPixelsTab[0]);
    fillHpel<8, true, true>(d.avgPixelsTab[1]);
    fillHpel<16, false, true>(d.avgNoRndPixelsTab);
    return d;
}

}

void simpleIdct(Dsp& d) {
    d.invTrans8x8 = simpleIdct8x8;
    d.invTrans8x4 = d.invTrans8x4Dc = simpleIdct84Add;
    d.invTrans4x8 = d.invTrans4x8Dc = simpleIdct48Add;
    d.invTrans4x4 = d.invTrans4x4Dc = simpleIdct44Add;
    d.invTrans8x8Dc = simpleIdct8x8Add;
}

const Dsp& dsp() {
    static const Dsp d = made();
    return d;
}

void putPixelsClamped(const int16_t* block, uint8_t* pixels, ptrdiff_t stride) {
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) pixels[j] = clipU8(block[j]);
        pixels += stride;
        block += 8;
    }
}

void putSignedPixelsClamped(const int16_t* block, uint8_t* pixels, ptrdiff_t stride) {
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) pixels[j] = block[j] < -128 ? 0 : block[j] > 127 ? 255 : uint8_t(block[j] + 128);
        pixels += stride;
        block += 8;
    }
}

void addPixelsClamped(const int16_t* block, uint8_t* pixels, ptrdiff_t stride) {
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) pixels[j] = clipU8(pixels[j] + block[j]);
        pixels += stride;
        block += 8;
    }
}

void emulatedEdgeMc(uint8_t* buf, const uint8_t* src, ptrdiff_t bufStride, ptrdiff_t srcStride, int blockW, int blockH, int srcX, int srcY, int w, int h) {
    if (!w || !h) return;
    if (srcY >= h) {
        src -= srcY * srcStride;
        src += (h - 1) * srcStride;
        srcY = h - 1;
    } else if (srcY <= -blockH) {
        src -= srcY * srcStride;
        src += (1 - blockH) * srcStride;
        srcY = 1 - blockH;
    }
    if (srcX >= w) {
        src -= (1 + srcX - w);
        srcX = w - 1;
    } else if (srcX <= -blockW) {
        src += (1 - blockW - srcX);
        srcX = 1 - blockW;
    }
    const int startY = std::max(0, -srcY), startX = std::max(0, -srcX);
    const int endY = std::min(blockH, h - srcY), endX = std::min(blockW, w - srcX);
    const int columns = endX - startX;
    src += startY * srcStride + startX;
    buf += startX;
    int y = 0;
    for (; y < startY; y++) {
        memcpy(buf, src, size_t(columns));
        buf += bufStride;
    }
    for (; y < endY; y++) {
        memcpy(buf, src, size_t(columns));
        src += srcStride;
        buf += bufStride;
    }
    src -= srcStride;
    for (; y < blockH; y++) {
        memcpy(buf, src, size_t(columns));
        buf += bufStride;
    }
    buf -= blockH * bufStride + startX;
    for (int row = 0; row < blockH; row++) {
        for (int x = 0; x < startX; x++) buf[x] = buf[startX];
        for (int x = endX; x < blockW; x++) buf[x] = buf[endX - 1];
        buf += bufStride;
    }
}

int unescapeBuffer(const uint8_t* src, int size, uint8_t* dst) {
    if (size < 4) {
        memcpy(dst, src, size_t(std::max(size, 0)));
        return std::max(size, 0);
    }
    int out = 0;
    for (int i = 0; i < size; i++, src++) {
        if (src[0] == 3 && i >= 2 && !src[-1] && !src[-2] && i < size - 1 && src[1] < 4) {
            dst[out++] = src[1];
            src++;
            i++;
        } else {
            dst[out++] = *src;
        }
    }
    return out;
}

const uint8_t* findNextMarker(const uint8_t* src, const uint8_t* end) {
    if (end - src < 4) return end;
    for (const uint8_t* p = src; p + 3 < end; ++p) {
        if (p[0] == 0 && p[1] == 0 && p[2] == 1) return p;
    }
    return end;
}

}

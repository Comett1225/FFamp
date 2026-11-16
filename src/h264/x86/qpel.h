#ifndef AVCODEC_X86_QPEL_H
#define AVCODEC_X86_QPEL_H

#include <stddef.h>
#include <stdint.h>

void ff_put_pixels8x8_l2_mmxext(uint8_t *dst,
                                const uint8_t *src1, const uint8_t *src2,
                                ptrdiff_t dstStride, ptrdiff_t src1Stride);
void ff_avg_pixels8x8_l2_mmxext(uint8_t *dst,
                                const uint8_t *src1, const uint8_t *src2,
                                ptrdiff_t dstStride, ptrdiff_t src1Stride);
void ff_put_pixels16x16_l2_sse2(uint8_t *dst,
                                const uint8_t *src1, const uint8_t *src2,
                                ptrdiff_t dstStride, ptrdiff_t src1Stride);
void ff_avg_pixels16x16_l2_sse2(uint8_t *dst,
                                const uint8_t *src1, const uint8_t *src2,
                                ptrdiff_t dstStride, ptrdiff_t src1Stride);

#endif

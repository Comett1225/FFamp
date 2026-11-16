#ifndef AVCODEC_X86_FPEL_H
#define AVCODEC_X86_FPEL_H

#include <stddef.h>
#include <stdint.h>

void ff_avg_pixels8_sse2(uint8_t *block, const uint8_t *pixels,
                         ptrdiff_t line_size, int h);
void ff_avg_pixels8x8_sse2(uint8_t *block, const uint8_t *pixels,
                           ptrdiff_t line_size);
void ff_avg_pixels16_sse2(uint8_t *block, const uint8_t *pixels,
                          ptrdiff_t line_size, int h);
void ff_avg_pixels16x16_sse2(uint8_t *block, const uint8_t *pixels,
                             ptrdiff_t line_size);
void ff_put_pixels8_sse2(uint8_t *block, const uint8_t *pixels,
                         ptrdiff_t line_size, int h);
void ff_put_pixels8x8_sse2(uint8_t *block, const uint8_t *pixels,
                           ptrdiff_t line_size);
void ff_put_pixels16_sse2(uint8_t *block, const uint8_t *pixels,
                          ptrdiff_t line_size, int h);
void ff_put_pixels16x16_sse2(uint8_t *block, const uint8_t *pixels,
                             ptrdiff_t line_size);

#endif

#ifndef AVCODEC_X86_VIDEODSP_H
#define AVCODEC_X86_VIDEODSP_H

#include <stddef.h>
#include <stdint.h>

void ff_emulated_edge_mc_sse2(uint8_t *buf, const uint8_t *src,
                              ptrdiff_t buf_stride,
                              ptrdiff_t src_stride,
                              int block_w, int block_h,
                              int src_x, int src_y, int w,
                              int h);

#endif

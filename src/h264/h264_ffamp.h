#ifndef FFAMP_H264_API_H
#define FFAMP_H264_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FFampH264 FFampH264;

typedef struct FFampH264Frame {
    const uint8_t *data[3];
    int linesize[3];
    int width, height;
    int chroma_shift_x, chroma_shift_y;
    int bits;
    int rgb;
    int full_range;
    int primaries, transfer, matrix;
    int key, interlaced, top_field_first, corrupt;
    int64_t pts;
} FFampH264Frame;

typedef void (*FFampH264Sink)(void *opaque, const FFampH264Frame *frame);

enum { FFAMP_H264_FRAME_THREADS = 1, FFAMP_H264_SLICE_THREADS = 2 };

FFampH264 *ffamp_h264_open(const uint8_t *extradata, int extradata_size, int threads, int thread_type);
int ffamp_h264_decode(FFampH264 *dec, const uint8_t *data, int size, int64_t pts, FFampH264Sink sink, void *opaque);
int ffamp_h264_drain(FFampH264 *dec, FFampH264Sink sink, void *opaque);
void ffamp_h264_flush(FFampH264 *dec);
void ffamp_h264_close(FFampH264 *dec);

#ifdef __cplusplus
}
#endif

#endif

#ifndef AVCODEC_H2645_SEI_H
#define AVCODEC_H2645_SEI_H

#include <stdint.h>

#include "ffbase.h"

#include "get_bits.h"
#include "h2645_vui.h"
#include "sei.h"

typedef struct H2645SEIUnregistered {
    int x264_build;
} H2645SEIUnregistered;

typedef struct H2645SEIFramePacking {
    int present;
    int arrangement_id;
    int arrangement_cancel_flag;
    SEIFpaType arrangement_type;
    int arrangement_repetition_period;
    int content_interpretation_type;
    int quincunx_sampling_flag;
    int current_frame_is_frame0_flag;
} H2645SEIFramePacking;

typedef struct H2645SEIDisplayOrientation {
    int present;
    int anticlockwise_rotation;
    int hflip, vflip;
} H2645SEIDisplayOrientation;

typedef struct H2645SEIAlternativeTransfer {
    int present;
    int preferred_transfer_characteristics;
} H2645SEIAlternativeTransfer;

typedef struct H2645SEIFilmGrainCharacteristics {
    int present;
} H2645SEIFilmGrainCharacteristics;

typedef struct H2645SEI {
    H2645SEIUnregistered unregistered;
    H2645SEIFramePacking frame_packing;
    H2645SEIDisplayOrientation display_orientation;
    H2645SEIAlternativeTransfer alternative_transfer;
    H2645SEIFilmGrainCharacteristics *film_grain_characteristics;
} H2645SEI;

enum {
    FF_H2645_SEI_MESSAGE_HANDLED = 0,
    FF_H2645_SEI_MESSAGE_UNHANDLED,
};

int ff_h2645_sei_message_decode(H2645SEI *h, enum SEIType type,
                                enum AVCodecID codec_id, GetBitContext *gb,
                                GetByteContext *gbyte, void *logctx);

int ff_h2645_sei_ctx_replace(H2645SEI *dst, const H2645SEI *src);

void ff_h2645_sei_reset(H2645SEI *s);

#endif

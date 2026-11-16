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
    int model_id;
    int separate_colour_description_present_flag;
    int bit_depth_luma;
    int bit_depth_chroma;
    int full_range;
    int color_primaries;
    int transfer_characteristics;
    int matrix_coeffs;
    int blending_mode_id;
    int log2_scale_factor;
    int comp_model_present_flag[3];
    uint16_t num_intensity_intervals[3];
    uint8_t num_model_values[3];
    uint8_t intensity_interval_lower_bound[3][256];
    uint8_t intensity_interval_upper_bound[3][256];
    int16_t comp_model_value[3][256][6];
    int repetition_period;
    int persistence_flag;
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

struct AVFilmGrainParams;
int ff_h2645_sei_film_grain(H2645SEI *sei, struct AVFilmGrainParams *fgp, const H2645VUI *vui,
                            unsigned bit_depth_luma, unsigned bit_depth_chroma, int width, int height, int seed);

#endif

#ifndef AVCODEC_H274_H
#define AVCODEC_H274_H

#include "ffbase.h"

typedef struct AVFilmGrainH274Params {
    int model_id;
    int blending_mode_id;
    int log2_scale_factor;
    int component_model_present[3];
    uint16_t num_intensity_intervals[3];
    uint8_t num_model_values[3];
    uint8_t intensity_interval_lower_bound[3][256];
    uint8_t intensity_interval_upper_bound[3][256];
    int16_t comp_model_value[3][256][6];
} AVFilmGrainH274Params;

typedef struct AVFilmGrainParams {
    uint64_t seed;
    int width, height;
    int subsampling_x, subsampling_y;
    int color_range, color_primaries, color_trc, color_space;
    int bit_depth_luma, bit_depth_chroma;
    struct {
        AVFilmGrainH274Params h274;
    } codec;
} AVFilmGrainParams;

int ff_h274_apply_film_grain(AVFrame *out, const AVFrame *in,
                             const AVFilmGrainParams *params);

#endif

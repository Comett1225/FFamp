#ifndef AVCODEC_H264_SEI_H
#define AVCODEC_H264_SEI_H

#include "get_bits.h"
#include "h2645_sei.h"
#include "h264_ps.h"
#include "sei.h"

typedef enum {
    H264_SEI_PIC_STRUCT_FRAME             = 0,
    H264_SEI_PIC_STRUCT_TOP_FIELD         = 1,
    H264_SEI_PIC_STRUCT_BOTTOM_FIELD      = 2,
    H264_SEI_PIC_STRUCT_TOP_BOTTOM        = 3,
    H264_SEI_PIC_STRUCT_BOTTOM_TOP        = 4,
    H264_SEI_PIC_STRUCT_TOP_BOTTOM_TOP    = 5,
    H264_SEI_PIC_STRUCT_BOTTOM_TOP_BOTTOM = 6,
    H264_SEI_PIC_STRUCT_FRAME_DOUBLING    = 7,
    H264_SEI_PIC_STRUCT_FRAME_TRIPLING    = 8
} H264_SEI_PicStructType;

typedef struct H264SEITimeCode {

    int full;
    int frame;
    int seconds;
    int minutes;
    int hours;
    int dropframe;
} H264SEITimeCode;

typedef struct H264SEIPictureTiming {

    uint8_t payload[40];
    int     payload_size_bytes;

    int present;
    H264_SEI_PicStructType pic_struct;

    int ct_type;

    int dpb_output_delay;

    int cpb_removal_delay;

    H264SEITimeCode timecode[3];

    int timecode_cnt;
} H264SEIPictureTiming;

typedef struct H264SEIRecoveryPoint {

    int recovery_frame_cnt;
} H264SEIRecoveryPoint;

typedef struct H264SEIBufferingPeriod {
    int present;
    int initial_cpb_removal_delay[32];
} H264SEIBufferingPeriod;

typedef struct H264SEIGreenMetaData {
    uint8_t green_metadata_type;
    uint8_t period_type;
    uint16_t num_seconds;
    uint16_t num_pictures;
    uint8_t percent_non_zero_macroblocks;
    uint8_t percent_intra_coded_macroblocks;
    uint8_t percent_six_tap_filtering;
    uint8_t percent_alpha_point_deblocking_instance;
    uint8_t xsd_metric_type;
    uint16_t xsd_metric_value;
} H264SEIGreenMetaData;

typedef struct H264SEIContext {
    H2645SEI common;
    H264SEIPictureTiming picture_timing;
    H264SEIRecoveryPoint recovery_point;
    H264SEIBufferingPeriod buffering_period;
    H264SEIGreenMetaData green_metadata;
} H264SEIContext;

struct H264ParamSets;

int ff_h264_sei_decode(H264SEIContext *h, GetBitContext *gb,
                       const struct H264ParamSets *ps, void *logctx);

void ff_h264_sei_uninit(H264SEIContext *h);

const char *ff_h264_sei_stereo_mode(const H2645SEIFramePacking *h);

int ff_h264_sei_process_picture_timing(H264SEIPictureTiming *h, const SPS *sps,
                                       void *logctx);

#endif

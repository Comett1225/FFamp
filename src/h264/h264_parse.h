#ifndef AVCODEC_H264_PARSE_H
#define AVCODEC_H264_PARSE_H

#include "ffbase.h"

#include <stdint.h>


#include "get_bits.h"
#include "h264_ps.h"

#define MB_TYPE_REF0       MB_TYPE_CODEC_SPECIFIC
#define MB_TYPE_8x8DCT     0x01000000

static const uint8_t scan8[16 * 3 + 3] = {
    4 +  1 * 8, 5 +  1 * 8, 4 +  2 * 8, 5 +  2 * 8,
    6 +  1 * 8, 7 +  1 * 8, 6 +  2 * 8, 7 +  2 * 8,
    4 +  3 * 8, 5 +  3 * 8, 4 +  4 * 8, 5 +  4 * 8,
    6 +  3 * 8, 7 +  3 * 8, 6 +  4 * 8, 7 +  4 * 8,
    4 +  6 * 8, 5 +  6 * 8, 4 +  7 * 8, 5 +  7 * 8,
    6 +  6 * 8, 7 +  6 * 8, 6 +  7 * 8, 7 +  7 * 8,
    4 +  8 * 8, 5 +  8 * 8, 4 +  9 * 8, 5 +  9 * 8,
    6 +  8 * 8, 7 +  8 * 8, 6 +  9 * 8, 7 +  9 * 8,
    4 + 11 * 8, 5 + 11 * 8, 4 + 12 * 8, 5 + 12 * 8,
    6 + 11 * 8, 7 + 11 * 8, 6 + 12 * 8, 7 + 12 * 8,
    4 + 13 * 8, 5 + 13 * 8, 4 + 14 * 8, 5 + 14 * 8,
    6 + 13 * 8, 7 + 13 * 8, 6 + 14 * 8, 7 + 14 * 8,
    0 +  0 * 8, 0 +  5 * 8, 0 + 10 * 8
};

typedef enum MMCOOpcode {
    MMCO_END = 0,
    MMCO_SHORT2UNUSED,
    MMCO_LONG2UNUSED,
    MMCO_SHORT2LONG,
    MMCO_SET_MAX_LONG,
    MMCO_RESET,
    MMCO_LONG,
} MMCOOpcode;

typedef struct H264PredWeightTable {
    int use_weight;
    int use_weight_chroma;
    int luma_log2_weight_denom;
    int chroma_log2_weight_denom;
    int luma_weight_flag[2];
    int chroma_weight_flag[2];

    int luma_weight[48][2][2];
    int chroma_weight[48][2][2][2];
    int implicit_weight[48][48][2];
} H264PredWeightTable;

typedef struct H264POCContext {
    int poc_lsb;
    int poc_msb;
    int delta_poc_bottom;
    int delta_poc[2];
    int frame_num;
    int prev_poc_msb;
    int prev_poc_lsb;
    int frame_num_offset;
    int prev_frame_num_offset;
    int prev_frame_num;
} H264POCContext;

int ff_h264_pred_weight_table(GetBitContext *gb, const SPS *sps,
                              const int *ref_count, int slice_type_nos,
                              H264PredWeightTable *pwt,
                              int picture_structure, void *logctx);

int ff_h264_check_intra4x4_pred_mode(int8_t *pred_mode_cache, void *logctx,
                                     int top_samples_available, int left_samples_available);

int ff_h264_check_intra_pred_mode(void *logctx, int top_samples_available,
                                  int left_samples_available,
                                  int mode, int is_chroma);

int ff_h264_parse_ref_count(int *plist_count, int ref_count[2],
                            GetBitContext *gb, const PPS *pps,
                            int slice_type_nos, int picture_structure, void *logctx);

int ff_h264_init_poc(int pic_field_poc[2], int *pic_poc,
                     const SPS *sps, H264POCContext *poc,
                     int picture_structure, int nal_ref_idc);

int ff_h264_decode_extradata(const uint8_t *data, int size, H264ParamSets *ps,
                             int *is_avc, int *nal_length_size,
                             int err_recognition, void *logctx);

static av_always_inline uint32_t pack16to32(unsigned a, unsigned b)
{
#if HAVE_BIGENDIAN
    return (b & 0xFFFF) + (a << 16);
#else
    return (a & 0xFFFF) + (b << 16);
#endif
}

#endif

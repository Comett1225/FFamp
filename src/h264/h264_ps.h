#ifndef AVCODEC_H264_PS_H
#define AVCODEC_H264_PS_H

#include <stdint.h>

#include "ffbase.h"

#include "get_bits.h"
#include "h264.h"
#include "h2645_vui.h"

#define MAX_SPS_COUNT          32
#define MAX_PPS_COUNT         256
#define MAX_LOG2_MAX_FRAME_NUM    (12 + 4)

typedef struct SPS {
    unsigned int sps_id;
    int profile_idc;
    int level_idc;
    int chroma_format_idc;
    int transform_bypass;
    int log2_max_frame_num;
    int poc_type;
    int log2_max_poc_lsb;
    int delta_pic_order_always_zero_flag;
    int offset_for_non_ref_pic;
    int offset_for_top_to_bottom_field;
    int poc_cycle_length;
    int ref_frame_count;
    int gaps_in_frame_num_allowed_flag;
    int mb_width;

    int mb_height;
    int frame_mbs_only_flag;
    int mb_aff;
    int direct_8x8_inference_flag;
    int crop;

    unsigned int crop_left;
    unsigned int crop_right;
    unsigned int crop_top;
    unsigned int crop_bottom;
    int vui_parameters_present_flag;
    H2645VUI vui;

    int timing_info_present_flag;
    uint32_t num_units_in_tick;
    uint32_t time_scale;
    int fixed_frame_rate_flag;
    int32_t offset_for_ref_frame[256];
    int bitstream_restriction_flag;
    int num_reorder_frames;
    int max_dec_frame_buffering;
    int scaling_matrix_present;
    uint16_t scaling_matrix_present_mask;
    uint8_t scaling_matrix4[6][16];
    uint8_t scaling_matrix8[6][64];
    int nal_hrd_parameters_present_flag;
    int vcl_hrd_parameters_present_flag;
    int pic_struct_present_flag;
    int time_offset_length;
    int cpb_cnt;
    int bit_rate_scale;
    uint32_t bit_rate_value[32];
    uint32_t cpb_size_value[32];
    uint32_t cpr_flag;
    int initial_cpb_removal_delay_length;
    int cpb_removal_delay_length;
    int dpb_output_delay_length;
    int bit_depth_luma;
    int bit_depth_chroma;
    int residual_color_transform_flag;
    int constraint_set_flags;
    uint8_t data[4096];
    size_t data_size;
} SPS;

typedef struct PPS {
    unsigned int pps_id;
    unsigned int sps_id;
    int cabac;
    int pic_order_present;
    int slice_group_count;
    int mb_slice_group_map_type;
    unsigned int ref_count[2];
    int weighted_pred;
    int weighted_bipred_idc;
    int init_qp;
    int init_qs;
    int chroma_qp_index_offset[2];
    int deblocking_filter_parameters_present;
    int constrained_intra_pred;
    int redundant_pic_cnt_present;
    int transform_8x8_mode;
    int pic_scaling_matrix_present_flag;
    uint16_t pic_scaling_matrix_present_mask;
    uint8_t scaling_matrix4[6][16];
    uint8_t scaling_matrix8[6][64];
    uint8_t chroma_qp_table[2][QP_MAX_NUM+1];
    int chroma_qp_diff;
    uint8_t data[4096];
    size_t data_size;

    uint32_t dequant4_buffer[6][QP_MAX_NUM + 1][16];
    uint32_t dequant8_buffer[6][QP_MAX_NUM + 1][64];
    uint32_t(*dequant4_coeff[6])[16];
    uint32_t(*dequant8_coeff[6])[64];

    const SPS   *sps;
} PPS;

typedef struct H264ParamSets {
    const SPS *sps_list[MAX_SPS_COUNT];
    const PPS *pps_list[MAX_PPS_COUNT];

    const PPS *pps;
    const SPS *sps;

    int overread_warning_printed[2];
} H264ParamSets;

int ff_h264_get_profile(const SPS *sps);

int ff_h264_decode_seq_parameter_set(GetBitContext *gb, AVCodecContext *avctx,
                                     H264ParamSets *ps, int ignore_truncation);

int ff_h264_decode_picture_parameter_set(GetBitContext *gb, AVCodecContext *avctx,
                                         H264ParamSets *ps, int bit_length);

void ff_h264_ps_uninit(H264ParamSets *ps);

#endif

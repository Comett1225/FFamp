#ifndef AVCODEC_H2645_VUI_H
#define AVCODEC_H2645_VUI_H

#include "ffbase.h"

#include "get_bits.h"

typedef struct H2645VUI {
    AVRational sar;
    int aspect_ratio_idc;
    int aspect_ratio_info_present_flag;

    int overscan_info_present_flag;
    int overscan_appropriate_flag;

    int video_signal_type_present_flag;
    int video_format;
    int video_full_range_flag;
    int colour_description_present_flag;
    enum AVColorPrimaries colour_primaries;
    enum AVColorTransferCharacteristic transfer_characteristics;
    enum AVColorSpace matrix_coeffs;

    int chroma_loc_info_present_flag;
    int chroma_sample_loc_type_top_field;
    int chroma_sample_loc_type_bottom_field;
    enum AVChromaLocation chroma_location;
} H2645VUI;

void ff_h2645_decode_common_vui_params(GetBitContext *gb, H2645VUI *vui, void *logctx);

#endif

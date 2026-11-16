#ifndef AVCODEC_H264DEC_H
#define AVCODEC_H264DEC_H

#include "ffbase.h"

#include "cabac.h"
#include "error_resilience.h"
#include "h264_parse.h"
#include "h264_ps.h"
#include "h264_sei.h"
#include "h2645_parse.h"
#include "h264chroma.h"
#include "h264dsp.h"
#include "h264pred.h"
#include "h264qpel.h"
#include "mpegutils.h"
#include "videodsp.h"

#define H264_MAX_PICTURE_COUNT 36

#define ALLOW_INTERLACE

#define FMO 0

#define MAX_SLICES 32

#ifdef ALLOW_INTERLACE
#define MB_MBAFF(h)    (h)->mb_mbaff
#define MB_FIELD(sl)  (sl)->mb_field_decoding_flag
#define FRAME_MBAFF(h) (h)->mb_aff_frame
#define FIELD_PICTURE(h) ((h)->picture_structure != PICT_FRAME)
#define LEFT_MBS 2
#define LTOP     0
#define LBOT     1
#define LEFT(i)  (i)
#else
#define MB_MBAFF(h)      0
#define MB_FIELD(sl)     0
#define FRAME_MBAFF(h)   0
#define FIELD_PICTURE(h) 0
#undef  IS_INTERLACED
#define IS_INTERLACED(mb_type) 0
#define LEFT_MBS 1
#define LTOP     0
#define LBOT     0
#define LEFT(i)  0
#endif
#define FIELD_OR_MBAFF_PICTURE(h) (FRAME_MBAFF(h) || FIELD_PICTURE(h))

#ifndef CABAC
#define CABAC(h) (h)->ps.pps->cabac
#endif

#define CHROMA(h)    ((h)->ps.sps->chroma_format_idc)
#define CHROMA422(h) ((h)->ps.sps->chroma_format_idc == 2)
#define CHROMA444(h) ((h)->ps.sps->chroma_format_idc == 3)

#define IS_REF0(a)         ((a) & MB_TYPE_REF0)
#define IS_8x8DCT(a)       ((a) & MB_TYPE_8x8DCT)
#define IS_SUB_8X8(a)      ((a) & MB_TYPE_16x16)
#define IS_SUB_8X4(a)      ((a) & MB_TYPE_16x8)
#define IS_SUB_4X8(a)      ((a) & MB_TYPE_8x16)
#define IS_SUB_4X4(a)      ((a) & MB_TYPE_8x8)
#define IS_DIR(a, part, list) ((a) & (MB_TYPE_P0L0 << ((part) + 2 * (list))))

#define USES_LIST(a, list) ((a) & ((MB_TYPE_P0L0 | MB_TYPE_P1L0) << (2 * (list))))

typedef struct MMCO {
    MMCOOpcode opcode;
    int short_pic_num;
    int long_arg;
} MMCO;

typedef struct H264Picture {
    AVFrame *f;
    ThreadFrame tf;

    AVFrame *f_grain;
    struct AVFilmGrainParams *fg;

    int8_t *qscale_table_base;
    int8_t *qscale_table;

    int16_t (*motion_val_base[2])[2];
    int16_t (*motion_val[2])[2];

    uint32_t *mb_type_base;
    uint32_t *mb_type;

    void *hwaccel_picture_private;

    int8_t *ref_index[2];

    int field_poc[2];
    int poc;
    int frame_num;
    int mmco_reset;
    int pic_id;
    int long_ref;
    int ref_poc[2][2][32];
    int ref_count[2][2];
    int mbaff;
    int field_picture;

#define DELAYED_PIC_REF  (1 << 2)
    int reference;
    int recovered;
    int invalid_gap;
    int sei_recovery_frame_cnt;
    int needs_fg;

    const PPS   *pps;

    int mb_width, mb_height;
    int mb_stride;

    atomic_int *decode_error_flags;

    int gray;
} H264Picture;

typedef struct H264Ref {
    uint8_t *data[3];
    int linesize[3];

    int reference;
    int poc;
    int pic_id;

    const H264Picture *parent;
} H264Ref;

typedef struct H264SliceContext {
    const struct H264Context *h264;
    GetBitContext gb;
    ERContext *er;

    int slice_num;
    int slice_type;
    int slice_type_nos;
    int slice_type_fixed;

    int qscale;
    int chroma_qp[2];
    int qp_thresh;
    int last_qscale_diff;

    int deblocking_filter;
    int slice_alpha_c0_offset;
    int slice_beta_offset;

    H264PredWeightTable pwt;

    int prev_mb_skipped;
    int next_mb_skipped;

    int chroma_pred_mode;
    int intra16x16_pred_mode;

    int8_t intra4x4_pred_mode_cache[5 * 8];
    int8_t(*intra4x4_pred_mode);

    int topleft_mb_xy;
    int top_mb_xy;
    int topright_mb_xy;
    int left_mb_xy[LEFT_MBS];

    int topleft_type;
    int top_type;
    int topright_type;
    int left_type[LEFT_MBS];

    const uint8_t *left_block;
    int topleft_partition;

    unsigned int topleft_samples_available;
    unsigned int top_samples_available;
    unsigned int topright_samples_available;
    unsigned int left_samples_available;

    ptrdiff_t linesize, uvlinesize;
    ptrdiff_t mb_linesize;
    ptrdiff_t mb_uvlinesize;

    int mb_x, mb_y;
    int mb_xy;
    int resync_mb_x;
    int resync_mb_y;
    unsigned int first_mb_addr;

    int next_slice_idx;
    int mb_skip_run;
    int is_complex;

    int picture_structure;
    int mb_field_decoding_flag;
    int mb_mbaff;

    int redundant_pic_count;

    int neighbor_transform_size;

    int direct_spatial_mv_pred;
    int col_parity;
    int col_fieldoff;

    int cbp;
    int top_cbp;
    int left_cbp;

    int dist_scale_factor[32];
    int dist_scale_factor_field[2][32];
    int map_col_to_list0[2][16 + 32];
    int map_col_to_list0_field[2][2][16 + 32];

    unsigned int ref_count[2];
    unsigned int list_count;
    H264Ref ref_list[2][48];
    struct {
        uint8_t op;
        uint32_t val;
    } ref_modifications[2][32];
    int nb_ref_modifications[2];

    unsigned int pps_id;

    const uint8_t *intra_pcm_ptr;

    uint8_t *bipred_scratchpad;
    uint8_t *edge_emu_buffer;
    uint8_t (*top_borders[2])[(16 * 3) * 2];
    int bipred_scratchpad_allocated;
    int edge_emu_buffer_allocated;
    int top_borders_allocated[2];

    DECLARE_ALIGNED(8, uint8_t, non_zero_count_cache)[15 * 8];

    DECLARE_ALIGNED(16, int16_t, mv_cache)[2][5 * 8][2];
    DECLARE_ALIGNED(8,  int8_t, ref_cache)[2][5 * 8];
    DECLARE_ALIGNED(16, uint8_t, mvd_cache)[2][5 * 8][2];
    uint8_t direct_cache[5 * 8];

    DECLARE_ALIGNED(8, uint16_t, sub_mb_type)[4];

    DECLARE_ALIGNED(16, int16_t, mb)[16 * 48 * 2];
    DECLARE_ALIGNED(16, int16_t, mb_luma_dc)[3][16 * 2];

    int16_t mb_padding[256 * 2];

    uint8_t (*mvd_table[2])[2];

    CABACContext cabac;
    uint8_t cabac_state[1024];
    int cabac_init_idc;

    MMCO mmco[H264_MAX_MMCO_COUNT];
    int  nb_mmco;
    int explicit_ref_marking;

    int frame_num;
    int idr_pic_id;
    int poc_lsb;
    int delta_poc_bottom;
    int delta_poc[2];
    int curr_pic_num;
    int max_pic_num;
} H264SliceContext;

typedef struct H264Context {
    const AVClass *class;
    AVCodecContext *avctx;
    VideoDSPContext vdsp;
    H264DSPContext h264dsp;
    H264ChromaContext h264chroma;
    H264QpelContext h264qpel;

    H264Picture DPB[H264_MAX_PICTURE_COUNT];
    H264Picture *cur_pic_ptr;
    H264Picture cur_pic;
    H264Picture last_pic_for_ec;

    H264SliceContext *slice_ctx;
    int            nb_slice_ctx;
    int            nb_slice_ctx_queued;

    H2645Packet pkt;

    int pixel_shift;

    int width, height;
    int chroma_x_shift, chroma_y_shift;

    int droppable;

    int context_initialized;
    int flags;
    int workaround_bugs;
    int x264_build;

    int postpone_filter;

    int picture_idr;

    int picture_intra_only;

    int crop_left;
    int crop_right;
    int crop_top;
    int crop_bottom;

    int8_t(*intra4x4_pred_mode);
    H264PredContext hpc;

    uint8_t (*non_zero_count)[48];

#define LIST_NOT_USED -1

    int block_offset[2 * (16 * 3)];

    uint32_t *mb2b_xy;
    uint32_t *mb2br_xy;
    int b_stride;

    uint16_t *slice_table;

    int mb_aff_frame;
    int picture_structure;
    int first_field;

    uint8_t *list_counts;

    uint16_t *cbp_table;

    uint8_t *chroma_pred_mode_table;
    uint8_t (*mvd_table[2])[2];
    uint8_t *direct_table;

    uint8_t scan_padding[16];
    uint8_t zigzag_scan[16];
    uint8_t zigzag_scan8x8[64];
    uint8_t zigzag_scan8x8_cavlc[64];
    uint8_t field_scan[16];
    uint8_t field_scan8x8[64];
    uint8_t field_scan8x8_cavlc[64];
    uint8_t zigzag_scan_q0[16];
    uint8_t zigzag_scan8x8_q0[64];
    uint8_t zigzag_scan8x8_cavlc_q0[64];
    uint8_t field_scan_q0[16];
    uint8_t field_scan8x8_q0[64];
    uint8_t field_scan8x8_cavlc_q0[64];

    int mb_y;
    int mb_height, mb_width;
    int mb_stride;
    int mb_num;

    int nal_ref_idc;
    int nal_unit_type;

    int has_slice;

    int is_avc;
    int nal_length_size;

    int bit_depth_luma;
    int chroma_format_idc;

    H264ParamSets ps;

    uint16_t *slice_table_base;

    H264POCContext poc;

    H264Ref default_ref[2];
    H264Picture *short_ref[32];
    H264Picture *long_ref[32];
    H264Picture *delayed_pic[H264_MAX_DPB_FRAMES + 2];
    int last_pocs[H264_MAX_DPB_FRAMES];
    H264Picture *next_output_pic;
    int next_outputed_poc;
    int poc_offset;

    MMCO mmco[H264_MAX_MMCO_COUNT];
    int  nb_mmco;
    int mmco_reset;
    int explicit_ref_marking;

    int long_ref_count;
    int short_ref_count;

    int current_slice;

    int prev_interlaced_frame;

    int valid_recovery_point;

    int recovery_frame;

#define FRAME_RECOVERED_IDR  (1 << 0)

#define FRAME_RECOVERED_SEI  (1 << 1)

#define FRAME_RECOVERED_HEURISTIC  (1 << 2)

    int frame_recovered;

    int has_recovery_point;

    int missing_fields;

    int setup_finished;

    int cur_chroma_format_idc;
    int cur_bit_depth_luma;
    int16_t slice_row[MAX_SLICES];

    int width_from_caller;
    int height_from_caller;

    int enable_er;
    ERContext er;
    int16_t *dc_val_base;

    H264SEIContext sei;

    struct AVRefStructPool *qscale_table_pool;
    struct AVRefStructPool *mb_type_pool;
    struct AVRefStructPool *motion_val_pool;
    struct AVRefStructPool *ref_index_pool;
    struct AVRefStructPool *decode_error_flags_pool;
    int ref2frm[MAX_SLICES][2][64];

    int non_gray;
    int noref_gray;
    int skip_gray;
} H264Context;

extern const uint16_t ff_h264_mb_sizes[4];

int ff_h264_get_slice_type(const H264SliceContext *sl);

int ff_h264_alloc_tables(H264Context *h);

int ff_h264_decode_ref_pic_list_reordering(H264SliceContext *sl, void *logctx);
int ff_h264_build_ref_list(H264Context *h, H264SliceContext *sl);
void ff_h264_remove_all_refs(H264Context *h);

int ff_h264_execute_ref_pic_marking(H264Context *h);

int ff_h264_decode_ref_pic_marking(H264SliceContext *sl, GetBitContext *gb,
                                   const H2645NAL *nal, void *logctx);

void ff_h264_hl_decode_mb(const H264Context *h, H264SliceContext *sl);
void ff_h264_decode_init_vlc(void);

int ff_h264_decode_mb_cavlc(const H264Context *h, H264SliceContext *sl);

int ff_h264_decode_mb_cabac(const H264Context *h, H264SliceContext *sl);

void ff_h264_init_cabac_states(const H264Context *h, H264SliceContext *sl);

void ff_h264_direct_dist_scale_factor(const H264Context *const h, H264SliceContext *sl);
void ff_h264_direct_ref_list_init(const H264Context *const h, H264SliceContext *sl);
void ff_h264_pred_direct_motion(const H264Context *const h, H264SliceContext *sl,
                                int *mb_type);

void ff_h264_filter_mb_fast(const H264Context *h, H264SliceContext *sl, int mb_x, int mb_y,
                            uint8_t *img_y, uint8_t *img_cb, uint8_t *img_cr,
                            unsigned int linesize, unsigned int uvlinesize);
void ff_h264_filter_mb(const H264Context *h, H264SliceContext *sl, int mb_x, int mb_y,
                       uint8_t *img_y, uint8_t *img_cb, uint8_t *img_cr,
                       unsigned int linesize, unsigned int uvlinesize);

#define LUMA_DC_BLOCK_INDEX   48
#define CHROMA_DC_BLOCK_INDEX 49

static av_always_inline int get_chroma_qp(const PPS *pps, int t, int qscale)
{
    return pps->chroma_qp_table[t][qscale];
}

int ff_h264_field_end(H264Context *h, H264SliceContext *sl, int in_setup);

int ff_h264_ref_picture(H264Picture *dst, const H264Picture *src);
int ff_h264_replace_picture(H264Picture *dst, const H264Picture *src);
void ff_h264_unref_picture(H264Picture *pic);

void ff_h264_slice_context_init(H264Context *h, H264SliceContext *sl);

void ff_h264_draw_horiz_band(const H264Context *h, H264SliceContext *sl, int y, int height);

int ff_h264_queue_decode_slice(H264Context *h, const H2645NAL *nal);
int ff_h264_execute_decode_slices(H264Context *h);
int ff_h264_update_thread_context(AVCodecContext *dst,
                                  const AVCodecContext *src);
int ff_h264_update_thread_context_for_user(AVCodecContext *dst,
                                           const AVCodecContext *src);

void ff_h264_flush_change(H264Context *h);

void ff_h264_free_tables(H264Context *h);

void ff_h264_set_erpic(ERPicture *dst, const H264Picture *src);

int ff_h264_decode_init(AVCodecContext *avctx);
int ff_h264_decode_end(AVCodecContext *avctx);
void ff_h264_decode_flush(AVCodecContext *avctx);
int ff_h264_decode_frame(AVCodecContext *avctx, AVFrame *pict,
                         int *got_frame, const uint8_t *buf, int buf_size);

#endif

#ifndef AVCODEC_H2645_PARSE_H
#define AVCODEC_H2645_PARSE_H

#include <stdint.h>

#include "ffbase.h"
#include "get_bits.h"

#define MAX_MBPAIR_SIZE (256*1024)

typedef struct H2645NAL {
    const uint8_t *data;
    int size;

    int size_bits;

    int raw_size;
    const uint8_t *raw_data;

    GetBitContext gb;

    int type;

    int ref_idc;

    int temporal_id;

    int nuh_layer_id;

    int skipped_bytes;
    int skipped_bytes_pos_size;
    int *skipped_bytes_pos;
} H2645NAL;

typedef struct H2645RBSP {
    uint8_t *rbsp_buffer;
    AVBufferRef *rbsp_buffer_ref;
    int rbsp_buffer_alloc_size;
    int rbsp_buffer_size;
} H2645RBSP;

typedef struct H2645Packet {
    H2645NAL *nals;
    H2645RBSP rbsp;
    int nb_nals;
    int nals_allocated;
    unsigned nal_buffer_size;
} H2645Packet;

int ff_h2645_extract_rbsp(const uint8_t *src, int length, H2645RBSP *rbsp,
                          H2645NAL *nal, int small_padding);

enum {
    H2645_FLAG_IS_NALFF =         (1 << 0),
    H2645_FLAG_SMALL_PADDING =    (1 << 1),
    H2645_FLAG_USE_REF =          (1 << 2),
};

int ff_h2645_packet_split(H2645Packet *pkt, const uint8_t *buf, int length,
                          void *logctx, int nal_length_size,
                          enum AVCodecID codec_id, int flags);

void ff_h2645_packet_uninit(H2645Packet *pkt);

int ff_h2645_unit_requires_zero_byte(enum AVCodecID codec_id,
                                     unsigned int type,
                                     int nal_unit_index);

static inline int get_nalsize(int nal_length_size, const uint8_t *buf,
                              int buf_size, int *buf_index, void *logctx)
{
    int i, nalsize = 0;

    if (*buf_index >= buf_size - nal_length_size) {

        return AVERROR_INVALIDDATA;
    }

    for (i = 0; i < nal_length_size; i++)
        nalsize = ((unsigned)nalsize << 8) | buf[(*buf_index)++];
    if (nalsize <= 0 || nalsize > buf_size - *buf_index) {
        av_log(logctx, AV_LOG_ERROR,
               "Invalid NAL unit size (%d > %d).\n", nalsize, buf_size - *buf_index);
        return AVERROR_INVALIDDATA;
    }
    return nalsize;
}

#endif

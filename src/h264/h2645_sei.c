#include "ffbase.h"

#include "get_bits.h"
#include "golomb.h"
#include "h2645_sei.h"

#define IS_H264(codec_id) 1

static int decode_unregistered_user_data(H2645SEIUnregistered *h,
                                         GetByteContext *gb,
                                         enum AVCodecID codec_id)
{
    char *user_data;
    int size = bytestream2_get_bytes_left(gb);

    if (size < 16 || size >= INT_MAX - 1)
        return AVERROR_INVALIDDATA;

    user_data = av_malloc(size + 1);
    if (!user_data)
        return AVERROR(ENOMEM);

    bytestream2_get_bufferu(gb, (uint8_t *)user_data, size);
    user_data[size] = 0;

    {
        int e, build;
        e = sscanf(user_data + 16, "x264 - core %d", &build);
        if (e == 1 && build > 0)
            h->x264_build = build;
        if (e == 1 && build == 1 && !strncmp(user_data+16, "x264 - core 0000", 16))
            h->x264_build = 67;
    }

    av_free(user_data);
    return 0;
}

static int decode_display_orientation(H2645SEIDisplayOrientation *h,
                                      GetBitContext *gb)
{
    h->present = !get_bits1(gb);

    if (h->present) {
        h->hflip = get_bits1(gb);
        h->vflip = get_bits1(gb);

        h->anticlockwise_rotation = get_bits(gb, 16);

    }

    return 0;
}

static int decode_frame_packing_arrangement(H2645SEIFramePacking *h,
                                            GetBitContext *gb,
                                            enum AVCodecID codec_id)
{
    h->arrangement_id          = get_ue_golomb_long(gb);
    h->arrangement_cancel_flag = get_bits1(gb);
    h->present = !h->arrangement_cancel_flag;

    if (h->present) {
        h->arrangement_type              = get_bits(gb, 7);
        h->quincunx_sampling_flag        = get_bits1(gb);
        h->content_interpretation_type   = get_bits(gb, 6);

        skip_bits(gb, 3);
        h->current_frame_is_frame0_flag  = get_bits1(gb);

        skip_bits(gb, 2);

        if (!h->quincunx_sampling_flag && h->arrangement_type != 5)
            skip_bits(gb, 16);
        skip_bits(gb, 8);
        if (IS_H264(codec_id))
            h->arrangement_repetition_period = get_ue_golomb_long(gb);
        else
            skip_bits1(gb);
    }

    skip_bits1(gb);

    return 0;
}

static int decode_alternative_transfer(H2645SEIAlternativeTransfer *s,
                                       GetByteContext *gb)
{
    if (bytestream2_get_bytes_left(gb) < 1)
        return AVERROR_INVALIDDATA;

    s->present = 1;
    s->preferred_transfer_characteristics = bytestream2_get_byteu(gb);

    return 0;
}

static int decode_film_grain_characteristics(H2645SEIFilmGrainCharacteristics *h,
                                             enum AVCodecID codec_id, GetBitContext *gb)
{
    h->present = !get_bits1(gb);
    if (h->present) {
        int separate, comp[3];
        get_bits(gb, 2);
        separate = get_bits1(gb);
        if (separate) {
            get_bits(gb, 3);
            get_bits(gb, 3);
            get_bits1(gb);
            get_bits(gb, 8);
            get_bits(gb, 8);
            get_bits(gb, 8);
        }
        get_bits(gb, 2);
        get_bits(gb, 4);
        for (int c = 0; c < 3; c++)
            comp[c] = get_bits1(gb);
        for (int c = 0; c < 3; c++) {
            if (comp[c]) {
                int intervals = get_bits(gb, 8) + 1;
                int values = get_bits(gb, 3) + 1;
                if (values > 6)
                    return AVERROR_INVALIDDATA;
                for (int i = 0; i < intervals; i++) {
                    get_bits(gb, 8);
                    get_bits(gb, 8);
                    for (int j = 0; j < values; j++)
                        get_se_golomb_long(gb);
                }
            }
        }
        get_ue_golomb_long(gb);
        h->present = 1;
    }
    return 0;
}

static int decode_ambient_viewing_environment(GetByteContext *gb)
{
    static const uint16_t max_ambient_light_value = 50000;
    if (bytestream2_get_bytes_left(gb) < 8)
        return AVERROR_INVALIDDATA;
    if (!bytestream2_get_be32u(gb))
        return AVERROR_INVALIDDATA;
    if (bytestream2_get_be16u(gb) > max_ambient_light_value)
        return AVERROR_INVALIDDATA;
    if (bytestream2_get_be16u(gb) > max_ambient_light_value)
        return AVERROR_INVALIDDATA;
    return 0;
}

static int decode_nal_sei_mastering_display_info(GetByteContext *gb)
{
    return bytestream2_get_bytes_left(gb) < 24 ? AVERROR_INVALIDDATA : 0;
}

static int decode_nal_sei_content_light_info(GetByteContext *gb)
{
    return bytestream2_get_bytes_left(gb) < 4 ? AVERROR_INVALIDDATA : 0;
}

int ff_h2645_sei_message_decode(H2645SEI *h, enum SEIType type,
                                enum AVCodecID codec_id, GetBitContext *gb,
                                GetByteContext *gbyte, void *logctx)
{
    switch (type) {
    case SEI_TYPE_USER_DATA_REGISTERED_ITU_T_T35:
        return 0;
    case SEI_TYPE_USER_DATA_UNREGISTERED:
        return decode_unregistered_user_data(&h->unregistered, gbyte, codec_id);
    case SEI_TYPE_DISPLAY_ORIENTATION:
        return decode_display_orientation(&h->display_orientation, gb);
    case SEI_TYPE_FILM_GRAIN_CHARACTERISTICS:
        av_refstruct_unref(&h->film_grain_characteristics);
        h->film_grain_characteristics = av_refstruct_allocz(sizeof(*h->film_grain_characteristics));
        if (!h->film_grain_characteristics)
            return AVERROR(ENOMEM);
        return decode_film_grain_characteristics(h->film_grain_characteristics, codec_id, gb);
    case SEI_TYPE_FRAME_PACKING_ARRANGEMENT:
        return decode_frame_packing_arrangement(&h->frame_packing, gb, codec_id);
    case SEI_TYPE_ALTERNATIVE_TRANSFER_CHARACTERISTICS:
        return decode_alternative_transfer(&h->alternative_transfer, gbyte);
    case SEI_TYPE_AMBIENT_VIEWING_ENVIRONMENT:
        return decode_ambient_viewing_environment(gbyte);
    case SEI_TYPE_MASTERING_DISPLAY_COLOUR_VOLUME:
        return decode_nal_sei_mastering_display_info(gbyte);
    case SEI_TYPE_CONTENT_LIGHT_LEVEL_INFO:
        return decode_nal_sei_content_light_info(gbyte);
    default:
        return FF_H2645_SEI_MESSAGE_UNHANDLED;
    }
}

int ff_h2645_sei_ctx_replace(H2645SEI *dst, const H2645SEI *src)
{
    av_refstruct_replace(&dst->film_grain_characteristics,
                          src->film_grain_characteristics);

    return 0;
}

void ff_h2645_sei_reset(H2645SEI *s)
{
    av_refstruct_unref(&s->film_grain_characteristics);
}

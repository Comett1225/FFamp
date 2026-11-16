#include "ffbase.h"
#include "h264_threads.h"

#ifdef _WIN32
#include <malloc.h>
#endif

static atomic_int cpu_flags = -1;

int av_get_cpu_flags(void)
{
    int flags = atomic_load_explicit(&cpu_flags, memory_order_relaxed);
    if (flags != -1)
        return flags;
#if ARCH_X86
    flags = ff_get_cpu_flags_x86();
#elif ARCH_AARCH64
    flags = AV_CPU_FLAG_ARMV8 | AV_CPU_FLAG_NEON;
#else
    flags = 0;
#endif
    const char *env = getenv("FFAMP_H264_CPU");
    if (env)
        flags &= (int)strtol(env, NULL, 0);
    atomic_store_explicit(&cpu_flags, flags, memory_order_relaxed);
    return flags;
}

static int log_level = -1;

void av_log(void *avcl, int level, const char *fmt, ...)
{
    if (log_level < 0) {
        const char *env = getenv("FFAMP_H264_LOG");
        log_level = env ? atoi(env) : 0;
    }
    if (level > log_level)
        return;
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

#define MAX_ALLOC ((size_t)INT_MAX)

void *av_malloc(size_t size)
{
    void *ptr = NULL;
    if (size > MAX_ALLOC)
        return NULL;
#ifdef _WIN32
    ptr = _aligned_malloc(size ? size : 1, 64);
#else
    if (posix_memalign(&ptr, 64, size ? size : 1))
        ptr = NULL;
#endif
    return ptr;
}

void *av_mallocz(size_t size)
{
    void *ptr = av_malloc(size);
    if (ptr)
        memset(ptr, 0, size);
    return ptr;
}

void *av_calloc(size_t nmemb, size_t size)
{
    if (size && nmemb > MAX_ALLOC / size)
        return NULL;
    return av_mallocz(nmemb * size);
}

void *av_malloc_array(size_t nmemb, size_t size)
{
    if (size && nmemb > MAX_ALLOC / size)
        return NULL;
    return av_malloc(nmemb * size);
}

void *av_realloc(void *ptr, size_t size)
{
    if (size > MAX_ALLOC)
        return NULL;
#ifdef _WIN32
    return _aligned_realloc(ptr, size + !size, 64);
#else
    return realloc(ptr, size + !size);
#endif
}

void *av_realloc_array(void *ptr, size_t nmemb, size_t size)
{
    if (size && nmemb > MAX_ALLOC / size)
        return NULL;
    return av_realloc(ptr, nmemb * size);
}

void *av_realloc_f(void *ptr, size_t nelem, size_t elsize)
{
    void *r;
    if (elsize && nelem > MAX_ALLOC / elsize) {
        av_free(ptr);
        return NULL;
    }
    r = av_realloc(ptr, nelem * elsize);
    if (!r)
        av_free(ptr);
    return r;
}

int av_reallocp_array(void *ptr, size_t nmemb, size_t size)
{
    void *val;
    memcpy(&val, ptr, sizeof(val));
    val = av_realloc_f(val, nmemb, size);
    memcpy(ptr, &val, sizeof(val));
    if (!val && nmemb && size)
        return AVERROR(ENOMEM);
    return 0;
}

void av_free(void *ptr)
{
#ifdef _WIN32
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}

void av_freep(void *arg)
{
    void *val;
    memcpy(&val, arg, sizeof(val));
    memcpy(arg, &(void *){ NULL }, sizeof(val));
    av_free(val);
}

void *av_fast_realloc(void *ptr, unsigned int *size, size_t min_size)
{
    size_t max_size;
    if (min_size <= *size)
        return ptr;
    max_size = MAX_ALLOC;
    max_size = FFMIN(max_size, UINT_MAX);
    if (min_size > max_size) {
        *size = 0;
        return NULL;
    }
    min_size = FFMIN(max_size, FFMAX(min_size + min_size / 16 + 32, min_size));
    ptr = av_realloc(ptr, min_size);
    if (!ptr)
        min_size = 0;
    *size = (unsigned)min_size;
    return ptr;
}

static void fast_malloc(void *ptr, unsigned int *size, size_t min_size, int zero_realloc)
{
    size_t max_size;
    void *val;
    memcpy(&val, ptr, sizeof(val));
    if (min_size <= *size)
        return;
    max_size = FFMIN(MAX_ALLOC, UINT_MAX);
    if (min_size > max_size) {
        av_freep(ptr);
        *size = 0;
        return;
    }
    min_size = FFMIN(max_size, FFMAX(min_size + min_size / 16 + 32, min_size));
    av_freep(ptr);
    val = zero_realloc ? av_mallocz(min_size) : av_malloc(min_size);
    memcpy(ptr, &val, sizeof(val));
    if (!val)
        min_size = 0;
    *size = (unsigned)min_size;
}

void av_fast_malloc(void *ptr, unsigned int *size, size_t min_size)
{
    fast_malloc(ptr, size, min_size, 0);
}

void av_fast_mallocz(void *ptr, unsigned int *size, size_t min_size)
{
    fast_malloc(ptr, size, min_size, 1);
}

void av_fast_padded_malloc(void *ptr, unsigned int *size, size_t min_size)
{
    uint8_t **p = ptr;
    if (min_size > SIZE_MAX - AV_INPUT_BUFFER_PADDING_SIZE) {
        av_freep(p);
        *size = 0;
        return;
    }
    av_fast_mallocz(p, size, min_size + AV_INPUT_BUFFER_PADDING_SIZE);
}

void *av_memdup(const void *p, size_t size)
{
    void *ptr = NULL;
    if (p) {
        ptr = av_malloc(size);
        if (ptr)
            memcpy(ptr, p, size);
    }
    return ptr;
}

void av_memcpy_backptr(uint8_t *dst, int back, int cnt)
{
    for (int i = 0; i < cnt; i++)
        dst[i] = dst[i - back];
}

static int64_t gcd64(int64_t a, int64_t b)
{
    while (b) {
        int64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

int av_reduce(int *dst_num, int *dst_den, int64_t num, int64_t den, int64_t max)
{
    AVRational a0 = { 0, 1 }, a1 = { 1, 0 };
    int sign = (num < 0) ^ (den < 0);
    int64_t gcd = gcd64(FFABS(num), FFABS(den));

    if (gcd) {
        num = FFABS(num) / gcd;
        den = FFABS(den) / gcd;
    }
    if (num <= max && den <= max) {
        a1 = (AVRational){ (int)num, (int)den };
        den = 0;
    }

    while (den) {
        uint64_t x = num / den;
        int64_t next_den = num - den * x;
        int64_t a2n = x * a1.num + a0.num;
        int64_t a2d = x * a1.den + a0.den;

        if (a2n > max || a2d > max) {
            if (a1.num) x = (max - a0.num) / a1.num;
            if (a1.den) x = FFMIN(x, (max - a0.den) / a1.den);

            if (den * (2 * x * a1.den + a0.den) > num * a1.den)
                a1 = (AVRational){ (int)(x * a1.num + a0.num), (int)(x * a1.den + a0.den) };
            break;
        }

        a0 = a1;
        a1 = (AVRational){ (int)a2n, (int)a2d };
        num = den;
        den = next_den;
    }

    *dst_num = sign ? -a1.num : a1.num;
    *dst_den = a1.den;

    return den == 0;
}

#define PIXFMT(fmt, nm, comps, cw, ch, fl, d) [fmt] = { nm, comps, cw, ch, AV_PIX_FMT_FLAG_PLANAR | (fl), { { 0, 0, 0, 0, d }, { 1, 0, 0, 0, d }, { 2, 0, 0, 0, d }, { 0 } } }
static const AVPixFmtDescriptor pix_fmt_descriptors[AV_PIX_FMT_NB] = {
    PIXFMT(AV_PIX_FMT_YUV420P, "yuv420p", 3, 1, 1, 0, 8),
    PIXFMT(AV_PIX_FMT_YUV422P, "yuv422p", 3, 1, 0, 0, 8),
    PIXFMT(AV_PIX_FMT_YUV444P, "yuv444p", 3, 0, 0, 0, 8),
    PIXFMT(AV_PIX_FMT_YUVJ420P, "yuvj420p", 3, 1, 1, 0, 8),
    PIXFMT(AV_PIX_FMT_YUVJ422P, "yuvj422p", 3, 1, 0, 0, 8),
    PIXFMT(AV_PIX_FMT_YUVJ444P, "yuvj444p", 3, 0, 0, 0, 8),
    PIXFMT(AV_PIX_FMT_YUV420P9, "yuv420p9", 3, 1, 1, 0, 9),
    PIXFMT(AV_PIX_FMT_YUV422P9, "yuv422p9", 3, 1, 0, 0, 9),
    PIXFMT(AV_PIX_FMT_YUV444P9, "yuv444p9", 3, 0, 0, 0, 9),
    PIXFMT(AV_PIX_FMT_YUV420P10, "yuv420p10", 3, 1, 1, 0, 10),
    PIXFMT(AV_PIX_FMT_YUV422P10, "yuv422p10", 3, 1, 0, 0, 10),
    PIXFMT(AV_PIX_FMT_YUV444P10, "yuv444p10", 3, 0, 0, 0, 10),
    PIXFMT(AV_PIX_FMT_YUV420P12, "yuv420p12", 3, 1, 1, 0, 12),
    PIXFMT(AV_PIX_FMT_YUV422P12, "yuv422p12", 3, 1, 0, 0, 12),
    PIXFMT(AV_PIX_FMT_YUV444P12, "yuv444p12", 3, 0, 0, 0, 12),
    PIXFMT(AV_PIX_FMT_YUV420P14, "yuv420p14", 3, 1, 1, 0, 14),
    PIXFMT(AV_PIX_FMT_YUV422P14, "yuv422p14", 3, 1, 0, 0, 14),
    PIXFMT(AV_PIX_FMT_YUV444P14, "yuv444p14", 3, 0, 0, 0, 14),
    PIXFMT(AV_PIX_FMT_GBRP, "gbrp", 3, 0, 0, AV_PIX_FMT_FLAG_RGB, 8),
    PIXFMT(AV_PIX_FMT_GBRP9, "gbrp9", 3, 0, 0, AV_PIX_FMT_FLAG_RGB, 9),
    PIXFMT(AV_PIX_FMT_GBRP10, "gbrp10", 3, 0, 0, AV_PIX_FMT_FLAG_RGB, 10),
    PIXFMT(AV_PIX_FMT_GBRP12, "gbrp12", 3, 0, 0, AV_PIX_FMT_FLAG_RGB, 12),
    PIXFMT(AV_PIX_FMT_GBRP14, "gbrp14", 3, 0, 0, AV_PIX_FMT_FLAG_RGB, 14),
    PIXFMT(AV_PIX_FMT_GRAY8, "gray", 1, 0, 0, 0, 8),
};

const AVPixFmtDescriptor *av_pix_fmt_desc_get(enum AVPixelFormat pix_fmt)
{
    if (pix_fmt < 0 || pix_fmt >= AV_PIX_FMT_NB)
        return NULL;
    return &pix_fmt_descriptors[pix_fmt];
}

int av_pix_fmt_get_chroma_sub_sample(enum AVPixelFormat pix_fmt, int *h_shift, int *v_shift)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(pix_fmt);
    if (!desc)
        return AVERROR(ENOSYS);
    *h_shift = desc->log2_chroma_w;
    *v_shift = desc->log2_chroma_h;
    return 0;
}

const char *av_get_pix_fmt_name(enum AVPixelFormat pix_fmt)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(pix_fmt);
    return desc ? desc->name : NULL;
}

void av_image_copy(uint8_t * const dst_data[4], const int dst_linesizes[4],
                   const uint8_t * const src_data[4], const int src_linesizes[4],
                   enum AVPixelFormat pix_fmt, int width, int height)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(pix_fmt);
    if (!desc)
        return;
    int bytes = desc->comp[0].depth > 8 ? 2 : 1;
    for (int p = 0; p < desc->nb_components; p++) {
        int chroma = p == 1 || p == 2;
        int w = chroma ? AV_CEIL_RSHIFT(width, desc->log2_chroma_w) : width;
        int h = chroma ? AV_CEIL_RSHIFT(height, desc->log2_chroma_h) : height;
        for (int y = 0; y < h; y++)
            memcpy(dst_data[p] + (ptrdiff_t)y * dst_linesizes[p], src_data[p] + (ptrdiff_t)y * src_linesizes[p], (size_t)w * bytes);
    }
}

int av_image_check_size(unsigned int w, unsigned int h, int log_offset, void *log_ctx)
{
    int64_t stride = 8LL * w + 128 * 8;
    if (w == 0 || h == 0 || w > INT32_MAX || h > INT32_MAX || stride >= INT_MAX || stride * (h + 128ULL) >= INT_MAX)
        return AVERROR(EINVAL);
    return 0;
}

const char *av_color_primaries_name(enum AVColorPrimaries primaries)
{
    unsigned v = primaries;
    return v <= 12 || v == 22 ? "known" : NULL;
}

const char *av_color_transfer_name(enum AVColorTransferCharacteristic transfer)
{
    return (unsigned)transfer < AVCOL_TRC_NB ? "known" : NULL;
}

const char *av_color_space_name(enum AVColorSpace space)
{
    return (unsigned)space < AVCOL_SPC_NB ? "known" : NULL;
}

struct AVBuffer {
    uint8_t *data;
    size_t size;
    atomic_uint refcount;
    void (*free)(void *opaque, uint8_t *data);
    void *opaque;
};

void av_buffer_default_free(void *opaque, uint8_t *data)
{
    av_free(data);
}

AVBufferRef *av_buffer_create(uint8_t *data, size_t size, void (*free_cb)(void *opaque, uint8_t *data), void *opaque, int flags)
{
    AVBuffer *buf = av_mallocz(sizeof(*buf));
    AVBufferRef *ref;
    if (!buf)
        return NULL;
    buf->data = data;
    buf->size = size;
    buf->free = free_cb ? free_cb : av_buffer_default_free;
    buf->opaque = opaque;
    atomic_init(&buf->refcount, 1);
    ref = av_mallocz(sizeof(*ref));
    if (!ref) {
        av_free(buf);
        return NULL;
    }
    ref->buffer = buf;
    ref->data = data;
    ref->size = size;
    return ref;
}

AVBufferRef *av_buffer_alloc(size_t size)
{
    uint8_t *data = av_malloc(size);
    AVBufferRef *ret;
    if (!data)
        return NULL;
    ret = av_buffer_create(data, size, av_buffer_default_free, NULL, 0);
    if (!ret)
        av_free(data);
    return ret;
}

AVBufferRef *av_buffer_ref(const AVBufferRef *buf)
{
    AVBufferRef *ret = av_mallocz(sizeof(*ret));
    if (!ret)
        return NULL;
    *ret = *buf;
    atomic_fetch_add_explicit(&buf->buffer->refcount, 1, memory_order_relaxed);
    return ret;
}

void av_buffer_unref(AVBufferRef **buf)
{
    AVBuffer *b;
    if (!buf || !*buf)
        return;
    b = (*buf)->buffer;
    av_freep(buf);
    if (atomic_fetch_sub_explicit(&b->refcount, 1, memory_order_acq_rel) == 1) {
        b->free(b->opaque, b->data);
        av_free(b);
    }
}

int av_buffer_get_ref_count(const AVBufferRef *buf)
{
    return (int)atomic_load(&buf->buffer->refcount);
}

int av_buffer_is_writable(const AVBufferRef *buf)
{
    return atomic_load(&buf->buffer->refcount) == 1;
}

int av_buffer_replace(AVBufferRef **pdst, const AVBufferRef *src)
{
    AVBufferRef *dst = *pdst;
    AVBufferRef *tmp;
    if (!src) {
        av_buffer_unref(pdst);
        return 0;
    }
    if (dst && dst->buffer == src->buffer) {
        dst->data = src->data;
        dst->size = src->size;
        return 0;
    }
    tmp = av_buffer_ref(src);
    if (!tmp)
        return AVERROR(ENOMEM);
    av_buffer_unref(pdst);
    *pdst = tmp;
    return 0;
}

static void frame_defaults(AVFrame *frame)
{
    memset(frame, 0, sizeof(*frame));
    frame->pts = AV_NOPTS_VALUE;
    frame->format = -1;
}

AVFrame *av_frame_alloc(void)
{
    AVFrame *frame = av_malloc(sizeof(*frame));
    if (frame)
        frame_defaults(frame);
    return frame;
}

void av_frame_free(AVFrame **frame)
{
    if (!frame || !*frame)
        return;
    av_frame_unref(*frame);
    av_freep(frame);
}

void av_frame_unref(AVFrame *frame)
{
    if (!frame)
        return;
    for (int i = 0; i < AV_NUM_DATA_POINTERS; i++)
        av_buffer_unref(&frame->buf[i]);
    frame_defaults(frame);
}

int av_frame_ref(AVFrame *dst, const AVFrame *src)
{
    *dst = *src;
    for (int i = 0; i < AV_NUM_DATA_POINTERS; i++) {
        dst->buf[i] = NULL;
        if (src->buf[i]) {
            dst->buf[i] = av_buffer_ref(src->buf[i]);
            if (!dst->buf[i]) {
                av_frame_unref(dst);
                return AVERROR(ENOMEM);
            }
        }
    }
    return 0;
}

int av_frame_replace(AVFrame *dst, const AVFrame *src)
{
    if (dst == src)
        return AVERROR(EINVAL);
    if (!src->buf[0]) {
        av_frame_unref(dst);
        return 0;
    }
    AVBufferRef *bufs[AV_NUM_DATA_POINTERS];
    memcpy(bufs, dst->buf, sizeof(bufs));
    for (int i = 0; i < AV_NUM_DATA_POINTERS; i++) {
        bufs[i] = dst->buf[i];
        if (av_buffer_replace(&bufs[i], src->buf[i]) < 0) {
            memcpy(dst->buf, bufs, sizeof(bufs));
            av_frame_unref(dst);
            return AVERROR(ENOMEM);
        }
    }
    *dst = *src;
    memcpy(dst->buf, bufs, sizeof(bufs));
    return 0;
}

int av_frame_copy_props(AVFrame *dst, const AVFrame *src)
{
    dst->flags = src->flags;
    dst->pict_type = src->pict_type;
    dst->decode_error_flags = src->decode_error_flags;
    dst->repeat_pict = src->repeat_pict;
    dst->pts = src->pts;
    dst->crop_top = src->crop_top;
    dst->crop_bottom = src->crop_bottom;
    dst->crop_left = src->crop_left;
    dst->crop_right = src->crop_right;
    dst->color_range = src->color_range;
    dst->color_primaries = src->color_primaries;
    dst->color_trc = src->color_trc;
    dst->colorspace = src->colorspace;
    return 0;
}

void av_frame_move_ref(AVFrame *dst, AVFrame *src)
{
    *dst = *src;
    frame_defaults(src);
}

typedef struct RefCount {
    atomic_uintptr_t refcount;
    void *opaque;
    void (*free_cb)(AVRefStructOpaque opaque, void *obj);
    AVRefStructPool *pool;
    size_t pad;
} RefCount;

#define REFCOUNT_OFFSET 64

static RefCount *get_refcount(void *obj)
{
    return (RefCount *)((char *)obj - REFCOUNT_OFFSET);
}

static void *get_userdata(void *buf)
{
    return (char *)buf + REFCOUNT_OFFSET;
}

void *av_refstruct_alloc_ext(size_t size, unsigned flags, void *opaque, void (*free_cb)(AVRefStructOpaque opaque, void *obj))
{
    void *buf, *obj;
    if (size > SIZE_MAX - REFCOUNT_OFFSET)
        return NULL;
    buf = av_malloc(size + REFCOUNT_OFFSET);
    if (!buf)
        return NULL;
    RefCount *ref = buf;
    atomic_init(&ref->refcount, 1);
    ref->opaque = opaque;
    ref->free_cb = free_cb;
    ref->pool = NULL;
    obj = get_userdata(buf);
    if (!(flags & AV_REFSTRUCT_FLAG_NO_ZEROING))
        memset(obj, 0, size);
    return obj;
}

void *av_refstruct_allocz(size_t size)
{
    return av_refstruct_alloc_ext(size, 0, NULL, NULL);
}

struct AVRefStructPool {
    size_t size;
    unsigned flags;
    FFMutex mutex;
    void **free_list;
    int nb_free, free_alloc;
    atomic_uintptr_t refcount;
    int uninited;
};

static void pool_unref(AVRefStructPool *pool)
{
    if (atomic_fetch_sub_explicit(&pool->refcount, 1, memory_order_acq_rel) == 1) {
        for (int i = 0; i < pool->nb_free; i++)
            av_free(get_refcount(pool->free_list[i]));
        av_free(pool->free_list);
        ff_mutex_destroy(&pool->mutex);
        av_free(pool);
    }
}

static void pool_return(AVRefStructPool *pool, void *obj)
{
    int keep = 0;
    ff_mutex_lock(&pool->mutex);
    if (!pool->uninited) {
        if (pool->nb_free == pool->free_alloc) {
            int n = pool->free_alloc ? pool->free_alloc * 2 : 8;
            void **list = av_realloc_array(pool->free_list, n, sizeof(*list));
            if (list) {
                pool->free_list = list;
                pool->free_alloc = n;
            }
        }
        if (pool->nb_free < pool->free_alloc) {
            pool->free_list[pool->nb_free++] = obj;
            keep = 1;
        }
    }
    ff_mutex_unlock(&pool->mutex);
    if (!keep)
        av_free(get_refcount(obj));
    pool_unref(pool);
}

void av_refstruct_unref(void *objp)
{
    void *obj;
    memcpy(&obj, objp, sizeof(obj));
    if (!obj)
        return;
    memcpy(objp, &(void *){ NULL }, sizeof(obj));
    RefCount *ref = get_refcount(obj);
    if (atomic_fetch_sub_explicit(&ref->refcount, 1, memory_order_acq_rel) == 1) {
        if (ref->pool) {
            atomic_init(&ref->refcount, 1);
            pool_return(ref->pool, obj);
            return;
        }
        if (ref->free_cb)
            ref->free_cb((AVRefStructOpaque){ .nc = ref->opaque }, obj);
        av_free(ref);
    }
}

void *av_refstruct_ref(void *obj)
{
    atomic_fetch_add_explicit(&get_refcount(obj)->refcount, 1, memory_order_relaxed);
    return obj;
}

const void *av_refstruct_ref_c(const void *obj)
{
    return av_refstruct_ref((void *)obj);
}

void av_refstruct_replace(void *dstp, const void *src)
{
    const void *dst;
    memcpy(&dst, dstp, sizeof(dst));
    if (src == dst)
        return;
    av_refstruct_unref(dstp);
    if (src) {
        dst = av_refstruct_ref_c(src);
        memcpy(dstp, &dst, sizeof(dst));
    }
}

AVRefStructPool *av_refstruct_pool_alloc(size_t size, unsigned flags)
{
    AVRefStructPool *pool = av_mallocz(sizeof(*pool));
    if (!pool)
        return NULL;
    pool->size = size;
    pool->flags = flags;
    atomic_init(&pool->refcount, 1);
    ff_mutex_init(&pool->mutex);
    return pool;
}

void *av_refstruct_pool_get(AVRefStructPool *pool)
{
    void *obj = NULL;
    ff_mutex_lock(&pool->mutex);
    if (pool->nb_free)
        obj = pool->free_list[--pool->nb_free];
    ff_mutex_unlock(&pool->mutex);
    if (!obj) {
        obj = av_refstruct_alloc_ext(pool->size, 0, NULL, NULL);
        if (!obj)
            return NULL;
    } else if (pool->flags & AV_REFSTRUCT_POOL_FLAG_ZERO_EVERY_TIME) {
        memset(obj, 0, pool->size);
    }
    get_refcount(obj)->pool = pool;
    atomic_fetch_add_explicit(&pool->refcount, 1, memory_order_relaxed);
    return obj;
}

void av_refstruct_pool_uninit(AVRefStructPool **poolp)
{
    AVRefStructPool *pool = *poolp;
    if (!pool)
        return;
    *poolp = NULL;
    ff_mutex_lock(&pool->mutex);
    pool->uninited = 1;
    for (int i = 0; i < pool->nb_free; i++)
        av_free(get_refcount(pool->free_list[i]));
    pool->nb_free = 0;
    ff_mutex_unlock(&pool->mutex);
    pool_unref(pool);
}

enum AVPixelFormat ff_get_format(AVCodecContext *avctx, const enum AVPixelFormat *fmt)
{
    return fmt[0];
}

int ff_set_sar(AVCodecContext *avctx, AVRational sar)
{
    int valid = 1;
    if (sar.den <= 0 || sar.num < 0) {
        valid = 0;
    } else if (sar.num && sar.num != sar.den) {
        int64_t scaled = sar.num < sar.den ? (int64_t)avctx->width * sar.num / sar.den
                                           : (int64_t)avctx->height * sar.den / sar.num;
        valid = scaled > 0;
    }
    avctx->sample_aspect_ratio = valid ? sar : (AVRational){ 0, 1 };
    return valid ? 0 : AVERROR(EINVAL);
}

#include "ffbase.h"
#include "h264_threads.h"
#include "h264dec.h"
#include "h264_ffamp.h"

#ifndef _WIN32
#include <unistd.h>
#endif

typedef struct ThreadFrameProgress {
    atomic_int progress[2];
} ThreadFrameProgress;

enum { STATE_INPUT_READY, STATE_SETTING_UP, STATE_SETUP_FINISHED };

typedef struct FramePool {
    FFMutex mutex;
    atomic_int refs;
    size_t size;
    uint8_t **free_list;
    int nb_free, free_alloc;
} FramePool;

typedef struct SlicePool {
    int count;
    FFThread *threads;
    FFMutex mutex;
    FFCond work_cond, done_cond;
    int (*func)(AVCodecContext *c, void *arg);
    AVCodecContext *avctx;
    char *arg;
    int *ret;
    int size, jobs, next_job, running, generation, die;
} SlicePool;

typedef struct Worker {
    struct FFampH264 *parent;
    FFThread thread;
    int started;
    FFMutex mutex, progress_mutex;
    FFCond input_cond, progress_cond, output_cond;
    AVCodecContext *avctx;
    uint8_t *pkt;
    unsigned pkt_alloc;
    int pkt_size;
    int64_t pkt_pts;
    int draining;
    AVFrame **frames;
    int nb_frames, frames_alloc;
    int result;
    atomic_int state;
    int die;
} Worker;

struct FFampH264 {
    int count;
    Worker *workers;
    Worker *prev;
    int next_decoding, next_finished;
    AVCodecContext *single;
    FramePool *pool;
    SlicePool *slices;
    uint8_t *extradata;
};

int ff_cpu_count(void)
{
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return (int)info.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#endif
}

#ifdef _WIN32
typedef struct ThreadStart {
    void *(*run)(void *);
    void *arg;
} ThreadStart;

static DWORD WINAPI thread_trampoline(LPVOID p)
{
    ThreadStart start = *(ThreadStart *)p;
    av_free(p);
    start.run(start.arg);
    return 0;
}

int ff_thread_start(FFThread *thread, void *(*run)(void *), void *arg)
{
    ThreadStart *start = av_malloc(sizeof(*start));
    if (!start)
        return AVERROR(ENOMEM);
    start->run = run;
    start->arg = arg;
    *thread = CreateThread(NULL, 0, thread_trampoline, start, 0, NULL);
    if (!*thread) {
        av_free(start);
        return AVERROR(EAGAIN);
    }
    return 0;
}

void ff_thread_join(FFThread thread)
{
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
}
#else
int ff_thread_start(FFThread *thread, void *(*run)(void *), void *arg)
{
    return pthread_create(thread, NULL, run, arg) ? AVERROR(EAGAIN) : 0;
}

void ff_thread_join(FFThread thread)
{
    pthread_join(thread, NULL);
}
#endif

int ff_thread_once(AVOnce *once, void (*init)(void))
{
    if (atomic_load_explicit(&once->state, memory_order_acquire) == 2)
        return 0;
    int expected = 0;
    if (atomic_compare_exchange_strong(&once->state, &expected, 1)) {
        init();
        atomic_store_explicit(&once->state, 2, memory_order_release);
        return 0;
    }
    while (atomic_load_explicit(&once->state, memory_order_acquire) != 2) {
#ifdef _WIN32
        SwitchToThread();
#else
        sched_yield();
#endif
    }
    return 0;
}

static void pool_unref(FramePool *pool)
{
    if (atomic_fetch_sub_explicit(&pool->refs, 1, memory_order_acq_rel) == 1) {
        for (int i = 0; i < pool->nb_free; i++)
            av_free(pool->free_list[i]);
        av_free(pool->free_list);
        ff_mutex_destroy(&pool->mutex);
        av_free(pool);
    }
}

static FramePool *pool_alloc(void)
{
    FramePool *pool = av_mallocz(sizeof(*pool));
    if (!pool)
        return NULL;
    ff_mutex_init(&pool->mutex);
    atomic_init(&pool->refs, 1);
    return pool;
}

static void pool_release(void *opaque, uint8_t *data)
{
    FramePool *pool = opaque;
    size_t size;
    memcpy(&size, data - 64, sizeof(size));
    ff_mutex_lock(&pool->mutex);
    if (size == pool->size && pool->nb_free < 64) {
        if (pool->nb_free == pool->free_alloc) {
            int n = pool->free_alloc ? pool->free_alloc * 2 : 16;
            uint8_t **list = av_realloc_array(pool->free_list, n, sizeof(*list));
            if (list) {
                pool->free_list = list;
                pool->free_alloc = n;
            }
        }
        if (pool->nb_free < pool->free_alloc) {
            pool->free_list[pool->nb_free++] = data - 64;
            data = NULL;
        }
    }
    ff_mutex_unlock(&pool->mutex);
    if (data)
        av_free(data - 64);
    pool_unref(pool);
}

static int fill_value(void)
{
    static atomic_int fill = -2;
    int v = atomic_load_explicit(&fill, memory_order_relaxed);
    if (v == -2) {
        const char *env = getenv("FFAMP_H264_FILL");
        v = env ? (atoi(env) & 255) : -1;
        atomic_store_explicit(&fill, v, memory_order_relaxed);
    }
    return v;
}

int ff_get_buffer(AVCodecContext *avctx, AVFrame *frame, int flags)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(avctx->pix_fmt);
    FramePool *pool = avctx->internal->thread_ctx ? ((Worker *)avctx->internal->thread_ctx)->parent->pool : NULL;
    int w, h, bytes, cw, ch;
    size_t luma, chroma, total;
    uint8_t *mem = NULL;

    if (!desc)
        return AVERROR(EINVAL);
    if (!pool)
        pool = ((FFampH264 *)avctx->opaque)->pool;
    w = FFMAX(avctx->width, avctx->coded_width);
    h = FFMAX(avctx->height, avctx->coded_height);
    if (av_image_check_size(w, h, 0, avctx) < 0)
        return AVERROR(EINVAL);
    bytes = desc->comp[0].depth > 8 ? 2 : 1;
    frame->width = w;
    frame->height = h;
    frame->format = avctx->pix_fmt;
    w = FFALIGN(w, 32);
    h = FFALIGN(h, 32);
    cw = w >> desc->log2_chroma_w;
    ch = h >> desc->log2_chroma_h;
    frame->linesize[0] = FFALIGN(w * bytes, 64);
    frame->linesize[1] = frame->linesize[2] = FFALIGN(cw * bytes, 64);
    luma = (size_t)frame->linesize[0] * h + 64;
    chroma = (size_t)frame->linesize[1] * ch + 64;
    total = luma + 2 * chroma + 64;

    ff_mutex_lock(&pool->mutex);
    if (pool->size != total) {
        for (int i = 0; i < pool->nb_free; i++)
            av_free(pool->free_list[i]);
        pool->nb_free = 0;
        pool->size = total;
    }
    if (pool->nb_free)
        mem = pool->free_list[--pool->nb_free];
    ff_mutex_unlock(&pool->mutex);
    if (!mem) {
        mem = av_malloc(total + 64);
        if (!mem)
            return AVERROR(ENOMEM);
        memcpy(mem, &total, sizeof(total));
    }
    frame->buf[0] = av_buffer_create(mem + 64, total, pool_release, pool, 0);
    if (!frame->buf[0]) {
        av_free(mem);
        return AVERROR(ENOMEM);
    }
    atomic_fetch_add_explicit(&pool->refs, 1, memory_order_relaxed);
    frame->data[0] = mem + 64;
    frame->data[1] = frame->data[0] + luma;
    frame->data[2] = frame->data[1] + chroma;
    if (fill_value() >= 0)
        memset(mem + 64, fill_value(), total);
    frame->pts = avctx->internal->pkt_pts;
    frame->color_range = avctx->color_range;
    frame->color_primaries = avctx->color_primaries;
    frame->color_trc = avctx->color_trc;
    frame->colorspace = avctx->colorspace;
    return 0;
}

int ff_thread_get_ext_buffer(AVCodecContext *avctx, ThreadFrame *f, int flags)
{
    int ret;
    f->owner[0] = f->owner[1] = avctx;
    if (!(avctx->active_thread_type & FF_THREAD_FRAME))
        return ff_get_buffer(avctx, f->f, flags);
    f->progress = av_refstruct_allocz(sizeof(*f->progress));
    if (!f->progress)
        return AVERROR(ENOMEM);
    atomic_init(&f->progress->progress[0], -1);
    atomic_init(&f->progress->progress[1], -1);
    ret = ff_get_buffer(avctx, f->f, flags);
    if (ret)
        av_refstruct_unref(&f->progress);
    return ret;
}

void ff_thread_release_ext_buffer(ThreadFrame *f)
{
    av_refstruct_unref(&f->progress);
    f->owner[0] = f->owner[1] = NULL;
    if (f->f)
        av_frame_unref(f->f);
}

int ff_thread_ref_frame(ThreadFrame *dst, const ThreadFrame *src)
{
    int ret;
    dst->owner[0] = src->owner[0];
    dst->owner[1] = src->owner[1];
    ret = av_frame_ref(dst->f, src->f);
    if (ret < 0)
        return ret;
    if (src->progress)
        dst->progress = av_refstruct_ref(src->progress);
    return 0;
}

int ff_thread_replace_frame(ThreadFrame *dst, const ThreadFrame *src)
{
    int ret;
    dst->owner[0] = src->owner[0];
    dst->owner[1] = src->owner[1];
    ret = av_frame_replace(dst->f, src->f);
    if (ret < 0)
        return ret;
    av_refstruct_replace(&dst->progress, src->progress);
    return 0;
}

void ff_thread_report_progress(ThreadFrame *f, int n, int field)
{
    atomic_int *progress = f->progress ? f->progress->progress : NULL;
    Worker *p;
    if (!progress || atomic_load_explicit(&progress[field], memory_order_relaxed) >= n)
        return;
    p = f->owner[field]->internal->thread_ctx;
    ff_mutex_lock(&p->progress_mutex);
    atomic_store_explicit(&progress[field], n, memory_order_release);
    ff_cond_broadcast(&p->progress_cond);
    ff_mutex_unlock(&p->progress_mutex);
}

void ff_thread_await_progress(const ThreadFrame *f, int n, int field)
{
    atomic_int *progress = f->progress ? f->progress->progress : NULL;
    Worker *p;
    if (!progress || atomic_load_explicit(&progress[field], memory_order_acquire) >= n)
        return;
    p = f->owner[field]->internal->thread_ctx;
    ff_mutex_lock(&p->progress_mutex);
    while (atomic_load_explicit(&progress[field], memory_order_relaxed) < n)
        ff_cond_wait(&p->progress_cond, &p->progress_mutex);
    ff_mutex_unlock(&p->progress_mutex);
}

void ff_thread_finish_setup(AVCodecContext *avctx)
{
    Worker *p;
    if (!(avctx->active_thread_type & FF_THREAD_FRAME))
        return;
    p = avctx->internal->thread_ctx;
    ff_mutex_lock(&p->progress_mutex);
    atomic_store(&p->state, STATE_SETUP_FINISHED);
    ff_cond_broadcast(&p->progress_cond);
    ff_mutex_unlock(&p->progress_mutex);
}

int ff_thread_can_start_frame(AVCodecContext *avctx)
{
    if (avctx->active_thread_type & FF_THREAD_FRAME) {
        Worker *p = avctx->internal->thread_ctx;
        if (atomic_load(&p->state) != STATE_SETTING_UP)
            return 0;
    }
    return 1;
}

static void *slice_worker(void *arg)
{
    SlicePool *s = arg;
    int generation = 0;
    ff_mutex_lock(&s->mutex);
    for (;;) {
        while (!s->die && s->generation == generation)
            ff_cond_wait(&s->work_cond, &s->mutex);
        if (s->die)
            break;
        generation = s->generation;
        while (s->next_job < s->jobs) {
            int job = s->next_job++;
            ff_mutex_unlock(&s->mutex);
            int r = s->func(s->avctx, s->arg + (size_t)job * s->size);
            ff_mutex_lock(&s->mutex);
            if (s->ret)
                s->ret[job] = r;
        }
        if (--s->running == 0)
            ff_cond_signal(&s->done_cond);
    }
    ff_mutex_unlock(&s->mutex);
    return NULL;
}

static int execute_serial(AVCodecContext *c, int (*func)(AVCodecContext *c2, void *arg), void *arg, int *ret, int count, int size)
{
    for (int i = 0; i < count; i++) {
        int r = func(c, (char *)arg + (size_t)i * size);
        if (ret)
            ret[i] = r;
    }
    return 0;
}

static int execute_threads(AVCodecContext *c, int (*func)(AVCodecContext *c2, void *arg), void *arg, int *ret, int count, int size)
{
    SlicePool *s = ((FFampH264 *)c->opaque)->slices;
    if (!s || count <= 1)
        return execute_serial(c, func, arg, ret, count, size);
    ff_mutex_lock(&s->mutex);
    s->func = func;
    s->avctx = c;
    s->arg = arg;
    s->ret = ret;
    s->size = size;
    s->jobs = count;
    s->next_job = 0;
    s->running = s->count;
    s->generation++;
    ff_cond_broadcast(&s->work_cond);
    while (s->next_job < s->jobs) {
        int job = s->next_job++;
        ff_mutex_unlock(&s->mutex);
        int r = func(c, (char *)arg + (size_t)job * size);
        ff_mutex_lock(&s->mutex);
        if (ret)
            ret[job] = r;
    }
    while (s->running)
        ff_cond_wait(&s->done_cond, &s->mutex);
    ff_mutex_unlock(&s->mutex);
    return 0;
}

static void slices_free(SlicePool *s)
{
    if (!s)
        return;
    ff_mutex_lock(&s->mutex);
    s->die = 1;
    ff_cond_broadcast(&s->work_cond);
    ff_mutex_unlock(&s->mutex);
    for (int i = 0; i < s->count; i++)
        ff_thread_join(s->threads[i]);
    av_free(s->threads);
    ff_cond_destroy(&s->work_cond);
    ff_cond_destroy(&s->done_cond);
    ff_mutex_destroy(&s->mutex);
    av_free(s);
}

static SlicePool *slices_alloc(int helpers)
{
    SlicePool *s = av_mallocz(sizeof(*s));
    if (!s)
        return NULL;
    ff_mutex_init(&s->mutex);
    ff_cond_init(&s->work_cond);
    ff_cond_init(&s->done_cond);
    s->threads = av_calloc(helpers, sizeof(*s->threads));
    if (!s->threads) {
        slices_free(s);
        return NULL;
    }
    for (int i = 0; i < helpers; i++) {
        if (ff_thread_start(&s->threads[i], slice_worker, s) < 0)
            break;
        s->count++;
    }
    return s;
}

static AVCodecContext *context_alloc(FFampH264 *dec, int threads, int type)
{
    AVCodecContext *avctx = av_mallocz(sizeof(*avctx));
    if (!avctx)
        return NULL;
    avctx->internal = av_mallocz(sizeof(*avctx->internal));
    avctx->priv_data = av_mallocz(sizeof(H264Context));
    if (!avctx->internal || !avctx->priv_data) {
        av_free(avctx->internal);
        av_free(avctx->priv_data);
        av_free(avctx);
        return NULL;
    }
    avctx->opaque = dec;
    avctx->codec_id = AV_CODEC_ID_H264;
    avctx->pix_fmt = AV_PIX_FMT_NONE;
    avctx->error_concealment = FF_EC_GUESS_MVS | FF_EC_DEBLOCK;
    avctx->workaround_bugs = FF_BUG_AUTODETECT;
    avctx->thread_count = threads;
    avctx->active_thread_type = type;
    avctx->sample_aspect_ratio = (AVRational){ 0, 1 };
    avctx->framerate = (AVRational){ 0, 1 };
    avctx->color_primaries = AVCOL_PRI_UNSPECIFIED;
    avctx->color_trc = AVCOL_TRC_UNSPECIFIED;
    avctx->colorspace = AVCOL_SPC_UNSPECIFIED;
    avctx->profile = AV_PROFILE_UNKNOWN;
    avctx->level = -99;
    avctx->extradata = dec->extradata;
    avctx->internal->pkt_pts = AV_NOPTS_VALUE;
    avctx->execute = (type & FF_THREAD_SLICE) ? execute_threads : execute_serial;
    return avctx;
}

static void context_free(AVCodecContext *avctx)
{
    if (!avctx)
        return;
    ff_h264_decode_end(avctx);
    av_free(avctx->priv_data);
    av_free(avctx->internal);
    av_free(avctx);
}

static void update_from_thread(AVCodecContext *dst, const AVCodecContext *src, int *err)
{
    dst->framerate = src->framerate;
    dst->width = src->width;
    dst->height = src->height;
    dst->pix_fmt = src->pix_fmt;
    dst->coded_width = src->coded_width;
    dst->coded_height = src->coded_height;
    dst->has_b_frames = src->has_b_frames;
    dst->sample_aspect_ratio = src->sample_aspect_ratio;
    dst->profile = src->profile;
    dst->level = src->level;
    dst->bits_per_raw_sample = src->bits_per_raw_sample;
    dst->color_primaries = src->color_primaries;
    dst->color_trc = src->color_trc;
    dst->colorspace = src->colorspace;
    dst->color_range = src->color_range;
    dst->chroma_sample_location = src->chroma_sample_location;
    *err = ff_h264_update_thread_context(dst, src);
}

static int push_frame(Worker *p, AVFrame *f)
{
    if (p->nb_frames == p->frames_alloc) {
        int n = p->frames_alloc ? p->frames_alloc * 2 : 4;
        AVFrame **list = av_realloc_array(p->frames, n, sizeof(*list));
        if (!list)
            return AVERROR(ENOMEM);
        p->frames = list;
        p->frames_alloc = n;
    }
    p->frames[p->nb_frames++] = f;
    return 0;
}

static void worker_decode(Worker *p)
{
    AVCodecContext *avctx = p->avctx;
    avctx->internal->pkt_pts = p->pkt_pts;
    for (;;) {
        AVFrame *f = av_frame_alloc();
        int got = 0, ret;
        if (!f) {
            p->result = AVERROR(ENOMEM);
            return;
        }
        ret = ff_h264_decode_frame(avctx, f, &got, p->draining ? NULL : p->pkt, p->draining ? 0 : p->pkt_size);
        if (got) {
            if (push_frame(p, f) < 0)
                av_frame_free(&f);
            avctx->frame_num++;
        } else {
            av_frame_free(&f);
        }
        p->result = ret < 0 ? ret : 0;
        if (!p->draining || !got || ret < 0)
            return;
    }
}

static void *worker_main(void *arg)
{
    Worker *p = arg;
    ff_mutex_lock(&p->mutex);
    for (;;) {
        while (atomic_load(&p->state) == STATE_INPUT_READY && !p->die)
            ff_cond_wait(&p->input_cond, &p->mutex);
        if (p->die)
            break;
        worker_decode(p);
        if (atomic_load(&p->state) == STATE_SETTING_UP)
            ff_thread_finish_setup(p->avctx);
        ff_mutex_lock(&p->progress_mutex);
        atomic_store(&p->state, STATE_INPUT_READY);
        ff_cond_broadcast(&p->progress_cond);
        ff_cond_signal(&p->output_cond);
        ff_mutex_unlock(&p->progress_mutex);
    }
    ff_mutex_unlock(&p->mutex);
    return NULL;
}

static void wait_idle(Worker *p)
{
    if (atomic_load(&p->state) != STATE_INPUT_READY) {
        ff_mutex_lock(&p->progress_mutex);
        while (atomic_load(&p->state) != STATE_INPUT_READY)
            ff_cond_wait(&p->output_cond, &p->progress_mutex);
        ff_mutex_unlock(&p->progress_mutex);
    }
}

static int submit(FFampH264 *dec, const uint8_t *data, int size, int64_t pts)
{
    Worker *p = &dec->workers[dec->next_decoding];
    Worker *prev = dec->prev;
    int err = 0;

    ff_mutex_lock(&p->mutex);
    if (size > 0) {
        av_fast_padded_malloc(&p->pkt, &p->pkt_alloc, size);
        if (!p->pkt) {
            ff_mutex_unlock(&p->mutex);
            return AVERROR(ENOMEM);
        }
        memcpy(p->pkt, data, size);
        memset(p->pkt + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    }
    p->pkt_size = size;
    p->pkt_pts = pts;
    p->draining = size <= 0;
    if (prev) {
        if (atomic_load(&prev->state) == STATE_SETTING_UP) {
            ff_mutex_lock(&prev->progress_mutex);
            while (atomic_load(&prev->state) == STATE_SETTING_UP)
                ff_cond_wait(&prev->progress_cond, &prev->progress_mutex);
            ff_mutex_unlock(&prev->progress_mutex);
        }
        update_from_thread(p->avctx, prev->avctx, &err);
        if (err) {
            ff_mutex_unlock(&p->mutex);
            return err;
        }
    }
    atomic_store(&p->state, STATE_SETTING_UP);
    ff_cond_signal(&p->input_cond);
    ff_mutex_unlock(&p->mutex);
    dec->prev = p;
    dec->next_decoding = (dec->next_decoding + 1) % dec->count;
    return 0;
}

static void deliver(AVFrame *f, FFampH264Sink sink, void *opaque)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(f->format);
    FFampH264Frame out;
    size_t offsets[3];
    int bytes, min_align = INT_MAX, crop_align;
    size_t left = f->crop_left, right = f->crop_right, top = f->crop_top, bottom = f->crop_bottom;

    if (!desc)
        return;
    if (left >= INT_MAX - right || top >= INT_MAX - bottom || left + right >= (size_t)f->width || top + bottom >= (size_t)f->height)
        left = right = top = bottom = 0;
    bytes = desc->comp[0].depth > 8 ? 2 : 1;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < 3; i++) {
            int sx = i ? desc->log2_chroma_w : 0, sy = i ? desc->log2_chroma_h : 0;
            offsets[i] = (top >> sy) * f->linesize[i] + (left >> sx) * bytes;
        }
        if (pass)
            break;
        crop_align = left ? ff_ctz((int)left) : INT_MAX;
        for (int i = 0; i < 3; i++)
            min_align = FFMIN(min_align, offsets[i] ? ff_ctzll((long long)offsets[i]) : INT_MAX);
        if (crop_align < min_align)
            return;
        if (min_align < 5 && crop_align != INT_MAX)
            left &= ~(((size_t)1 << (5 + crop_align - min_align)) - 1);
        else
            break;
    }
    memset(&out, 0, sizeof(out));
    for (int i = 0; i < 3; i++) {
        out.data[i] = f->data[i] + offsets[i];
        out.linesize[i] = f->linesize[i];
    }
    out.width = f->width - (int)(left + right);
    out.height = f->height - (int)(top + bottom);
    out.chroma_shift_x = desc->log2_chroma_w;
    out.chroma_shift_y = desc->log2_chroma_h;
    out.bits = desc->comp[0].depth;
    out.rgb = !!(desc->flags & AV_PIX_FMT_FLAG_RGB);
    out.key = !!(f->flags & AV_FRAME_FLAG_KEY);
    out.interlaced = !!(f->flags & AV_FRAME_FLAG_INTERLACED);
    out.top_field_first = !!(f->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST);
    out.corrupt = !!(f->flags & AV_FRAME_FLAG_CORRUPT) || f->decode_error_flags;
    out.pts = f->pts;
    out.full_range = f->color_range == AVCOL_RANGE_JPEG;
    out.primaries = f->color_primaries;
    out.transfer = f->color_trc;
    out.matrix = f->colorspace;
    sink(opaque, &out);
}

static int collect(FFampH264 *dec, FFampH264Sink sink, void *opaque)
{
    Worker *p = &dec->workers[dec->next_finished];
    int result;
    dec->next_finished = (dec->next_finished + 1) % dec->count;
    wait_idle(p);
    for (int i = 0; i < p->nb_frames; i++) {
        if (sink)
            deliver(p->frames[i], sink, opaque);
        av_frame_free(&p->frames[i]);
    }
    p->nb_frames = 0;
    result = p->result;
    p->result = 0;
    return result;
}

static int single_decode(FFampH264 *dec, const uint8_t *data, int size, int64_t pts, FFampH264Sink sink, void *opaque)
{
    AVCodecContext *avctx = dec->single;
    int last = 0;
    uint8_t *copy = NULL;
    unsigned copy_size = 0;
    if (size > 0) {
        av_fast_padded_malloc(&copy, &copy_size, size);
        if (!copy)
            return AVERROR(ENOMEM);
        memcpy(copy, data, size);
    }
    avctx->internal->pkt_pts = pts;
    for (;;) {
        AVFrame *f = av_frame_alloc();
        int got = 0, ret;
        if (!f) {
            av_free(copy);
            return AVERROR(ENOMEM);
        }
        ret = ff_h264_decode_frame(avctx, f, &got, copy, size > 0 ? size : 0);
        if (got) {
            avctx->frame_num++;
            if (sink)
                deliver(f, sink, opaque);
        }
        av_frame_free(&f);
        last = ret < 0 ? ret : 0;
        if (size > 0 || !got || ret < 0)
            break;
    }
    av_free(copy);
    return last;
}

FFampH264 *ffamp_h264_open(const uint8_t *extradata, int extradata_size, int threads, int thread_type)
{
    FFampH264 *dec = av_mallocz(sizeof(*dec));
    if (!dec)
        return NULL;
    dec->pool = pool_alloc();
    if (!dec->pool)
        goto fail;
    if (extradata_size > 0) {
        dec->extradata = av_mallocz(extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!dec->extradata)
            goto fail;
        memcpy(dec->extradata, extradata, extradata_size);
    }
    if (threads <= 0)
        threads = FFMIN(ff_cpu_count() + 1, 16);
    if (threads <= 1 || thread_type == FFAMP_H264_SLICE_THREADS) {
        int type = threads > 1 ? FF_THREAD_SLICE : 0;
        if (type) {
            dec->slices = slices_alloc(threads - 1);
            if (!dec->slices)
                goto fail;
        }
        dec->count = 1;
        dec->single = context_alloc(dec, threads, type);
        if (!dec->single)
            goto fail;
        dec->single->extradata_size = extradata_size;
        if (ff_h264_decode_init(dec->single) < 0)
            goto fail;
        return dec;
    }
    dec->count = threads;
    dec->workers = av_calloc(threads, sizeof(*dec->workers));
    if (!dec->workers)
        goto fail;
    for (int i = 0; i < threads; i++) {
        Worker *p = &dec->workers[i];
        p->parent = dec;
        ff_mutex_init(&p->mutex);
        ff_mutex_init(&p->progress_mutex);
        ff_cond_init(&p->input_cond);
        ff_cond_init(&p->progress_cond);
        ff_cond_init(&p->output_cond);
        atomic_init(&p->state, STATE_INPUT_READY);
        p->avctx = context_alloc(dec, threads, FF_THREAD_FRAME);
        if (!p->avctx)
            goto fail;
        p->avctx->internal->thread_ctx = p;
        p->avctx->internal->is_copy = i > 0;
        p->avctx->extradata_size = extradata_size;
        if (ff_h264_decode_init(p->avctx) < 0)
            goto fail;
        if (ff_thread_start(&p->thread, worker_main, p) < 0)
            goto fail;
        p->started = 1;
    }
    return dec;
fail:
    ffamp_h264_close(dec);
    return NULL;
}

int ffamp_h264_decode(FFampH264 *dec, const uint8_t *data, int size, int64_t pts, FFampH264Sink sink, void *opaque)
{
    int ret;
    if (size <= 0)
        return 0;
    if (dec->single)
        return single_decode(dec, data, size, pts, sink, opaque);
    ret = submit(dec, data, size, pts);
    if (ret < 0)
        return ret;
    if (dec->next_decoding != dec->next_finished)
        return 0;
    return collect(dec, sink, opaque);
}

int ffamp_h264_drain(FFampH264 *dec, FFampH264Sink sink, void *opaque)
{
    int ret, last = 0, target;
    if (dec->single)
        return single_decode(dec, NULL, 0, AV_NOPTS_VALUE, sink, opaque);
    if (!dec->prev)
        return 0;
    target = dec->next_decoding;
    ret = submit(dec, NULL, 0, AV_NOPTS_VALUE);
    if (ret < 0)
        target = (dec->next_decoding + dec->count - 1) % dec->count;
    do {
        int done = dec->next_finished;
        ret = collect(dec, sink, opaque);
        if (ret < 0)
            last = ret;
        if (done == target)
            break;
    } while (1);
    return last;
}

void ffamp_h264_flush(FFampH264 *dec)
{
    if (!dec)
        return;
    if (dec->single) {
        ff_h264_decode_flush(dec->single);
        return;
    }
    for (int i = 0; i < dec->count; i++)
        wait_idle(&dec->workers[i]);
    if (dec->prev && dec->prev != &dec->workers[0]) {
        int err;
        update_from_thread(dec->workers[0].avctx, dec->prev->avctx, &err);
    }
    dec->next_decoding = dec->next_finished = 0;
    dec->prev = NULL;
    for (int i = 0; i < dec->count; i++) {
        Worker *p = &dec->workers[i];
        for (int j = 0; j < p->nb_frames; j++)
            av_frame_free(&p->frames[j]);
        p->nb_frames = 0;
        p->result = 0;
        ff_h264_decode_flush(p->avctx);
    }
}

void ffamp_h264_close(FFampH264 *dec)
{
    if (!dec)
        return;
    if (dec->workers) {
        for (int i = 0; i < dec->count; i++)
            if (dec->workers[i].started)
                wait_idle(&dec->workers[i]);
        for (int i = 0; i < dec->count; i++) {
            Worker *p = &dec->workers[i];
            if (p->started) {
                ff_mutex_lock(&p->mutex);
                p->die = 1;
                ff_cond_signal(&p->input_cond);
                ff_mutex_unlock(&p->mutex);
                ff_thread_join(p->thread);
            }
        }
        for (int i = 0; i < dec->count; i++) {
            Worker *p = &dec->workers[i];
            context_free(p->avctx);
            for (int j = 0; j < p->nb_frames; j++)
                av_frame_free(&p->frames[j]);
            av_free(p->frames);
            av_free(p->pkt);
            ff_mutex_destroy(&p->mutex);
            ff_mutex_destroy(&p->progress_mutex);
            ff_cond_destroy(&p->input_cond);
            ff_cond_destroy(&p->progress_cond);
            ff_cond_destroy(&p->output_cond);
        }
        av_free(dec->workers);
    }
    context_free(dec->single);
    slices_free(dec->slices);
    if (dec->pool)
        pool_unref(dec->pool);
    av_free(dec->extradata);
    av_free(dec);
}

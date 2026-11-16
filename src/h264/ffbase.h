#ifndef FFAMP_H264_FFBASE_H
#define FFAMP_H264_FFBASE_H

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__x86_64__) || defined(_M_X64)
#define FFAMP_X86_64 1
#else
#define FFAMP_X86_64 0
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
#define FFAMP_AARCH64 1
#else
#define FFAMP_AARCH64 0
#endif

#ifndef FFAMP_H264_ASM
#define FFAMP_H264_ASM 0
#endif

#define ARCH_X86 (FFAMP_X86_64 && FFAMP_H264_ASM)
#define ARCH_X86_64 ARCH_X86
#define ARCH_X86_32 0
#define ARCH_AARCH64 (FFAMP_AARCH64 && FFAMP_H264_ASM)
#define ARCH_ARM 0
#define ARCH_LOONGARCH 0
#define ARCH_LOONGARCH64 0
#define ARCH_MIPS 0
#define ARCH_PPC 0
#define ARCH_RISCV 0
#define HAVE_X86ASM ARCH_X86
#define HAVE_MMX ARCH_X86
#define HAVE_MMXEXT ARCH_X86
#define HAVE_SSE2 ARCH_X86
#define HAVE_SSSE3 ARCH_X86
#define HAVE_AVX ARCH_X86
#define HAVE_AVX2 ARCH_X86
#define HAVE_NEON ARCH_AARCH64
#define HAVE_INLINE_ASM 0
#define HAVE_THREADS 1
#define HAVE_BIGENDIAN 0
#define HAVE_FAST_UNALIGNED 1
#define HAVE_FAST_64BIT (FFAMP_X86_64 || FFAMP_AARCH64)
#define HAVE_FAST_CLZ 1
#define CONFIG_ERROR_RESILIENCE 1
#define CONFIG_GRAY 0
#define CONFIG_SMALL 0
#define CONFIG_SAFE_BITSTREAM_READER 1
#define CONFIG_MPEGVIDEODEC 0
#define CONFIG_H264_SEI 1
#define CONFIG_HEVC_SEI 0
#define CONFIG_VVC_SEI 0
#define CONFIG_H264_DXVA2_HWACCEL 0
#define CONFIG_H264_D3D11VA_HWACCEL 0
#define CONFIG_H264_D3D11VA2_HWACCEL 0
#define CONFIG_H264_D3D12VA_HWACCEL 0
#define CONFIG_H264_NVDEC_HWACCEL 0
#define CONFIG_H264_VAAPI_HWACCEL 0
#define CONFIG_H264_VDPAU_HWACCEL 0
#define CONFIG_H264_VIDEOTOOLBOX_HWACCEL 0
#define CONFIG_H264_VULKAN_HWACCEL 0
#define EXTERN extern

#define av_always_inline __attribute__((always_inline)) inline
#define av_noinline __attribute__((noinline))
#define av_pure __attribute__((pure))
#define av_const __attribute__((const))
#define av_cold __attribute__((cold))
#define av_flatten __attribute__((flatten))
#define av_unused __attribute__((unused))
#define av_used __attribute__((used))
#define av_uninit(x) x = x
#define av_builtin_constant_p __builtin_constant_p
#define av_noreturn __attribute__((noreturn))
#define av_fallthrough __attribute__((fallthrough))
#define av_unreachable(msg) __builtin_unreachable()
#define av_likely(x) __builtin_expect(!!(x), 1)
#define av_unlikely(x) __builtin_expect(!!(x), 0)
#define attribute_align_arg
#define av_restrict restrict

#define DECLARE_ALIGNED(n, t, v) t __attribute__((aligned(n))) v
#define DECLARE_ASM_ALIGNED(n, t, v) t av_used __attribute__((aligned(n))) v
#define DECLARE_ASM_CONST(n, t, v) static const t av_used __attribute__((aligned(n))) v
#define FF_E1(x) x
#define LOCAL_ALIGNED_D(a, t, v, s, o, ...) DECLARE_ALIGNED(a, t, la_##v) s o; t (*v) o = la_##v
#define LOCAL_ALIGNED(a, t, v, ...) FF_E1(LOCAL_ALIGNED_D(a, t, v, __VA_ARGS__,,))
#define LOCAL_ALIGNED_16(t, v, ...) LOCAL_ALIGNED(16, t, v, __VA_ARGS__)
#define LOCAL_ALIGNED_32(t, v, ...) LOCAL_ALIGNED(32, t, v, __VA_ARGS__)

#define SUINT unsigned
#define SUINT32 uint32_t
typedef struct AVClass AVClass;
typedef void (*qpel_mc_func)(uint8_t *dst, const uint8_t *src, ptrdiff_t stride);
struct ThreadProgress;
static inline void ff_thread_progress_await(const struct ThreadProgress *p, int n) { }

#define AV_STRINGIFY(s) AV_TOSTRING(s)
#define AV_TOSTRING(s) #s
#define AV_GLUE(a, b) a##b
#define AV_JOIN(a, b) AV_GLUE(a, b)

#define MKTAG(a, b, c, d) ((unsigned)(a) | ((unsigned)(b) << 8) | ((unsigned)(c) << 16) | ((unsigned)(d) << 24))
#define MKBETAG(a, b, c, d) ((unsigned)(d) | ((unsigned)(c) << 8) | ((unsigned)(b) << 16) | ((unsigned)(a) << 24))
#define FFERRTAG(a, b, c, d) (-(int)MKTAG(a, b, c, d))
#define AVERROR(e) (-(e))
#define AVERROR_BUG FFERRTAG('B', 'U', 'G', '!')
#define AVERROR_EOF FFERRTAG('E', 'O', 'F', ' ')
#define AVERROR_INVALIDDATA FFERRTAG('I', 'N', 'D', 'A')
#define AVERROR_PATCHWELCOME FFERRTAG('P', 'A', 'W', 'E')
#define AVERROR_UNKNOWN FFERRTAG('U', 'N', 'K', 'N')
#define av_err2str(e) "error"

#define FFABS(a) ((a) >= 0 ? (a) : (-(a)))
#define FFSIGN(a) ((a) > 0 ? 1 : -1)
#define FFMAX(a, b) ((a) > (b) ? (a) : (b))
#define FFMAX3(a, b, c) FFMAX(FFMAX(a, b), c)
#define FFMIN(a, b) ((a) > (b) ? (b) : (a))
#define FFMIN3(a, b, c) FFMIN(FFMIN(a, b), c)
#define FFSWAP(type, a, b) do { type SWAP_tmp = b; b = a; a = SWAP_tmp; } while (0)
#define FF_ARRAY_ELEMS(a) (sizeof(a) / sizeof((a)[0]))
#define FFALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define AV_CEIL_RSHIFT(a, b) (!av_builtin_constant_p(b) ? -((-(a)) >> (b)) : ((a) + (1 << (b)) - 1) >> (b))
#define FF_CEIL_RSHIFT AV_CEIL_RSHIFT
#define FFUDIV(a, b) (((a) > 0 ? (a) : (a) - (b) + 1) / (b))
#define FFUMOD(a, b) ((a) - (b) * FFUDIV(a, b))
#define ROUNDED_DIV(a, b) (((a) >= 0 ? (a) + ((b) >> 1) : (a) - ((b) >> 1)) / (b))
#define AV_NOPTS_VALUE ((int64_t)UINT64_C(0x8000000000000000))

static av_always_inline av_const int av_log2(unsigned v) { return 31 - __builtin_clz(v | 1); }
static av_always_inline av_const int av_log2_16bit(unsigned v) { return 31 - __builtin_clz((v & 0xffff) | 1); }
static av_always_inline av_const int ff_ctz(int v) { return __builtin_ctz(v); }
static av_always_inline av_const int ff_ctzll(long long v) { return __builtin_ctzll(v); }
static av_always_inline av_const unsigned ff_clz(unsigned v) { return __builtin_clz(v); }
static av_always_inline av_const int av_ceil_log2(int x) { return av_log2((x - 1U) << 1); }
static av_always_inline av_const int av_popcount(uint32_t x) { return __builtin_popcount(x); }

static av_always_inline av_const int av_clip(int a, int amin, int amax)
{
    if (a < amin) return amin;
    else if (a > amax) return amax;
    else return a;
}
static av_always_inline av_const int64_t av_clip64(int64_t a, int64_t amin, int64_t amax)
{
    if (a < amin) return amin;
    else if (a > amax) return amax;
    else return a;
}
static av_always_inline av_const uint8_t av_clip_uint8(int a)
{
    if (a & (~0xFF)) return (~a) >> 31;
    else return a;
}
static av_always_inline av_const int8_t av_clip_int8(int a)
{
    if ((a + 0x80U) & ~0xFF) return (a >> 31) ^ 0x7F;
    else return a;
}
static av_always_inline av_const uint16_t av_clip_uint16(int a)
{
    if (a & (~0xFFFF)) return (~a) >> 31;
    else return a;
}
static av_always_inline av_const int16_t av_clip_int16(int a)
{
    if ((a + 0x8000U) & ~0xFFFF) return (a >> 31) ^ 0x7FFF;
    else return a;
}
static av_always_inline av_const int32_t av_clipl_int32(int64_t a)
{
    if ((a + UINT64_C(0x80000000)) & ~UINT64_C(0xFFFFFFFF)) return (int32_t)((a >> 63) ^ 0x7FFFFFFF);
    else return (int32_t)a;
}
static av_always_inline av_const int av_clip_intp2(int a, int p)
{
    if (((unsigned)a + (1U << p)) & ~((2U << p) - 1)) return (a >> 31) ^ ((1 << p) - 1);
    else return a;
}
static av_always_inline av_const unsigned av_clip_uintp2(int a, int p)
{
    if (a & ~((1U << p) - 1)) return (~a) >> 31 & ((1U << p) - 1);
    else return a;
}
static av_always_inline av_const unsigned av_zero_extend(unsigned a, unsigned p)
{
    return a & ((1U << p) - 1);
}
static av_always_inline av_const unsigned av_mod_uintp2(unsigned a, unsigned p)
{
    return a & ((1U << p) - 1);
}

static av_always_inline uint16_t av_bswap16(uint16_t x) { return (uint16_t)((x >> 8) | (x << 8)); }
static av_always_inline uint32_t av_bswap32(uint32_t x) { return __builtin_bswap32(x); }
static av_always_inline uint64_t av_bswap64(uint64_t x) { return __builtin_bswap64(x); }
#define av_be2ne16(x) av_bswap16(x)
#define av_be2ne32(x) av_bswap32(x)
#define av_be2ne64(x) av_bswap64(x)
#define av_le2ne16(x) (x)
#define av_le2ne32(x) (x)
#define av_le2ne64(x) (x)

static av_always_inline uint16_t ff_rn16(const void *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static av_always_inline uint32_t ff_rn32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static av_always_inline uint64_t ff_rn64(const void *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static av_always_inline void ff_wn16(void *p, uint16_t v) { memcpy(p, &v, 2); }
static av_always_inline void ff_wn32(void *p, uint32_t v) { memcpy(p, &v, 4); }
static av_always_inline void ff_wn64(void *p, uint64_t v) { memcpy(p, &v, 8); }

#define AV_RN16(p) ff_rn16(p)
#define AV_RN32(p) ff_rn32(p)
#define AV_RN64(p) ff_rn64(p)
#define AV_WN16(p, v) ff_wn16(p, (uint16_t)(v))
#define AV_WN32(p, v) ff_wn32(p, (uint32_t)(v))
#define AV_WN64(p, v) ff_wn64(p, (uint64_t)(v))
#define AV_RN16A(p) (*(const uint16_t *)(const void *)(p))
#define AV_RN32A(p) (*(const uint32_t *)(const void *)(p))
#define AV_RN64A(p) (*(const uint64_t *)(const void *)(p))
#define AV_WN16A(p, v) (*(uint16_t *)(void *)(p) = (uint16_t)(v))
#define AV_WN32A(p, v) (*(uint32_t *)(void *)(p) = (uint32_t)(v))
#define AV_WN64A(p, v) (*(uint64_t *)(void *)(p) = (uint64_t)(v))
#define AV_RB16(p) av_bswap16(ff_rn16(p))
#define AV_RB32(p) av_bswap32(ff_rn32(p))
#define AV_RB64(p) av_bswap64(ff_rn64(p))
#define AV_RL16(p) ff_rn16(p)
#define AV_RL32(p) ff_rn32(p)
#define AV_RL64(p) ff_rn64(p)
#define AV_RB24(p) ((((const uint8_t *)(p))[0] << 16) | (((const uint8_t *)(p))[1] << 8) | ((const uint8_t *)(p))[2])
#define AV_RL24(p) ((((const uint8_t *)(p))[2] << 16) | (((const uint8_t *)(p))[1] << 8) | ((const uint8_t *)(p))[0])
#define AV_RB8(p) (*(const uint8_t *)(p))
#define AV_WB16(p, v) ff_wn16(p, av_bswap16((uint16_t)(v)))
#define AV_WB32(p, v) ff_wn32(p, av_bswap32((uint32_t)(v)))
#define AV_WL16(p, v) ff_wn16(p, (uint16_t)(v))
#define AV_WL32(p, v) ff_wn32(p, (uint32_t)(v))
#define AV_RB(s, p) AV_RB##s(p)
#define AV_RL(s, p) AV_RL##s(p)
#define AV_COPY16(d, s) AV_WN16A(d, AV_RN16A(s))
#define AV_COPY32(d, s) AV_WN32A(d, AV_RN32A(s))
#define AV_COPY64(d, s) AV_WN64A(d, AV_RN64A(s))
#define AV_COPY128(d, s) do { AV_COPY64(d, s); AV_COPY64((char *)(d) + 8, (const char *)(s) + 8); } while (0)
#define AV_COPY16U(d, s) AV_WN16(d, AV_RN16(s))
#define AV_COPY32U(d, s) AV_WN32(d, AV_RN32(s))
#define AV_COPY64U(d, s) AV_WN64(d, AV_RN64(s))
#define AV_COPY128U(d, s) do { AV_COPY64U(d, s); AV_COPY64U((char *)(d) + 8, (const char *)(s) + 8); } while (0)
#define AV_SWAP64(a, b) do { uint64_t swap_tmp = AV_RN64A(a); AV_WN64A(a, AV_RN64A(b)); AV_WN64A(b, swap_tmp); } while (0)
#define AV_ZERO16(d) AV_WN16A(d, 0)
#define AV_ZERO32(d) AV_WN32A(d, 0)
#define AV_ZERO64(d) AV_WN64A(d, 0)
#define AV_ZERO128(d) do { AV_ZERO64(d); AV_ZERO64((char *)(d) + 8); } while (0)

#define av_assert0(cond) do { if (!(cond)) abort(); } while (0)
#define av_assert1(cond) ((void)0)
#define av_assert2(cond) ((void)0)

#define AV_LOG_QUIET -8
#define AV_LOG_PANIC 0
#define AV_LOG_FATAL 8
#define AV_LOG_ERROR 16
#define AV_LOG_WARNING 24
#define AV_LOG_INFO 32
#define AV_LOG_VERBOSE 40
#define AV_LOG_DEBUG 48
#define AV_LOG_TRACE 56
void av_log(void *avcl, int level, const char *fmt, ...);
#define av_log_once(ctx, first, later, state, ...) av_log(ctx, first, __VA_ARGS__)
#define ff_dlog(ctx, ...) do { } while (0)
#define ff_tlog(ctx, ...) do { } while (0)
#define avpriv_request_sample(ctx, ...) av_log(ctx, AV_LOG_WARNING, __VA_ARGS__)
#define avpriv_report_missing_feature(ctx, ...) av_log(ctx, AV_LOG_WARNING, __VA_ARGS__)

void *av_malloc(size_t size);
void *av_mallocz(size_t size);
void *av_calloc(size_t nmemb, size_t size);
void *av_malloc_array(size_t nmemb, size_t size);
void *av_realloc(void *ptr, size_t size);
void *av_realloc_array(void *ptr, size_t nmemb, size_t size);
void *av_realloc_f(void *ptr, size_t nelem, size_t elsize);
int av_reallocp_array(void *ptr, size_t nmemb, size_t size);
void *av_fast_realloc(void *ptr, unsigned int *size, size_t min_size);
void av_fast_malloc(void *ptr, unsigned int *size, size_t min_size);
void av_fast_mallocz(void *ptr, unsigned int *size, size_t min_size);
void av_fast_padded_malloc(void *ptr, unsigned int *size, size_t min_size);
void *av_memdup(const void *p, size_t size);
void av_free(void *ptr);
void av_freep(void *ptr);
void av_memcpy_backptr(uint8_t *dst, int back, int cnt);
#define FF_ALLOC_TYPED_ARRAY(p, nelem) (p = av_malloc_array(nelem, sizeof(*p)))
#define FF_ALLOCZ_TYPED_ARRAY(p, nelem) (p = av_calloc(nelem, sizeof(*p)))

#define AV_INPUT_BUFFER_PADDING_SIZE 64
#define AV_NUM_DATA_POINTERS 8

typedef struct AVRational { int num, den; } AVRational;
static inline AVRational av_make_q(int num, int den) { AVRational r = { num, den }; return r; }
static inline int av_cmp_q(AVRational a, AVRational b)
{
    const int64_t tmp = a.num * (int64_t)b.den - b.num * (int64_t)a.den;
    if (tmp) return (int)((tmp ^ a.den ^ b.den) >> 63) | 1;
    else if (b.den && a.den) return 0;
    else if (a.num && b.num) return (a.num >> 31) - (b.num >> 31);
    else return INT_MIN;
}
static inline double av_q2d(AVRational a) { return a.num / (double)a.den; }
int av_reduce(int *dst_num, int *dst_den, int64_t num, int64_t den, int64_t max);

enum AVCodecID { AV_CODEC_ID_NONE, AV_CODEC_ID_H264, AV_CODEC_ID_HEVC, AV_CODEC_ID_VVC, AV_CODEC_ID_LCEVC, AV_CODEC_ID_MPEG2VIDEO, AV_CODEC_ID_SVQ3, AV_CODEC_ID_RV40, AV_CODEC_ID_VP7, AV_CODEC_ID_VP8 };

enum AVPictureType {
    AV_PICTURE_TYPE_NONE = 0, AV_PICTURE_TYPE_I, AV_PICTURE_TYPE_P, AV_PICTURE_TYPE_B,
    AV_PICTURE_TYPE_S, AV_PICTURE_TYPE_SI, AV_PICTURE_TYPE_SP, AV_PICTURE_TYPE_BI,
};
static inline char av_get_picture_type_char(enum AVPictureType t)
{
    return t >= 0 && t <= AV_PICTURE_TYPE_BI ? "?IPBSisb"[t] : '?';
}

enum AVPixelFormat {
    AV_PIX_FMT_NONE = -1,
    AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUV444P,
    AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_YUVJ422P, AV_PIX_FMT_YUVJ444P,
    AV_PIX_FMT_YUV420P9, AV_PIX_FMT_YUV422P9, AV_PIX_FMT_YUV444P9,
    AV_PIX_FMT_YUV420P10, AV_PIX_FMT_YUV422P10, AV_PIX_FMT_YUV444P10,
    AV_PIX_FMT_YUV420P12, AV_PIX_FMT_YUV422P12, AV_PIX_FMT_YUV444P12,
    AV_PIX_FMT_YUV420P14, AV_PIX_FMT_YUV422P14, AV_PIX_FMT_YUV444P14,
    AV_PIX_FMT_GBRP, AV_PIX_FMT_GBRP9, AV_PIX_FMT_GBRP10, AV_PIX_FMT_GBRP12, AV_PIX_FMT_GBRP14,
    AV_PIX_FMT_GRAY8,
    AV_PIX_FMT_NB
};

#define AV_PIX_FMT_FLAG_PLANAR (1 << 4)
#define AV_PIX_FMT_FLAG_RGB (1 << 5)
typedef struct AVComponentDescriptor { int plane, step, offset, shift, depth; } AVComponentDescriptor;
typedef struct AVPixFmtDescriptor {
    const char *name;
    uint8_t nb_components;
    uint8_t log2_chroma_w;
    uint8_t log2_chroma_h;
    uint64_t flags;
    AVComponentDescriptor comp[4];
} AVPixFmtDescriptor;
const AVPixFmtDescriptor *av_pix_fmt_desc_get(enum AVPixelFormat pix_fmt);
int av_pix_fmt_get_chroma_sub_sample(enum AVPixelFormat pix_fmt, int *h_shift, int *v_shift);
const char *av_get_pix_fmt_name(enum AVPixelFormat pix_fmt);
void av_image_copy(uint8_t * const dst_data[4], const int dst_linesizes[4],
                   const uint8_t * const src_data[4], const int src_linesizes[4],
                   enum AVPixelFormat pix_fmt, int width, int height);
int av_image_check_size(unsigned int w, unsigned int h, int log_offset, void *log_ctx);

enum AVColorPrimaries {
    AVCOL_PRI_RESERVED0 = 0, AVCOL_PRI_BT709 = 1, AVCOL_PRI_UNSPECIFIED = 2, AVCOL_PRI_RESERVED = 3,
    AVCOL_PRI_BT470M = 4, AVCOL_PRI_BT470BG = 5, AVCOL_PRI_SMPTE170M = 6, AVCOL_PRI_SMPTE240M = 7,
    AVCOL_PRI_FILM = 8, AVCOL_PRI_BT2020 = 9, AVCOL_PRI_SMPTE428 = 10, AVCOL_PRI_SMPTE431 = 11,
    AVCOL_PRI_SMPTE432 = 12, AVCOL_PRI_EBU3213 = 22, AVCOL_PRI_NB
};
enum AVColorTransferCharacteristic {
    AVCOL_TRC_RESERVED0 = 0, AVCOL_TRC_BT709 = 1, AVCOL_TRC_UNSPECIFIED = 2, AVCOL_TRC_RESERVED = 3,
    AVCOL_TRC_GAMMA22 = 4, AVCOL_TRC_GAMMA28 = 5, AVCOL_TRC_SMPTE170M = 6, AVCOL_TRC_SMPTE240M = 7,
    AVCOL_TRC_LINEAR = 8, AVCOL_TRC_LOG = 9, AVCOL_TRC_LOG_SQRT = 10, AVCOL_TRC_IEC61966_2_4 = 11,
    AVCOL_TRC_BT1361_ECG = 12, AVCOL_TRC_IEC61966_2_1 = 13, AVCOL_TRC_BT2020_10 = 14, AVCOL_TRC_BT2020_12 = 15,
    AVCOL_TRC_SMPTE2084 = 16, AVCOL_TRC_SMPTE428 = 17, AVCOL_TRC_ARIB_STD_B67 = 18, AVCOL_TRC_NB
};
enum AVColorSpace {
    AVCOL_SPC_RGB = 0, AVCOL_SPC_BT709 = 1, AVCOL_SPC_UNSPECIFIED = 2, AVCOL_SPC_RESERVED = 3,
    AVCOL_SPC_FCC = 4, AVCOL_SPC_BT470BG = 5, AVCOL_SPC_SMPTE170M = 6, AVCOL_SPC_SMPTE240M = 7,
    AVCOL_SPC_YCGCO = 8, AVCOL_SPC_BT2020_NCL = 9, AVCOL_SPC_BT2020_CL = 10, AVCOL_SPC_SMPTE2085 = 11,
    AVCOL_SPC_CHROMA_DERIVED_NCL = 12, AVCOL_SPC_CHROMA_DERIVED_CL = 13, AVCOL_SPC_ICTCP = 14,
    AVCOL_SPC_IPT_C2 = 15, AVCOL_SPC_YCGCO_RE = 16, AVCOL_SPC_YCGCO_RO = 17, AVCOL_SPC_NB
};
enum AVColorRange { AVCOL_RANGE_UNSPECIFIED = 0, AVCOL_RANGE_MPEG = 1, AVCOL_RANGE_JPEG = 2 };
enum AVChromaLocation {
    AVCHROMA_LOC_UNSPECIFIED = 0, AVCHROMA_LOC_LEFT = 1, AVCHROMA_LOC_CENTER = 2, AVCHROMA_LOC_TOPLEFT = 3,
    AVCHROMA_LOC_TOP = 4, AVCHROMA_LOC_BOTTOMLEFT = 5, AVCHROMA_LOC_BOTTOM = 6
};
const char *av_color_primaries_name(enum AVColorPrimaries primaries);
const char *av_color_transfer_name(enum AVColorTransferCharacteristic transfer);
const char *av_color_space_name(enum AVColorSpace space);

enum AVDiscard {
    AVDISCARD_NONE = -16, AVDISCARD_DEFAULT = 0, AVDISCARD_NONREF = 8, AVDISCARD_BIDIR = 16,
    AVDISCARD_NONINTRA = 24, AVDISCARD_NONKEY = 32, AVDISCARD_ALL = 48,
};

#define AV_EF_CRCCHECK (1 << 0)
#define AV_EF_BITSTREAM (1 << 1)
#define AV_EF_BUFFER (1 << 2)
#define AV_EF_EXPLODE (1 << 3)
#define AV_EF_IGNORE_ERR (1 << 15)
#define AV_EF_CAREFUL (1 << 16)
#define AV_EF_COMPLIANT (1 << 17)
#define AV_EF_AGGRESSIVE (1 << 18)
#define FF_COMPLIANCE_VERY_STRICT 2
#define FF_COMPLIANCE_STRICT 1
#define FF_COMPLIANCE_NORMAL 0
#define FF_BUG_AUTODETECT 1
#define FF_BUG_TRUNCATED 0
#define FF_EC_GUESS_MVS 1
#define FF_EC_DEBLOCK 2
#define FF_EC_FAVOR_INTER 256
#define FF_DEBUG_PICT_INFO 1
#define FF_DEBUG_MMCO 0x00000800
#define FF_DEBUG_ER 0x00000400
#define FF_DEBUG_GREEN_MD 0x00800000
#define FF_DECODE_ERROR_INVALID_BITSTREAM 1
#define FF_DECODE_ERROR_MISSING_REFERENCE 2
#define FF_DECODE_ERROR_CONCEALMENT_ACTIVE 4
#define FF_DECODE_ERROR_DECODE_SLICES 8
#define FF_THREAD_FRAME 1
#define FF_THREAD_SLICE 2
#define AV_CODEC_FLAG_UNALIGNED (1 << 0)
#define AV_CODEC_FLAG_OUTPUT_CORRUPT (1 << 3)
#define AV_CODEC_FLAG_GRAY (1 << 13)
#define AV_CODEC_FLAG2_FAST (1 << 0)
#define AV_CODEC_FLAG2_CHUNKS (1 << 15)
#define AV_CODEC_FLAG2_IGNORE_CROP (1 << 16)
#define AV_CODEC_FLAG2_SHOW_ALL (1 << 22)
#define AV_CODEC_EXPORT_DATA_VIDEO_ENC_PARAMS (1 << 2)
#define AV_CODEC_EXPORT_DATA_FILM_GRAIN (1 << 3)
#define AV_GET_BUFFER_FLAG_REF (1 << 0)
#define AV_FRAME_FLAG_CORRUPT (1 << 0)
#define AV_FRAME_FLAG_KEY (1 << 1)
#define AV_FRAME_FLAG_DISCARD (1 << 2)
#define AV_FRAME_FLAG_INTERLACED (1 << 3)
#define AV_FRAME_FLAG_TOP_FIELD_FIRST (1 << 4)

#define AV_PROFILE_UNKNOWN -99
#define AV_PROFILE_H264_CONSTRAINED (1 << 9)
#define AV_PROFILE_H264_INTRA (1 << 11)
#define AV_PROFILE_H264_BASELINE 66
#define AV_PROFILE_H264_HIGH_10 110
#define AV_PROFILE_H264_HIGH_422 122
#define AV_PROFILE_H264_HIGH_444_PREDICTIVE 244

typedef struct AVBuffer AVBuffer;
typedef struct AVBufferRef {
    AVBuffer *buffer;
    uint8_t *data;
    size_t size;
} AVBufferRef;
AVBufferRef *av_buffer_alloc(size_t size);
AVBufferRef *av_buffer_create(uint8_t *data, size_t size, void (*free)(void *opaque, uint8_t *data), void *opaque, int flags);
AVBufferRef *av_buffer_ref(const AVBufferRef *buf);
void av_buffer_unref(AVBufferRef **buf);
int av_buffer_is_writable(const AVBufferRef *buf);
int av_buffer_get_ref_count(const AVBufferRef *buf);
int av_buffer_replace(AVBufferRef **dst, const AVBufferRef *src);
void av_buffer_default_free(void *opaque, uint8_t *data);

typedef struct AVFrame {
    uint8_t *data[AV_NUM_DATA_POINTERS];
    int linesize[AV_NUM_DATA_POINTERS];
    AVBufferRef *buf[AV_NUM_DATA_POINTERS];
    int width, height;
    int format;
    int flags;
    enum AVPictureType pict_type;
    int decode_error_flags;
    int repeat_pict;
    int64_t pts;
    size_t crop_top, crop_bottom, crop_left, crop_right;
    int color_range, color_primaries, color_trc, colorspace;
} AVFrame;
AVFrame *av_frame_alloc(void);
void av_frame_free(AVFrame **frame);
int av_frame_ref(AVFrame *dst, const AVFrame *src);
void av_frame_unref(AVFrame *frame);
void av_frame_move_ref(AVFrame *dst, AVFrame *src);
int av_frame_replace(AVFrame *dst, const AVFrame *src);

#define AV_REFSTRUCT_FLAG_NO_ZEROING (1 << 0)
#define AV_REFSTRUCT_POOL_FLAG_ZERO_EVERY_TIME (1 << 4)
typedef struct AVRefStructPool AVRefStructPool;
typedef union { void *nc; const void *c; } AVRefStructOpaque;
void *av_refstruct_alloc_ext(size_t size, unsigned flags, void *opaque, void (*free_cb)(AVRefStructOpaque opaque, void *obj));
void *av_refstruct_allocz(size_t size);
void av_refstruct_unref(void *objp);
void *av_refstruct_ref(void *obj);
const void *av_refstruct_ref_c(const void *obj);
void av_refstruct_replace(void *dstp, const void *src);
AVRefStructPool *av_refstruct_pool_alloc(size_t size, unsigned flags);
void *av_refstruct_pool_get(AVRefStructPool *pool);
void av_refstruct_pool_uninit(AVRefStructPool **poolp);

typedef struct AVCodecInternal {
    int is_copy;
    int draining;
    void *thread_ctx;
    int64_t pkt_pts;
} AVCodecInternal;

typedef struct AVCodecContext AVCodecContext;
struct AVCodecContext {
    void *priv_data;
    void *opaque;
    AVCodecInternal *internal;
    enum AVCodecID codec_id;
    unsigned int codec_tag;
    int width, height;
    int coded_width, coded_height;
    enum AVPixelFormat pix_fmt;
    int has_b_frames;
    int flags, flags2;
    int err_recognition;
    int error_concealment;
    int workaround_bugs;
    int strict_std_compliance;
    int debug;
    int thread_count;
    int active_thread_type;
    enum AVDiscard skip_frame;
    enum AVDiscard skip_loop_filter;
    enum AVDiscard skip_idct;
    int skip_top, skip_bottom;
    int lowres;
    int export_side_data;
    int slice_flags;
    int refs;
    int profile, level;
    int bits_per_raw_sample;
    int frame_num;
    const uint8_t *extradata;
    int extradata_size;
    AVRational sample_aspect_ratio;
    AVRational framerate;
    enum AVColorPrimaries color_primaries;
    enum AVColorTransferCharacteristic color_trc;
    enum AVColorSpace colorspace;
    enum AVColorRange color_range;
    enum AVChromaLocation chroma_sample_location;
    void *hwaccel;
    void (*draw_horiz_band)(void);
    int (*execute)(AVCodecContext *c, int (*func)(AVCodecContext *c2, void *arg), void *arg2, int *ret, int count, int size);
};

#define FF_HW_HAS_CB(avctx, function) 0
#define FF_HW_CALL(avctx, function, ...) 0
#define FF_HW_SIMPLE_CALL(avctx, function) 0

int ff_get_buffer(AVCodecContext *avctx, AVFrame *frame, int flags);
enum AVPixelFormat ff_get_format(AVCodecContext *avctx, const enum AVPixelFormat *fmt);
int ff_set_sar(AVCodecContext *avctx, AVRational sar);

struct ThreadFrameProgress;
typedef struct ThreadFrame {
    AVFrame *f;
    AVCodecContext *owner[2];
    struct ThreadFrameProgress *progress;
} ThreadFrame;
void ff_thread_report_progress(ThreadFrame *f, int progress, int field);
void ff_thread_await_progress(const ThreadFrame *f, int progress, int field);
int ff_thread_get_ext_buffer(AVCodecContext *avctx, ThreadFrame *f, int flags);
void ff_thread_release_ext_buffer(ThreadFrame *f);
int ff_thread_ref_frame(ThreadFrame *dst, const ThreadFrame *src);
int ff_thread_replace_frame(ThreadFrame *dst, const ThreadFrame *src);
void ff_thread_finish_setup(AVCodecContext *avctx);
int ff_thread_can_start_frame(AVCodecContext *avctx);
typedef struct AVOnce { atomic_int state; } AVOnce;
#define AV_ONCE_INIT { 0 }
int ff_thread_once(AVOnce *once, void (*init)(void));

static inline void emms_c(void) { }

typedef struct GetByteContext {
    const uint8_t *buffer, *buffer_end, *buffer_start;
} GetByteContext;
static av_always_inline void bytestream2_init(GetByteContext *g, const uint8_t *buf, int buf_size)
{
    g->buffer = buf;
    g->buffer_start = buf;
    g->buffer_end = buf + buf_size;
}
static av_always_inline int bytestream2_get_bytes_left(const GetByteContext *g) { return (int)(g->buffer_end - g->buffer); }
static av_always_inline int bytestream2_tell(const GetByteContext *g) { return (int)(g->buffer - g->buffer_start); }
static av_always_inline void bytestream2_skip(GetByteContext *g, unsigned size) { g->buffer += FFMIN(g->buffer_end - g->buffer, size); }
static av_always_inline void bytestream2_skipu(GetByteContext *g, unsigned size) { g->buffer += size; }
static av_always_inline unsigned bytestream2_get_byteu(GetByteContext *g) { return *g->buffer++; }
static av_always_inline unsigned bytestream2_peek_byteu(const GetByteContext *g) { return *g->buffer; }
static av_always_inline unsigned bytestream2_get_byte(GetByteContext *g)
{
    if (g->buffer_end - g->buffer < 1) { g->buffer = g->buffer_end; return 0; }
    return *g->buffer++;
}
static av_always_inline unsigned bytestream2_get_be16u(GetByteContext *g) { unsigned v = AV_RB16(g->buffer); g->buffer += 2; return v; }
static av_always_inline unsigned bytestream2_get_be32u(GetByteContext *g) { unsigned v = AV_RB32(g->buffer); g->buffer += 4; return v; }
static av_always_inline unsigned bytestream2_get_be16(GetByteContext *g)
{
    if (g->buffer_end - g->buffer < 2) { g->buffer = g->buffer_end; return 0; }
    return bytestream2_get_be16u(g);
}
static av_always_inline unsigned bytestream2_get_be24(GetByteContext *g)
{
    if (g->buffer_end - g->buffer < 3) { g->buffer = g->buffer_end; return 0; }
    unsigned v = AV_RB24(g->buffer); g->buffer += 3; return v;
}
static av_always_inline unsigned bytestream2_get_be32(GetByteContext *g)
{
    if (g->buffer_end - g->buffer < 4) { g->buffer = g->buffer_end; return 0; }
    return bytestream2_get_be32u(g);
}
static av_always_inline unsigned bytestream2_peek_ne16(const GetByteContext *g)
{
    if (g->buffer_end - g->buffer < 2) return 0;
    return AV_RN16(g->buffer);
}
static av_always_inline unsigned bytestream2_peek_be24(const GetByteContext *g)
{
    if (g->buffer_end - g->buffer < 3) return 0;
    return AV_RB24(g->buffer);
}
static av_always_inline unsigned bytestream2_peek_be32(const GetByteContext *g)
{
    if (g->buffer_end - g->buffer < 4) return 0;
    return AV_RB32(g->buffer);
}
typedef struct PutByteContext {
    uint8_t *buffer, *buffer_end, *buffer_start;
    int eof;
} PutByteContext;
static av_always_inline void bytestream2_init_writer(PutByteContext *p, uint8_t *buf, int buf_size)
{
    p->buffer = buf;
    p->buffer_start = buf;
    p->buffer_end = buf + buf_size;
    p->eof = 0;
}
static av_always_inline void bytestream2_put_byte(PutByteContext *p, unsigned v)
{
    if (!p->eof && p->buffer_end - p->buffer >= 1) *p->buffer++ = (uint8_t)v;
    else p->eof = 1;
}
static av_always_inline void bytestream2_put_be24(PutByteContext *p, unsigned v)
{
    if (!p->eof && p->buffer_end - p->buffer >= 3) {
        p->buffer[0] = (uint8_t)(v >> 16);
        p->buffer[1] = (uint8_t)(v >> 8);
        p->buffer[2] = (uint8_t)v;
        p->buffer += 3;
    } else {
        p->eof = 1;
    }
}
static av_always_inline int bytestream2_tell_p(const PutByteContext *p) { return (int)(p->buffer - p->buffer_start); }
static av_always_inline unsigned bytestream2_get_bufferu(GetByteContext *g, uint8_t *dst, unsigned size)
{
    memcpy(dst, g->buffer, size);
    g->buffer += size;
    return size;
}

#endif

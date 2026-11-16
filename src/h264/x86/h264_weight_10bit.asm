%include "x86util.asm"

SECTION_RODATA 32

sq_1: dq 1
      dq 0

cextern pw_1
cextern pw_1023
%define pw_pixel_max pw_1023

SECTION .text

%macro WEIGHT_PROLOGUE 0
.prologue:
    PROLOGUE 0,6,8
    movifnidn  r0, r0mp
    movifnidn r1d, r1m
    movifnidn r2d, r2m
    movifnidn r4d, r4m
    movifnidn r5d, r5m
%endmacro

%macro WEIGHT_SETUP 0
    mova       m0, [pw_1]
    movd       m2, r3m
    pslld      m0, m2
    SPLATW     m0, m0
    shl        r5, 19
    lea        r5, [r5+r4*2+0x10000]
    movd       m3, r5d
    pshufd     m3, m3, 0
    mova       m4, [pw_pixel_max]
    paddw      m2, [sq_1]
%if notcpuflag(sse4)
    pxor       m7, m7
%endif
%endmacro

%macro WEIGHT_OP 1-2
%if %0==1
    mova        m5, [r0+%1]
    punpckhwd   m6, m5, m0
    punpcklwd   m5, m0
%else
    movq        m5, [r0+%1]
    movq        m6, [r0+%2]
    punpcklwd   m5, m0
    punpcklwd   m6, m0
%endif
    pmaddwd     m5, m3
    pmaddwd     m6, m3
    psrad       m5, m2
    psrad       m6, m2
%if cpuflag(sse4)
    packusdw    m5, m6
    pminsw      m5, m4
%else
    packssdw    m5, m6
    CLIPW       m5, m7, m4
%endif
%endmacro

%macro WEIGHT_FUNC_DBL 0
cglobal h264_weight_16_10
    WEIGHT_PROLOGUE
    WEIGHT_SETUP
.nextrow:
    WEIGHT_OP  0
    mova [r0   ], m5
    WEIGHT_OP 16
    mova [r0+16], m5
    add       r0, r1
    dec       r2d
    jnz .nextrow
    RET
%endmacro

INIT_XMM sse2
WEIGHT_FUNC_DBL
INIT_XMM sse4
WEIGHT_FUNC_DBL

%macro WEIGHT_FUNC_MM 0
cglobal h264_weight_8_10
    WEIGHT_PROLOGUE
    WEIGHT_SETUP
.nextrow:
    WEIGHT_OP   0
    mova     [r0], m5
    add        r0, r1
    dec        r2d
    jnz .nextrow
    RET
%endmacro

INIT_XMM sse2
WEIGHT_FUNC_MM
INIT_XMM sse4
WEIGHT_FUNC_MM

%macro WEIGHT_FUNC_HALF_MM 0
cglobal h264_weight_4_10
    WEIGHT_PROLOGUE
    sar         r2d, 1
    WEIGHT_SETUP
    lea         r3, [r1*2]
.nextrow:
    WEIGHT_OP    0, r1
    movh      [r0], m5
    movhps [r0+r1], m5
    add         r0, r3
    dec         r2d
    jnz .nextrow
    RET
%endmacro

INIT_XMM sse2
WEIGHT_FUNC_HALF_MM
INIT_XMM sse4
WEIGHT_FUNC_HALF_MM

%if ARCH_X86_32
DECLARE_REG_TMP 3
%else
DECLARE_REG_TMP 7
%endif

%macro BIWEIGHT_PROLOGUE 0
.prologue:
    PROLOGUE 0,8,8
    movifnidn  r0, r0mp
    movifnidn  r1, r1mp
    movifnidn r2d, r2m
    movifnidn r5d, r5m
    movifnidn r6d, r6m
    movifnidn t0d, r7m
%endmacro

%macro BIWEIGHT_SETUP 0
    lea        t0, [t0*4+1]
    or         t0, 1
    shl        r6, 16
    or         r5, r6
    movd       m4, r5d
    movd       m5, t0d
    movd       m6, r4m
    pslld      m5, m6
    paddd      m6, [sq_1]
    pshufd     m4, m4, 0
    pshufd     m5, m5, 0
    mova       m3, [pw_pixel_max]
    movifnidn r3d, r3m
%if notcpuflag(sse4)
    pxor       m7, m7
%endif
%endmacro

%macro BIWEIGHT 1-2
%if %0==1
    mova       m0, [r0+%1]
    mova       m1, [r1+%1]
    punpckhwd  m2, m0, m1
    punpcklwd  m0, m1
%else
    movq       m0, [r0+%1]
    movq       m1, [r1+%1]
    punpcklwd  m0, m1
    movq       m2, [r0+%2]
    movq       m1, [r1+%2]
    punpcklwd  m2, m1
%endif
    pmaddwd    m0, m4
    pmaddwd    m2, m4
    paddd      m0, m5
    paddd      m2, m5
    psrad      m0, m6
    psrad      m2, m6
%if cpuflag(sse4)
    packusdw   m0, m2
    pminsw     m0, m3
%else
    packssdw   m0, m2
    CLIPW      m0, m7, m3
%endif
%endmacro

%macro BIWEIGHT_FUNC_DBL 0
cglobal h264_biweight_16_10
    BIWEIGHT_PROLOGUE
    BIWEIGHT_SETUP
.nextrow:
    BIWEIGHT   0
    mova [r0   ], m0
    BIWEIGHT  16
    mova [r0+16], m0
    add       r0, r2
    add       r1, r2
    dec       r3d
    jnz .nextrow
    RET
%endmacro

INIT_XMM sse2
BIWEIGHT_FUNC_DBL
INIT_XMM sse4
BIWEIGHT_FUNC_DBL

%macro BIWEIGHT_FUNC 0
cglobal h264_biweight_8_10
    BIWEIGHT_PROLOGUE
    BIWEIGHT_SETUP
.nextrow:
    BIWEIGHT  0
    mova   [r0], m0
    add      r0, r2
    add      r1, r2
    dec      r3d
    jnz .nextrow
    RET
%endmacro

INIT_XMM sse2
BIWEIGHT_FUNC
INIT_XMM sse4
BIWEIGHT_FUNC

%macro BIWEIGHT_FUNC_HALF 0
cglobal h264_biweight_4_10
    BIWEIGHT_PROLOGUE
    BIWEIGHT_SETUP
    sar        r3d, 1
    lea        r4, [r2*2]
.nextrow:
    BIWEIGHT     0, r2
    movh   [r0   ], m0
    movhps [r0+r2], m0
    add         r0, r4
    add         r1, r4
    dec         r3d
    jnz .nextrow
    RET
%endmacro

INIT_XMM sse2
BIWEIGHT_FUNC_HALF
INIT_XMM sse4
BIWEIGHT_FUNC_HALF

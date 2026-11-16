%include "x86util.asm"

SECTION .text

cextern pw_2
cextern pw_3
cextern pw_4
cextern pw_1023
%define pw_pixel_max pw_1023

%macro ABS_SUB 5
    psubusw %5, %2, %1
    psubusw %4, %1, %2
    por     %4, %5
    psubw   %4, %3
%endmacro

%macro DIFF_LT   5
    psubusw %4, %2, %1
    psubusw %5, %1, %2
    por     %5, %4
    pxor    %4, %4
    psubw   %5, %3
    pcmpgtw %4, %5
%endmacro

%macro LOAD_AB 4
    movd       %1, %3
    movd       %2, %4
    SPLATW     %1, %1
    SPLATW     %2, %2
%endmacro

%macro LOAD_TC 2
    movd        %1, [%2]
    punpcklbw   %1, %1
    pshuflw     %1, %1, 01010000b
    pshufd      %1, %1, 01010000b
    psraw       %1, 6
%endmacro

%macro LOAD_MASK 9
    ABS_SUB     %2, %3, %5, %8, %7
    ABS_SUB     %1, %2, %6, %9, %7
    pand        %8, %9
    ABS_SUB     %3, %4, %6, %9, %7
    pxor        %7, %7
    pand        %8, %9
    pcmpgtw     %7, %8
%endmacro

%macro DEBLOCK_P0_Q0 7
    psubw   %3, %4
    pxor    %7, %7
    paddw   %3, [pw_4]
    psubw   %7, %5
    psubw   %6, %2, %1
    psllw   %6, 2
    paddw   %3, %6
    psraw   %3, 3
    mova    %6, [pw_pixel_max]
    CLIPW   %3, %7, %5
    pxor    %7, %7
    paddw   %1, %3
    psubw   %2, %3
    CLIPW   %1, %7, %6
    CLIPW   %2, %7, %6
%endmacro

%macro LUMA_Q1 6
    pavgw       %6, %3, %4
    paddw       %1, %6
    pxor        %6, %6
    psraw       %1, 1
    psubw       %6, %5
    psubw       %1, %2
    CLIPW       %1, %6, %5
    paddw       %1, %2
%endmacro

%macro LUMA_DEBLOCK_ONE 3
    DIFF_LT     m5, %1, bm, m4, m6
    pxor        m6, m6
    mova        %3, m4
    pcmpgtw     m6, tcm
    pand        m4, tcm
    pandn       m6, m7
    pand        m4, m6
    LUMA_Q1 m5, %2, m1, m2, m4, m6
%endmacro

%macro LUMA_H_STORE 2
    movq        [r0-4], m0
    movhps      [r0+r1-4], m0
    movq        [r0+r1*2-4], m1
    movhps      [%1-4], m1
    movq        [%1+r1-4], m2
    movhps      [%1+r1*2-4], m2
    movq        [%1+%2-4], m3
    movhps      [%1+r1*4-4], m3
%endmacro

%macro DEBLOCK_LUMA 0

cglobal deblock_v_luma_10, 5,5,8,-5*mmsize
    %define tcm [rsp]
    %define ms1 [rsp+mmsize]
    %define ms2 [rsp+mmsize*2]
    %define am  [rsp+mmsize*3]
    %define bm  [rsp+mmsize*4]
    shl        r2d, 2
    shl        r3d, 2
    LOAD_AB     m4, m5, r2d, r3d
    mov         r3, 32/mmsize
    mov         r2, r0
    sub         r0, r1
    mova        am, m4
    sub         r0, r1
    mova        bm, m5
    sub         r0, r1
.loop:
    mova        m0, [r0+r1]
    mova        m1, [r0+r1*2]
    mova        m2, [r2]
    mova        m3, [r2+r1]

    LOAD_MASK   m0, m1, m2, m3, am, bm, m7, m4, m6
    LOAD_TC     m6, r4
    mova       tcm, m6

    mova        m5, [r0]
    LUMA_DEBLOCK_ONE m1, m0, ms1
    mova   [r0+r1], m5

    mova        m5, [r2+r1*2]
    LUMA_DEBLOCK_ONE m2, m3, ms2
    mova   [r2+r1], m5

    pxor        m5, m5
    mova        m6, tcm
    pcmpgtw     m5, tcm
    psubw       m6, ms1
    pandn       m5, m7
    psubw       m6, ms2
    pand        m5, m6
    DEBLOCK_P0_Q0 m1, m2, m0, m3, m5, m7, m6
    mova [r0+r1*2], m1
    mova      [r2], m2

    add         r0, mmsize
    add         r2, mmsize
    add         r4, mmsize/8
    dec         r3
    jg .loop
    RET

cglobal deblock_h_luma_10, 5,6,8,-7*mmsize
    %define tcm [rsp]
    %define ms1 [rsp+mmsize]
    %define ms2 [rsp+mmsize*2]
    %define p1m [rsp+mmsize*3]
    %define p2m [rsp+mmsize*4]
    %define am  [rsp+mmsize*5]
    %define bm  [rsp+mmsize*6]
    shl        r2d, 2
    shl        r3d, 2
    LOAD_AB     m4, m5, r2d, r3d
    mov         r3, r1
    mova        am, m4
    add         r3, r1
    mov         r5, 32/mmsize
    mova        bm, m5
    add         r3, r1
    mov         r2, r0
    add         r2, r3
.loop:
    movu        m5, [r0-8]
    movu        m0, [r0+r1-8]
    movu        m2, [r0+r1*2-8]
    movu        m3, [r2-8]
    TRANSPOSE4x4W 5, 0, 2, 3, 6
    mova       tcm, m3

    movu        m4, [r2+r1-8]
    movu        m1, [r2+r1*2-8]
    movu        m3, [r2+r3-8]
    movu        m7, [r2+r1*4-8]
    TRANSPOSE4x4W 4, 1, 3, 7, 6

    mova        m6, tcm
    punpcklqdq  m6, m7
    punpckhqdq  m5, m4
    SBUTTERFLY qdq, 0, 1, 7
    SBUTTERFLY qdq, 2, 3, 7

    mova       p2m, m6
    LOAD_MASK   m0, m1, m2, m3, am, bm, m7, m4, m6
    LOAD_TC     m6, r4
    mova       tcm, m6

    LUMA_DEBLOCK_ONE m1, m0, ms1
    mova       p1m, m5

    mova        m5, p2m
    LUMA_DEBLOCK_ONE m2, m3, ms2
    mova       p2m, m5

    pxor        m5, m5
    mova        m6, tcm
    pcmpgtw     m5, tcm
    psubw       m6, ms1
    pandn       m5, m7
    psubw       m6, ms2
    pand        m5, m6
    DEBLOCK_P0_Q0 m1, m2, m0, m3, m5, m7, m6
    mova        m0, p1m
    mova        m3, p2m
    TRANSPOSE4x4W 0, 1, 2, 3, 4
    LUMA_H_STORE r2, r3

    add         r4, mmsize/8
    lea         r0, [r0+r1*(mmsize/2)]
    lea         r2, [r2+r1*(mmsize/2)]
    dec         r5
    jg .loop
    RET
%endmacro

%if ARCH_X86_64

%macro DEBLOCK_LUMA_INTER_SSE2 0
    LOAD_MASK   m0, m1, m2, m3, m12, m13, m7, m4, m6
    LOAD_TC     m6, r4
    DIFF_LT     m8, m1, m13, m10, m4
    DIFF_LT     m9, m2, m13, m11, m4
    pand        m6, m7

    mova       m14, m6
    pxor        m4, m4
    pcmpgtw     m6, m4
    pand        m6, m14

    mova        m5, m10
    pand        m5, m6
    LUMA_Q1 m8, m0, m1, m2, m5, m4

    mova        m5, m11
    pand        m5, m6
    LUMA_Q1 m9, m3, m1, m2, m5, m4

    pxor        m4, m4
    psubw       m6, m10
    pcmpgtw     m4, m14
    pandn       m4, m7
    psubw       m6, m11
    pand        m4, m6
    DEBLOCK_P0_Q0 m1, m2, m0, m3, m4, m5, m6

    SWAP         0, 8
    SWAP         3, 9
%endmacro

%macro DEBLOCK_LUMA_64 0
cglobal deblock_v_luma_10, 5,5,15
    %define p2 m8
    %define p1 m0
    %define p0 m1
    %define q0 m2
    %define q1 m3
    %define q2 m9
    %define mask0 m7
    %define mask1 m10
    %define mask2 m11
    shl        r2d, 2
    shl        r3d, 2
    LOAD_AB    m12, m13, r2d, r3d
    mov         r2, r0
    sub         r0, r1
    sub         r0, r1
    sub         r0, r1
    mov         r3, 2
.loop:
    mova        p2, [r0]
    mova        p1, [r0+r1]
    mova        p0, [r0+r1*2]
    mova        q0, [r2]
    mova        q1, [r2+r1]
    mova        q2, [r2+r1*2]
    DEBLOCK_LUMA_INTER_SSE2
    mova   [r0+r1], p1
    mova [r0+r1*2], p0
    mova      [r2], q0
    mova   [r2+r1], q1
    add         r0, mmsize
    add         r2, mmsize
    add         r4, 2
    dec         r3
    jg .loop
    RET

cglobal deblock_h_luma_10, 5,7,15
    shl        r2d, 2
    shl        r3d, 2
    LOAD_AB    m12, m13, r2d, r3d
    mov         r2, r1
    add         r2, r1
    add         r2, r1
    mov         r5, r0
    add         r5, r2
    mov         r6, 2
.loop:
    movu        m8, [r0-8]
    movu        m0, [r0+r1-8]
    movu        m2, [r0+r1*2-8]
    movu        m9, [r5-8]
    movu        m5, [r5+r1-8]
    movu        m1, [r5+r1*2-8]
    movu        m3, [r5+r2-8]
    movu        m7, [r5+r1*4-8]

    TRANSPOSE4x4W 8, 0, 2, 9, 10
    TRANSPOSE4x4W 5, 1, 3, 7, 10

    punpckhqdq  m8, m5
    SBUTTERFLY qdq, 0, 1, 10
    SBUTTERFLY qdq, 2, 3, 10
    punpcklqdq  m9, m7

    DEBLOCK_LUMA_INTER_SSE2

    TRANSPOSE4x4W 0, 1, 2, 3, 4
    LUMA_H_STORE r5, r2
    add         r4, 2
    lea         r0, [r0+r1*8]
    lea         r5, [r5+r1*8]
    dec         r6
    jg .loop
    RET
%endmacro

INIT_XMM sse2
DEBLOCK_LUMA_64
%if HAVE_AVX_EXTERNAL
INIT_XMM avx
DEBLOCK_LUMA_64
%endif
%endif

%macro SWAPMOVA 2
%ifid %1
    SWAP %1, %2
%else
    mova %1, %2
%endif
%endmacro

%macro LUMA_INTRA_P012 12
%if ARCH_X86_64
    paddw     t0, %3, %2
    mova      t2, %4
    paddw     t2, %3
%else
    mova      t0, %3
    mova      t2, %4
    paddw     t0, %2
    paddw     t2, %3
%endif
    paddw     t0, %1
    paddw     t2, t2
    paddw     t0, %5
    paddw     t2, %9
    paddw     t0, %9
    paddw     t2, t0

    psrlw     t2, 3
    psrlw     t1, t0, 2
    psubw     t2, %3
    psubw     t1, %2
    pand      t2, %8
    pand      t1, %8
    paddw     t2, %3
    paddw     t1, %2
    SWAPMOVA %11, t1

    psubw     t1, t0, %3
    paddw     t0, t0
    psubw     t1, %5
    psubw     t0, %3
    paddw     t1, %6
    paddw     t1, %2
    paddw     t0, %6
    psrlw     t1, 2
    psrlw     t0, 3

    pxor      t0, t1
    pxor      t1, %1
    pand      t0, %8
    pand      t1, %7
    pxor      t0, t1
    pxor      t0, %1
    SWAPMOVA %10, t0
    SWAPMOVA %12, t2
%endmacro

%macro LUMA_INTRA_INIT 1
    %define t0 m4
    %define t1 m5
    %define t2 m6
    %define t3 m7
    %assign i 4
%rep %1
    CAT_XDEFINE t, i, [rsp+mmsize*(i-4)]
    %assign i i+1
%endrep
%endmacro

%macro LUMA_INTRA_INTER 5
    LOAD_AB t0, t1, r2d, r3d
    mova    %1, t0
    LOAD_MASK m0, m1, m2, m3, %1, t1, t0, t2, t3
%if ARCH_X86_64
    mova    %2, t0
    psrlw   t3, %1, 2
%else
    mova    t3, %1
    mova    %2, t0
    psrlw   t3, 2
%endif
    paddw   t3, [pw_2]
    DIFF_LT m1, m2, t3, t2, t0
    pand    t2, %2
    mova    t3, %5
    mova    %1, t2
    DIFF_LT t3, m2, t1, t2, t0
    pand    t2, %1
    mova    t3, %4
    mova    %3, t2
    DIFF_LT t3, m1, t1, t2, t0
    pand    t2, %1
    mova    %1, t2
%endmacro

%macro LUMA_H_INTRA_LOAD 0
    movu    t0, [r0-8]
    movu    t1, [r0+r1-8]
    movu    m0, [r0+r1*2-8]
    movu    m1, [r0+r5-8]
    movu    m2, [r4-8]
    movu    m3, [r4+r1-8]
    movu    t2, [r4+r1*2-8]
    movu    t3, [r4+r5-8]
    TRANSPOSE8x8W 4, 5, 0, 1, 2, 3, 6, 7, t4, t5
    mova    t4, t0
    mova    t5, t1
    mova    t6, t2
    mova    t7, t3
%endmacro

%macro LUMA_H_INTRA_STORE 9
    TRANSPOSE2x4x4W %1, %2, %3, %4, %9
    movq       [r0-8], m%1
    movq       [r0+r1-8], m%2
    movq       [r0+r1*2-8], m%3
    movq       [r0+r5-8], m%4
    movhps     [r4-8], m%1
    movhps     [r4+r1-8], m%2
    movhps     [r4+r1*2-8], m%3
    movhps     [r4+r5-8], m%4
%ifnum %8
    SWAP       %1, %8
%else
    mova       m%1, %8
%endif
    TRANSPOSE2x4x4W %5, %6, %7, %1, %9
    movq       [r0], m%5
    movq       [r0+r1], m%6
    movq       [r0+r1*2], m%7
    movq       [r0+r5], m%1
    movhps     [r4], m%5
    movhps     [r4+r1], m%6
    movhps     [r4+r1*2], m%7
    movhps     [r4+r5], m%1
%endmacro

%if ARCH_X86_64

%macro DEBLOCK_LUMA_INTRA_64 0
cglobal deblock_v_luma_intra_10, 4,7,16
    %define t0 m1
    %define t1 m2
    %define t2 m4
    %define p2 m8
    %define p1 m9
    %define p0 m10
    %define q0 m11
    %define q1 m12
    %define q2 m13
    %define aa m5
    %define bb m14
    lea     r4, [r1*4]
    lea     r5, [r1*3]
    neg     r4
    add     r4, r0
    mov     r6, 2
    mova    m0, [pw_2]
    shl    r2d, 2
    shl    r3d, 2
    LOAD_AB aa, bb, r2d, r3d
.loop:
    mova    p2, [r4+r1]
    mova    p1, [r4+2*r1]
    mova    p0, [r4+r5]
    mova    q0, [r0]
    mova    q1, [r0+r1]
    mova    q2, [r0+2*r1]

    LOAD_MASK p1, p0, q0, q1, aa, bb, m3, t0, t1
    mova    t2, aa
    psrlw   t2, 2
    paddw   t2, m0
    DIFF_LT p0, q0, t2, m6, t0
    DIFF_LT p2, p0, bb, t1, t0
    DIFF_LT q2, q0, bb, m7, t0
    pand    m6, m3
    pand    m7, m6
    pand    m6, t1
    LUMA_INTRA_P012 p0, p1, p2, [r4], q0, q1, m3, m6, m0, [r4+r5], [r4+2*r1], [r4+r1]
    LUMA_INTRA_P012 q0, q1, q2, [r0+r5], p0, p1, m3, m7, m0, [r0], [r0+r1], [r0+2*r1]
    add     r0, mmsize
    add     r4, mmsize
    dec     r6
    jg .loop
    RET

cglobal deblock_h_luma_intra_10, 4,7,16,mmsize
    %define t0 m15
    %define t1 m14
    %define t2 m2
    %define q3 m5
    %define q2 m8
    %define q1 m9
    %define q0 m10
    %define p0 m11
    %define p1 m12
    %define p2 m13
    %define p3 m4
    %define spill [rsp]
    lea     r4, [r0+r1*4]
    lea     r5, [r1*3]
    mov     r6, 2
    mova    m0, [pw_2]
    shl    r2d, 2
    shl    r3d, 2
.loop:
    movu    q3, [r0-8]
    movu    q2, [r0+r1-8]
    movu    q1, [r0+r1*2-8]
    movu    q0, [r0+r5-8]
    movu    p0, [r4-8]
    movu    p1, [r4+r1-8]
    movu    p2, [r4+r1*2-8]
    movu    p3, [r4+r5-8]
    TRANSPOSE8x8W 5, 8, 9, 10, 11, 12, 13, 4, 1

    LOAD_AB m1, m2, r2d, r3d
    LOAD_MASK q1, q0, p0, p1, m1, m2, m3, t0, t1
    psrlw   m1, 2
    paddw   m1, m0
    DIFF_LT p0, q0, m1, m6, t0
    DIFF_LT q2, q0, m2, t1, t0
    DIFF_LT p0, p2, m2, m7, t0
    pand    m6, m3
    pand    m7, m6
    pand    m6, t1

    mova spill, q3
    LUMA_INTRA_P012 q0, q1, q2, q3, p0, p1, m3, m6, m0, m5, m1, q2
    LUMA_INTRA_P012 p0, p1, p2, p3, q0, q1, m3, m7, m0, p0, m6, p2
    mova    m7, spill

    LUMA_H_INTRA_STORE 7, 8, 1, 5, 11, 6, 13, 4, 14

    lea     r0, [r0+r1*8]
    lea     r4, [r4+r1*8]
    dec     r6
    jg .loop
    RET
%endmacro

INIT_XMM sse2
DEBLOCK_LUMA_INTRA_64
%if HAVE_AVX_EXTERNAL
INIT_XMM avx
DEBLOCK_LUMA_INTRA_64
%endif

%endif

%macro DEBLOCK_LUMA_INTRA 0

cglobal deblock_v_luma_intra_10, 4,7,8,-3*mmsize
    LUMA_INTRA_INIT 3
    lea     r4, [r1*4]
    lea     r5, [r1*3]
    neg     r4
    add     r4, r0
    mov     r6, 32/mmsize
    shl    r2d, 2
    shl    r3d, 2
.loop:
    mova    m0, [r4+r1*2]
    mova    m1, [r4+r5]
    mova    m2, [r0]
    mova    m3, [r0+r1]
    LUMA_INTRA_INTER t4, t5, t6, [r4+r1], [r0+r1*2]
    LUMA_INTRA_P012 m1, m0, t3, [r4], m2, m3, t5, t4, [pw_2], [r4+r5], [r4+2*r1], [r4+r1]
    mova    t3, [r0+r1*2]
    LUMA_INTRA_P012 m2, m3, t3, [r0+r5], m1, m0, t5, t6, [pw_2], [r0], [r0+r1], [r0+2*r1]
    add     r0, mmsize
    add     r4, mmsize
    dec     r6
    jg .loop
    RET

cglobal deblock_h_luma_intra_10, 4,7,8,-8*mmsize
    LUMA_INTRA_INIT 8
    lea     r4, [r0+r1*4]
    lea     r5, [r1*3]
    mov     r6, 32/mmsize
    shl    r2d, 2
    shl    r3d, 2
.loop:
    LUMA_H_INTRA_LOAD
    LUMA_INTRA_INTER t8, t9, t10, t5, t6

    LUMA_INTRA_P012 m1, m0, t3, t4, m2, m3, t9, t8, [pw_2], t8, t5, t11
    mova    t3, t6
    LUMA_INTRA_P012 m2, m3, t3, t7, m1, m0, t9, t10, [pw_2], m4, t6, m5

    mova    m2, t4
    mova    m0, t11
    mova    m1, t5
    mova    m3, t8
    mova    m6, t6

    LUMA_H_INTRA_STORE 2, 0, 1, 3, 4, 6, 5, t7, 7

    lea     r0, [r0+r1*(mmsize/2)]
    lea     r4, [r4+r1*(mmsize/2)]
    dec     r6
    jg .loop
    RET
%endmacro

%if ARCH_X86_64 == 0
INIT_XMM sse2
DEBLOCK_LUMA
DEBLOCK_LUMA_INTRA
%if HAVE_AVX_EXTERNAL
INIT_XMM avx
DEBLOCK_LUMA
DEBLOCK_LUMA_INTRA
%endif
%endif

%macro CHROMA_DEBLOCK_P0_Q0_INTRA 7
    mova    %6, [pw_2]
    paddw   %6, %3
    paddw   %6, %4
    paddw   %7, %6, %2
    paddw   %6, %1
    paddw   %6, %3
    paddw   %7, %4
    psraw   %6, 2
    psraw   %7, 2
    psubw   %6, %1
    psubw   %7, %2
    pand    %6, %5
    pand    %7, %5
    paddw   %1, %6
    paddw   %2, %7
%endmacro

%macro CHROMA_V_LOAD 1
    mova        m0, [r0]
    mova        m1, [r0+r1]
    mova        m2, [%1]
    mova        m3, [%1+r1]
%endmacro

%macro CHROMA_V_STORE 0
    mova [r0+1*r1], m1
    mova [r0+2*r1], m2
%endmacro

%macro TRANSPOSE4x8W_LOAD 8
    movq             m0, %1
    movq             m2, %2
    movq             m1, %3
    movq             m3, %4

    punpcklwd        m0, m2
    punpcklwd        m1, m3
    punpckhdq        m2, m0, m1
    punpckldq        m0, m1

    movq             m4, %5
    movq             m6, %6
    movq             m5, %7
    movq             m3, %8

    punpcklwd        m4, m6
    punpcklwd        m5, m3
    punpckhdq        m6, m4, m5
    punpckldq        m4, m5

    punpckhqdq       m1, m0, m4
    punpcklqdq       m0, m4
    punpckhqdq       m3, m2, m6
    punpcklqdq       m2, m6
%endmacro

%macro TRANSPOSE8x4W_STORE 8
    TRANSPOSE4x4W     0, 1, 2, 3, 4
    movq             %1, m0
    movhps           %2, m0
    movq             %3, m1
    movhps           %4, m1
    movq             %5, m2
    movhps           %6, m2
    movq             %7, m3
    movhps           %8, m3
%endmacro

%macro CHROMA_H_LOAD 4
    TRANSPOSE4x8W_LOAD PASS8ROWS(pix_q-4, %1-4, stride_q, %2)
    mova %3, m0
    mova %4, m3
%endmacro

%macro CHROMA_H_STORE 4
    mova m0, %3
    mova m3, %4
    TRANSPOSE8x4W_STORE PASS8ROWS(pix_q-4, %1-4, stride_q, %2)
%endmacro

%macro CHROMA_V_LOAD_TC 2
    movd        %1, [%2]
    punpcklbw   %1, %1
    punpcklwd   %1, %1
    psraw       %1, 6
%endmacro

%macro DEBLOCK_CHROMA 0

cglobal deblock_v_chroma_10, 5,6,8
    mov         r5, r0
    sub         r0, r1
    sub         r0, r1
    shl        r2d, 2
    shl        r3d, 2
    CHROMA_V_LOAD r5
    LOAD_AB     m4, m5, r2d, r3d
    LOAD_MASK   m0, m1, m2, m3, m4, m5, m7, m6, m4
    pxor        m4, m4
    CHROMA_V_LOAD_TC m6, r4
    psubw       m6, [pw_3]
    pmaxsw      m6, m4
    pand        m7, m6
    DEBLOCK_P0_Q0 m1, m2, m0, m3, m7, m5, m6
    CHROMA_V_STORE
    RET

cglobal deblock_v_chroma_intra_10, 4,5,8
    mov         r4, r0
    sub         r0, r1
    sub         r0, r1
    shl        r2d, 2
    shl        r3d, 2
    CHROMA_V_LOAD r4
    LOAD_AB     m4, m5, r2d, r3d
    LOAD_MASK   m0, m1, m2, m3, m4, m5, m7, m6, m4
    CHROMA_DEBLOCK_P0_Q0_INTRA m1, m2, m0, m3, m7, m5, m6
    CHROMA_V_STORE
    RET

cglobal deblock_h_chroma_10, 5, 7, 8, 0-2*mmsize, pix_, stride_, alpha_, beta_, tc0_
    shl alpha_d,  2
    shl beta_d,   2
    mov r5,       pix_q
    lea r6,      [3*stride_q]
    add r5,       r6

        CHROMA_H_LOAD r5, r6, [rsp], [rsp + mmsize]
        LOAD_AB          m4,  m5, alpha_d, beta_d
        LOAD_MASK        m0,  m1, m2, m3, m4, m5, m7, m6, m4
        pxor             m4,  m4
        CHROMA_V_LOAD_TC m6,  tc0_q
        psubw            m6, [pw_3]
        pmaxsw           m6,  m4
        pand             m7,  m6
        DEBLOCK_P0_Q0    m1,  m2, m0, m3, m7, m5, m6
        CHROMA_H_STORE r5, r6, [rsp], [rsp + mmsize]

RET

cglobal deblock_h_chroma422_10, 5, 7, 8, 0-3*mmsize, pix_, stride_, alpha_, beta_, tc0_
    shl alpha_d,  2
    shl beta_d,   2

    movd m0, [tc0_q]
    punpcklbw m0, m0
    psraw m0, 6
    movq [rsp], m0

    mov r5,       pix_q
    lea r6,      [3*stride_q]
    add r5,       r6

    mov r4, -8
    .loop:

        CHROMA_H_LOAD r5, r6, [rsp + 1*mmsize], [rsp + 2*mmsize]
        LOAD_AB          m4,  m5, alpha_d, beta_d
        LOAD_MASK        m0,  m1, m2, m3, m4, m5, m7, m6, m4
        pxor             m4,  m4
        movd             m6, [rsp + r4 + 8]
        punpcklwd        m6,  m6
        punpcklwd        m6,  m6
        psubw            m6, [pw_3]
        pmaxsw           m6,  m4
        pand             m7,  m6
        DEBLOCK_P0_Q0    m1,  m2, m0, m3, m7, m5, m6
        CHROMA_H_STORE r5, r6, [rsp + 1*mmsize], [rsp + 2*mmsize]

        lea pix_q, [pix_q + (mmsize/2)*stride_q]
        lea r5,    [r5 +    (mmsize/2)*stride_q]
        add r4, (mmsize/4)
    jl .loop
RET

%endmacro

INIT_XMM sse2
DEBLOCK_CHROMA
%if HAVE_AVX_EXTERNAL
INIT_XMM avx
DEBLOCK_CHROMA
%endif

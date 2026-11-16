%include "x86util.asm"

SECTION_RODATA

pb_A1: times 16 db 0xA1
pb_3_1: times 4 db 3, 1

SECTION .text

cextern pb_0
cextern pb_1
cextern pb_3

%macro TRANSPOSE8x4B_STORE 10
    punpcklwd   m6, %1, %2
    movd        %3, m6
    pshufd      m7, m6, 00110001b
    punpckhqdq  m6, m6
    movd        %4, m7
    punpckhqdq  m7, m7
    punpckhwd   %1, %2
    movd        %5, m6
    movd        %6, m7
    pshufd      m6, %1, 00110001b
    movd        %7, %1
    punpckhqdq  %1, %1
    movd        %8, m6
    punpckhqdq  m6, m6
    movd        %9, %1
    movd       %10, m6
%endmacro

%macro SBUTTERFLY3 4
    punpckh%1  %4, %2, %3
    punpckl%1  %2, %3
%endmacro

%macro TRANSPOSE6x8_MEM 9
    RESET_MM_PERMUTATION
    movq  m0, %1
    movq  m1, %2
    movq  m2, %3
    movq  m3, %4
    movq  m4, %5
    movq  m5, %6
    movq  m6, %7
    SBUTTERFLY bw, 0, 1, 7
    SBUTTERFLY bw, 2, 3, 7
    SBUTTERFLY bw, 4, 5, 7
    movq  [%9+0x10], m3
    SBUTTERFLY3 bw, m6, %8, m7
    SBUTTERFLY wd, 0, 2, 3
    SBUTTERFLY wd, 4, 6, 3
    punpckhdq m0, m4
    movq  [%9+0x00], m0
    SBUTTERFLY3 wd, m1, [%9+0x10], m3
    SBUTTERFLY wd, 5, 7, 0
    SBUTTERFLY dq, 1, 5, 0
    SBUTTERFLY dq, 2, 6, 0
    punpckldq m3, m7
    movq  [%9+0x10], m2
    movq  [%9+0x20], m6
    movq  [%9+0x30], m1
    movq  [%9+0x40], m5
    movq  [%9+0x50], m3
    RESET_MM_PERMUTATION
%endmacro

%macro TRANSPOSE8x8_MEM 16
    RESET_MM_PERMUTATION
    movq  m0, %1
    movq  m1, %2
    movq  m2, %3
    movq  m3, %4
    movq  m4, %5
    movq  m5, %6
    movq  m6, %7
    SBUTTERFLY bw, 0, 1, 7
    SBUTTERFLY bw, 2, 3, 7
    SBUTTERFLY bw, 4, 5, 7
    SBUTTERFLY3 bw, m6, %8, m7
    movq  %9,  m5
    SBUTTERFLY wd, 0, 2, 5
    SBUTTERFLY wd, 4, 6, 5
    SBUTTERFLY wd, 1, 3, 5
    movq  %11, m6
    movq  m6,  %9
    SBUTTERFLY wd, 6, 7, 5
    SBUTTERFLY dq, 0, 4, 5
    SBUTTERFLY dq, 1, 6, 5
    movq  %9,  m0
    movq  %10, m4
    movq  %13, m1
    movq  %14, m6
    SBUTTERFLY3 dq, m2, %11, m0
    SBUTTERFLY dq, 3, 7, 4
    movq  %11, m2
    movq  %12, m0
    movq  %15, m3
    movq  %16, m7
    RESET_MM_PERMUTATION
%endmacro

%macro DIFF_GT 5
%if avx_enabled == 0
    mova    %5, %2
    mova    %4, %1
    psubusb %5, %1
    psubusb %4, %2
%else
    psubusb %5, %2, %1
    psubusb %4, %1, %2
%endif
    por     %4, %5
    psubusb %4, %3
%endmacro

%macro DIFF_GT2 5
%if ARCH_X86_64
    psubusb %5, %2, %1
    psubusb %4, %1, %2
%else
    mova    %5, %2
    mova    %4, %1
    psubusb %5, %1
    psubusb %4, %2
%endif
    psubusb %5, %3
    psubusb %4, %3
    pcmpeqb %4, %5
%endmacro

%macro LOAD_MASK 2-3
    movd     m4, %1
    movd     m5, %2
%if cpuflag(ssse3)
    pxor     m6, m6
    pshufb   m4, m6
    pshufb   m5, m6
%else
    SPLATW   m4, m4
    SPLATW   m5, m5
    packuswb m4, m4
    packuswb m5, m5
%endif
%if %0>2
    mova     %3, m4
%endif
    DIFF_GT  m1, m2, m4, m7, m6
    DIFF_GT  m0, m1, m5, m4, m6
    por      m7, m4
    DIFF_GT  m3, m2, m5, m4, m6
    por      m7, m4
    pxor     m6, m6
    pcmpeqb  m7, m6
%endmacro

%macro DEBLOCK_P0_Q0 0
    pcmpeqb m4, m4
    pxor    m5, m1, m2
    pxor    m3, m4
    pand    m5, [pb_1]
    pavgb   m3, m0
    pxor    m4, m1
    pavgb   m3, [pb_3]
    pavgb   m4, m2
    pavgb   m3, m5
    mova    m6, [pb_A1]
    paddusb m3, m4
    psubusb m4, m6, m3
    psubusb m3, m6
    pminub  m4, m7
    pminub  m3, m7
    psubusb m1, m4
    psubusb m2, m3
    paddusb m1, m3
    paddusb m2, m4
%endmacro

%macro LUMA_Q1 6
    pavgb   %6, m1, m2
    pavgb   %2, %6
    pxor    %6, %3
    pand    %6, [pb_1]
    psubusb %2, %6
    psubusb %6, %1, %5
    paddusb %5, %1
    pmaxub  %2, %6
    pminub  %2, %5
    mova    %4, %2
%endmacro

%if ARCH_X86_64

%macro DEBLOCK_LUMA 0
cglobal deblock_v_luma_8, 5,5,10, pix_, stride_, alpha_, beta_, base3_
    movd    m8, [r4]
    lea     r4, [stride_q*3]
    dec     alpha_d
    neg     r4
    dec     beta_d
    add     base3_q, pix_q

    mova    m0, [base3_q + stride_q]
    mova    m1, [base3_q + 2*stride_q]
    mova    m2, [pix_q]
    mova    m3, [pix_q + stride_q]
    LOAD_MASK r2d, r3d

    punpcklbw m8, m8
    punpcklbw m8, m8
    pcmpeqb m9, m9
    pcmpeqb m9, m8
    pandn   m9, m7
    pand    m8, m9

    movdqa  m3, [base3_q]
    DIFF_GT2 m1, m3, m5, m6, m7
    pand    m6, m9
    psubb   m7, m8, m6
    pand    m6, m8
    LUMA_Q1 m0, m3, [base3_q], [base3_q + stride_q], m6, m4

    movdqa  m4, [pix_q + 2*stride_q]
    DIFF_GT2 m2, m4, m5, m6, m3
    pand    m6, m9
    pand    m8, m6
    psubb   m7, m6
    mova    m3, [pix_q + stride_q]
    LUMA_Q1 m3, m4, [pix_q + 2*stride_q], [pix_q + stride_q], m8, m6

    DEBLOCK_P0_Q0
    mova    [base3_q + 2*stride_q], m1
    mova    [pix_q], m2
    RET

cglobal deblock_h_luma_8, 5,9,8,0x60+16*WIN64
    INIT_MMX cpuname
    lea    r8,  [r1+r1*2]
    lea    r6,  [r0-4]
    lea    r5,  [r0-4+r8]
    mov    r7,  r1
%if WIN64
    %define pix_tmp rsp+0x30
%else
    %define pix_tmp rsp
%endif

    TRANSPOSE6x8_MEM  PASS8ROWS(r6, r5, r1, r8), pix_tmp
    lea    r6, [r6+r1*8]
    lea    r5, [r5+r1*8]
    TRANSPOSE6x8_MEM  PASS8ROWS(r6, r5, r1, r8), pix_tmp+8

    lea    r0, [pix_tmp+0x30]
    mov    r1d, 0x10
%if WIN64
    mov    [rsp+0x20], r4
%endif
    call   deblock_v_luma_8

    add    r6, 2
    add    r5, 2

    INIT_XMM cpuname

    mova       m0, [pix_tmp+0x10]

    mova       m3, [pix_tmp+0x40]

    punpckhbw  m4, m0, m1
    punpckhbw  m5, m2, m3

    TRANSPOSE8x4B_STORE m4, m5, PASS8ROWS(r6, r5, r7, r8)

    punpcklbw  m0, m1
    punpcklbw  m2, m3
    shl    r7,  3
    sub    r6,  r7
    sub    r5,  r7
    shr    r7,  3

    TRANSPOSE8x4B_STORE m0, m2, PASS8ROWS(r6, r5, r7, r8)
    RET
%endmacro

%macro DEBLOCK_H_LUMA_MBAFF 0

cglobal deblock_h_luma_mbaff_8, 5, 9, 10, 8*16, pix_, stride_, alpha_, beta_, tc0_, base3_, stride3_
    dec    alpha_d
    dec    beta_d
    mov    base3_q,    pix_q
    lea    stride3_q, [3*stride_q]
    add    base3_q,    stride3_q

    movq m0, [pix_q - 4]
    movq m1, [pix_q + stride_q - 4]
    movq m2, [pix_q + 2*stride_q - 4]
    movq m3, [base3_q - 4]
    movq m4, [base3_q + stride_q - 4]
    movq m5, [base3_q + 2*stride_q - 4]
    movq m6, [base3_q + stride3_q - 4]
    movq m7, [base3_q + 4*stride_q - 4]

    TRANSPOSE_8X8B 0,1,2,3,4,5,6,7

    %assign i 0
    %rep 8
        movq [rsp + 16*i], m %+ i
        %assign i i+1
    %endrep

    SWAP 0, 2
    SWAP 1, 3
    SWAP 2, 4
    SWAP 3, 5

    LOAD_MASK alpha_d, beta_d
    movd m8, [tc0_q]
    punpcklbw m8, m8
    pcmpeqb m9, m9
    pcmpeqb m9, m8
    pandn   m9, m7
    pand    m8, m9

    movdqa  m3, [rsp + 16]
    DIFF_GT2 m1, m3, m5, m6, m7
    pand    m6, m9
    psubb   m7, m8, m6
    pand    m6, m8
    LUMA_Q1 m0, m3, [rsp + 16], [rsp + 32], m6, m4

    movdqa  m4, [rsp + 96]
    DIFF_GT2 m2, m4, m5, m6, m3
    pand    m6, m9
    pand    m8, m6
    psubb   m7, m6
    mova    m3, [rsp + 80]
    LUMA_Q1 m3, m4, [rsp + 96], [rsp + 80], m8, m6

    DEBLOCK_P0_Q0
    SWAP 1, 3
    SWAP 2, 4
    movq m0, [rsp]
    movq m1, [rsp + 16]
    movq m2, [rsp + 32]
    movq m5, [rsp + 80]
    movq m6, [rsp + 96]
    movq m7, [rsp + 112]

    TRANSPOSE_8X8B 0,1,2,3,4,5,6,7
    movq [pix_q - 4], m0
    movq [pix_q + stride_q - 4], m1
    movq [pix_q + 2*stride_q - 4], m2
    movq [base3_q - 4], m3
    movq [base3_q + stride_q - 4], m4
    movq [base3_q + 2*stride_q - 4], m5
    movq [base3_q + stride3_q - 4], m6
    movq [base3_q + 4*stride_q - 4], m7

RET

%endmacro

INIT_XMM sse2
DEBLOCK_H_LUMA_MBAFF
DEBLOCK_LUMA

%if HAVE_AVX_EXTERNAL
INIT_XMM avx
DEBLOCK_H_LUMA_MBAFF
DEBLOCK_LUMA
%endif

%else

%macro DEBLOCK_LUMA 1

cglobal deblock_v_luma_8, 5,5,8,2*%1
    lea     r4, [r1*3]
    dec     r2
    neg     r4
    dec     r3
    add     r4, r0

    mova    m0, [r4+r1]
    mova    m1, [r4+2*r1]
    mova    m2, [r0]
    mova    m3, [r0+r1]
    LOAD_MASK r2, r3

    mov     r3, r4mp
    pcmpeqb m3, m3
    movd    m4, [r3]
    punpcklbw m4, m4
    punpcklbw m4, m4
    mova   [esp+%1], m4
    pcmpgtb m4, m3
    mova    m3, [r4]
    pand    m4, m7
    mova   [esp], m4

    DIFF_GT2 m1, m3, m5, m6, m7
    pand    m6, m4
    pand    m4, [esp+%1]
    psubb   m7, m4, m6
    pand    m6, m4
    LUMA_Q1 m0, m3, [r4], [r4+r1], m6, m4

    mova    m4, [r0+2*r1]
    DIFF_GT2 m2, m4, m5, m6, m3
    pand    m6, [esp]
    mova    m5, [esp+%1]
    psubb   m7, m6
    pand    m5, m6
    mova    m3, [r0+r1]
    LUMA_Q1 m3, m4, [r0+2*r1], [r0+r1], m5, m6

    DEBLOCK_P0_Q0
    mova    [r4+2*r1], m1
    mova    [r0], m2
    RET

cglobal deblock_h_luma_8, 0,5,8,0x60+12
    INIT_MMX cpuname
    mov    r0, r0mp
    mov    r3, r1m
    lea    r4, [r3*3]
    sub    r0, 4
    lea    r1, [r0+r4]
%define pix_tmp esp+12

    TRANSPOSE6x8_MEM  PASS8ROWS(r0, r1, r3, r4), pix_tmp
    lea    r0, [r0+r3*8]
    lea    r1, [r1+r3*8]
    TRANSPOSE6x8_MEM  PASS8ROWS(r0, r1, r3, r4), pix_tmp+8

    lea    r0, [pix_tmp+0x30]
    PUSH   dword r4m
    PUSH   dword r3m
    PUSH   dword r2m
    PUSH   dword 16
    PUSH   dword r0
    call   deblock_v_luma_8
    ADD    esp, 20

    INIT_XMM cpuname

    mova       m0, [pix_tmp+0x10]

    mova       m3, [pix_tmp+0x40]

    mov        r0, r0mp
    punpcklbw  m4, m0, m1
    sub        r0, 2
    punpcklbw  m5, m2, m3
    lea        r1, [r0+r4]

    TRANSPOSE8x4B_STORE m4, m5, PASS8ROWS(r0, r1, r3, r4)

    punpckhbw   m0, m1
    lea    r0, [r0+r3*8]
    punpckhbw   m2, m3
    lea    r1, [r1+r3*8]

    TRANSPOSE8x4B_STORE m0, m2, PASS8ROWS(r0, r1, r3, r4)

    RET
%endmacro

INIT_XMM sse2
DEBLOCK_LUMA 16
%if HAVE_AVX_EXTERNAL
INIT_XMM avx
DEBLOCK_LUMA 16
%endif

%endif

%macro LUMA_INTRA_P012 4
%if ARCH_X86_64
    pavgb t0, p2, p1
    pavgb t1, p0, q0
%else
    mova  t0, p2
    mova  t1, p0
    pavgb t0, p1
    pavgb t1, q0
%endif
    pavgb t0, t1
    mova  t5, t1
%if ARCH_X86_64
    paddb t2, p2, p1
    paddb t3, p0, q0
%else
    mova  t2, p2
    mova  t3, p0
    paddb t2, p1
    paddb t3, q0
%endif
    paddb t2, t3
    mova  t3, t2
    mova  t4, t2
    psrlw t2, 1
    pavgb t2, mpb_0
    pxor  t2, t0
    pand  t2, mpb_1
    psubb t0, t2

%if ARCH_X86_64
    pavgb t1, p2, q1
    psubb t2, p2, q1
%else
    mova  t1, p2
    mova  t2, p2
    pavgb t1, q1
    psubb t2, q1
%endif
    paddb t3, t3
    psubb t3, t2
    pand  t2, mpb_1
    psubb t1, t2
    pavgb t1, p1
    pavgb t1, t5
    psrlw t3, 2
    pavgb t3, mpb_0
    pxor  t3, t1
    pand  t3, mpb_1
    psubb t1, t3

    pxor  t3, p0, q1
    pavgb t2, p0, q1
    pand  t3, mpb_1
    psubb t2, t3
    pavgb t2, p1

    pxor  t1, t2
    pxor  t2, p0
    pand  t1, mask1p
    pand  t2, mask0
    pxor  t1, t2
    pxor  t1, p0
    mova  %1, t1

    mova  t1, %4
    paddb t2, t1, p2
    pavgb t1, p2
    pavgb t1, t0
    paddb t2, t2
    paddb t2, t4
    psrlw t2, 2
    pavgb t2, mpb_0
    pxor  t2, t1
    pand  t2, mpb_1
    psubb t1, t2

    pxor  t0, p1
    pxor  t1, p2
    pand  t0, mask1p
    pand  t1, mask1p
    pxor  t0, p1
    pxor  t1, p2
    mova  %2, t0
    mova  %3, t1
%endmacro

%macro LUMA_INTRA_SWAP_PQ 0
    %define q1 m0
    %define q0 m1
    %define p0 m2
    %define p1 m3
    %define p2 q2
    %define mask1p mask1q
%endmacro

%macro DEBLOCK_LUMA_INTRA 1
    %define p1 m0
    %define p0 m1
    %define q0 m2
    %define q1 m3
    %define t0 m4
    %define t1 m5
    %define t2 m6
    %define t3 m7
%if ARCH_X86_64
    %define p2 m8
    %define q2 m9
    %define t4 m10
    %define t5 m11
    %define mask0 m12
    %define mask1p m13
%if WIN64
    %define mask1q [rsp]
%else
    %define mask1q [rsp-24]
%endif
    %define mpb_0 m14
    %define mpb_1 m15
%else
    %define spill(x) [esp+16*x]
    %define p2 [r4+r1]
    %define q2 [r0+2*r1]
    %define t4 spill(0)
    %define t5 spill(1)
    %define mask0 spill(2)
    %define mask1p spill(3)
    %define mask1q spill(4)
    %define mpb_0 [pb_0]
    %define mpb_1 [pb_1]
%endif

%if WIN64
cglobal deblock_v_luma_intra_8, 4,6,16,0x10
%else
cglobal deblock_v_luma_intra_8, 4,6,16,ARCH_X86_64*0x50-0x50
%endif
    lea     r4, [r1*4]
    lea     r5, [r1*3]
    dec     r2d
    neg     r4
    dec     r3d
    add     r4, r0
    mova    p1, [r4+2*r1]
    mova    p0, [r4+r5]
    mova    q0, [r0]
    mova    q1, [r0+r1]
%if ARCH_X86_64
    pxor    mpb_0, mpb_0
    mova    mpb_1, [pb_1]
    LOAD_MASK r2d, r3d, t5
    SWAP    7, 12
    pavgb   t5, mpb_0
    pavgb   t5, mpb_1
    movdqa  p2, [r4+r1]
    movdqa  q2, [r0+2*r1]
    DIFF_GT2 p0, q0, t5, t0, t3
    DIFF_GT2 p0, p2, m5, t2, t5
    DIFF_GT2 q0, q2, m5, t4, t5
    pand    t0, mask0
    pand    t4, t0
    pand    t2, t0
    mova    mask1q, t4
    mova    mask1p, t2
%else
    LOAD_MASK r2d, r3d, t5
    mova    m4, t5
    mova    mask0, m7
    pavgb   m4, [pb_0]
    pavgb   m4, [pb_1]
    DIFF_GT2 p0, q0, m4, m6, m7
    pand    m6, mask0
    DIFF_GT2 p0, p2, m5, m4, m7
    pand    m4, m6
    mova    mask1p, m4
    DIFF_GT2 q0, q2, m5, m4, m7
    pand    m4, m6
    mova    mask1q, m4
%endif
    LUMA_INTRA_P012 [r4+r5], [r4+2*r1], [r4+r1], [r4]
    LUMA_INTRA_SWAP_PQ
    LUMA_INTRA_P012 [r0], [r0+r1], [r0+2*r1], [r0+r5]
    RET

INIT_MMX cpuname
%if ARCH_X86_64

cglobal deblock_h_luma_intra_8, 4,9,0,0x80
    lea    r8,  [r1*3]
    lea    r6,  [r0-4]
    lea    r5,  [r0-4+r8]
    mov    r7,  r1
%if WIN64
    %define pix_tmp rsp+0x20
%else
    %define pix_tmp rsp
%endif

    TRANSPOSE8x8_MEM  PASS8ROWS(r6, r5, r1, r8), PASS8ROWS(pix_tmp, pix_tmp+0x30, 0x10, 0x30)
    lea    r6, [r6+r1*8]
    lea    r5, [r5+r1*8]
    TRANSPOSE8x8_MEM  PASS8ROWS(r6, r5, r1, r8), PASS8ROWS(pix_tmp+8, pix_tmp+0x38, 0x10, 0x30)

    lea    r0,  [pix_tmp+0x40]
    mov    r1,  0x10
    call   deblock_v_luma_intra_8

    lea    r5, [r6+r8]
    TRANSPOSE8x8_MEM  PASS8ROWS(pix_tmp+8, pix_tmp+0x38, 0x10, 0x30), PASS8ROWS(r6, r5, r7, r8)
    shl    r7,  3
    sub    r6,  r7
    sub    r5,  r7
    shr    r7,  3
    TRANSPOSE8x8_MEM  PASS8ROWS(pix_tmp, pix_tmp+0x30, 0x10, 0x30), PASS8ROWS(r6, r5, r7, r8)
    RET
%else
cglobal deblock_h_luma_intra_8, 2,4,8,0x80
    lea    r3,  [r1*3]
    sub    r0,  4
    lea    r2,  [r0+r3]
    %define pix_tmp rsp

    TRANSPOSE8x8_MEM  PASS8ROWS(r0, r2, r1, r3), PASS8ROWS(pix_tmp, pix_tmp+0x30, 0x10, 0x30)
    lea    r0,  [r0+r1*8]
    lea    r2,  [r2+r1*8]
    TRANSPOSE8x8_MEM  PASS8ROWS(r0, r2, r1, r3), PASS8ROWS(pix_tmp+8, pix_tmp+0x38, 0x10, 0x30)

    lea    r0,  [pix_tmp+0x40]
    PUSH   dword r3m
    PUSH   dword r2m
    PUSH   dword 16
    PUSH   r0
    call   deblock_v_luma_intra_8
    ADD    esp, 16

    mov    r1,  r1m
    mov    r0,  r0mp
    lea    r3,  [r1*3]
    sub    r0,  4
    lea    r2,  [r0+r3]

    TRANSPOSE8x8_MEM  PASS8ROWS(pix_tmp, pix_tmp+0x30, 0x10, 0x30), PASS8ROWS(r0, r2, r1, r3)
    lea    r0,  [r0+r1*8]
    lea    r2,  [r2+r1*8]
    TRANSPOSE8x8_MEM  PASS8ROWS(pix_tmp+8, pix_tmp+0x38, 0x10, 0x30), PASS8ROWS(r0, r2, r1, r3)
    RET
%endif
%endmacro

INIT_XMM sse2
DEBLOCK_LUMA_INTRA v
%if HAVE_AVX_EXTERNAL
INIT_XMM avx
DEBLOCK_LUMA_INTRA v
%endif

%macro LOAD_8_ROWS 8
    movd m0, %1
    movd m1, %2
    movd m2, %3
    movd m3, %4
    movd m4, %5
    movd m5, %6
    movd m6, %7
    movd m7, %8
%endmacro

%macro STORE_8_ROWS 8
    movd %1, m0
    movd %2, m1
    movd %3, m2
    movd %4, m3
    movd %5, m4
    movd %6, m5
    movd %7, m6
    movd %8, m7
%endmacro

%macro TRANSPOSE_8x4B_XMM 0
    punpcklbw m0, m1
    punpcklbw m2, m3
    punpcklbw m4, m5
    punpcklbw m6, m7
    punpcklwd m0, m2
    punpcklwd m4, m6
    punpckhdq m2, m0, m4
    punpckldq m0, m4
    MOVHL m1, m0
    MOVHL m3, m2
%endmacro

%macro TRANSPOSE_4x8B_XMM 0
    punpcklbw m0, m1
    punpcklbw m2, m3
    punpckhwd m4, m0, m2
    punpcklwd m0, m2
    MOVHL m6, m4
    MOVHL m2, m0
    pshufd m1, m0, 1
    pshufd m3, m2, 1
    pshufd m5, m4, 1
    pshufd m7, m6, 1
%endmacro

%macro CHROMA_INTER_BODY_XMM 1
    LOAD_MASK alpha_d, beta_d
    movd m6, [tc0_q]
    %rep %1
        punpcklbw m6, m6
    %endrep
    pand m7, m6
    DEBLOCK_P0_Q0
%endmacro

%macro CHROMA_INTRA_BODY_XMM 0
    LOAD_MASK alpha_d, beta_d
    mova    m5,  m1
    mova    m6,  m2
    pxor    m4,  m1, m3
    pand    m4, [pb_1]
    pavgb   m1,  m3
    psubusb m1,  m4
    pavgb   m1,  m0
    pxor    m4,  m2, m0
    pand    m4, [pb_1]
    pavgb   m2,  m0
    psubusb m2,  m4
    pavgb   m2,  m3
    psubb   m1,  m5
    psubb   m2,  m6
    pand    m1,  m7
    pand    m2,  m7
    paddb   m1,  m5
    paddb   m2,  m6
%endmacro

%macro CHROMA_V_START_XMM 1
    dec alpha_d
    dec beta_d
    mov %1, pix_q
    sub %1, stride_q
    sub %1, stride_q
%endmacro

%macro CHROMA_H_START_XMM 2
    dec alpha_d
    dec beta_d
    lea %2, [3*stride_q]
    mov %1,  pix_q
    add %1,  %2
%endmacro

%macro DEBLOCK_CHROMA_XMM 1

INIT_XMM %1

cglobal deblock_v_chroma_8, 5, 6, 8, pix_, stride_, alpha_, beta_, tc0_
    CHROMA_V_START_XMM r5
    movq m0, [r5]
    movq m1, [r5 + stride_q]
    movq m2, [pix_q]
    movq m3, [pix_q + stride_q]
    CHROMA_INTER_BODY_XMM 1
    movq [r5 + stride_q], m1
    movq [pix_q], m2
RET

cglobal deblock_h_chroma_8, 5, 7, 8, 0-16, pix_, stride_, alpha_, beta_, tc0_
    CHROMA_H_START_XMM r5, r6
    LOAD_8_ROWS PASS8ROWS(pix_q - 2, r5 - 2, stride_q, r6)
    TRANSPOSE_8x4B_XMM
    movq [rsp], m0
    movq [rsp + 8], m3
    CHROMA_INTER_BODY_XMM 1
    movq m0, [rsp]
    movq m3, [rsp + 8]
    TRANSPOSE_4x8B_XMM
    STORE_8_ROWS PASS8ROWS(pix_q - 2, r5 - 2, stride_q, r6)
RET

cglobal deblock_h_chroma422_8, 5, 7, 8, 0-16, pix_, stride_, alpha_, beta_, tc0_
    CHROMA_H_START_XMM r5, r6
    LOAD_8_ROWS PASS8ROWS(pix_q - 2, r5 - 2, stride_q, r6)
    TRANSPOSE_8x4B_XMM
    movq [rsp], m0
    movq [rsp + 8], m3
    CHROMA_INTER_BODY_XMM 2
    movq m0, [rsp]
    movq m3, [rsp + 8]
    TRANSPOSE_4x8B_XMM
    STORE_8_ROWS PASS8ROWS(pix_q - 2, r5 - 2, stride_q, r6)

    lea pix_q, [pix_q + 8*stride_q]
    lea r5,    [r5    + 8*stride_q]
    add tc0_q,  2

    LOAD_8_ROWS PASS8ROWS(pix_q - 2, r5 - 2, stride_q, r6)
    TRANSPOSE_8x4B_XMM
    movq [rsp], m0
    movq [rsp + 8], m3
    CHROMA_INTER_BODY_XMM 2
    movq m0, [rsp]
    movq m3, [rsp + 8]
    TRANSPOSE_4x8B_XMM
    STORE_8_ROWS PASS8ROWS(pix_q - 2, r5 - 2, stride_q, r6)
RET

cglobal deblock_v_chroma_intra_8, 4, 5, 8, pix_, stride_, alpha_, beta_
    CHROMA_V_START_XMM r4
    movq m0, [r4]
    movq m1, [r4 + stride_q]
    movq m2, [pix_q]
    movq m3, [pix_q + stride_q]
    CHROMA_INTRA_BODY_XMM
    movq [r4 + stride_q], m1
    movq [pix_q], m2
RET

cglobal deblock_h_chroma_intra_8, 4, 6, 8, pix_, stride_, alpha_, beta_
    CHROMA_H_START_XMM r4, r5
    LOAD_8_ROWS PASS8ROWS(pix_q - 2, r4 - 2, stride_q, r5)
    TRANSPOSE_8x4B_XMM
    CHROMA_INTRA_BODY_XMM
    TRANSPOSE_4x8B_XMM
    STORE_8_ROWS PASS8ROWS(pix_q - 2, r4 - 2, stride_q, r5)
RET

cglobal deblock_h_chroma422_intra_8, 4, 6, 8, pix_, stride_, alpha_, beta_
    CHROMA_H_START_XMM r4, r5
    LOAD_8_ROWS PASS8ROWS(pix_q - 2, r4 - 2, stride_q, r5)
    TRANSPOSE_8x4B_XMM
    CHROMA_INTRA_BODY_XMM
    TRANSPOSE_4x8B_XMM
    STORE_8_ROWS PASS8ROWS(pix_q - 2, r4 - 2, stride_q, r5)

    lea pix_q, [pix_q + 8*stride_q]
    lea r4,    [r4    + 8*stride_q]

    LOAD_8_ROWS PASS8ROWS(pix_q - 2, r4 - 2, stride_q, r5)
    TRANSPOSE_8x4B_XMM
    CHROMA_INTRA_BODY_XMM
    TRANSPOSE_4x8B_XMM
    STORE_8_ROWS PASS8ROWS(pix_q - 2, r4 - 2, stride_q, r5)
RET

%endmacro

DEBLOCK_CHROMA_XMM sse2
DEBLOCK_CHROMA_XMM avx

%macro loop_filter_strength_iteration 7

%define edgesd    %1
%define stepd     %2
%define mask_mvd  %3
%define dir       %4
%define d_idx     %5
%define mask_dir  %6
%define bidir     %7
    xor          b_idxd, b_idxd
%%.b_idx_loop:
%if mask_dir == 0
    pxor             m0, m0
%endif
    test         b_idxd, dword mask_mvd
    jnz %%.skip_loop_iter
%if bidir == 1
    movd             m2, [refq+b_idxq+d_idx+12]
    punpckldq        m2, [refq+b_idxq+d_idx+52]
    pshufw           m0, [refq+b_idxq+12], 0x44
    pshufw           m1, [refq+b_idxq+52], 0x44
    pshufw           m3, m2, 0x4E
    psubb            m0, m2

    psubb            m1, m3

    por              m0, m1
    mova             m1, [mvq+b_idxq*4+(d_idx+12)*4]
    mova             m2, [mvq+b_idxq*4+(d_idx+12)*4+mmsize]
    mova             m3, m1
    mova             m4, m2
    psubw            m1, [mvq+b_idxq*4+12*4]
    psubw            m2, [mvq+b_idxq*4+12*4+mmsize]
    psubw            m3, [mvq+b_idxq*4+52*4]
    psubw            m4, [mvq+b_idxq*4+52*4+mmsize]
    packsswb         m1, m2
    packsswb         m3, m4
    paddb            m1, m6
    paddb            m3, m6
    psubusb          m1, m5
    psubusb          m3, m5
    packsswb         m1, m3

    por              m0, m1
    mova             m1, [mvq+b_idxq*4+(d_idx+52)*4]
    mova             m2, [mvq+b_idxq*4+(d_idx+52)*4+mmsize]
    mova             m3, m1
    mova             m4, m2
    psubw            m1, [mvq+b_idxq*4+12*4]
    psubw            m2, [mvq+b_idxq*4+12*4+mmsize]
    psubw            m3, [mvq+b_idxq*4+52*4]
    psubw            m4, [mvq+b_idxq*4+52*4+mmsize]
    packsswb         m1, m2
    packsswb         m3, m4
    paddb            m1, m6
    paddb            m3, m6
    psubusb          m1, m5
    psubusb          m3, m5
    packsswb         m1, m3

    pshufw           m1, m1, 0x4E
    por              m0, m1
    pshufw           m1, m0, 0x4E
    pminub           m0, m1
%else
    movd             m0, [refq+b_idxq+12]
    psubb            m0, [refq+b_idxq+d_idx+12]

    mova             m1, [mvq+b_idxq*4+12*4]
    mova             m2, [mvq+b_idxq*4+12*4+mmsize]
    psubw            m1, [mvq+b_idxq*4+(d_idx+12)*4]
    psubw            m2, [mvq+b_idxq*4+(d_idx+12)*4+mmsize]
    packsswb         m1, m2
    paddb            m1, m6
    psubusb          m1, m5
    packsswb         m1, m1
    por              m0, m1
%endif

%%.skip_loop_iter:
    movd             m1, [nnzq+b_idxq+12]
    por              m1, [nnzq+b_idxq+d_idx+12]

    pminub           m1, m7
    pminub           m0, m7
    psllw            m1, 1
    pxor             m2, m2
    pmaxub           m1, m0
    punpcklbw        m1, m2
    movq [bsq+b_idxq+32*dir], m1

    add          b_idxd, dword stepd
    cmp          b_idxd, dword edgesd
    jl %%.b_idx_loop
%endmacro

INIT_MMX mmxext
cglobal h264_loop_filter_strength, 9, 9, 0, bs, nnz, ref, mv, bidir, edges, \
                                            step, mask_mv0, mask_mv1, field
%define b_idxq bidirq
%define b_idxd bidird
    cmp    dword fieldm, 0
    mova             m7, [pb_1]
    mova             m5, [pb_3]
    je .nofield
    mova             m5, [pb_3_1]
.nofield:
    mova             m6, m5
    paddb            m5, m5

    shl     dword stepd, 3
    shl    dword edgesd, 3
%if ARCH_X86_32
%define mask_mv0d mask_mv0m
%define mask_mv1d mask_mv1m
%endif
    shl dword mask_mv1d, 3
    shl dword mask_mv0d, 3

    cmp    dword bidird, 0
    jne .bidir
    loop_filter_strength_iteration edgesd, stepd, mask_mv1d, 1, -8,  0, 0
    loop_filter_strength_iteration     32,     8, mask_mv0d, 0, -1, -1, 0

    mova             m0, [bsq+mmsize*0]
    mova             m1, [bsq+mmsize*1]
    mova             m2, [bsq+mmsize*2]
    mova             m3, [bsq+mmsize*3]
    TRANSPOSE4x4W 0, 1, 2, 3, 4
    mova  [bsq+mmsize*0], m0
    mova  [bsq+mmsize*1], m1
    mova  [bsq+mmsize*2], m2
    mova  [bsq+mmsize*3], m3
    RET

.bidir:
    loop_filter_strength_iteration edgesd, stepd, mask_mv1d, 1, -8,  0, 1
    loop_filter_strength_iteration     32,     8, mask_mv0d, 0, -1, -1, 1

    mova             m0, [bsq+mmsize*0]
    mova             m1, [bsq+mmsize*1]
    mova             m2, [bsq+mmsize*2]
    mova             m3, [bsq+mmsize*3]
    TRANSPOSE4x4W 0, 1, 2, 3, 4
    mova  [bsq+mmsize*0], m0
    mova  [bsq+mmsize*1], m1
    mova  [bsq+mmsize*2], m2
    mova  [bsq+mmsize*3], m3
    RET

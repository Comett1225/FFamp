%include "x86util.asm"

SECTION .text

%macro op_avg 2
    pavgb  %1, %2
    mova   %2, %1
%endmacro

%macro op_put 2
    mova   %2, %1
%endmacro

%macro PIXELS_L2 2
%define OP op_%1

cglobal %1_pixels%2x%2_l2, 5,6,2
    mov         r5d, %2
.loop:
    movu         m0, [r1]
    movu         m1, [r1+r4]
    lea          r1, [r1+2*r4]
    pavgb        m0, [r2]
    pavgb        m1, [r2+mmsize]
    OP           m0, [r0]
    OP           m1, [r0+r3]
    lea          r0, [r0+2*r3]
    movu         m0, [r1]
    movu         m1, [r1+r4]
    lea          r1, [r1+2*r4]
    pavgb        m0, [r2+2*mmsize]
    pavgb        m1, [r2+3*mmsize]
    OP           m0, [r0]
    OP           m1, [r0+r3]
    lea          r0, [r0+2*r3]
    add          r2, 4*mmsize
    sub         r5d, 4
    jne       .loop
    RET
%endmacro

INIT_MMX mmxext
PIXELS_L2 put, 8
PIXELS_L2 avg, 8

INIT_XMM sse2
PIXELS_L2 put, 16
PIXELS_L2 avg, 16

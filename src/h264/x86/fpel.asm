%include "x86util.asm"

SECTION .text

INIT_XMM sse2

cglobal avg_pixels8x8, 3,5,6
    mov         r3d, 8
    jmp         avg_pixels8_after_prologue

cglobal avg_pixels8, 4,5,6
avg_pixels8_after_prologue:
    lea          r4, [r2*3]
.loop:
    movq         m0, [r1]
    movq         m1, [r0]
    movhps       m0, [r1+r2]
    movhps       m1, [r0+r2]
    movq         m2, [r1+r2*2]
    movq         m3, [r0+r2*2]
    pavgb        m0, m1
    movq         m4, [r1+r4]
    pavgb        m2, m3
    movq         m5, [r0+r4]
    lea          r1, [r1+r2*4]
    pavgb        m4, m5
    movq       [r0], m0
    movhps  [r0+r2], m0
    movq  [r0+r2*2], m2
    movq    [r0+r4], m4
    lea          r0, [r0+r2*4]
    sub         r3d, 4
    jne       .loop
    RET

%macro OP_PIXELS 2-3 0
%if %2 == mmsize/2
%define LOAD movh
%define SAVE movh
%else
%define LOAD movu
%define SAVE mova
%endif
cglobal %1_pixels%2x%2, 3,5+4*%3,4
    mov         r3d, %2
    jmp         %1_pixels%2_after_prologue

cglobal %1_pixels%2, 4,5+4*%3,4
%1_pixels%2_after_prologue:
    lea          r4, [r2*3]
.loop:
%if %3

    mov         r5q, [r1]
    mov         r6q, [r1+r2]
    mov         r7q, [r1+r2*2]
    mov         r8q, [r1+r4]
    mov        [r0], r5q
    mov     [r0+r2], r6q
    mov   [r0+r2*2], r7q
    mov     [r0+r4], r8q
%else
    LOAD         m0, [r1]
    LOAD         m1, [r1+r2]
    LOAD         m2, [r1+r2*2]
    LOAD         m3, [r1+r4]
%ifidn %1, avg
    pavgb        m0, [r0]
    pavgb        m1, [r0+r2]
    pavgb        m2, [r0+r2*2]
    pavgb        m3, [r0+r4]
%endif
    SAVE       [r0], m0
    SAVE    [r0+r2], m1
    SAVE  [r0+r2*2], m2
    SAVE    [r0+r4], m3
%endif
    sub         r3d, 4
    lea          r1, [r1+r2*4]
    lea          r0, [r0+r2*4]
    jne       .loop
    RET
%endmacro

OP_PIXELS put, 8, UNIX64
OP_PIXELS put, 16
OP_PIXELS avg, 16

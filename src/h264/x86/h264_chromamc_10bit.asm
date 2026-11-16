%include "x86util.asm"

SECTION_RODATA

cextern pw_4
cextern pw_8
cextern pw_32
cextern pw_64

SECTION .text

%macro MV0_PIXELS_MC8 0
    lea           r4, [r2*3   ]
    lea           r5, [r2*4   ]
.next4rows:
    movu          m0, [r1     ]
    movu          m1, [r1+r2  ]
    CHROMAMC_AVG  m0, [r0     ]
    CHROMAMC_AVG  m1, [r0+r2  ]
    mova   [r0     ], m0
    mova   [r0+r2  ], m1
    movu          m0, [r1+r2*2]
    movu          m1, [r1+r4  ]
    CHROMAMC_AVG  m0, [r0+r2*2]
    CHROMAMC_AVG  m1, [r0+r4  ]
    mova   [r0+r2*2], m0
    mova   [r0+r4  ], m1
    add           r1, r5
    add           r0, r5
    sub          r3d, 4
    jne .next4rows
%endmacro

%macro CHROMA_MC8 1
cglobal %1_h264_chroma_mc8_10, 6,7,8
    mov          r6d, r5d
    or           r6d, r4d
    jne .at_least_one_non_zero

    MV0_PIXELS_MC8
    RET

.at_least_one_non_zero:
    mov          r6d, 2
    test         r5d, r5d
    je .x_interpolation
    mov           r6, r2
    test         r4d, r4d
    jne .xy_interpolation
.x_interpolation:

    or           r4d, r5d
    movd          m5, r4d
    mova          m4, [pw_8]
    mova          m6, [pw_4]
    SPLATW        m5, m5
    psubw         m4, m5

.next1drow:
    movu          m0, [r1   ]
    movu          m2, [r1+r6]

    pmullw        m0, m4
    pmullw        m2, m5

    paddw         m0, m6
    paddw         m0, m2
    psrlw         m0, 3
    CHROMAMC_AVG  m0, [r0]
    mova        [r0], m0

    add           r0, r2
    add           r1, r2
    dec           r3d
    jne .next1drow
    RET

.xy_interpolation:
    movd          m4, r4m
    movd          m6, r5m

    SPLATW        m4, m4
    SPLATW        m6, m6
    psllw         m5, m4, 3
    pmullw        m4, m6
    psllw         m6, 3
    paddw         m1, m5, m6
    mova          m7, m4
    psubw         m5, m4
    psubw         m6, m4
    paddw         m4, [pw_64]
    psubw         m4, m1

    movu          m0, [r1  ]
    movu          m1, [r1+2]
.next2drow:
    add           r1, r2

    pmullw        m2, m0, m4
    pmullw        m1, m5
    paddw         m2, m1

    movu          m0, [r1]
    movu          m1, [r1+2]
    pmullw        m3, m0, m6
    paddw         m2, m3
    pmullw        m3, m1, m7
    paddw         m2, m3

    paddw         m2, [pw_32]
    psrlw         m2, 6
    CHROMAMC_AVG  m2, [r0]
    mova        [r0], m2

    add           r0, r2
    dec          r3d
    jne .next2drow
    RET
%endmacro

%macro MC4_OP 2
    movq          %1, [r1  ]
    movq          m1, [r1+2]
    add           r1, r2
    pmullw        %1, m4
    pmullw        m1, m2
    paddw         m1, %1
    mova          %1, m1

    pmullw        %2, m5
    pmullw        m1, m3
    paddw         %2, [pw_32]
    paddw         m1, %2
    psrlw         m1, 6
    CHROMAMC_AVG  m1, %2, [r0]
    movq        [r0], m1
    add           r0, r2
%endmacro

%macro CHROMA_MC4 1
cglobal %1_h264_chroma_mc4_10, 6,6,7
    movd          m2, r4m
    movd          m3, r5m
    mova          m4, [pw_8]
    mova          m5, m4
    SPLATW        m2, m2
    SPLATW        m3, m3
    psubw         m4, m2
    psubw         m5, m3

    movq          m0, [r1  ]
    movq          m6, [r1+2]
    add           r1, r2
    pmullw        m0, m4
    pmullw        m6, m2
    paddw         m6, m0

.next2rows:
    MC4_OP m0, m6
    MC4_OP m6, m0
    sub   r3d, 2
    jnz .next2rows
    RET
%endmacro

%macro CHROMA_MC2 1
cglobal %1_h264_chroma_mc2_10, 6,7
    mov          r6d, r4d
    shl          r4d, 16
    sub          r4d, r6d
    add          r4d, 8
    imul         r5d, r4d
    shl          r4d, 3
    sub          r4d, r5d

    movd          m5, r4d
    movd          m6, r5d
    punpckldq     m5, m5
    punpckldq     m6, m6
    pxor          m7, m7
    pshufw        m2, [r1], 0x94

.nextrow:
    add           r1, r2
    movq          m1, m2
    pmaddwd       m1, m5
    pshufw        m0, [r1], 0x94
    movq          m2, m0
    pmaddwd       m0, m6
    paddw         m1, [pw_32]
    paddw         m1, m0
    psrlw         m1, 6
    packssdw      m1, m7
    CHROMAMC_AVG  m1, m3, [r0]
    movd        [r0], m1
    add           r0, r2
    dec          r3d
    jnz .nextrow
    RET
%endmacro

%macro NOTHING 2-3
%endmacro
%macro AVG 2-3
%if %0==3
    movq          %2, %3
%endif
    pavgw         %1, %2
%endmacro

%define CHROMAMC_AVG  NOTHING
INIT_XMM sse2
CHROMA_MC8 put
%if HAVE_AVX_EXTERNAL
INIT_XMM avx
CHROMA_MC8 put
%endif
INIT_MMX mmxext
CHROMA_MC4 put
CHROMA_MC2 put

%define CHROMAMC_AVG  AVG
INIT_XMM sse2
CHROMA_MC8 avg
%if HAVE_AVX_EXTERNAL
INIT_XMM avx
CHROMA_MC8 avg
%endif
INIT_MMX mmxext
CHROMA_MC4 avg
CHROMA_MC2 avg

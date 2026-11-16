%include "x86util.asm"

SECTION .text

%macro V_COPY_ROW 2
.%1_y_loop:
    mov              wq, r7mp
.%1_x_loop:
    movu             m0, [srcq+wq]
    movu      [dstq+wq], m0
    add              wq, mmsize
    cmp              wq, -mmsize
    jl .%1_x_loop
    movu             m0, [srcq-mmsize]
    movu  [dstq-mmsize], m0
%ifidn %1, body
    add            srcq, src_strideq
%endif
    add            dstq, dst_strideq
    dec              %2
    jnz .%1_y_loop
%endmacro

INIT_XMM sse
%if ARCH_X86_64
cglobal emu_edge_vvar, 7, 8, 1, dst, dst_stride, src, src_stride, \
                                start_y, end_y, bh, w
%else
cglobal emu_edge_vvar, 1, 6, 1, dst, src, start_y, end_y, bh, w
%define src_strideq r3mp
%define dst_strideq r1mp
    mov            srcq, r2mp
    mov        start_yq, r4mp
    mov          end_yq, r5mp
    mov             bhq, r6mp
%endif
    sub             bhq, end_yq
    sub          end_yq, start_yq
    add            srcq, r7mp
    add            dstq, r7mp
    neg            r7mp
    test       start_yq, start_yq
    jz .body
    V_COPY_ROW      top, start_yq
.body:
    V_COPY_ROW     body, end_yq
    test            bhq, bhq
    jz .end
    sub            srcq, src_strideq
    V_COPY_ROW   bottom, bhq
.end:
    RET

%macro hvar_fn 0
cglobal emu_edge_hvar, 5, 6, 1, dst, dst_stride, start_x, n_words, h, w
    lea            dstq, [dstq+n_wordsq*2]
    neg        n_wordsq
    lea        start_xq, [start_xq+n_wordsq*2]
.y_loop:
%if cpuflag(avx2)
    vpbroadcastb     m0, [dstq+start_xq]
    mov              wq, n_wordsq
%else
    movzx            wd, byte [dstq+start_xq]
    imul             wd, 0x01010101
    movd             m0, wd
    mov              wq, n_wordsq
    pshufd           m0, m0, q0000
%endif
.x_loop:
    movu    [dstq+wq*2], m0
    add              wq, mmsize/2
    cmp              wq, -(mmsize/2)
    jl .x_loop
    movu  [dstq-mmsize], m0
    add            dstq, dst_strideq
    dec              hq
    jnz .y_loop
    RET
%endmacro

INIT_XMM sse2
hvar_fn

%if HAVE_AVX2_EXTERNAL
INIT_XMM avx2
hvar_fn
%endif

%macro READ_NUM_BYTES 2
%assign %%off 0
%assign %%xmm_idx 0

%rep %2/mmsize
    movu   xmm %+ %%xmm_idx, [srcq+%%off]
%assign %%xmm_idx %%xmm_idx+1
%assign %%off %%off+mmsize
%endrep

%if (%2-%%off) >= 8
%if %2 > 16 && (%2-%%off) > 8
    movu   xmm %+ %%xmm_idx, [srcq+%2-16]
%assign %%xmm_idx %%xmm_idx+1
%assign %%off %2
%else
    movq   xmm %+ %%xmm_idx, [srcq+%%off]
%assign %%xmm_idx %%xmm_idx+1
%assign %%off %%off+8
%endif
%endif

%if (%2-%%off) >= 4
%if %2 > 8 && (%2-%%off) > 4
    movq   xmm %+ %%xmm_idx, [srcq+%2-8]
%assign %%off %2
%else
    movd   xmm %+ %%xmm_idx, [srcq+%%off]
%assign %%off %%off+4
%endif
%assign %%xmm_idx %%xmm_idx+1
%endif

%if (%2-%%off) >= 1
%if %2 >= 4
    movd xmm %+ %%xmm_idx, [srcq+%2-4]
%elif (%2-%%off) == 1
    mov            valb, [srcq+%2-1]
%elif (%2-%%off) == 2
    mov            valw, [srcq+%2-2]
%else
    mov            valb, [srcq+%2-1]
    ror            vald, 16
    mov            valw, [srcq+%2-3]
%endif
%endif
%endmacro

%macro WRITE_NUM_BYTES 2
%assign %%off 0
%assign %%xmm_idx 0

%rep %2/mmsize
    movu   [dstq+%%off], xmm %+ %%xmm_idx
%assign %%xmm_idx %%xmm_idx+1
%assign %%off %%off+mmsize
%endrep

%if (%2-%%off) >= 8
%if %2 > 16 && (%2-%%off) > 8
    movu   [dstq+%2-16], xmm %+ %%xmm_idx
%assign %%xmm_idx %%xmm_idx+1
%assign %%off %2
%else
    movq   [dstq+%%off], xmm %+ %%xmm_idx
%assign %%xmm_idx %%xmm_idx+1
%assign %%off %%off+8
%endif
%endif

%if (%2-%%off) >= 4
%if %2 > 8 && (%2-%%off) > 4
    movq    [dstq+%2-8], xmm %+ %%xmm_idx
%assign %%off %2
%else
    movd   [dstq+%%off], xmm %+ %%xmm_idx
%assign %%off %%off+4
%endif
%assign %%xmm_idx %%xmm_idx+1
%endif

%if (%2-%%off) >= 1
%if %2 >= 4
    movd    [dstq+%2-4], xmm %+ %%xmm_idx
%elif (%2-%%off) == 1
    mov     [dstq+%2-1], valb
%elif (%2-%%off) == 2
    mov     [dstq+%2-2], valw
%else
    mov     [dstq+%2-3], valw
    ror            vald, 16
    mov     [dstq+%2-1], valb
%ifnidn %1, body
    ror            vald, 16
%endif
%endif
%endif
%endmacro

%macro VERTICAL_EXTEND 2
%assign %%n %1
%rep 1+%2-%1
%if %%n <= 3
%if ARCH_X86_64
cglobal emu_edge_vfix %+ %%n, 6, 8, 0, dst, dst_stride, src, src_stride, \
                                       start_y, end_y, val, bh
    mov             bhq, r6mp
%else
cglobal emu_edge_vfix %+ %%n, 0, 6, 0, val, dst, src, start_y, end_y, bh
    mov            dstq, r0mp
    mov            srcq, r2mp
    mov        start_yq, r4mp
    mov          end_yq, r5mp
    mov             bhq, r6mp
%define dst_strideq r1mp
%define src_strideq r3mp
%endif
%else
%if ARCH_X86_64
cglobal emu_edge_vfix %+ %%n, 7, 7, 1, dst, dst_stride, src, src_stride, \
                                       start_y, end_y, bh
%else
cglobal emu_edge_vfix %+ %%n, 1, 5, 1, dst, src, start_y, end_y, bh
    mov            srcq, r2mp
    mov        start_yq, r4mp
    mov          end_yq, r5mp
    mov             bhq, r6mp
%define dst_strideq r1mp
%define src_strideq r3mp
%endif
%endif

    sub             bhq, end_yq
    sub          end_yq, start_yq

    test       start_yq, start_yq
    jz .body_loop
    READ_NUM_BYTES  top, %%n
.top_loop:
    WRITE_NUM_BYTES top, %%n
    add            dstq, dst_strideq
    dec        start_yq
    jnz .top_loop

.body_loop:
    READ_NUM_BYTES  body, %%n
    WRITE_NUM_BYTES body, %%n
    add            dstq, dst_strideq
    add            srcq, src_strideq
    dec          end_yq
    jnz .body_loop

    test            bhq, bhq
    jz .end
    sub            srcq, src_strideq
    READ_NUM_BYTES  bottom, %%n
.bottom_loop:
    WRITE_NUM_BYTES bottom, %%n
    add            dstq, dst_strideq
    dec             bhq
    jnz .bottom_loop

.end:
    RET
%assign %%n %%n+1
%endrep
%endmacro

INIT_XMM sse2
VERTICAL_EXTEND 1, 22

%macro READ_V_PIXEL 2
%if cpuflag(avx2)
    vpbroadcastb     m0, %2
%else
    movzx          vald, byte %2
    imul           vald, 0x01010101
%if %1 >= 8
    movd             m0, vald
    pshufd           m0, m0, q0000
%endif
%endif
%endmacro

%macro WRITE_V_PIXEL 2
%assign %%off 0

%if %1 >= 8

%rep %1/mmsize
    movu     [%2+%%off], m0
%assign %%off %%off+mmsize
%endrep

%if %1-%%off >= 8
%if %1 > 16 && %1-%%off > 8
    movu     [%2+%1-16], m0
%assign %%off %1
%else
    movq     [%2+%%off], m0
%assign %%off %%off+8
%endif
%endif

%if %1-%%off >= 4
%if %1 > 8 && %1-%%off > 4
    movq      [%2+%1-8], m0
%assign %%off %1
%else
    movd     [%2+%%off], m0
%assign %%off %%off+4
%endif
%endif

%else

%rep %1/4
    mov      [%2+%%off], vald
%assign %%off %%off+4
%endrep

%endif

%if %1-%%off == 2
%if cpuflag(avx2)
    movd     [%2+%%off-2], m0
%else
    mov      [%2+%%off], valw
%endif
%endif
%endmacro

%macro H_EXTEND 2
%assign %%n %1
%rep 1+(%2-%1)/2
%if cpuflag(avx2)
cglobal emu_edge_hfix %+ %%n, 4, 4, 1, dst, dst_stride, start_x, bh
%else
cglobal emu_edge_hfix %+ %%n, 4, 5, 1, dst, dst_stride, start_x, bh, val
%endif
.loop_y:
    READ_V_PIXEL    %%n, [dstq+start_xq]
    WRITE_V_PIXEL   %%n, dstq
    add            dstq, dst_strideq
    dec             bhq
    jnz .loop_y
    RET
%assign %%n %%n+2
%endrep
%endmacro

INIT_XMM sse2
H_EXTEND 2, 22

%if HAVE_AVX2_EXTERNAL
INIT_XMM avx2
H_EXTEND 8, 22
%endif

INIT_MMX mmxext
cglobal prefetch, 3, 3, 0, buf, stride, h
.loop:
    prefetcht0 [bufq]
    add      bufq, strideq
    dec        hd
    jg .loop
    RET

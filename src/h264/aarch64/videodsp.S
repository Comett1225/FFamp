#include "asm.S"

function ff_prefetch_aarch64, export=1
1:
        subs            w2,  w2,  #2
        prfm            pldl1strm, [x0]
        prfm            pldl1strm, [x0,  x1]
        add             x0,  x0,  x1,  lsl #1
        b.gt            1b
        ret
endfunc

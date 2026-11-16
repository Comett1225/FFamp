#include "ffbase.h"
#include "startcode.h"

int ff_startcode_find_candidate_c(const uint8_t *buf, int size)
{
    int i = 0;
#if HAVE_FAST_UNALIGNED

#if HAVE_FAST_64BIT
    while (i < size &&
            !((~AV_RN64(buf + i) &
                    (AV_RN64(buf + i) - 0x0101010101010101ULL)) &
                    0x8080808080808080ULL))
        i += 8;
#else
    while (i < size &&
            !((~AV_RN32(buf + i) &
                    (AV_RN32(buf + i) - 0x01010101U)) &
                    0x80808080U))
        i += 4;
#endif
#endif
    for (; i < size; i++)
        if (!buf[i])
            break;
    return i;
}

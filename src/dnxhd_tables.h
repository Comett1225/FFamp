#pragma once
#include <cstdint>

namespace absent::dnxhd {

struct CidEntry {
    int cid;
    unsigned width, height;
    unsigned frameSize;
    unsigned codingUnitSize;
    uint16_t flags;
    int indexBits;
    int bitDepth;
    int eobIndex;
    const uint8_t *lumaWeight, *chromaWeight;
    const uint8_t *dcCodes, *dcBits;
    const uint16_t* acCodes;
    const uint8_t *acBits, *acInfo;
    const uint16_t* runCodes;
    const uint8_t *runBits, *run;
};

const CidEntry* cidEntry(int cid);

}

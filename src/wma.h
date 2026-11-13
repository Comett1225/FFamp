#pragma once
#include "bits.h"
#include "vlc.h"
#include <vector>

namespace absent::wma {

struct CoefTable {
    int count;
    int levelCount;
    const uint32_t* codes;
    const uint8_t* lengths;
    const uint16_t* levels;
};

struct RunLevel {
    Vlc vlc;
    std::vector<float> levels;
    std::vector<uint16_t> runs;
};

extern const uint16_t kCriticalFrequencies[25];
extern const uint8_t kExponentBands[3][3][25];
extern const uint8_t kHighGainSymbols[37];
extern const uint8_t kHighGainLengths[37];
extern const float kLspCodebook[10][16];
extern const uint32_t kScalefactorCodes[121];
extern const uint8_t kScalefactorLengths[121];
extern const float kPowers[156];
extern const CoefTable kCoefTables[6];

int frameLengthBits(int rate, int version, unsigned flags);
uint32_t largeValue(BitReader& in);
const RunLevel& coefficientTable(int index);
bool runLevel(BitReader& in, const RunLevel& table, int version, float* out, int offset, int count, int blockLength, int frameBits, int escapeBits);

}

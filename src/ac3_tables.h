#pragma once
#include <cstdint>

namespace absent::ac3 {

extern const uint16_t kFrameSizes[38][3];
extern const uint8_t kChannels[8];
extern const uint8_t kChannelMap[8][2][6];
extern const int kSampleRates[4];
extern const uint8_t kRematrixBands[5];
extern const uint8_t kDefaultCouplingBands[18];
extern const uint8_t kBapTable[64];
extern const uint8_t kSlowDecay[4];
extern const uint8_t kFastDecay[4];
extern const uint16_t kSlowGain[4];
extern const uint16_t kDbPerBit[4];
extern const int16_t kFloors[8];
extern const uint16_t kFastGain[8];
extern const uint64_t kCustomChannels[16][2];
extern const uint16_t kLayouts[8];
extern const uint8_t kBandStart[51];
extern const uint8_t kBinToBand[253];
extern const uint8_t kLogAdd[260];
extern const uint16_t kHearingThreshold[50][3];
extern const uint8_t kUngroup3In5[32][3];
extern const uint8_t kQuantizationBits[16];
extern const uint8_t kHighBapTable[64];
extern const uint8_t kDefaultSpxBands[17];
extern const uint8_t kBitsForHighBap[20];
extern const int16_t kGaqRemap1[12];
extern const int16_t kGaqRemapA[9][2];
extern const int16_t kGaqRemapB[9][2];
extern const int16_t (*const kMantissaVq[8])[6];
extern const uint8_t kFrameExponentStrategies[32][6];
extern const float kSpxAttenuation[32][3];
extern const uint32_t kDitherSeed[64];

}

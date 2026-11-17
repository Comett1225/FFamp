#pragma once
#include <cstdint>

namespace absent::dts {

extern const int16_t kAdpcmVb[4096][4];
extern const uint32_t kScaleQuant6[64];
extern const uint32_t kScaleQuant7[128];
extern const uint32_t kJointScales[129];
extern const uint32_t kScaleAdjust[4];
extern const uint32_t kQuantLevels[32];
extern const uint32_t kLossyQuant[32];
extern const uint32_t kLosslessQuant[32];
extern const int8_t kHighFreqVq[1024][32];
extern const float kFirPerfect[512];
extern const float kFirNonperfect[512];
extern const float kLfeFir64[256];
extern const float kLfeFir128[256];
extern const uint16_t kDmixTable[242];
extern const uint32_t kInvDmixTable[201];
extern const uint8_t kVlcSource[2709][2];

}

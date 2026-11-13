#pragma once
#include <cstdint>

namespace absent::vc1 {

extern const uint8_t kImodeCodes[7];
extern const uint8_t kImodeBits[7];
extern const uint8_t kNorm2Codes[4];
extern const uint8_t kNorm2Bits[4];
extern const uint16_t kNorm6Codes[64];
extern const uint8_t kNorm6Bits[64];
extern const uint8_t k4mvBlockPatternCodes[4][16];
extern const uint8_t k4mvBlockPatternBits[4][16];
extern const uint8_t k2mvBlockPatternCodes[4][4];
extern const uint8_t k2mvBlockPatternBits[4][4];
extern const uint16_t kIntfr4mvMbmodeCodes[4][15];
extern const uint8_t kIntfr4mvMbmodeBits[4][15];
extern const uint8_t kIntfrNon4mvMbmodeCodes[4][9];
extern const uint8_t kIntfrNon4mvMbmodeBits[4][9];
extern const uint8_t kIfMmvMbmodeCodes[8][8];
extern const uint8_t kIfMmvMbmodeBits[8][8];
extern const uint8_t kIf1mvMbmodeCodes[8][6];
extern const uint8_t kIf1mvMbmodeBits[8][6];
extern const uint32_t k1refMvdataCodes[4][72];
extern const uint8_t k1refMvdataBits[4][72];
extern const uint32_t k2refMvdataCodes[8][126];
extern const uint8_t k2refMvdataBits[8][126];
extern const uint16_t kCbpcyPCodes[4][64];
extern const uint8_t kCbpcyPBits[4][64];
extern const uint16_t kIcbpcyPCodes[8][63];
extern const uint8_t kIcbpcyPBits[8][63];
extern const uint16_t kTtmbCodes[3][16];
extern const uint8_t kTtmbBits[3][16];
extern const uint8_t kTtblkCodes[3][8];
extern const uint8_t kTtblkBits[3][8];
extern const uint8_t kSubblkpatCodes[3][15];
extern const uint8_t kSubblkpatBits[3][15];
extern const uint16_t kMvDiffCodes[4][73];
extern const uint8_t kMvDiffBits[4][73];
extern const uint32_t kAcTables[8][186][2];
extern const int kAcSizes[8];
extern const int kLastDecodeTable[8];
extern const uint8_t kIndexDecodeTable[8][185][2];
extern const uint8_t kDeltaLevelTable[8][31];
extern const uint8_t kLastDeltaLevelTable[8][44];
extern const uint8_t kDeltaRunTable[8][57];
extern const uint8_t kLastDeltaRunTable[8][10];
extern const uint8_t kPquantTable[3][32];
extern const uint8_t kWmv3DcScaleTable[32];
extern const uint8_t kSimpleProgressive4x4Zz[16];
extern const uint8_t kAdvProgressive8x4Zz[32];
extern const uint8_t kAdvProgressive4x8Zz[32];
extern const uint8_t kAdvInterlaced8x8Zz[64];
extern const uint8_t kAdvInterlaced8x4Zz[32];
extern const uint8_t kAdvInterlaced4x8Zz[32];
extern const uint8_t kAdvInterlaced4x4Zz[16];
extern const int32_t kDqscale[63];
extern const uint16_t kFieldMvpredScales[2][7][4];
extern const uint16_t kBFieldMvpredScales[7][4];
extern const uint16_t kMbITable[64][2];
extern const uint32_t kDcTables[2][2][120][2];
extern const uint8_t kWmv1Scantable[4][64];
extern const uint8_t kWmv2ScantableA[64];
extern const uint8_t kWmv2ScantableB[64];

}

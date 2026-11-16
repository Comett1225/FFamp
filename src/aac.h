#pragma once
#include "bits.h"
#include "mdct.h"
#include "vlc.h"
#include <cstdint>
#include <cstring>

namespace absent::aac {

extern const uint8_t kNumSwb1024[13];
extern const uint8_t kNumSwb128[13];
extern const uint32_t kScalefactorCode[121];
extern const uint8_t kScalefactorBits[121];
extern const uint16_t* const kSpectralCodes[11];
extern const uint8_t* const kSpectralBits[11];
extern const uint16_t kSpectralSizes[11];
extern const float* const kVectorValues[11];
extern const uint16_t* const kVectorIndex[11];
extern const uint16_t* const kSwbOffset1024[13];
extern const uint16_t* const kSwbOffset128[13];
extern const uint8_t kTnsMaxBands1024[13];
extern const uint8_t kTnsMaxBands128[13];
extern const float* const kTnsMap[4];
extern const int8_t kTagsPerConfig[16];
extern const uint8_t kSbrHuffman[604][2];
extern const uint8_t kSbrHuffmanCounts[10];
extern const int8_t kSbrHuffmanOffsets[10];
extern const int8_t kSbrOffset[6][16];
extern const float kQmfWindowDown[320];
extern const float kQmfWindowUp[640];
extern const float kSbrNoise[520][2];
extern const uint8_t kPsHuffmanSizes[10];
extern const uint8_t kPsHuffman[242][2];
extern const int8_t kPsHuffmanOffsets[10];
extern const int8_t kKToI20[71];
extern const int8_t kKToI34[91];

enum ElementType { kSce = 0, kCpe, kCce, kLfe, kDse, kPce, kFil, kEnd };

struct StreamConfig {
    int objectType = 0;
    int samplingIndex = 0;
    int sampleRate = 0;
    int channelConfig = 0;
    int sbr = -1;
    int ps = -1;
    int extSampleRate = 0;
    int frameLengthShort = 0;
};

struct PsCommon {
    int start, enableIid, iidQuant, nrIidPar, nrIpdopdPar, enableIcc, iccMode, nrIccPar, enableExt, frameClass;
    int numEnvOld, numEnv, enableIpdopd;
    int borderPosition[6];
    int8_t iidPar[5][34], iccPar[5][34], ipdPar[5][34], opdPar[5][34];
    int is34bands, is34bandsOld;
};

struct Ps {
    PsCommon common;
    alignas(16) float inBuf[5][44][2];
    alignas(16) float delay[91][46][2];
    alignas(16) float apDelay[50][3][37][2];
    float peakDecayNrg[34], powerSmooth[34], peakDecayDiffSmooth[34];
    float H11[2][6][34], H12[2][6][34], H21[2][6][34], H22[2][6][34];
    int8_t opdHist[34], ipdHist[34];
};

struct PsScratch {
    alignas(16) float Lbuf[91][32][2];
    alignas(16) float Rbuf[91][32][2];
};

struct SpectrumParameters {
    uint8_t startFreq, stopFreq, xoverBand, freqScale, alterScale, noiseBands;
};

struct SbrData {
    unsigned frameClass, addHarmonicFlag;
    int numEnv;
    uint8_t freqRes[9];
    int numNoise;
    uint8_t dfEnv[9], dfNoise[2], invfMode[2][5], addHarmonic[48];
    unsigned ampRes;
    alignas(32) float synthesis[(1280 - 128) * 2];
    alignas(32) float analysis[1312];
    int synthesisOffset;
    int ea[2];
    float bwArray[5];
    float W[2][32][32][2];
    int Ypos;
    alignas(16) float Y[2][38][64][2];
    float gTemp[42][48], qTemp[42][48];
    uint8_t sIndexMapped[9][48];
    uint8_t envFacsQ[9][48];
    float envFacs[9][48];
    uint8_t noiseFacsQ[3][5];
    float noiseFacs[3][5];
    uint8_t tEnv[9];
    uint8_t tEnvNumEnvOld;
    uint8_t tQ[3];
    unsigned fIndexNoise, fIndexSine;
};

struct Sbr {
    int sampleRate, start, readyForDequant, idAac, reset;
    SpectrumParameters spectrum;
    int ampResHeader;
    int limiterBands, limiterGains, interpolFreq, smoothingMode, coupling;
    int k[5], kx[2], m[2];
    int kxAndMPushed;
    int nMaster;
    SbrData data[2];
    Ps ps;
    int n[2], nQ, nLim;
    uint16_t fMaster[49], fTableLow[25], fTableHigh[49], fTableNoise[6], fTableLim[30];
    int numPatches;
    uint8_t patchNumSubbands[6], patchStartSubband[6];
};

struct SbrScratch {
    alignas(16) float Xlow[32][40][2];
    alignas(16) float Xhigh[64][40][2];
    alignas(16) float X[2][2][38][64];
    alignas(16) float alpha0[64][2];
    alignas(16) float alpha1[64][2];
    float eOrigMapped[8][48], qMapped[8][48];
    uint8_t sMapped[8][48];
    float eCurr[8][48], qM[8][48], sM[8][48], gain[8][48];
    PsScratch ps;
};

struct SbrTools {
    PreciseMdct synthesis{6, 1.0 / (64 * 32768)};
    PreciseMdct analysis{6, -2.0 * 32768};
    SbrScratch scratch;
};

void sbrInit(Sbr& sbr, int idAac);
void sbrTurnoff(Sbr& sbr);
int sbrDecodeExtension(Sbr& sbr, StreamConfig& config, BitReader& host, int crc, int cnt, int idAac);
void sbrApply(Sbr& sbr, SbrTools& tools, const StreamConfig& config, int idAac, float* left, float* right);

bool fastFma();
float fmaHardware(float a, float b, float c);

inline float mulAdd(float a, float b, float c) {
    if (fastFma()) return fmaHardware(a, b, c);
    const float product = a * b;
    return product + c;
}

inline void vectorFmulWindow(float* dst, const float* src0, const float* src1, const float* win, int len) {
    dst += len;
    win += len;
    src0 += len;
    for (int i = -len, j = len - 1; i < 0; i++, j--) {
        const float s0 = src0[i], s1 = src1[j], wi = win[i], wj = win[j];
        dst[i] = s0 * wj - s1 * wi;
        dst[j] = s0 * wi + s1 * wj;
    }
}

}

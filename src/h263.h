#pragma once
#include "vc1.h"
#include "intrax8.h"

namespace absent::h263 {

using vc1::Bits;
using vc1::Frame;
using vc1::Pic;
using vc1::clipU8;
using vc1::midPred;

extern const uint8_t kIntraMcbpcCode[9];
extern const uint8_t kIntraMcbpcBits[9];
extern const uint8_t kInterMcbpcCode[28];
extern const uint8_t kInterMcbpcBits[28];
extern const uint8_t kCbpyTab[16][2];
extern const uint8_t kMvTab[33][2];
extern const uint16_t kInterVlc[103][2];
extern const int8_t kInterLevel[102];
extern const int8_t kInterRun[102];
extern const uint8_t kLoopFilterStrength[32];
extern const uint8_t kDcTabLum[13][2];
extern const uint8_t kDcTabChrom[13][2];
extern const uint16_t kMpeg4IntraVlc[103][2];
extern const int8_t kMpeg4IntraLevel[102];
extern const int8_t kMpeg4IntraRun[102];
extern const uint8_t kAlternateHorizontalScan[64];
extern const uint8_t kAlternateVerticalScan[64];
extern const uint8_t kZigzagDirect[64];
extern const uint32_t kMbNonIntra1[128][2];
extern const uint32_t kMbNonIntra2[128][2];
extern const uint32_t kMbNonIntra3[128][2];
extern const uint32_t kMbNonIntra4[128][2];
extern const uint16_t kTable0Vlc[133][2];
extern const int8_t kTable0Level[132];
extern const int8_t kTable0Run[132];
extern const uint16_t kTable1Vlc[149][2];
extern const int8_t kTable1Level[148];
extern const int8_t kTable1Run[148];
extern const uint16_t kTable2Vlc[186][2];
extern const int8_t kTable2Level[185];
extern const int8_t kTable2Run[185];
extern const uint16_t kTable4Vlc[169][2];
extern const int8_t kTable4Level[168];
extern const int8_t kTable4Run[168];
extern const uint16_t kMvTable0[1100];
extern const uint8_t kMvTable0Lens[1100];
extern const uint16_t kMvTable1[1100];
extern const uint8_t kMvTable1Lens[1100];
extern const uint8_t kV2MbType[8][2];
extern const uint8_t kV2IntraCbpc[4][2];
extern const uint8_t kWmv1YDcScale[32];
extern const uint8_t kWmv1CDcScale[32];
extern const uint8_t kOldYDcScale[32];
extern const uint8_t kInterIntra[4][2];
extern const uint8_t kX8OrientLowquant[4][12][2];
extern const uint8_t kX8OrientHighquant[2][12][2];
extern const uint8_t kX8DcQuant[2][8][34][2];
extern const uint8_t kX8AcQuant[2][2][8][77][2];

constexpr int kPictureI = 1, kPictureP = 2;
constexpr int kMsmpeg4v1 = 1, kMsmpeg4v2 = 2, kMsmpeg4v3 = 3, kWmv1 = 4, kWmv2 = 5;
constexpr uint32_t kMbSkip = 1;

using IdctOp = void (*)(uint8_t* dest, ptrdiff_t stride, int16_t* block);
using PixelsOp = vc1::PixelsOp;

struct RlTable {
    Vlc vlc;
    int n = 0, last = 0;
    const int8_t* run = nullptr;
    const int8_t* level = nullptr;
    int8_t maxLevel[2][65] = {};
    int8_t maxRun[2][65] = {};

    void read(BitReader& in, int qmul, int qadd, int& value, int& steps) const {
        const int v = vlc.read(in);
        if (v < 0) {
            value = 64;
            steps = 66;
            return;
        }
        steps = v >> 8;
        value = (v & 0xFF) ? (v & 0xFF) * qmul + qadd : 0;
    }
};

struct Tables {
    Vlc intraMcbpc, interMcbpc, cbpy, mv, v2DcLum, v2DcChroma, v2IntraCbpc, v2MbType, interIntra;
    Vlc msMv[2], mbNonIntra[4];
    RlTable rl[6];
};
const Tables& tables();

struct Mpv {
    int version = 0;
    int width = 0, height = 0;
    int mbWidth = 0, mbHeight = 0, mbStride = 0, b8Stride = 0, mbNum = 0;
    int hEdgePos = 0, vEdgePos = 0;
    int linesize = 0, uvlinesize = 0;
    vc1::FramePool pool;
    Pic cur, last, next;
    int pictType = kPictureI;
    int qscale = 1, chromaQscale = 1, yDcScale = 0, cDcScale = 0;
    const uint8_t* yDcScaleTable = nullptr;
    const uint8_t* cDcScaleTable = nullptr;
    int mbX = 0, mbY = 0, resyncMbX = 0, resyncMbY = 0, firstSliceLine = 0;
    int blockIndex[6] = {}, blockWrap[6] = {};
    uint8_t* dest[3] = {};
    int mbIntra = 0, mbSkipped = 0, acPred = 0, aicDir = 0, interIntraPred = 0, noRounding = 0, mspel = 0;
    int mv[2] = {};
    int blockLastIndex[6] = {};
    alignas(16) int16_t block[6][64] = {};
    std::vector<int16_t> dcValBase;
    int16_t* dcVal = nullptr;
    std::vector<int16_t> acValBase;
    int16_t (*acVal)[16] = nullptr;
    std::vector<uint8_t> codedBlockBase;
    uint8_t* codedBlock = nullptr;
    std::vector<uint8_t> mbIntraTable;
    uint8_t intraScan[64] = {}, interScan[64] = {}, intraHScan[64] = {}, intraVScan[64] = {}, intraRasterEnd[64] = {};
    std::vector<uint8_t> edgeEmu;
    IdctOp idctPut = vc1::neonSimpleIdctPut;
    IdctOp idctAdd = vc1::neonSimpleIdctAdd;
    const vc1::Dsp* dsp = &vc1::dsp();

    bool allocate(int w, int h);
    void setScans(const uint8_t* intra, const uint8_t* inter, const uint8_t* horizontal, const uint8_t* vertical);
    void frameStart();
    void initBlockIndex();
    void updateBlockIndex() {
        for (int i = 0; i < 4; i++) blockIndex[i] += 2;
        blockIndex[4]++;
        blockIndex[5]++;
        dest[0] += 16;
        dest[1] += 8;
        dest[2] += 8;
    }
    void setQscale(int q);
    void predMotion(int& px, int& py);
    void updateMotionVal();
    void cleanIntraTableEntries();
    void cleanBuffers();
    void predAc(int16_t* block, int n, int dir);
    void mpegMotion(const uint8_t* const ref[3], PixelsOp const (*ops)[4], int motionX, int motionY);
    void putDct(int n, uint8_t* dst, int stride, int q);
    void loopFilter();
};

}

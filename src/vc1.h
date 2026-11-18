#pragma once
#include "bits.h"
#include "vlc.h"
#include "vc1_tables.h"
#include "intrax8.h"
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <vector>

namespace absent::vc1 {

struct Bits : BitReader {
    static constexpr size_t kPadding = 32;
    using BitReader::BitReader;
    void reset(const uint8_t* data, size_t bytes) { static_cast<BitReader&>(*this) = BitReader(data, bytes); }
    uint32_t show(int n) const { return peek(n); }
    uint32_t bits(int n) { return read(n); }
    int bit() { return int(read(1)); }
    int left() const { return int(BitReader::left()); }
    int count() const { return int(position()); }
    int unary(int stop, int length) {
        int i = 0;
        while (i < length && bit() != stop) ++i;
        return i;
    }
    int decode012() { return bit() ? bit() + 1 : 0; }
    int decode210() { return bit() ? 0 : 2 - bit(); }
    int vlc(const Vlc& table) { return table.read(*this); }
    int vlc(const Vlc* table) { return table->read(*this); }
};

constexpr int kQuantFrameImplicit = 0, kQuantFrameExplicit = 1, kQuantNonUniform = 2, kQuantUniform = 3;
constexpr int kDqprofileFourEdges = 0, kDqprofileDoubleEdges = 1, kDqprofileSingleEdge = 2, kDqprofileAllMbs = 3;
constexpr int kMvPmode1mvHpelBilin = 0, kMvPmode1mv = 1, kMvPmode1mvHpel = 2, kMvPmodeMixedMv = 3, kMvPmodeIntensityComp = 4;
constexpr int kMvPmodeIntfr1mv = 0, kMvPmodeIntfr2mvField = 1, kMvPmodeIntfr2mv = 2, kMvPmodeIntfr4mvField = 3, kMvPmodeIntfr4mv = 4, kMvPmodeIntfrIntra = 5;
constexpr int kBmvTypeBackward = 0, kBmvTypeForward = 1, kBmvTypeInterpolated = 2, kBmvTypeDirect = 3;
constexpr int kTt8x8 = 0, kTt8x4Bottom = 1, kTt8x4Top = 2, kTt8x4 = 3, kTt4x8Right = 4, kTt4x8Left = 5, kTt4x8 = 6, kTt4x4 = 7;
constexpr int kCsHighMotIntra = 0, kCsHighMotInter = 1, kCsLowMotIntra = 2, kCsLowMotInter = 3, kCsMidRateIntra = 4, kCsMidRateInter = 5, kCsHighRateIntra = 6, kCsHighRateInter = 7;
constexpr int kCondoverNone = 0, kCondoverAll = 1, kCondoverSelect = 2;
constexpr int kProgressive = 0, kIlaceFrame = 1, kIlaceField = 2;
constexpr int kImodeRaw = 0, kImodeNorm2 = 1, kImodeDiff2 = 2, kImodeNorm6 = 3, kImodeDiff6 = 4, kImodeRowskip = 5, kImodeColskip = 6;
constexpr int kProfileSimple = 0, kProfileMain = 1, kProfileComplex = 2, kProfileAdvanced = 3;
constexpr int kPictureI = 1, kPictureP = 2, kPictureB = 3, kPictureBI = 7;
constexpr uint32_t kMbTypeIntra = 1, kMbType16x16 = 8, kMbTypeSkip = 1u << 17;
constexpr uint32_t kCodeEndOfSequence = 0x10A, kCodeSlice = 0x10B, kCodeField = 0x10C, kCodeFrame = 0x10D, kCodeEntryPoint = 0x10E, kCodeSequence = 0x10F;
constexpr int kBFractionDen = 256;

extern const int kTtblkToTt[3][8];
extern const int kTtfrmToTt[4];
extern const uint8_t kMvPmodeTable[2][5];
extern const uint8_t kMvPmodeTable2[2][4];
extern const uint8_t kMbmodeIntfrp[2][15][4];
extern const int16_t kBfractionLut[23];

struct Vlcs {
    Vlc imode, norm2, norm6;
    Vlc ttmb[3], ttblk[3], subblkpat[3];
    Vlc fourMvBlockPattern[4], cbpcyP[4], mvDiff[4], intfr4mvMbmode[4], intfrNon4mvMbmode[4], oneRefMvdata[4], twoMvBlockPattern[4];
    Vlc ac[8], twoRefMvdata[8], icbpcy[8], ifMmvMbmode[8], if1mvMbmode[8];
    Vlc dc[2][2], mbI;
};
const Vlcs& tables();

inline int clip(int value, int low, int high) { return value < low ? low : value > high ? high : value; }
inline uint8_t clipU8(int value) { return uint8_t(value < 0 ? 0 : value > 255 ? 255 : value); }
inline uint8_t rangeScaled(int value, bool down) { return down ? uint8_t(((value - 128) >> 1) + 128) : clipU8(2 * value - 128); }
inline int midPred(int a, int b, int c) {
    if (a > b) {
        if (c > b) {
            if (c > a) b = a;
            else b = c;
        }
    } else {
        if (b > c) {
            if (c > a) b = c;
            else b = a;
        }
    }
    return b;
}

inline int scaleMv(int value, int bfrac, int inv, int qs) {
    int n = bfrac;
    if (inv) n -= 256;
    if (!qs) return 2 * ((value * n + 255) >> 9);
    return (value * n + 128) >> 8;
}

inline int median4(int a, int b, int c, int d) {
    if (a < b) {
        if (c < d) return (std::min(b, d) + std::max(a, c)) / 2;
        return (std::min(b, c) + std::max(a, d)) / 2;
    }
    if (c < d) return (std::min(a, d) + std::max(b, c)) / 2;
    return (std::min(a, c) + std::max(b, d)) / 2;
}

using Transform = void (*)(uint8_t* dest, ptrdiff_t stride, int16_t* block);
using MspelMc = void (*)(uint8_t* dst, const uint8_t* src, ptrdiff_t stride, int rnd);
using ChromaMc = void (*)(uint8_t* dst, const uint8_t* src, ptrdiff_t stride, int h, int x, int y);
using PixelsOp = void (*)(uint8_t* dst, const uint8_t* src, ptrdiff_t stride, int h);

struct Dsp {
    void (*invTrans8x8)(int16_t* block);
    Transform invTrans8x4, invTrans4x8, invTrans4x4;
    Transform invTrans8x8Dc, invTrans8x4Dc, invTrans4x8Dc, invTrans4x4Dc;
    void (*vOverlap)(uint8_t* src, ptrdiff_t stride);
    void (*hOverlap)(uint8_t* src, ptrdiff_t stride);
    void (*vSOverlap)(int16_t* top, int16_t* bottom);
    void (*hSOverlap)(int16_t* left, int16_t* right, ptrdiff_t leftStride, ptrdiff_t rightStride, int flags);
    void (*vLoopFilter4)(uint8_t* src, ptrdiff_t stride, int pq);
    void (*hLoopFilter4)(uint8_t* src, ptrdiff_t stride, int pq);
    void (*vLoopFilter8)(uint8_t* src, ptrdiff_t stride, int pq);
    void (*hLoopFilter8)(uint8_t* src, ptrdiff_t stride, int pq);
    void (*vLoopFilter16)(uint8_t* src, ptrdiff_t stride, int pq);
    void (*hLoopFilter16)(uint8_t* src, ptrdiff_t stride, int pq);
    MspelMc putVc1MspelPixelsTab[2][16];
    MspelMc avgVc1MspelPixelsTab[2][16];
    ChromaMc putNoRndVc1ChromaPixelsTab[2];
    ChromaMc avgNoRndVc1ChromaPixelsTab[2];
    ChromaMc putH264ChromaPixelsTab[2];
    ChromaMc avgH264ChromaPixelsTab[2];
    PixelsOp putPixelsTab[2][4];
    PixelsOp putNoRndPixelsTab[2][4];
    PixelsOp avgPixelsTab[2][4];
    PixelsOp avgNoRndPixelsTab[4];
};
const Dsp& dsp();
void simpleIdct(Dsp& d);
void simpleIdct84Add(uint8_t* dest, ptrdiff_t stride, int16_t* block);
void simpleIdct48Add(uint8_t* dest, ptrdiff_t stride, int16_t* block);
void neonSimpleIdctPut(uint8_t* dest, ptrdiff_t stride, int16_t* block);
void neonSimpleIdctAdd(uint8_t* dest, ptrdiff_t stride, int16_t* block);
void putPixelsClamped(const int16_t* block, uint8_t* pixels, ptrdiff_t stride);
void putSignedPixelsClamped(const int16_t* block, uint8_t* pixels, ptrdiff_t stride);
void addPixelsClamped(const int16_t* block, uint8_t* pixels, ptrdiff_t stride);
void emulatedEdgeMc(uint8_t* buf, const uint8_t* src, ptrdiff_t bufStride, ptrdiff_t srcStride, int blockW, int blockH, int srcX, int srcY, int w, int h);
int unescapeBuffer(const uint8_t* src, int size, uint8_t* dst);
const uint8_t* findNextMarker(const uint8_t* src, const uint8_t* end);
inline uint32_t readBe32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

constexpr int kFrameMargin = 64;

struct Frame {
    std::vector<uint8_t> memory;
    std::vector<int16_t> motionMemory[2];
    std::vector<uint32_t> mbTypeMemory;
    std::vector<int8_t> qscaleMemory;
    uint8_t* data[3] = {nullptr, nullptr, nullptr};
    int linesize[3] = {0, 0, 0};
    int16_t (*motionVal[2])[2] = {nullptr, nullptr};
    uint32_t* mbType = nullptr;
    int8_t* qscaleTable = nullptr;
    bool interlaced = false;
    bool fieldPicture = false;
    bool reduced = false;
};

struct Pic {
    std::shared_ptr<Frame> ptr;
    uint8_t* data[3] = {nullptr, nullptr, nullptr};
    int linesize[3] = {0, 0, 0};
    int16_t (*motionVal[2])[2] = {nullptr, nullptr};
    uint32_t* mbType = nullptr;
    int8_t* qscaleTable = nullptr;
    void set(const std::shared_ptr<Frame>& frame) {
        ptr = frame;
        for (int i = 0; i < 3; ++i) {
            data[i] = frame ? frame->data[i] : nullptr;
            linesize[i] = frame ? frame->linesize[i] : 0;
        }
        for (int i = 0; i < 2; ++i) motionVal[i] = frame ? frame->motionVal[i] : nullptr;
        mbType = frame ? frame->mbType : nullptr;
        qscaleTable = frame ? frame->qscaleTable : nullptr;
    }
};

struct FramePool {
    std::vector<std::shared_ptr<Frame>> frames;
    int mbWidth = 0, mbHeight = 0;
    void resize(int width, int height) {
        frames.clear();
        mbWidth = width;
        mbHeight = height;
    }
    std::shared_ptr<Frame> get();
    void grey(Frame& f) const;
};

struct Vc1 {
    Bits gb;
    Dsp vc1dsp;
    const Vlcs* vlcs = &tables();
    int codedWidth = 0, codedHeight = 0;
    int width = 0, height = 0;
    int mbX = 0, mbY = 0, mbWidth = 0, mbHeight = 0, mbStride = 0, b8Stride = 0;
    int blockIndex[6] = {}, blockWrap[6] = {};
    uint8_t* dest[3] = {};
    int linesize = 0, uvlinesize = 0;
    Pic curPic, lastPic, nextPic;
    int mv[2][4][2] = {};
    int mbIntra = 0, mspel = 0, quarterSample = 0, hEdgePos = 0, vEdgePos = 0;
    int startMbY = 0, endMbY = 0, firstSliceLine = 0;
    std::vector<int16_t> dcValMemory;
    int16_t* dcVal = nullptr;
    std::vector<int16_t> acValMemory;
    int16_t (*acVal)[16] = nullptr;
    std::vector<uint8_t> mbskipMemory;
    std::vector<uint8_t> codedBlockMemory;
    uint8_t* codedBlock = nullptr;
    uint8_t* mbskipTable = nullptr;
    int yDcScale = 0, cDcScale = 0;
    int pictType = kPictureI, lowDelay = 1, acPred = 0;
    std::vector<uint8_t> edgeEmuBuffer;
    int edgeEmuStride = 0;
    bool contextReady = false;
    bool sequenceSeen = false;
    bool entrySeen = false;
    bool advancedFraming = false;
    int maxBFramesSeen = 0;

    int resSprite = 0, resY411 = 0, resX8 = 0, multires = 0, resFasttx = 0, resTranstab = 0, rangered = 0, resRtmFlag = 0;
    int level = 0, chromaformat = 0, postprocflag = 0, broadcast = 0, interlace = 0, tfcntrflag = 0, panscanflag = 0, refdistFlag = 0;
    int extendedDmv = 0, colorPrim = 0, transferChar = 0, matrixCoef = 0, hrdParamFlag = 0, psf = 0;
    int profile = 0, frmrtqPostproc = 0, bitrtqPostproc = 0, loopFilter = 0, maxCodedWidth = 0, maxCodedHeight = 0;
    int fastuvmc = 0, extendedMv = 0, dquant = 0, vstransform = 0, overlap = 0, maxBFrames = 0, quantizerMode = 0, finterpflag = 0;
    uint8_t mvMode = 0, mvMode2 = 0;
    int kX = 0, kY = 0, rangeX = 0, rangeY = 0;
    uint8_t pq = 0, altpq = 0;
    uint8_t zz8x8[4][64] = {};
    int leftBlkSh = 0, topBlkSh = 0;
    const uint8_t* zz8x4 = nullptr;
    const uint8_t* zz4x8 = nullptr;
    uint8_t dquantfrm = 0, dqprofile = 0, dqsbedge = 0, dqbilevel = 0;
    int dcTableIndex = 0, cAcTableIndex = 0, yAcTableIndex = 0, esc3LevelLength = 0, esc3RunLength = 0, ttfrm = 0;
    uint8_t ttmbf = 0;
    std::vector<int> ttblkMemory;
    int* ttblkBase = nullptr;
    int* ttblk = nullptr;
    int codingset = 0, codingset2 = 0, pqindex = 0, aAvail = 0, cAvail = 0;
    std::vector<uint8_t> mbTypeMemory;
    uint8_t* mbType = nullptr;
    uint8_t lumscale = 0, lumshift = 0;
    int16_t bfraction = 0;
    uint8_t halfpq = 0, respic = 0, mvrange = 0, pquantizer = 0;
    const Vlc* cbpcyVlc = nullptr;
    int ttIndex = 0, mvTableIndex = 0;
    std::vector<uint8_t> planes;
    uint8_t* mvTypeMbPlane = nullptr;
    uint8_t* directMbPlane = nullptr;
    uint8_t* forwardMbPlane = nullptr;
    uint8_t* fieldtxPlane = nullptr;
    uint8_t* acpredPlane = nullptr;
    uint8_t* overFlagsPlane = nullptr;
    int mvTypeIsRaw = 0, dmbIsRaw = 0, fmbIsRaw = 0, skipIsRaw = 0, acpredIsRaw = 0, overflgIsRaw = 0, fieldtxIsRaw = 0;
    uint8_t lastLuty[2][256] = {}, lastLutuv[2][256] = {};
    uint8_t auxLuty[2][256] = {}, auxLutuv[2][256] = {};
    uint8_t nextLuty[2][256] = {}, nextLutuv[2][256] = {};
    uint8_t (*currLuty)[256] = nullptr;
    uint8_t (*currLutuv)[256] = nullptr;
    int lastUseIc = 0, nextUseIc = 0, auxUseIc = 0;
    int* currUseIc = &auxUseIc;
    int lastInterlaced = 0, nextInterlaced = 0;
    int lastReduced = 0, nextReduced = 0;
    int rnd = 0, cbptab = 0;
    uint8_t rangeredfrm = 0, interpfrm = 0;
    int fcm = kProgressive;
    uint8_t rptfrm = 0, tff = 0, rff = 0, uvsamp = 0, postproc = 0;
    int hrdNumLeakyBuckets = 0;
    uint8_t condover = 0, rangeMapyFlag = 0, rangeMapuvFlag = 0, rangeMapy = 0, rangeMapuv = 0, dmvrange = 0;
    int fourmvswitch = 0, intcomp = 0;
    uint8_t lumscale2 = 0, lumshift2 = 0;
    const Vlc* mbmodeVlc = nullptr;
    const Vlc* imvVlc = nullptr;
    const Vlc* twomvbpVlc = nullptr;
    const Vlc* fourmvbpVlc = nullptr;
    uint8_t twomvbp = 0, fourmvbp = 0;
    uint8_t zzi8x8[64] = {};
    std::vector<uint8_t> blkMvTypeMemory;
    uint8_t* blkMvType = nullptr;
    std::vector<uint8_t> mvFMemory, mvFNextMemory;
    uint8_t* mvF[2] = {};
    uint8_t* mvFNext[2] = {};
    int fieldMode = 0, fptype = 0, secondField = 0, refdist = 0, numref = 0, reffield = 0, intcompfield = 0, curFieldType = 0;
    int refFieldType[2] = {};
    int blocksOff = 0, mbOff = 0, bmvtype = 0, frfd = 0, brfd = 0, firstPicHeaderFlag = 0, picHeaderFlag = 0;
    int mbmodetab = 0, icbptab = 0, imvtab = 0, twomvbptab = 0, fourmvbptab = 0;
    int pFrameSkipped = 0, biType = 0, x8Type = 0;
    std::vector<int16_t> blockMemory;
    int16_t (*block)[6][64] = nullptr;
    int nAllocatedBlks = 0, curBlkIdx = 0, leftBlkIdx = 0, topleftBlkIdx = 0, topBlkIdx = 0;
    std::vector<uint32_t> cbpMemory;
    uint32_t* cbpBase = nullptr;
    uint32_t* cbp = nullptr;
    std::vector<uint8_t> isIntraMemory;
    uint8_t* isIntraBase = nullptr;
    uint8_t* isIntra = nullptr;
    std::vector<int16_t> lumaMvMemory;
    int16_t (*lumaMvBase)[2] = nullptr;
    int16_t (*lumaMv)[2] = nullptr;
    uint8_t bfractionLutIndex = 0, brokenLink = 0, closedEntry = 0;
    int endMbX = 0, resyncMarker = 0;
    alignas(32) int16_t blocks[6][64] = {};
    FramePool pool;
    IntraX8 x8;
    int lumaLines = 0, chromaLines = 0;

    int bitplaneDecoding(uint8_t* data, int *rawFlag);
    int vopDquantDecoding();
    int decodeSequenceHeader(Bits& gb);
    int decodeSequenceHeaderAdv(Bits& gb);
    int decodeEntryPoint(Bits& gb);
    void rotateLuts();
    int readBfraction(Bits& gb);
    int parseFrameHeader(Bits& gb);
    int parseFrameHeaderAdv(Bits& gb);
    void initBlockIndex();
    void ffInitBlockIndex();
    void updateBlockIndex();
    void putBlocksClamped(int putSigned);
    int getMquant(int mquant);
    void getMvdata(int& dmvX, int& dmvY, int& mbHasCoeffs);
    void getMvdataInterlaced(int *dmvX, int *dmvY, int *predFlag);
    void bMc(int*, int*, int direct, int mode);
    int iPredDc(int overlap, int pq, int n, int16_t **dcValPtr, int *dirPtr);
    int predDc(int, int, int n, int aAvail, int cAvail, int16_t **dcValPtr, int *dirPtr);
    int codedBlockPred(int n, int diff);
    int decodeAcCoeff(int *last, int *skip, int *value, int codingset);
    int decodeIBlock(int16_t block[64], int n, int coded, int codingset);
    int decodeIBlockAdv(int16_t block[64], int n, int coded, int codingset, int mquant);
    int decodeIntraBlock(int16_t block[64], int n, int coded, int mquant, int codingset);
    int decodePBlock(int16_t block[64], int n, int mquant, int ttmb, int firstBlock, uint8_t *dst, int linesize, int skipBlock, int *ttmbOut);
    int decodePMb();
    int decodePMbIntfr();
    int decodePMbIntfi();
    int decodeBMb();
    int decodeBMbIntfi();
    int decodeBMbIntfr();
    void decodeIBlocks();
    int decodeIBlocksAdv();
    void decodePBlocks();
    void decodeBBlocks();
    void decodeSkipBlocks();
    void decodeBlocks();
    void conceal(int first, int last);
    int scaleforsameX(const int n, int dir);
    int scaleforsameY(const int n, int dir);
    int scaleforoppX(const int n);
    int scaleforoppY(const int n, int dir);
    int scaleforsame(int n, int dim, int dir);
    int scaleforopp(int n, int dim, int dir);
    void predMv(int n, int dmvX, int dmvY, int mv1, int rX, int rY, uint8_t* isIntra, int predFlag, int dir);
    void predMvIntfr(int n, int dmvX, int dmvY, int mvn, int rX, int rY, int dir);
    void predBMv(int dmvX[2], int dmvY[2], int direct, int mvtype);
    void predBMvIntfi(int n, int *dmvX, int *dmvY, int mv1, int *predFlag);
    int getLumaMv(int dir, int16_t *tx, int16_t *ty);
    int getChromaMv(int dir, int16_t *tx, int16_t *ty);
    void mc1mv(int dir);
    void mc4mvLuma(int n, int dir, int avg);
    void mc4mvChroma(int dir);
    void mc4mvChroma4(int dir, int dir2, int avg);
    void interpMc();
    void hOverlapFilter(int16_t (*leftBlock)[64], int16_t (*rightBlock)[64], int leftFieldtx, int rightFieldtx, int blockNum);
    void vOverlapFilter(int16_t (*topBlock)[64], int16_t (*bottomBlock)[64], int blockNum);
    void iOverlapFilter();
    void pOverlapFilter();
    void iHLoopFilter(uint8_t *dest, uint32_t flags, int blockNum);
    void iVLoopFilter(uint8_t *dest, uint32_t flags, uint8_t fieldtx, int blockNum);
    void iLoopFilter();
    void pHLoopFilter(uint8_t *dest, uint32_t *cbp, uint8_t *isIntra, int16_t (*mv)[2], uint8_t *mvF, int *ttblk, uint32_t flags, int blockNum);
    void pVLoopFilter(uint8_t *dest, const uint32_t *cbp, const uint8_t *isIntra, int16_t (*mv)[2], const uint8_t *mvF, const int *ttblk, uint32_t flags, int blockNum);
    void pLoopFilter();
    void pHIntfrLoopFilter(uint8_t *dest, const int *ttblk, uint32_t flags, uint8_t fieldtx, int blockNum);
    void pVIntfrLoopFilter(uint8_t *dest, const int *ttblk, uint32_t flags, uint8_t fieldtx, int blockNum);
    void pIntfrLoopFilter();
    void bHIntfiLoopFilter(uint8_t *dest, const uint32_t *cbp, const int *ttblk, uint32_t flags, int blockNum);
    void bVIntfiLoopFilter(uint8_t *dest, const uint32_t *cbp, const int *ttblk, uint32_t flags, int blockNum);
    void bIntfiLoopFilter();
    bool allocate();
    bool frameStart();
    bool start(const uint8_t* config, size_t size, bool wmv3, int w, int h);
    void copyFrom(const Vc1& o);
    int peekType(const uint8_t* data, size_t size);
    bool decodeFrame(const uint8_t* buf, size_t size);
};

}

#include "vc1.h"
#include <cstring>

namespace absent::vc1 {


static const uint8_t offsetTable[2][9] = {
    {  0,  1,  2,  4,  8, 16, 32,  64, 128 },
    {  0,  1,  3,  7, 15, 31, 63, 127, 255 },
};
static const int blockMap[6] = {0, 2, 1, 3, 4, 5};
void Vc1::initBlockIndex()
{
    ffInitBlockIndex();
    if (fieldMode && !(secondField ^ tff)) {
        dest[0] += curPic.ptr->linesize[0];
        dest[1] += curPic.ptr->linesize[1];
        dest[2] += curPic.ptr->linesize[2];
    }
}
void Vc1::ffInitBlockIndex()
{
    const int lines = curPic.linesize[0], uvLines = curPic.linesize[1];
    blockIndex[0] = b8Stride * (mbY * 2) - 2 + mbX * 2;
    blockIndex[1] = b8Stride * (mbY * 2) - 1 + mbX * 2;
    blockIndex[2] = b8Stride * (mbY * 2 + 1) - 2 + mbX * 2;
    blockIndex[3] = b8Stride * (mbY * 2 + 1) - 1 + mbX * 2;
    blockIndex[4] = mbStride * (mbY + 1) + b8Stride * mbHeight * 2 + mbX - 1;
    blockIndex[5] = mbStride * (mbY + mbHeight + 2) + b8Stride * mbHeight * 2 + mbX - 1;
    dest[0] = curPic.data[0] + (mbX - 1) * 16;
    dest[1] = curPic.data[1] + (mbX - 1) * 8;
    dest[2] = curPic.data[2] + (mbX - 1) * 8;
    dest[0] += ptrdiff_t(mbY) * lines * 16;
    dest[1] += ptrdiff_t(mbY) * uvLines * 8;
    dest[2] += ptrdiff_t(mbY) * uvLines * 8;
}
void Vc1::updateBlockIndex()
{
    blockIndex[0] += 2;
    blockIndex[1] += 2;
    blockIndex[2] += 2;
    blockIndex[3] += 2;
    blockIndex[4]++;
    blockIndex[5]++;
    dest[0] += 16;
    dest[1] += 8;
    dest[2] += 8;
}
void Vc1::putBlocksClamped(int putSigned)
{
    uint8_t *dest;
    int blockCount = 6;
    int fieldtx = 0;
    int i;
    if (!firstSliceLine && fcm != kIlaceFrame) {
        if (mbX) {
            for (i = 0; i < blockCount; i++) {
                if (i > 3 ? mbType[blockIndex[i] - blockWrap[i] - 1] :
                            mbType[blockIndex[i] - 2 * blockWrap[i] - 2]) {
                    dest = this->dest[0] + ((i & 2) - 4) * 4 * linesize + ((i & 1) - 2) * 8;
                    if (putSigned)
                        putSignedPixelsClamped(block[topleftBlkIdx][blockMap[i]],
                                                          i > 3 ? this->dest[i - 3] - 8 * uvlinesize - 8 : dest,
                                                          i > 3 ? uvlinesize : linesize);
                    else
                        putPixelsClamped(block[topleftBlkIdx][blockMap[i]],
                                                   i > 3 ? this->dest[i - 3] - 8 * uvlinesize - 8 : dest,
                                                   i > 3 ? uvlinesize : linesize);
                }
            }
        }
        if (mbX == endMbX - 1) {
            for (i = 0; i < blockCount; i++) {
                if (i > 3 ? mbType[blockIndex[i] - blockWrap[i]] :
                            mbType[blockIndex[i] - 2 * blockWrap[i]]) {
                    dest = this->dest[0] + ((i & 2) - 4) * 4 * linesize + (i & 1) * 8;
                    if (putSigned)
                        putSignedPixelsClamped(block[topBlkIdx][blockMap[i]],
                                                          i > 3 ? this->dest[i - 3] - 8 * uvlinesize : dest,
                                                          i > 3 ? uvlinesize : linesize);
                    else
                        putPixelsClamped(block[topBlkIdx][blockMap[i]],
                                                   i > 3 ? this->dest[i - 3] - 8 * uvlinesize : dest,
                                                   i > 3 ? uvlinesize : linesize);
                }
            }
        }
    }
    if (mbY == endMbY - 1 || fcm == kIlaceFrame) {
        if (mbX) {
            if (fcm == kIlaceFrame)
                fieldtx = fieldtxPlane[mbY * mbStride + mbX - 1];
            for (i = 0; i < blockCount; i++) {
                if (i > 3 ? mbType[blockIndex[i] - 1] :
                            mbType[blockIndex[i] - 2]) {
                    if (fieldtx)
                        dest = this->dest[0] + ((i & 2) >> 1) * linesize + ((i & 1) - 2) * 8;
                    else
                        dest = this->dest[0] + (i & 2) * 4 * linesize + ((i & 1) - 2) * 8;
                    if (putSigned)
                        putSignedPixelsClamped(block[leftBlkIdx][blockMap[i]],
                                                          i > 3 ? this->dest[i - 3] - 8 : dest,
                                                          i > 3 ? uvlinesize : linesize << fieldtx);
                    else
                        putPixelsClamped(block[leftBlkIdx][blockMap[i]],
                                                   i > 3 ? this->dest[i - 3] - 8 : dest,
                                                   i > 3 ? uvlinesize : linesize << fieldtx);
                }
            }
        }
        if (mbX == endMbX - 1) {
            if (fcm == kIlaceFrame)
                fieldtx = fieldtxPlane[mbY * mbStride + mbX];
            for (i = 0; i < blockCount; i++) {
                if (mbType[blockIndex[i]]) {
                    if (fieldtx)
                        dest = this->dest[0] + ((i & 2) >> 1) * linesize + (i & 1) * 8;
                    else
                        dest = this->dest[0] + (i & 2) * 4 * linesize + (i & 1) * 8;
                    if (putSigned)
                        putSignedPixelsClamped(block[curBlkIdx][blockMap[i]],
                                                          i > 3 ? this->dest[i - 3] : dest,
                                                          i > 3 ? uvlinesize : linesize << fieldtx);
                    else
                        putPixelsClamped(block[curBlkIdx][blockMap[i]],
                                                   i > 3 ? this->dest[i - 3] : dest,
                                                   i > 3 ? uvlinesize : linesize << fieldtx);
                }
            }
        }
    }
}
inline void incBlkIdx(int& idx, int count) {
    idx++;
    if (idx >= count) idx = 0;
}
int Vc1::getMquant(int mquant)
{
    if (dquantfrm) {
        int edges = 0;
        if (dqprofile == kDqprofileAllMbs) {
            if (dqbilevel) {
                mquant = (gb.bit()) ? -altpq : pq;
            } else {
                const int mqdiff = gb.bits(3);
                if (mqdiff != 7)
                    mquant = -pq - mqdiff;
                else
                    mquant = -int(gb.bits(5));
            }
        }
        if (dqprofile == kDqprofileSingleEdge)
            edges = 1 << dqsbedge;
        else if (dqprofile == kDqprofileDoubleEdges)
            edges = (3 << dqsbedge) % 15;
        else if (dqprofile == kDqprofileFourEdges)
            edges = 15;
        if ((edges&1) && !mbX)
            mquant = -altpq;
        if ((edges&2) && !mbY)
            mquant = -altpq;
        if ((edges&4) && mbX == (mbWidth - 1))
            mquant = -altpq;
        if ((edges&8) && mbY == ((mbHeight >> fieldMode) - 1))
            mquant = -altpq;
        if (!mquant || mquant > 31 || mquant < -31) {
            mquant = 1;
        }
    }
    return mquant;
}
static const uint8_t sizeTable[6] = { 0, 2, 3, 4,  5,  8 };
void Vc1::getMvdata(int& dmvX, int& dmvY, int& mbHasCoeffs)
{
    int index = 1 + gb.vlc(vlcs->mvDiff[mvTableIndex]);
    if (index > 36) {
        mbHasCoeffs = 1;
        index -= 37;
    } else
        mbHasCoeffs = 0;
    mbIntra = 0;
    if (!index) {
        dmvX = dmvY = 0;
    } else if (index == 35) {
        dmvX = gb.bits(kX - 1 + quarterSample);
        dmvY = gb.bits(kY - 1 + quarterSample);
    } else if (index == 36) {
        dmvX = 0;
        dmvY = 0;
        mbIntra = 1;
    } else {
        int index1 = index % 6;
        dmvX = offsetTable[1][index1];
        int val = sizeTable[index1] - (!quarterSample && index1 == 5);
        if (val > 0) {
            val = gb.bits(val);
            const int sign = 0 - (val & 1);
            dmvX = (sign ^ ((val >> 1) + dmvX)) - sign;
        }
        index1 = index / 6;
        dmvY = offsetTable[1][index1];
        val = sizeTable[index1] - (!quarterSample && index1 == 5);
        if (val > 0) {
            val = gb.bits(val);
            const int sign = 0 - (val & 1);
            dmvY = (sign ^ ((val >> 1) + dmvY)) - sign;
        }
    }
}
void Vc1::getMvdataInterlaced(int *dmvX,
                                                   int *dmvY, int *predFlag)
{
    int index, index1;
    int extendX, extendY;
    int esc;
    int val, sign;
    if (numref) {
        esc  = 125;
    } else {
        esc  = 71;
    }
    extendX = dmvrange & 1;
    extendY = (dmvrange >> 1) & 1;
    index = gb.vlc(imvVlc);
    if (index == esc) {
        *dmvX = gb.bits(kX);
        *dmvY = gb.bits(kY);
        if (numref) {
            if (predFlag)
                *predFlag = *dmvY & 1;
            *dmvY = (*dmvY + (*dmvY & 1)) >> 1;
        }
    }
    else {
        index1 = (index + 1) % 9;
        if (index1 != 0) {
            val    = gb.bits(index1 + extendX);
            sign   = 0 - (val & 1);
            *dmvX = (sign ^ ((val >> 1) + offsetTable[extendX][index1])) - sign;
        } else
            *dmvX = 0;
        index1 = (index + 1) / 9;
        if (index1 > numref) {
            val    = gb.bits((index1 >> numref) + extendY);
            sign   = 0 - (val & 1);
            *dmvY = (sign ^ ((val >> 1) + offsetTable[extendY][index1 >> numref])) - sign;
        } else
            *dmvY = 0;
        if (numref && predFlag)
            *predFlag = index1 & 1;
    }
}
void Vc1::bMc(int*, int*,
                            int direct, int mode)
{
    if (direct) {
        mc1mv(0);
        interpMc();
        return;
    }
    if (mode == kBmvTypeInterpolated) {
        mc1mv(0);
        interpMc();
        return;
    }
    mc1mv((mode == kBmvTypeBackward));
}
int Vc1::iPredDc(int overlap, int pq, int n,
                                int16_t **dcValPtr, int *dirPtr)
{
    int a, b, c, wrap, pred, scale;
    int16_t *dcVal;
    static const uint16_t dcpred[32] = {
        0xFFFF, 1024,  512,  341,  256,  205,  171,  146,  128,
             114,  102,   93,   85,   79,   73,   68,   64,
              60,   57,   54,   51,   49,   47,   45,   43,
              41,   39,   38,   37,   35,   34,   33
    };
    scale = yDcScale;
    wrap   = blockWrap[n];
    dcVal = this->dcVal + blockIndex[n];
    c = dcVal[ - 1];
    b = dcVal[ - 1 - wrap];
    a = dcVal[ - wrap];
    if (pq < 9 || !overlap) {
        if (firstSliceLine && (n != 2 && n != 3))
            b = a = dcpred[scale];
        if (mbX == 0 && (n != 1 && n != 3))
            b = c = dcpred[scale];
    } else {
        if (firstSliceLine && (n != 2 && n != 3))
            b = a = 0;
        if (mbX == 0 && (n != 1 && n != 3))
            b = c = 0;
    }
    if (abs(a - b) <= abs(b - c)) {
        pred     = c;
        *dirPtr = 1;
    } else {
        pred     = a;
        *dirPtr = 0;
    }
    *dcValPtr = &dcVal[0];
    return pred;
}
int Vc1::predDc(int, int, int n,
                              int aAvail, int cAvail,
                              int16_t **dcValPtr, int *dirPtr)
{
    int a, b, c, wrap, pred;
    int16_t *dcVal;
    int mbPos = mbX + mbY * mbStride;
    int q1, q2 = 0;
    int dqscaleIndex;
    q1 = std::abs(curPic.qscaleTable[mbPos]);
    dqscaleIndex = kWmv3DcScaleTable[q1] - 1;
    if (dqscaleIndex < 0)
        return 0;
    wrap = blockWrap[n];
    dcVal = this->dcVal + blockIndex[n];
    c = dcVal[ - 1];
    b = dcVal[ - 1 - wrap];
    a = dcVal[ - wrap];
    if (cAvail && (n != 1 && n != 3)) {
        q2 = std::abs(curPic.qscaleTable[mbPos - 1]);
        if (q2 && q2 != q1)
            c = (int)((unsigned)c * kWmv3DcScaleTable[q2] * kDqscale[dqscaleIndex] + 0x20000) >> 18;
    }
    if (aAvail && (n != 2 && n != 3)) {
        q2 = std::abs(curPic.qscaleTable[mbPos - mbStride]);
        if (q2 && q2 != q1)
            a = (int)((unsigned)a * kWmv3DcScaleTable[q2] * kDqscale[dqscaleIndex] + 0x20000) >> 18;
    }
    if (aAvail && cAvail && (n != 3)) {
        int off = mbPos;
        if (n != 1)
            off--;
        if (n != 2)
            off -= mbStride;
        q2 = std::abs(curPic.qscaleTable[off]);
        if (q2 && q2 != q1)
            b = (int)((unsigned)b * kWmv3DcScaleTable[q2] * kDqscale[dqscaleIndex] + 0x20000) >> 18;
    }
    if (cAvail && (!aAvail || abs(a - b) <= abs(b - c))) {
        pred     = c;
        *dirPtr = 1;
    } else if (aAvail) {
        pred     = a;
        *dirPtr = 0;
    } else {
        pred     = 0;
        *dirPtr = 1;
    }
    *dcValPtr = &dcVal[0];
    return pred;
}
int Vc1::codedBlockPred(int n, int diff)
{
    int xy, wrap, pred, a, b, c;
    xy   = blockIndex[n];
    wrap = b8Stride;
    a = codedBlock[xy - 1       ];
    b = codedBlock[xy - 1 - wrap];
    c = codedBlock[xy     - wrap];
    if (b == c) {
        pred = a;
    } else {
        pred = c;
    }
    codedBlock[xy] = pred ^ diff;
    return pred ^ diff;
}
int Vc1::decodeAcCoeff(int *last, int *skip,
                                int *value, int codingset)
{
    int index, run, level, lst, sign;
    index = gb.vlc(vlcs->ac[codingset]);
    if (index < 0)
        return index;
    if (index != kAcSizes[codingset] - 1) {
        run   = kIndexDecodeTable[codingset][index][0];
        level = kIndexDecodeTable[codingset][index][1];
        lst   = index >= kLastDecodeTable[codingset] || gb.left() < 0;
        sign  = gb.bit();
    } else {
        int escape = gb.decode210();
        if (escape != 2) {
            index = gb.vlc(vlcs->ac[codingset]);
            if (unsigned(index) >= unsigned(kAcSizes[codingset]) - 1u)
                return -1;
            run   = kIndexDecodeTable[codingset][index][0];
            level = kIndexDecodeTable[codingset][index][1];
            lst   = index >= kLastDecodeTable[codingset];
            if (escape == 0) {
                if (lst)
                    level += kLastDeltaLevelTable[codingset][run];
                else
                    level += kDeltaLevelTable[codingset][run];
            } else {
                if (lst)
                    run += kLastDeltaRunTable[codingset][level] + 1;
                else
                    run += kDeltaRunTable[codingset][level] + 1;
            }
            sign = gb.bit();
        } else {
            lst = gb.bit();
            if (esc3LevelLength == 0) {
                if (pq < 8 || dquantfrm) {
                    esc3LevelLength = gb.bits(3);
                    if (!esc3LevelLength)
                        esc3LevelLength = gb.bits(2) + 8;
                } else {
                    esc3LevelLength = gb.unary(1, 6) + 2;
                }
                esc3RunLength = 3 + gb.bits(2);
            }
            run   = gb.bits(esc3RunLength);
            sign  = gb.bit();
            level = gb.bits(esc3LevelLength);
        }
    }
    *last  = lst;
    *skip  = run;
    *value = (level ^ -sign) + sign;
    return 0;
}
int Vc1::decodeIBlock(int16_t block[64], int n,
                              int coded, int codingset)
{
    int dcPredDir = 0;
    int16_t *dcVal;
    int16_t *acVal, *acVal2;
    int dcdiff, scale;
    dcdiff = gb.vlc(vlcs->dc[dcTableIndex][n >= 4]);
    if (dcdiff) {
        const int m = (pq == 1 || pq == 2) ? 3 - pq : 0;
        if (dcdiff == 119 ) {
            dcdiff = gb.bits(8 + m);
        } else {
            if (m)
                dcdiff = (dcdiff << m) + gb.bits(m) - ((1 << m) - 1);
        }
        if (gb.bit())
            dcdiff = -dcdiff;
    }
    dcdiff += iPredDc(overlap, pq, n, &dcVal, &dcPredDir);
    *dcVal = dcdiff;
    block[0] = dcdiff * yDcScale;
    acVal  = this->acVal[blockIndex[n]];
    acVal2 = acVal;
    if (dcPredDir)
        acVal -= 16;
    else
        acVal -= 16 * blockWrap[n];
    scale = pq * 2 + halfpq;
    if (coded) {
        int last = 0, skip, value;
        const uint8_t *zzTable;
        int k;
        if (acPred) {
            if (!dcPredDir)
                zzTable = zz8x8[2];
            else
                zzTable = zz8x8[3];
        } else
            zzTable = zz8x8[1];
        for (int i = 1; !last; ++i) {
            int ret = decodeAcCoeff(&last, &skip, &value, codingset);
            if (ret < 0)
                return ret;
            i += skip;
            if (i > 63)
                break;
            block[zzTable[i]] = value;
        }
        if (acPred) {
            int sh;
            if (dcPredDir) {
                sh = leftBlkSh;
            } else {
                sh = topBlkSh;
                acVal += 8;
            }
            for (k = 1; k < 8; k++)
                block[k << sh] += acVal[k];
        }
        for (k = 1; k < 8; k++) {
            acVal2[k]     = block[k << leftBlkSh];
            acVal2[k + 8] = block[k << topBlkSh];
        }
        for (k = 1; k < 64; k++)
            if (block[k]) {
                block[k] *= scale;
                if (!pquantizer)
                    block[k] += (block[k] < 0) ? -pq : pq;
            }
    } else {
        int k;
        memset(acVal2, 0, 16 * 2);
        if (acPred) {
            int sh;
            if (dcPredDir) {
                sh = leftBlkSh;
            } else {
                sh = topBlkSh;
                acVal  += 8;
                acVal2 += 8;
            }
            memcpy(acVal2, acVal, 8 * 2);
            for (k = 1; k < 8; k++) {
                block[k << sh] = acVal[k] * scale;
                if (!pquantizer && block[k << sh])
                    block[k << sh] += (block[k << sh] < 0) ? -pq : pq;
            }
        }
    }
    return 0;
}
int Vc1::decodeIBlockAdv(int16_t block[64], int n,
                                  int coded, int codingset, int mquant)
{
    int dcPredDir = 0;
    int16_t *dcVal = nullptr;
    int16_t *acVal, *acVal2;
    int dcdiff;
    int aAvail = this->aAvail, cAvail = this->cAvail;
    int usePred = acPred;
    int scale;
    int q1, q2 = 0;
    int mbPos = mbX + mbY * mbStride;
    int quant = std::abs(mquant);
    dcdiff = gb.vlc(vlcs->dc[dcTableIndex][n >= 4]);
    if (dcdiff) {
        const int m = (quant == 1 || quant == 2) ? 3 - quant : 0;
        if (dcdiff == 119 ) {
            dcdiff = gb.bits(8 + m);
        } else {
            if (m)
                dcdiff = (dcdiff << m) + gb.bits(m) - ((1 << m) - 1);
        }
        if (gb.bit())
            dcdiff = -dcdiff;
    }
    dcdiff += predDc(overlap, quant, n, this->aAvail, this->cAvail, &dcVal, &dcPredDir);
    *dcVal = dcdiff;
    block[0] = dcdiff * yDcScale;
    if (!aAvail && !cAvail)
        usePred = 0;
    scale = quant * 2 + ((mquant < 0) ? 0 : halfpq);
    acVal  = this->acVal[blockIndex[n]];
    acVal2 = acVal;
    if (dcPredDir)
        acVal -= 16;
    else
        acVal -= 16 * blockWrap[n];
    q1 = curPic.qscaleTable[mbPos];
    if (n == 3)
        q2 = q1;
    else if (dcPredDir) {
        if (n == 1)
            q2 = q1;
        else if (cAvail && mbPos)
            q2 = curPic.qscaleTable[mbPos - 1];
    } else {
        if (n == 2)
            q2 = q1;
        else if (aAvail && mbPos >= mbStride)
            q2 = curPic.qscaleTable[mbPos - mbStride];
    }
    if (coded) {
        int last = 0, skip, value;
        const uint8_t *zzTable;
        int k;
        if (acPred) {
            if (!usePred && fcm == kIlaceFrame) {
                zzTable = zzi8x8;
            } else {
                if (!dcPredDir)
                    zzTable = zz8x8[2];
                else
                    zzTable = zz8x8[3];
            }
        } else {
            if (fcm != kIlaceFrame)
                zzTable = zz8x8[1];
            else
                zzTable = zzi8x8;
        }
        for (int i = 1; !last; ++i) {
            int ret = decodeAcCoeff(&last, &skip, &value, codingset);
            if (ret < 0)
                return ret;
            i += skip;
            if (i > 63)
                break;
            block[zzTable[i]] = value;
        }
        if (usePred) {
            int sh;
            if (dcPredDir) {
                sh = leftBlkSh;
            } else {
                sh = topBlkSh;
                acVal += 8;
            }
            q1 = std::abs(q1) * 2 + ((q1 < 0) ? 0 : halfpq) - 1;
            if (q1 < 1)
                return -1;
            if (q2)
                q2 = std::abs(q2) * 2 + ((q2 < 0) ? 0 : halfpq) - 1;
            if (q2 && q1 != q2) {
                for (k = 1; k < 8; k++)
                    block[k << sh] += (int)(acVal[k] * (unsigned)q2 * kDqscale[q1 - 1] + 0x20000) >> 18;
            } else {
                for (k = 1; k < 8; k++)
                    block[k << sh] += acVal[k];
            }
        }
        for (k = 1; k < 8; k++) {
            acVal2[k    ] = block[k << leftBlkSh];
            acVal2[k + 8] = block[k << topBlkSh];
        }
        for (k = 1; k < 64; k++)
            if (block[k]) {
                block[k] *= scale;
                if (!pquantizer)
                    block[k] += (block[k] < 0) ? -quant : quant;
            }
    } else {
        int k;
        memset(acVal2, 0, 16 * 2);
        if (usePred) {
            int sh;
            if (dcPredDir) {
                sh = leftBlkSh;
            } else {
                sh = topBlkSh;
                acVal  += 8;
                acVal2 += 8;
            }
            memcpy(acVal2, acVal, 8 * 2);
            q1 = std::abs(q1) * 2 + ((q1 < 0) ? 0 : halfpq) - 1;
            if (q1 < 1)
                return -1;
            if (q2)
                q2 = std::abs(q2) * 2 + ((q2 < 0) ? 0 : halfpq) - 1;
            if (q2 && q1 != q2) {
                for (k = 1; k < 8; k++)
                    acVal2[k] = (int)(acVal2[k] * q2 * (unsigned)kDqscale[q1 - 1] + 0x20000) >> 18;
            }
            for (k = 1; k < 8; k++) {
                block[k << sh] = acVal2[k] * scale;
                if (!pquantizer && block[k << sh])
                    block[k << sh] += (block[k << sh] < 0) ? -quant : quant;
            }
        }
    }
    return 0;
}
int Vc1::decodeIntraBlock(int16_t block[64], int n,
                                  int coded, int mquant, int codingset)
{
    int dcPredDir = 0;
    int16_t *dcVal = nullptr;
    int16_t *acVal, *acVal2;
    int dcdiff;
    int mbPos = mbX + mbY * mbStride;
    int aAvail = this->aAvail, cAvail = this->cAvail;
    int usePred = acPred;
    int scale;
    int q1, q2 = 0;
    int quant = std::abs(mquant);
    memset(block, 0, sizeof(int16_t) * 64);
    quant = clip(quant, 0, 31);
    yDcScale = kWmv3DcScaleTable[quant];
    dcdiff = gb.vlc(vlcs->dc[dcTableIndex][n >= 4]);
    if (dcdiff) {
        const int m = (quant == 1 || quant == 2) ? 3 - quant : 0;
        if (dcdiff == 119 ) {
            dcdiff = gb.bits(8 + m);
        } else {
            if (m)
                dcdiff = (dcdiff << m) + gb.bits(m) - ((1 << m) - 1);
        }
        if (gb.bit())
            dcdiff = -dcdiff;
    }
    dcdiff += predDc(overlap, quant, n, aAvail, cAvail, &dcVal, &dcPredDir);
    *dcVal = dcdiff;
    block[0] = dcdiff * yDcScale;
    if (!aAvail) dcPredDir = 1;
    if (!cAvail) dcPredDir = 0;
    if (!aAvail && !cAvail) usePred = 0;
    acVal  = this->acVal[blockIndex[n]];
    acVal2 = acVal;
    scale = quant * 2 + ((mquant < 0) ? 0 : halfpq);
    if (dcPredDir)
        acVal -= 16;
    else
        acVal -= 16 * blockWrap[n];
    q1 = curPic.qscaleTable[mbPos];
    if (dcPredDir && cAvail && mbPos)
        q2 = curPic.qscaleTable[mbPos - 1];
    if (!dcPredDir && aAvail && mbPos >= mbStride)
        q2 = curPic.qscaleTable[mbPos - mbStride];
    if (dcPredDir && n == 1)
        q2 = q1;
    if (!dcPredDir && n == 2)
        q2 = q1;
    if (n == 3) q2 = q1;
    if (coded) {
        int last = 0, skip, value;
        int k;
        for (int i = 1; !last; ++i) {
            int ret = decodeAcCoeff(&last, &skip, &value, codingset);
            if (ret < 0)
                return ret;
            i += skip;
            if (i > 63)
                break;
            if (fcm == kProgressive)
                block[zz8x8[0][i]] = value;
            else {
                if (usePred && (fcm == kIlaceFrame)) {
                    if (!dcPredDir)
                        block[zz8x8[2][i]] = value;
                    else
                        block[zz8x8[3][i]] = value;
                } else {
                    block[zzi8x8[i]] = value;
                }
            }
        }
        if (usePred) {
            q1 = std::abs(q1) * 2 + ((q1 < 0) ? 0 : halfpq) - 1;
            if (q1 < 1)
                return -1;
            if (q2)
                q2 = std::abs(q2) * 2 + ((q2 < 0) ? 0 : halfpq) - 1;
            if (q2 && q1 != q2) {
                if (dcPredDir) {
                    for (k = 1; k < 8; k++)
                        block[k << leftBlkSh] += (int)(acVal[k] * q2 * (unsigned)kDqscale[q1 - 1] + 0x20000) >> 18;
                } else {
                    for (k = 1; k < 8; k++)
                        block[k << topBlkSh] += (int)(acVal[k + 8] * q2 * (unsigned)kDqscale[q1 - 1] + 0x20000) >> 18;
                }
            } else {
                if (dcPredDir) {
                    for (k = 1; k < 8; k++)
                        block[k << leftBlkSh] += acVal[k];
                } else {
                    for (k = 1; k < 8; k++)
                        block[k << topBlkSh] += acVal[k + 8];
                }
            }
        }
        for (k = 1; k < 8; k++) {
            acVal2[k    ] = block[k << leftBlkSh];
            acVal2[k + 8] = block[k << topBlkSh];
        }
        for (k = 1; k < 64; k++)
            if (block[k]) {
                block[k] *= scale;
                if (!pquantizer)
                    block[k] += (block[k] < 0) ? -quant : quant;
            }
    } else {
        int k;
        memset(acVal2, 0, 16 * 2);
        if (dcPredDir) {
            if (usePred) {
                memcpy(acVal2, acVal, 8 * 2);
                q1 = std::abs(q1) * 2 + ((q1 < 0) ? 0 : halfpq) - 1;
                if (q1 < 1)
                    return -1;
                if (q2)
                    q2 = std::abs(q2) * 2 + ((q2 < 0) ? 0 : halfpq) - 1;
                if (q2 && q1 != q2) {
                    for (k = 1; k < 8; k++)
                        acVal2[k] = (int)(acVal2[k] * (unsigned)q2 * kDqscale[q1 - 1] + 0x20000) >> 18;
                }
            }
        } else {
            if (usePred) {
                memcpy(acVal2 + 8, acVal + 8, 8 * 2);
                q1 = std::abs(q1) * 2 + ((q1 < 0) ? 0 : halfpq) - 1;
                if (q1 < 1)
                    return -1;
                if (q2)
                    q2 = std::abs(q2) * 2 + ((q2 < 0) ? 0 : halfpq) - 1;
                if (q2 && q1 != q2) {
                    for (k = 1; k < 8; k++)
                        acVal2[k + 8] = (int)(acVal2[k + 8] * (unsigned)q2 * kDqscale[q1 - 1] + 0x20000) >> 18;
                }
            }
        }
        if (usePred) {
            if (dcPredDir) {
                for (k = 1; k < 8; k++) {
                    block[k << leftBlkSh] = acVal2[k] * scale;
                    if (!pquantizer && block[k << leftBlkSh])
                        block[k << leftBlkSh] += (block[k << leftBlkSh] < 0) ? -quant : quant;
                }
            } else {
                for (k = 1; k < 8; k++) {
                    block[k << topBlkSh] = acVal2[k + 8] * scale;
                    if (!pquantizer && block[k << topBlkSh])
                        block[k << topBlkSh] += (block[k << topBlkSh] < 0) ? -quant : quant;
                }
            }
        }
    }
    return 0;
}
int Vc1::decodePBlock(int16_t block[64], int n,
                              int mquant, int ttmb, int firstBlock,
                              uint8_t *dst, int linesize, int skipBlock,
                              int *ttmbOut)
{
    int i, j;
    int subblkpat = 0;
    int scale, off, idx, last, skip, value;
    int ttblk = ttmb & 7;
    int pat = 0;
    int quant = std::abs(mquant);
    memset(block, 0, sizeof(int16_t) * 64);
    if (ttmb == -1) {
        ttblk = kTtblkToTt[ttIndex][gb.vlc(vlcs->ttblk[ttIndex])];
    }
    if (ttblk == kTt4x4) {
        subblkpat = ~(gb.vlc(vlcs->subblkpat[ttIndex]) + 1);
    }
    if ((ttblk != kTt8x8 && ttblk != kTt4x4)
        && ((ttmbf || (ttmb != -1 && (ttmb & 8) && !firstBlock))
            || (!resRtmFlag && !firstBlock))) {
        subblkpat = gb.decode012();
        if (subblkpat)
            subblkpat ^= 3;
        if (ttblk == kTt8x4Top || ttblk == kTt8x4Bottom)
            ttblk = kTt8x4;
        if (ttblk == kTt4x8Right || ttblk == kTt4x8Left)
            ttblk = kTt4x8;
    }
    scale = quant * 2 + ((mquant < 0) ? 0 : halfpq);
    if (ttblk == kTt8x4Top || ttblk == kTt8x4Bottom) {
        subblkpat = 2 - (ttblk == kTt8x4Top);
        ttblk     = kTt8x4;
    }
    if (ttblk == kTt4x8Right || ttblk == kTt4x8Left) {
        subblkpat = 2 - (ttblk == kTt4x8Left);
        ttblk     = kTt4x8;
    }
    switch (ttblk) {
    case kTt8x8:
        pat  = 0xF;
        i    = 0;
        last = 0;
        while (!last) {
            int ret = decodeAcCoeff(&last, &skip, &value, codingset2);
            if (ret < 0)
                return ret;
            i += skip;
            if (i > 63)
                break;
            if (!fcm)
                idx = zz8x8[0][i++];
            else
                idx = zzi8x8[i++];
            block[idx] = value * scale;
            if (!pquantizer)
                block[idx] += (block[idx] < 0) ? -quant : quant;
        }
        if (!skipBlock) {
            if (i == 1)
                vc1dsp.invTrans8x8Dc(dst, linesize, block);
            else {
                vc1dsp.invTrans8x8(block);
                addPixelsClamped(block, dst, linesize);
            }
        }
        break;
    case kTt4x4:
        pat = ~subblkpat & 0xF;
        for (j = 0; j < 4; j++) {
            last = subblkpat & (1 << (3 - j));
            i    = 0;
            off  = (j & 1) * 4 + (j & 2) * 16;
            while (!last) {
                int ret = decodeAcCoeff(&last, &skip, &value, codingset2);
                if (ret < 0)
                    return ret;
                i += skip;
                if (i > 15)
                    break;
                if (!fcm)
                    idx = kSimpleProgressive4x4Zz[i++];
                else
                    idx = kAdvInterlaced4x4Zz[i++];
                block[idx + off] = value * scale;
                if (!pquantizer)
                    block[idx + off] += (block[idx + off] < 0) ? -quant : quant;
            }
            if (!(subblkpat & (1 << (3 - j))) && !skipBlock) {
                if (i == 1)
                    vc1dsp.invTrans4x4Dc(dst + (j & 1) * 4 + (j & 2) * 2 * linesize, linesize, block + off);
                else
                    vc1dsp.invTrans4x4(dst + (j & 1) * 4 + (j & 2) *  2 * linesize, linesize, block + off);
            }
        }
        break;
    case kTt8x4:
        pat = ~((subblkpat & 2) * 6 + (subblkpat & 1) * 3) & 0xF;
        for (j = 0; j < 2; j++) {
            last = subblkpat & (1 << (1 - j));
            i    = 0;
            off  = j * 32;
            while (!last) {
                int ret = decodeAcCoeff(&last, &skip, &value, codingset2);
                if (ret < 0)
                    return ret;
                i += skip;
                if (i > 31)
                    break;
                if (!fcm)
                    idx = zz8x4[i++] + off;
                else
                    idx = kAdvInterlaced8x4Zz[i++] + off;
                block[idx] = value * scale;
                if (!pquantizer)
                    block[idx] += (block[idx] < 0) ? -quant : quant;
            }
            if (!(subblkpat & (1 << (1 - j))) && !skipBlock) {
                if (i == 1)
                    vc1dsp.invTrans8x4Dc(dst + j * 4 * linesize, linesize, block + off);
                else
                    vc1dsp.invTrans8x4(dst + j * 4 * linesize, linesize, block + off);
            }
        }
        break;
    case kTt4x8:
        pat = ~(subblkpat * 5) & 0xF;
        for (j = 0; j < 2; j++) {
            last = subblkpat & (1 << (1 - j));
            i    = 0;
            off  = j * 4;
            while (!last) {
                int ret = decodeAcCoeff(&last, &skip, &value, codingset2);
                if (ret < 0)
                    return ret;
                i += skip;
                if (i > 31)
                    break;
                if (!fcm)
                    idx = zz4x8[i++] + off;
                else
                    idx = kAdvInterlaced4x8Zz[i++] + off;
                block[idx] = value * scale;
                if (!pquantizer)
                    block[idx] += (block[idx] < 0) ? -quant : quant;
            }
            if (!(subblkpat & (1 << (1 - j))) && !skipBlock) {
                if (i == 1)
                    vc1dsp.invTrans4x8Dc(dst + j * 4, linesize, block + off);
                else
                    vc1dsp.invTrans4x8(dst + j*4, linesize, block + off);
            }
        }
        break;
    }
    if (ttmbOut)
        *ttmbOut |= ttblk << (n * 4);
    return pat;
}
int Vc1::decodePMb()
{
    int i, j;
    int mbPos = mbX + mbY * mbStride;
    int cbp;
    int mquant;
    int ttmb = ttfrm;
    int mbHasCoeffs = 1;
    int dmvX, dmvY;
    int val;
    int firstBlock = 1;
    int dstIdx, off;
    int skipped, fourmv;
    int blockCbp = 0, pat, blockTt = 0, blockIntra = 0;
    int ret;
    mquant = pq;
    if (mvTypeIsRaw)
        fourmv = gb.bit();
    else
        fourmv = mvTypeMbPlane[mbPos];
    if (skipIsRaw)
        skipped = gb.bit();
    else
        skipped = mbskipTable[mbPos];
    if (!fourmv) {
        if (!skipped) {
            getMvdata(dmvX, dmvY, mbHasCoeffs);
            if (mbIntra) {
                curPic.motionVal[1][blockIndex[0]][0] = 0;
                curPic.motionVal[1][blockIndex[0]][1] = 0;
            }
            curPic.mbType[mbPos] = mbIntra ? kMbTypeIntra : kMbType16x16;
            predMv(0, dmvX, dmvY, 1, rangeX, rangeY, mbType, 0, 0);
            if (mbIntra && !mbHasCoeffs) {
                mquant = getMquant(mquant);
                acPred = gb.bit();
                cbp        = 0;
            } else if (mbHasCoeffs) {
                if (mbIntra)
                    acPred = gb.bit();
                cbp = gb.vlc(cbpcyVlc);
                mquant = getMquant(mquant);
            } else {
                mquant = pq;
                cbp    = 0;
            }
            curPic.qscaleTable[mbPos] = mquant;
            if (!ttmbf && !mbIntra && mbHasCoeffs)
                ttmb = gb.vlc(vlcs->ttmb[ttIndex]);
            if (!mbIntra) mc1mv(0);
            dstIdx = 0;
            for (i = 0; i < 6; i++) {
                dcVal[blockIndex[i]] = 0;
                dstIdx += i >> 2;
                val = ((cbp >> (5 - i)) & 1);
                off = (i & 4) ? 0 : ((i & 1) * 8 + (i & 2) * 4 * linesize);
                mbType[blockIndex[i]] = mbIntra;
                if (mbIntra) {
                    aAvail = cAvail = 0;
                    if (i == 2 || i == 3 || !firstSliceLine)
                        aAvail = mbType[blockIndex[i] - blockWrap[i]];
                    if (i == 1 || i == 3 || mbX)
                        cAvail = mbType[blockIndex[i] - 1];
                    ret = decodeIntraBlock(block[curBlkIdx][blockMap[i]], i, val, mquant,
                                                 (i & 4) ? codingset2 : codingset);
                    if (ret < 0)
                        return ret;
                    vc1dsp.invTrans8x8(block[curBlkIdx][blockMap[i]]);
                    if (rangeredfrm)
                        for (j = 0; j < 64; j++)
                            block[curBlkIdx][blockMap[i]][j] *= 2;
                    blockCbp   |= 0xF << (i << 2);
                    blockIntra |= 1 << i;
                } else if (val) {
                    pat = decodePBlock(block[curBlkIdx][blockMap[i]], i, mquant, ttmb, firstBlock,
                                             dest[dstIdx] + off, (i & 4) ? uvlinesize : linesize,
                                             0, &blockTt);
                    if (pat < 0)
                        return pat;
                    blockCbp |= pat << (i << 2);
                    if (!ttmbf && ttmb < 8)
                        ttmb = -1;
                    firstBlock = 0;
                }
            }
        } else {
            mbIntra = 0;
            for (i = 0; i < 6; i++) {
                mbType[blockIndex[i]] = 0;
                dcVal[blockIndex[i]]  = 0;
            }
            curPic.mbType[mbPos]      = kMbTypeSkip;
            curPic.qscaleTable[mbPos] = 0;
            predMv(0, 0, 0, 1, rangeX, rangeY, mbType, 0, 0);
            mc1mv(0);
        }
    } else {
        if (!skipped ) {
            int intraCount = 0, codedInter = 0;
            int isIntra[6], isCoded[6];
            cbp = gb.vlc(cbpcyVlc);
            for (i = 0; i < 6; i++) {
                val = ((cbp >> (5 - i)) & 1);
                dcVal[blockIndex[i]] = 0;
                mbIntra                     = 0;
                if (i < 4) {
                    dmvX = dmvY = 0;
                    mbIntra   = 0;
                    mbHasCoeffs = 0;
                    if (val) {
                        getMvdata(dmvX, dmvY, mbHasCoeffs);
                    }
                    predMv(i, dmvX, dmvY, 0, rangeX, rangeY, mbType, 0, 0);
                    if (!mbIntra)
                        mc4mvLuma(i, 0, 0);
                    intraCount += mbIntra;
                    isIntra[i]  = mbIntra;
                    isCoded[i]  = mbHasCoeffs;
                }
                if (i & 4) {
                    isIntra[i] = (intraCount >= 3);
                    isCoded[i] = val;
                }
                if (i == 4)
                    mc4mvChroma(0);
                mbType[blockIndex[i]] = isIntra[i];
                if (!codedInter)
                    codedInter = (!isIntra[i]) & isCoded[i];
            }
            dstIdx = 0;
            if (!intraCount && !codedInter)
                goto end;
            mquant = getMquant(mquant);
            curPic.qscaleTable[mbPos] = mquant;
            {
                int intrapred = 0;
                for (i = 0; i < 6; i++)
                    if (isIntra[i]) {
                        if (((!firstSliceLine || (i == 2 || i == 3)) && mbType[blockIndex[i] - blockWrap[i]])
                            || ((mbX || (i == 1 || i == 3)) && mbType[blockIndex[i] - 1])) {
                            intrapred = 1;
                            break;
                        }
                    }
                if (intrapred)
                    acPred = gb.bit();
                else
                    acPred = 0;
            }
            if (!ttmbf && codedInter)
                ttmb = gb.vlc(vlcs->ttmb[ttIndex]);
            for (i = 0; i < 6; i++) {
                dstIdx    += i >> 2;
                off         = (i & 4) ? 0 : ((i & 1) * 8 + (i & 2) * 4 * linesize);
                mbIntra = isIntra[i];
                if (isIntra[i]) {
                    aAvail = cAvail = 0;
                    if (i == 2 || i == 3 || !firstSliceLine)
                        aAvail = mbType[blockIndex[i] - blockWrap[i]];
                    if (i == 1 || i == 3 || mbX)
                        cAvail = mbType[blockIndex[i] - 1];
                    ret = decodeIntraBlock(block[curBlkIdx][blockMap[i]], i, isCoded[i], mquant,
                                                 (i & 4) ? codingset2 : codingset);
                    if (ret < 0)
                        return ret;
                    vc1dsp.invTrans8x8(block[curBlkIdx][blockMap[i]]);
                    if (rangeredfrm)
                        for (j = 0; j < 64; j++)
                            block[curBlkIdx][blockMap[i]][j] *= 2;
                    blockCbp   |= 0xF << (i << 2);
                    blockIntra |= 1 << i;
                } else if (isCoded[i]) {
                    pat = decodePBlock(block[curBlkIdx][blockMap[i]], i, mquant, ttmb,
                                             firstBlock, dest[dstIdx] + off,
                                             (i & 4) ? uvlinesize : linesize,
                                             0,
                                             &blockTt);
                    if (pat < 0)
                        return pat;
                    blockCbp |= pat << (i << 2);
                    if (!ttmbf && ttmb < 8)
                        ttmb = -1;
                    firstBlock = 0;
                }
            }
        } else {
            mbIntra                               = 0;
            curPic.qscaleTable[mbPos] = 0;
            for (i = 0; i < 6; i++) {
                mbType[blockIndex[i]] = 0;
                dcVal[blockIndex[i]]  = 0;
            }
            for (i = 0; i < 4; i++) {
                predMv(i, 0, 0, 0, rangeX, rangeY, mbType, 0, 0);
                mc4mvLuma(i, 0, 0);
            }
            mc4mvChroma(0);
            curPic.qscaleTable[mbPos] = 0;
        }
    }
end:
    if (overlap && pq >= 9)
        pOverlapFilter();
    putBlocksClamped(1);
    this->cbp[mbX]      = blockCbp;
    ttblk[mbX]    = blockTt;
    this->isIntra[mbX] = blockIntra;
    return 0;
}
int Vc1::decodePMbIntfr()
{
    int i;
    int mbPos = mbX + mbY * mbStride;
    int cbp = 0;
    int mquant;
    int ttmb = ttfrm;
    int mbHasCoeffs = 1;
    int dmvX, dmvY;
    int val;
    int firstBlock = 1;
    int dstIdx, off;
    int skipped, fourmv = 0, twomv = 0;
    int blockCbp = 0, pat, blockTt = 0;
    int idxMbmode = 0, mvbp;
    int fieldtx;
    int ret;
    mquant = pq;
    if (skipIsRaw)
        skipped = gb.bit();
    else
        skipped = mbskipTable[mbPos];
    if (!skipped) {
        if (fourmvswitch)
            idxMbmode = gb.vlc(mbmodeVlc);
        else
            idxMbmode = gb.vlc(mbmodeVlc);
        switch (kMbmodeIntfrp[fourmvswitch][idxMbmode][0]) {
        case kMvPmodeIntfr4mv:
            fourmv = 1;
            blkMvType[blockIndex[0]] = 0;
            blkMvType[blockIndex[1]] = 0;
            blkMvType[blockIndex[2]] = 0;
            blkMvType[blockIndex[3]] = 0;
            break;
        case kMvPmodeIntfr4mvField:
            fourmv = 1;
            blkMvType[blockIndex[0]] = 1;
            blkMvType[blockIndex[1]] = 1;
            blkMvType[blockIndex[2]] = 1;
            blkMvType[blockIndex[3]] = 1;
            break;
        case kMvPmodeIntfr2mvField:
            twomv = 1;
            blkMvType[blockIndex[0]] = 1;
            blkMvType[blockIndex[1]] = 1;
            blkMvType[blockIndex[2]] = 1;
            blkMvType[blockIndex[3]] = 1;
            break;
        case kMvPmodeIntfr1mv:
            blkMvType[blockIndex[0]] = 0;
            blkMvType[blockIndex[1]] = 0;
            blkMvType[blockIndex[2]] = 0;
            blkMvType[blockIndex[3]] = 0;
            break;
        }
        if (kMbmodeIntfrp[fourmvswitch][idxMbmode][0] == kMvPmodeIntfrIntra) {
            for (i = 0; i < 4; i++) {
                curPic.motionVal[1][blockIndex[i]][0] = 0;
                curPic.motionVal[1][blockIndex[i]][1] = 0;
            }
            isIntra[mbX] = 0x3f;
            mbIntra          = 1;
            curPic.mbType[mbPos] = kMbTypeIntra;
            fieldtx = fieldtxPlane[mbPos] = gb.bit();
            mbHasCoeffs = gb.bit();
            if (mbHasCoeffs)
                cbp = 1 + gb.vlc(cbpcyVlc);
            acPred = acpredPlane[mbPos] = gb.bit();
            mquant = getMquant(mquant);
            curPic.qscaleTable[mbPos] = mquant;
            yDcScale = kWmv3DcScaleTable[std::abs(mquant)];
            dstIdx = 0;
            for (i = 0; i < 6; i++) {
                aAvail = cAvail          = 0;
                mbType[blockIndex[i]] = 1;
                dcVal[blockIndex[i]]  = 0;
                dstIdx += i >> 2;
                val = ((cbp >> (5 - i)) & 1);
                if (i == 2 || i == 3 || !firstSliceLine)
                    aAvail = mbType[blockIndex[i] - blockWrap[i]];
                if (i == 1 || i == 3 || mbX)
                    cAvail = mbType[blockIndex[i] - 1];
                ret = decodeIntraBlock(block[curBlkIdx][blockMap[i]], i, val, mquant,
                                             (i & 4) ? codingset2 : codingset);
                if (ret < 0)
                    return ret;
                vc1dsp.invTrans8x8(block[curBlkIdx][blockMap[i]]);
                blockCbp |= 0xf << (i << 2);
            }
        } else {
            mbHasCoeffs = kMbmodeIntfrp[fourmvswitch][idxMbmode][3];
            if (mbHasCoeffs)
                cbp = 1 + gb.vlc(cbpcyVlc);
            if (kMbmodeIntfrp[fourmvswitch][idxMbmode][0] == kMvPmodeIntfr2mvField) {
                twomvbp = gb.vlc(twomvbpVlc);
            } else {
                if ((kMbmodeIntfrp[fourmvswitch][idxMbmode][0] == kMvPmodeIntfr4mv)
                    || (kMbmodeIntfrp[fourmvswitch][idxMbmode][0] == kMvPmodeIntfr4mvField)) {
                    fourmvbp = gb.vlc(fourmvbpVlc);
                }
            }
            mbIntra = isIntra[mbX] = 0;
            for (i = 0; i < 6; i++)
                mbType[blockIndex[i]] = 0;
            fieldtx = fieldtxPlane[mbPos] = kMbmodeIntfrp[fourmvswitch][idxMbmode][1];
            dstIdx = 0;
            if (fourmv) {
                mvbp = fourmvbp;
                for (i = 0; i < 4; i++) {
                    dmvX = dmvY = 0;
                    if (mvbp & (8 >> i))
                        getMvdataInterlaced(&dmvX, &dmvY, 0);
                    predMvIntfr(i, dmvX, dmvY, 0, rangeX, rangeY, 0);
                    mc4mvLuma(i, 0, 0);
                }
                mc4mvChroma4(0, 0, 0);
            } else if (twomv) {
                mvbp  = twomvbp;
                dmvX = dmvY = 0;
                if (mvbp & 2) {
                    getMvdataInterlaced(&dmvX, &dmvY, 0);
                }
                predMvIntfr(0, dmvX, dmvY, 2, rangeX, rangeY, 0);
                mc4mvLuma(0, 0, 0);
                mc4mvLuma(1, 0, 0);
                dmvX = dmvY = 0;
                if (mvbp & 1) {
                    getMvdataInterlaced(&dmvX, &dmvY, 0);
                }
                predMvIntfr(2, dmvX, dmvY, 2, rangeX, rangeY, 0);
                mc4mvLuma(2, 0, 0);
                mc4mvLuma(3, 0, 0);
                mc4mvChroma4(0, 0, 0);
            } else {
                mvbp = kMbmodeIntfrp[fourmvswitch][idxMbmode][2];
                dmvX = dmvY = 0;
                if (mvbp) {
                    getMvdataInterlaced(&dmvX, &dmvY, 0);
                }
                predMvIntfr(0, dmvX, dmvY, 1, rangeX, rangeY, 0);
                mc1mv(0);
            }
            if (cbp)
                mquant = getMquant(mquant);
            curPic.qscaleTable[mbPos] = mquant;
            if (!ttmbf && cbp)
                ttmb = gb.vlc(vlcs->ttmb[ttIndex]);
            for (i = 0; i < 6; i++) {
                dcVal[blockIndex[i]] = 0;
                dstIdx += i >> 2;
                val = ((cbp >> (5 - i)) & 1);
                if (!fieldtx)
                    off = (i & 4) ? 0 : ((i & 1) * 8 + (i & 2) * 4 * linesize);
                else
                    off = (i & 4) ? 0 : ((i & 1) * 8 + ((i > 1) * linesize));
                if (val) {
                    pat = decodePBlock(block[curBlkIdx][blockMap[i]], i, mquant, ttmb,
                                             firstBlock, dest[dstIdx] + off,
                                             (i & 4) ? uvlinesize : (linesize << fieldtx),
                                             0, &blockTt);
                    if (pat < 0)
                        return pat;
                    blockCbp |= pat << (i << 2);
                    if (!ttmbf && ttmb < 8)
                        ttmb = -1;
                    firstBlock = 0;
                }
            }
        }
    } else {
        mbIntra = isIntra[mbX] = 0;
        for (i = 0; i < 6; i++) {
            mbType[blockIndex[i]] = 0;
            dcVal[blockIndex[i]] = 0;
        }
        curPic.mbType[mbPos]      = kMbTypeSkip;
        curPic.qscaleTable[mbPos] = 0;
        blkMvType[blockIndex[0]] = 0;
        blkMvType[blockIndex[1]] = 0;
        blkMvType[blockIndex[2]] = 0;
        blkMvType[blockIndex[3]] = 0;
        predMvIntfr(0, 0, 0, 1, rangeX, rangeY, 0);
        mc1mv(0);
        fieldtxPlane[mbPos] = 0;
    }
    if (overlap && pq >= 9)
        pOverlapFilter();
    putBlocksClamped(1);
    this->cbp[mbX]      = blockCbp;
    ttblk[mbX]    = blockTt;
    return 0;
}
int Vc1::decodePMbIntfi()
{
    int i;
    int mbPos = mbX + mbY * mbStride;
    int cbp = 0;
    int mquant;
    int ttmb = ttfrm;
    int mbHasCoeffs = 1;
    int dmvX, dmvY;
    int val;
    int firstBlock = 1;
    int dstIdx, off;
    int predFlag = 0;
    int blockCbp = 0, pat, blockTt = 0;
    int idxMbmode = 0;
    int ret;
    mquant = pq;
    idxMbmode = gb.vlc(mbmodeVlc);
    if (idxMbmode <= 1) {
        isIntra[mbX] = 0x3f;
        mbIntra          = 1;
        curPic.motionVal[1][blockIndex[0] + blocksOff][0] = 0;
        curPic.motionVal[1][blockIndex[0] + blocksOff][1] = 0;
        curPic.mbType[mbPos + mbOff] = kMbTypeIntra;
        mquant = getMquant(mquant);
        curPic.qscaleTable[mbPos] = mquant;
        yDcScale = kWmv3DcScaleTable[std::abs(mquant)];
        acPred  = acpredPlane[mbPos] = gb.bit();
        mbHasCoeffs = idxMbmode & 1;
        if (mbHasCoeffs)
            cbp = 1 + gb.vlc(cbpcyVlc);
        dstIdx = 0;
        for (i = 0; i < 6; i++) {
            aAvail = cAvail          = 0;
            mbType[blockIndex[i]] = 1;
            dcVal[blockIndex[i]]  = 0;
            dstIdx += i >> 2;
            val = ((cbp >> (5 - i)) & 1);
            if (i == 2 || i == 3 || !firstSliceLine)
                aAvail = mbType[blockIndex[i] - blockWrap[i]];
            if (i == 1 || i == 3 || mbX)
                cAvail = mbType[blockIndex[i] - 1];
            ret = decodeIntraBlock(block[curBlkIdx][blockMap[i]], i, val, mquant,
                                         (i & 4) ? codingset2 : codingset);
            if (ret < 0)
                return ret;
            vc1dsp.invTrans8x8(block[curBlkIdx][blockMap[i]]);
            blockCbp |= 0xf << (i << 2);
        }
    } else {
        mbIntra = isIntra[mbX] = 0;
        curPic.mbType[mbPos + mbOff] = kMbType16x16;
        for (i = 0; i < 6; i++)
            mbType[blockIndex[i]] = 0;
        if (idxMbmode <= 5) {
            dmvX = dmvY = predFlag = 0;
            if (idxMbmode & 1) {
                getMvdataInterlaced(&dmvX, &dmvY, &predFlag);
            }
            predMv(0, dmvX, dmvY, 1, rangeX, rangeY, mbType, predFlag, 0);
            mc1mv(0);
            mbHasCoeffs = !(idxMbmode & 2);
        } else {
            fourmvbp = gb.vlc(fourmvbpVlc);
            for (i = 0; i < 4; i++) {
                dmvX = dmvY = predFlag = 0;
                if (fourmvbp & (8 >> i))
                    getMvdataInterlaced(&dmvX, &dmvY, &predFlag);
                predMv(i, dmvX, dmvY, 0, rangeX, rangeY, mbType, predFlag, 0);
                mc4mvLuma(i, 0, 0);
            }
            mc4mvChroma(0);
            mbHasCoeffs = idxMbmode & 1;
        }
        if (mbHasCoeffs)
            cbp = 1 + gb.vlc(cbpcyVlc);
        if (cbp) {
            mquant = getMquant(mquant);
        }
        curPic.qscaleTable[mbPos] = mquant;
        if (!ttmbf && cbp) {
            ttmb = gb.vlc(vlcs->ttmb[ttIndex]);
        }
        dstIdx = 0;
        for (i = 0; i < 6; i++) {
            dcVal[blockIndex[i]] = 0;
            dstIdx += i >> 2;
            val = ((cbp >> (5 - i)) & 1);
            off = (i & 4) ? 0 : (i & 1) * 8 + (i & 2) * 4 * linesize;
            if (val) {
                pat = decodePBlock(block[curBlkIdx][blockMap[i]], i, mquant, ttmb,
                                         firstBlock, dest[dstIdx] + off,
                                         (i & 4) ? uvlinesize : linesize,
                                         0,
                                         &blockTt);
                if (pat < 0)
                    return pat;
                blockCbp |= pat << (i << 2);
                if (!ttmbf && ttmb < 8)
                    ttmb = -1;
                firstBlock = 0;
            }
        }
    }
    if (overlap && pq >= 9)
        pOverlapFilter();
    putBlocksClamped(1);
    this->cbp[mbX]      = blockCbp;
    ttblk[mbX]    = blockTt;
    return 0;
}
int Vc1::decodeBMb()
{
    int i, j;
    int mbPos = mbX + mbY * mbStride;
    int cbp = 0;
    int mquant;
    int ttmb = ttfrm;
    int mbHasCoeffs = 0;
    int val;
    int firstBlock = 1;
    int dstIdx, off;
    int skipped, direct;
    int dmvX[2], dmvY[2];
    int bmvtype = kBmvTypeBackward;
    int ret;
    mquant      = pq;
    mbIntra = 0;
    if (dmbIsRaw)
        direct = gb.bit();
    else
        direct = directMbPlane[mbPos];
    if (skipIsRaw)
        skipped = gb.bit();
    else
        skipped = mbskipTable[mbPos];
    dmvX[0] = dmvX[1] = dmvY[0] = dmvY[1] = 0;
    for (i = 0; i < 6; i++) {
        mbType[blockIndex[i]] = 0;
        dcVal[blockIndex[i]]  = 0;
    }
    curPic.qscaleTable[mbPos] = 0;
    if (!direct) {
        if (!skipped) {
            getMvdata(dmvX[0], dmvY[0], mbHasCoeffs);
            dmvX[1] = dmvX[0];
            dmvY[1] = dmvY[0];
        }
        if (skipped || !mbIntra) {
            bmvtype = gb.decode012();
            switch (bmvtype) {
            case 0:
                bmvtype = (bfraction >= (kBFractionDen/2)) ? kBmvTypeBackward : kBmvTypeForward;
                break;
            case 1:
                bmvtype = (bfraction >= (kBFractionDen/2)) ? kBmvTypeForward : kBmvTypeBackward;
                break;
            case 2:
                bmvtype  = kBmvTypeInterpolated;
                dmvX[0] = dmvY[0] = 0;
            }
        }
    }
    for (i = 0; i < 6; i++)
        mbType[blockIndex[i]] = mbIntra;
    if (skipped) {
        if (direct)
            bmvtype = kBmvTypeInterpolated;
        predBMv(dmvX, dmvY, direct, bmvtype);
        bMc(dmvX, dmvY, direct, bmvtype);
        return 0;
    }
    if (direct) {
        cbp = gb.vlc(cbpcyVlc);
        mquant = getMquant(mquant);
        mbIntra = 0;
        curPic.qscaleTable[mbPos] = mquant;
        if (!ttmbf)
            ttmb = gb.vlc(vlcs->ttmb[ttIndex]);
        dmvX[0] = dmvY[0] = dmvX[1] = dmvY[1] = 0;
        predBMv(dmvX, dmvY, direct, bmvtype);
        bMc(dmvX, dmvY, direct, bmvtype);
    } else {
        if (!mbHasCoeffs && !mbIntra) {
            predBMv(dmvX, dmvY, direct, bmvtype);
            bMc(dmvX, dmvY, direct, bmvtype);
            return 0;
        }
        if (mbIntra && !mbHasCoeffs) {
            mquant = getMquant(mquant);
            curPic.qscaleTable[mbPos] = mquant;
            acPred = gb.bit();
            cbp = 0;
            predBMv(dmvX, dmvY, direct, bmvtype);
        } else {
            if (bmvtype == kBmvTypeInterpolated) {
                getMvdata(dmvX[0], dmvY[0], mbHasCoeffs);
                if (!mbHasCoeffs) {
                    predBMv(dmvX, dmvY, direct, bmvtype);
                    bMc(dmvX, dmvY, direct, bmvtype);
                    return 0;
                }
            }
            predBMv(dmvX, dmvY, direct, bmvtype);
            if (!mbIntra) {
                bMc(dmvX, dmvY, direct, bmvtype);
            }
            if (mbIntra)
                acPred = gb.bit();
            cbp = gb.vlc(cbpcyVlc);
            mquant = getMquant(mquant);
            curPic.qscaleTable[mbPos] = mquant;
            if (!ttmbf && !mbIntra && mbHasCoeffs)
                ttmb = gb.vlc(vlcs->ttmb[ttIndex]);
        }
    }
    dstIdx = 0;
    for (i = 0; i < 6; i++) {
        dcVal[blockIndex[i]] = 0;
        dstIdx += i >> 2;
        val = ((cbp >> (5 - i)) & 1);
        off = (i & 4) ? 0 : ((i & 1) * 8 + (i & 2) * 4 * linesize);
        mbType[blockIndex[i]] = mbIntra;
        if (mbIntra) {
            aAvail = cAvail = 0;
            if (i == 2 || i == 3 || !firstSliceLine)
                aAvail = mbType[blockIndex[i] - blockWrap[i]];
            if (i == 1 || i == 3 || mbX)
                cAvail = mbType[blockIndex[i] - 1];
            ret = decodeIntraBlock(blocks[i], i, val, mquant,
                                         (i & 4) ? codingset2 : codingset);
            if (ret < 0)
                return ret;
            vc1dsp.invTrans8x8(blocks[i]);
            if (rangeredfrm)
                for (j = 0; j < 64; j++)
                    blocks[i][j] *= 2;
            putSignedPixelsClamped(blocks[i],
                                              dest[dstIdx] + off,
                                              i & 4 ? uvlinesize
                                                    : linesize);
        } else if (val) {
            int pat = decodePBlock(blocks[i], i, mquant, ttmb,
                                         firstBlock, dest[dstIdx] + off,
                                         (i & 4) ? uvlinesize : linesize,
                                         0, nullptr);
            if (pat < 0)
                return pat;
            if (!ttmbf && ttmb < 8)
                ttmb = -1;
            firstBlock = 0;
        }
    }
    return 0;
}
int Vc1::decodeBMbIntfi()
{
    int i, j;
    int mbPos = mbX + mbY * mbStride;
    int cbp = 0;
    int mquant;
    int ttmb = ttfrm;
    int mbHasCoeffs = 0;
    int val;
    int firstBlock = 1;
    int dstIdx, off;
    int fwd;
    int dmvX[2], dmvY[2], predFlag[2];
    int bmvtype = kBmvTypeBackward;
    int blockCbp = 0, pat, blockTt = 0;
    int idxMbmode;
    int ret;
    mquant      = pq;
    mbIntra = 0;
    idxMbmode = gb.vlc(mbmodeVlc);
    if (idxMbmode <= 1) {
        isIntra[mbX] = 0x3f;
        mbIntra          = 1;
        curPic.motionVal[1][blockIndex[0]][0] = 0;
        curPic.motionVal[1][blockIndex[0]][1] = 0;
        curPic.mbType[mbPos + mbOff]         = kMbTypeIntra;
        mquant = getMquant(mquant);
        curPic.qscaleTable[mbPos] = mquant;
        yDcScale = kWmv3DcScaleTable[std::abs(mquant)];
        acPred  = acpredPlane[mbPos] = gb.bit();
        mbHasCoeffs = idxMbmode & 1;
        if (mbHasCoeffs)
            cbp = 1 + gb.vlc(cbpcyVlc);
        dstIdx = 0;
        for (i = 0; i < 6; i++) {
            aAvail = cAvail          = 0;
            mbType[blockIndex[i]] = 1;
            dcVal[blockIndex[i]]  = 0;
            dstIdx += i >> 2;
            val = ((cbp >> (5 - i)) & 1);
            if (i == 2 || i == 3 || !firstSliceLine)
                aAvail = mbType[blockIndex[i] - blockWrap[i]];
            if (i == 1 || i == 3 || mbX)
                cAvail = mbType[blockIndex[i] - 1];
            ret = decodeIntraBlock(blocks[i], i, val, mquant,
                                         (i & 4) ? codingset2 : codingset);
            if (ret < 0)
                return ret;
            vc1dsp.invTrans8x8(blocks[i]);
            if (rangeredfrm)
                for (j = 0; j < 64; j++)
                    blocks[i][j] <<= 1;
            off  = (i & 4) ? 0 : ((i & 1) * 8 + (i & 2) * 4 * linesize);
            putSignedPixelsClamped(blocks[i],
                                              dest[dstIdx] + off,
                                              (i & 4) ? uvlinesize
                                                      : linesize);
        }
    } else {
        mbIntra = isIntra[mbX] = 0;
        curPic.mbType[mbPos + mbOff] = kMbType16x16;
        for (i = 0; i < 6; i++)
            mbType[blockIndex[i]] = 0;
        if (fmbIsRaw)
            fwd = forwardMbPlane[mbPos] = gb.bit();
        else
            fwd = forwardMbPlane[mbPos];
        if (idxMbmode <= 5) {
            int interpmvp = 0;
            dmvX[0]     = dmvX[1] = dmvY[0] = dmvY[1] = 0;
            predFlag[0] = predFlag[1] = 0;
            if (fwd)
                bmvtype = kBmvTypeForward;
            else {
                bmvtype = gb.decode012();
                switch (bmvtype) {
                case 0:
                    bmvtype = kBmvTypeBackward;
                    break;
                case 1:
                    bmvtype = kBmvTypeDirect;
                    break;
                case 2:
                    bmvtype   = kBmvTypeInterpolated;
                    interpmvp = gb.bit();
                }
            }
            this->bmvtype = bmvtype;
            if (bmvtype != kBmvTypeDirect && idxMbmode & 1) {
                getMvdataInterlaced(&dmvX[bmvtype == kBmvTypeBackward], &dmvY[bmvtype == kBmvTypeBackward], &predFlag[bmvtype == kBmvTypeBackward]);
            }
            if (interpmvp) {
                getMvdataInterlaced(&dmvX[1], &dmvY[1], &predFlag[1]);
            }
            if (bmvtype == kBmvTypeDirect) {
                dmvX[0] = dmvY[0] = predFlag[0] = 0;
                dmvX[1] = dmvY[1] = predFlag[0] = 0;
                if (!nextPic.ptr->fieldPicture) {
                    return -1;
                }
            }
            predBMvIntfi(0, dmvX, dmvY, 1, predFlag);
            bMc(dmvX, dmvY, (bmvtype == kBmvTypeDirect), bmvtype);
            mbHasCoeffs = !(idxMbmode & 2);
        } else {
            if (fwd)
                bmvtype = kBmvTypeForward;
            this->bmvtype  = bmvtype;
            fourmvbp = gb.vlc(fourmvbpVlc);
            for (i = 0; i < 4; i++) {
                dmvX[0] = dmvY[0] = predFlag[0] = 0;
                dmvX[1] = dmvY[1] = predFlag[1] = 0;
                if (fourmvbp & (8 >> i)) {
                    getMvdataInterlaced(&dmvX[bmvtype == kBmvTypeBackward],
                                             &dmvY[bmvtype == kBmvTypeBackward],
                                         &predFlag[bmvtype == kBmvTypeBackward]);
                }
                predBMvIntfi(i, dmvX, dmvY, 0, predFlag);
                mc4mvLuma(i, bmvtype == kBmvTypeBackward, 0);
            }
            mc4mvChroma(bmvtype == kBmvTypeBackward);
            mbHasCoeffs = idxMbmode & 1;
        }
        if (mbHasCoeffs)
            cbp = 1 + gb.vlc(cbpcyVlc);
        if (cbp) {
            mquant = getMquant(mquant);
        }
        curPic.qscaleTable[mbPos] = mquant;
        if (!ttmbf && cbp) {
            ttmb = gb.vlc(vlcs->ttmb[ttIndex]);
        }
        dstIdx = 0;
        for (i = 0; i < 6; i++) {
            dcVal[blockIndex[i]] = 0;
            dstIdx += i >> 2;
            val = ((cbp >> (5 - i)) & 1);
            off = (i & 4) ? 0 : (i & 1) * 8 + (i & 2) * 4 * linesize;
            if (val) {
                pat = decodePBlock(blocks[i], i, mquant, ttmb,
                                         firstBlock, dest[dstIdx] + off,
                                         (i & 4) ? uvlinesize : linesize,
                                         0, &blockTt);
                if (pat < 0)
                    return pat;
                blockCbp |= pat << (i << 2);
                if (!ttmbf && ttmb < 8)
                    ttmb = -1;
                firstBlock = 0;
            }
        }
    }
    this->cbp[mbX]      = blockCbp;
    ttblk[mbX]    = blockTt;
    return 0;
}
int Vc1::decodeBMbIntfr()
{
    int i, j;
    int mbPos = mbX + mbY * mbStride;
    int cbp = 0;
    int mquant;
    int ttmb = ttfrm;
    int mvsw = 0;
    int mbHasCoeffs = 1;
    int dmvX, dmvY;
    int val;
    int firstBlock = 1;
    int dstIdx, off;
    int skipped, direct, twomv = 0;
    int blockCbp = 0, pat, blockTt = 0;
    int idxMbmode = 0, mvbp;
    int strideY, fieldtx;
    int bmvtype = kBmvTypeBackward;
    int dir, dir2;
    int ret;
    mquant = pq;
    mbIntra = 0;
    if (skipIsRaw)
        skipped = gb.bit();
    else
        skipped = mbskipTable[mbPos];
    if (!skipped) {
        idxMbmode = gb.vlc(mbmodeVlc);
        if (kMbmodeIntfrp[0][idxMbmode][0] == kMvPmodeIntfr2mvField) {
            twomv = 1;
            blkMvType[blockIndex[0]] = 1;
            blkMvType[blockIndex[1]] = 1;
            blkMvType[blockIndex[2]] = 1;
            blkMvType[blockIndex[3]] = 1;
        } else {
            blkMvType[blockIndex[0]] = 0;
            blkMvType[blockIndex[1]] = 0;
            blkMvType[blockIndex[2]] = 0;
            blkMvType[blockIndex[3]] = 0;
        }
    }
    if (kMbmodeIntfrp[0][idxMbmode][0] == kMvPmodeIntfrIntra) {
        for (i = 0; i < 4; i++) {
            mv[0][i][0] = curPic.motionVal[0][blockIndex[i]][0] = 0;
            mv[0][i][1] = curPic.motionVal[0][blockIndex[i]][1] = 0;
            mv[1][i][0] = curPic.motionVal[1][blockIndex[i]][0] = 0;
            mv[1][i][1] = curPic.motionVal[1][blockIndex[i]][1] = 0;
        }
        isIntra[mbX] = 0x3f;
        mbIntra          = 1;
        curPic.mbType[mbPos] = kMbTypeIntra;
        fieldtx = fieldtxPlane[mbPos] = gb.bit();
        mbHasCoeffs = gb.bit();
        if (mbHasCoeffs)
            cbp = 1 + gb.vlc(cbpcyVlc);
        acPred = acpredPlane[mbPos] = gb.bit();
        mquant = getMquant(mquant);
        curPic.qscaleTable[mbPos] = mquant;
        yDcScale = kWmv3DcScaleTable[std::abs(mquant)];
        dstIdx = 0;
        for (i = 0; i < 6; i++) {
            aAvail = cAvail          = 0;
            mbType[blockIndex[i]] = 1;
            dcVal[blockIndex[i]]  = 0;
            dstIdx += i >> 2;
            val = ((cbp >> (5 - i)) & 1);
            if (i == 2 || i == 3 || !firstSliceLine)
                aAvail = mbType[blockIndex[i] - blockWrap[i]];
            if (i == 1 || i == 3 || mbX)
                cAvail = mbType[blockIndex[i] - 1];
            ret = decodeIntraBlock(blocks[i], i, val, mquant,
                                         (i & 4) ? codingset2 : codingset);
            if (ret < 0)
                return ret;
            vc1dsp.invTrans8x8(blocks[i]);
            if (i < 4) {
                strideY = linesize << fieldtx;
                off = (fieldtx) ? ((i & 1) * 8) + ((i & 2) >> 1) * linesize : (i & 1) * 8 + 4 * (i & 2) * linesize;
            } else {
                strideY = uvlinesize;
                off = 0;
            }
            putSignedPixelsClamped(blocks[i],
                                              dest[dstIdx] + off,
                                              strideY);
        }
    } else {
        mbIntra = isIntra[mbX] = 0;
        if (dmbIsRaw)
            direct = gb.bit();
        else
            direct = directMbPlane[mbPos];
        if (direct) {
            if (nextPic.ptr->fieldPicture)
                {}
            mv[0][0][0] = curPic.motionVal[0][blockIndex[0]][0] = scaleMv(nextPic.motionVal[1][blockIndex[0]][0], bfraction, 0, quarterSample);
            mv[0][0][1] = curPic.motionVal[0][blockIndex[0]][1] = scaleMv(nextPic.motionVal[1][blockIndex[0]][1], bfraction, 0, quarterSample);
            mv[1][0][0] = curPic.motionVal[1][blockIndex[0]][0] = scaleMv(nextPic.motionVal[1][blockIndex[0]][0], bfraction, 1, quarterSample);
            mv[1][0][1] = curPic.motionVal[1][blockIndex[0]][1] = scaleMv(nextPic.motionVal[1][blockIndex[0]][1], bfraction, 1, quarterSample);
            if (twomv) {
                mv[0][2][0] = curPic.motionVal[0][blockIndex[2]][0] = scaleMv(nextPic.motionVal[1][blockIndex[2]][0], bfraction, 0, quarterSample);
                mv[0][2][1] = curPic.motionVal[0][blockIndex[2]][1] = scaleMv(nextPic.motionVal[1][blockIndex[2]][1], bfraction, 0, quarterSample);
                mv[1][2][0] = curPic.motionVal[1][blockIndex[2]][0] = scaleMv(nextPic.motionVal[1][blockIndex[2]][0], bfraction, 1, quarterSample);
                mv[1][2][1] = curPic.motionVal[1][blockIndex[2]][1] = scaleMv(nextPic.motionVal[1][blockIndex[2]][1], bfraction, 1, quarterSample);
                for (i = 1; i < 4; i += 2) {
                    mv[0][i][0] = curPic.motionVal[0][blockIndex[i]][0] = mv[0][i-1][0];
                    mv[0][i][1] = curPic.motionVal[0][blockIndex[i]][1] = mv[0][i-1][1];
                    mv[1][i][0] = curPic.motionVal[1][blockIndex[i]][0] = mv[1][i-1][0];
                    mv[1][i][1] = curPic.motionVal[1][blockIndex[i]][1] = mv[1][i-1][1];
                }
            } else {
                for (i = 1; i < 4; i++) {
                    mv[0][i][0] = curPic.motionVal[0][blockIndex[i]][0] = mv[0][0][0];
                    mv[0][i][1] = curPic.motionVal[0][blockIndex[i]][1] = mv[0][0][1];
                    mv[1][i][0] = curPic.motionVal[1][blockIndex[i]][0] = mv[1][0][0];
                    mv[1][i][1] = curPic.motionVal[1][blockIndex[i]][1] = mv[1][0][1];
                }
            }
        }
        if (!direct) {
            if (skipped || !mbIntra) {
                bmvtype = gb.decode012();
                switch (bmvtype) {
                case 0:
                    bmvtype = (bfraction >= (kBFractionDen/2)) ? kBmvTypeBackward : kBmvTypeForward;
                    break;
                case 1:
                    bmvtype = (bfraction >= (kBFractionDen/2)) ? kBmvTypeForward : kBmvTypeBackward;
                    break;
                case 2:
                    bmvtype  = kBmvTypeInterpolated;
                }
            }
            if (twomv && bmvtype != kBmvTypeInterpolated)
                mvsw = gb.bit();
        }
        if (!skipped) {
            mbHasCoeffs = kMbmodeIntfrp[0][idxMbmode][3];
            if (mbHasCoeffs)
                cbp = 1 + gb.vlc(cbpcyVlc);
            if (!direct) {
                if (bmvtype == kBmvTypeInterpolated && twomv) {
                    fourmvbp = gb.vlc(fourmvbpVlc);
                } else if (bmvtype == kBmvTypeInterpolated || twomv) {
                    twomvbp = gb.vlc(twomvbpVlc);
                }
            }
            for (i = 0; i < 6; i++)
                mbType[blockIndex[i]] = 0;
            fieldtx = fieldtxPlane[mbPos] = kMbmodeIntfrp[0][idxMbmode][1];
            dstIdx = 0;
            if (direct) {
                if (twomv) {
                    for (i = 0; i < 4; i++) {
                        mc4mvLuma(i, 0, 0);
                        mc4mvLuma(i, 1, 1);
                    }
                    mc4mvChroma4(0, 0, 0);
                    mc4mvChroma4(1, 1, 1);
                } else {
                    mc1mv(0);
                    interpMc();
                }
            } else if (twomv && bmvtype == kBmvTypeInterpolated) {
                mvbp = fourmvbp;
                for (i = 0; i < 4; i++) {
                    dir = i==1 || i==3;
                    dmvX = dmvY = 0;
                    val = ((mvbp >> (3 - i)) & 1);
                    if (val)
                        getMvdataInterlaced(&dmvX, &dmvY, 0);
                    j = i > 1 ? 2 : 0;
                    predMvIntfr(j, dmvX, dmvY, 2, rangeX, rangeY, dir);
                    mc4mvLuma(j, dir, dir);
                    mc4mvLuma(j+1, dir, dir);
                }
                mc4mvChroma4(0, 0, 0);
                mc4mvChroma4(1, 1, 1);
            } else if (bmvtype == kBmvTypeInterpolated) {
                mvbp = twomvbp;
                dmvX = dmvY = 0;
                if (mvbp & 2)
                    getMvdataInterlaced(&dmvX, &dmvY, 0);
                predMvIntfr(0, dmvX, dmvY, 1, rangeX, rangeY, 0);
                mc1mv(0);
                dmvX = dmvY = 0;
                if (mvbp & 1)
                    getMvdataInterlaced(&dmvX, &dmvY, 0);
                predMvIntfr(0, dmvX, dmvY, 1, rangeX, rangeY, 1);
                interpMc();
            } else if (twomv) {
                dir = bmvtype == kBmvTypeBackward;
                dir2 = dir;
                if (mvsw)
                    dir2 = !dir;
                mvbp = twomvbp;
                dmvX = dmvY = 0;
                if (mvbp & 2)
                    getMvdataInterlaced(&dmvX, &dmvY, 0);
                predMvIntfr(0, dmvX, dmvY, 2, rangeX, rangeY, dir);
                dmvX = dmvY = 0;
                if (mvbp & 1)
                    getMvdataInterlaced(&dmvX, &dmvY, 0);
                predMvIntfr(2, dmvX, dmvY, 2, rangeX, rangeY, dir2);
                if (mvsw) {
                    for (i = 0; i < 2; i++) {
                        mv[dir][i+2][0] = mv[dir][i][0] = curPic.motionVal[dir][blockIndex[i+2]][0] = curPic.motionVal[dir][blockIndex[i]][0];
                        mv[dir][i+2][1] = mv[dir][i][1] = curPic.motionVal[dir][blockIndex[i+2]][1] = curPic.motionVal[dir][blockIndex[i]][1];
                        mv[dir2][i+2][0] = mv[dir2][i][0] = curPic.motionVal[dir2][blockIndex[i]][0] = curPic.motionVal[dir2][blockIndex[i+2]][0];
                        mv[dir2][i+2][1] = mv[dir2][i][1] = curPic.motionVal[dir2][blockIndex[i]][1] = curPic.motionVal[dir2][blockIndex[i+2]][1];
                    }
                } else {
                    predMvIntfr(0, 0, 0, 2, rangeX, rangeY, !dir);
                    predMvIntfr(2, 0, 0, 2, rangeX, rangeY, !dir);
                }
                mc4mvLuma(0, dir, 0);
                mc4mvLuma(1, dir, 0);
                mc4mvLuma(2, dir2, 0);
                mc4mvLuma(3, dir2, 0);
                mc4mvChroma4(dir, dir2, 0);
            } else {
                dir = bmvtype == kBmvTypeBackward;
                mvbp = kMbmodeIntfrp[0][idxMbmode][2];
                dmvX = dmvY = 0;
                if (mvbp)
                    getMvdataInterlaced(&dmvX, &dmvY, 0);
                predMvIntfr(0, dmvX, dmvY, 1, rangeX, rangeY, dir);
                blkMvType[blockIndex[0]] = 1;
                blkMvType[blockIndex[1]] = 1;
                blkMvType[blockIndex[2]] = 1;
                blkMvType[blockIndex[3]] = 1;
                predMvIntfr(0, 0, 0, 2, rangeX, rangeY, !dir);
                for (i = 0; i < 2; i++) {
                    mv[!dir][i+2][0] = mv[!dir][i][0] = curPic.motionVal[!dir][blockIndex[i+2]][0] = curPic.motionVal[!dir][blockIndex[i]][0];
                    mv[!dir][i+2][1] = mv[!dir][i][1] = curPic.motionVal[!dir][blockIndex[i+2]][1] = curPic.motionVal[!dir][blockIndex[i]][1];
                }
                mc1mv(dir);
            }
            if (cbp)
                mquant = getMquant(mquant);
            curPic.qscaleTable[mbPos] = mquant;
            if (!ttmbf && cbp)
                ttmb = gb.vlc(vlcs->ttmb[ttIndex]);
            for (i = 0; i < 6; i++) {
                dcVal[blockIndex[i]] = 0;
                dstIdx += i >> 2;
                val = ((cbp >> (5 - i)) & 1);
                if (!fieldtx)
                    off = (i & 4) ? 0 : ((i & 1) * 8 + (i & 2) * 4 * linesize);
                else
                    off = (i & 4) ? 0 : ((i & 1) * 8 + ((i > 1) * linesize));
                if (val) {
                    pat = decodePBlock(blocks[i], i, mquant, ttmb,
                                             firstBlock, dest[dstIdx] + off,
                                             (i & 4) ? uvlinesize : (linesize << fieldtx),
                                             0, &blockTt);
                    if (pat < 0)
                        return pat;
                    blockCbp |= pat << (i << 2);
                    if (!ttmbf && ttmb < 8)
                        ttmb = -1;
                    firstBlock = 0;
                }
            }
        } else {
            dir = 0;
            for (i = 0; i < 6; i++) {
                mbType[blockIndex[i]] = 0;
                dcVal[blockIndex[i]] = 0;
            }
            curPic.mbType[mbPos]      = kMbTypeSkip;
            curPic.qscaleTable[mbPos] = 0;
            blkMvType[blockIndex[0]] = 0;
            blkMvType[blockIndex[1]] = 0;
            blkMvType[blockIndex[2]] = 0;
            blkMvType[blockIndex[3]] = 0;
            if (!direct) {
                if (bmvtype == kBmvTypeInterpolated) {
                    predMvIntfr(0, 0, 0, 1, rangeX, rangeY, 0);
                    predMvIntfr(0, 0, 0, 1, rangeX, rangeY, 1);
                } else {
                    dir = bmvtype == kBmvTypeBackward;
                    predMvIntfr(0, 0, 0, 1, rangeX, rangeY, dir);
                    if (mvsw) {
                        int dir2 = dir;
                        if (mvsw)
                            dir2 = !dir;
                        for (i = 0; i < 2; i++) {
                            mv[dir][i+2][0] = mv[dir][i][0] = curPic.motionVal[dir][blockIndex[i+2]][0] = curPic.motionVal[dir][blockIndex[i]][0];
                            mv[dir][i+2][1] = mv[dir][i][1] = curPic.motionVal[dir][blockIndex[i+2]][1] = curPic.motionVal[dir][blockIndex[i]][1];
                            mv[dir2][i+2][0] = mv[dir2][i][0] = curPic.motionVal[dir2][blockIndex[i]][0] = curPic.motionVal[dir2][blockIndex[i+2]][0];
                            mv[dir2][i+2][1] = mv[dir2][i][1] = curPic.motionVal[dir2][blockIndex[i]][1] = curPic.motionVal[dir2][blockIndex[i+2]][1];
                        }
                    } else {
                        blkMvType[blockIndex[0]] = 1;
                        blkMvType[blockIndex[1]] = 1;
                        blkMvType[blockIndex[2]] = 1;
                        blkMvType[blockIndex[3]] = 1;
                        predMvIntfr(0, 0, 0, 2, rangeX, rangeY, !dir);
                        for (i = 0; i < 2; i++) {
                            mv[!dir][i+2][0] = mv[!dir][i][0] = curPic.motionVal[!dir][blockIndex[i+2]][0] = curPic.motionVal[!dir][blockIndex[i]][0];
                            mv[!dir][i+2][1] = mv[!dir][i][1] = curPic.motionVal[!dir][blockIndex[i+2]][1] = curPic.motionVal[!dir][blockIndex[i]][1];
                        }
                    }
                }
            }
            mc1mv(dir);
            if (direct || bmvtype == kBmvTypeInterpolated) {
                interpMc();
            }
            fieldtxPlane[mbPos] = 0;
        }
    }
    this->cbp[mbX]      = blockCbp;
    ttblk[mbX]    = blockTt;
    return 0;
}
void Vc1::decodeIBlocks()
{
    int k, j;
    int cbp, val;
    int mbPos;
    switch (yAcTableIndex) {
    case 0:
        codingset = (pqindex <= 8) ? kCsHighRateIntra : kCsLowMotIntra;
        break;
    case 1:
        codingset = kCsHighMotIntra;
        break;
    case 2:
        codingset = kCsMidRateIntra;
        break;
    }
    switch (cAcTableIndex) {
    case 0:
        codingset2 = (pqindex <= 8) ? kCsHighRateInter : kCsLowMotInter;
        break;
    case 1:
        codingset2 = kCsHighMotInter;
        break;
    case 2:
        codingset2 = kCsMidRateInter;
        break;
    }
    yDcScale = kWmv3DcScaleTable[pq];
    mbX = mbY = 0;
    mbIntra         = 1;
    firstSliceLine = 1;
    for (mbY = startMbY; mbY < endMbY; mbY++) {
        mbX = 0;
        initBlockIndex();
        for (; mbX < endMbX; mbX++) {
            updateBlockIndex();
            memset(block[curBlkIdx][0], 0, sizeof(int16_t) * 6 * 64);
            mbPos = mbX + mbY * mbWidth;
            curPic.mbType[mbPos]                     = kMbTypeIntra;
            curPic.qscaleTable[mbPos]                = pq;
            for (int i = 0; i < 4; i++) {
                curPic.motionVal[1][blockIndex[i]][0] = 0;
                curPic.motionVal[1][blockIndex[i]][1] = 0;
            }
            cbp = gb.vlc(vlcs->mbI);
            acPred = gb.bit();
            for (k = 0; k < 6; k++) {
                mbType[blockIndex[k]] = 1;
                val = ((cbp >> (5 - k)) & 1);
                if (k < 4)
                    val = codedBlockPred(k, val);
                cbp |= val << (5 - k);
                decodeIBlock(block[curBlkIdx][blockMap[k]], k, val, (k < 4) ? codingset : codingset2);
                vc1dsp.invTrans8x8(block[curBlkIdx][blockMap[k]]);
            }
            if (overlap && pq >= 9) {
                iOverlapFilter();
                if (rangeredfrm)
                    for (k = 0; k < 6; k++)
                        for (j = 0; j < 64; j++)
                            block[curBlkIdx][blockMap[k]][j] *= 2;
                putBlocksClamped(1);
            } else {
                if (rangeredfrm)
                    for (k = 0; k < 6; k++)
                        for (j = 0; j < 64; j++)
                            block[curBlkIdx][blockMap[k]][j] = (block[curBlkIdx][blockMap[k]][j] - 64) * 2;
                putBlocksClamped(0);
            }
            if (loopFilter)
                iLoopFilter();
            if (gb.left() < 0) {
                return;
            }
            topleftBlkIdx = (topleftBlkIdx + 1) % (endMbX + 2);
            topBlkIdx = (topBlkIdx + 1) % (endMbX + 2);
            leftBlkIdx = (leftBlkIdx + 1) % (endMbX + 2);
            curBlkIdx = (curBlkIdx + 1) % (endMbX + 2);
        }
        firstSliceLine = 0;
    }
}
int Vc1::decodeIBlocksAdv()
{
    int k;
    int cbp, val;
    int mbPos;
    int mquant;
    if (gb.left() <= 1)
        return -1;
    switch (yAcTableIndex) {
    case 0:
        codingset = (pqindex <= 8) ? kCsHighRateIntra : kCsLowMotIntra;
        break;
    case 1:
        codingset = kCsHighMotIntra;
        break;
    case 2:
        codingset = kCsMidRateIntra;
        break;
    }
    switch (cAcTableIndex) {
    case 0:
        codingset2 = (pqindex <= 8) ? kCsHighRateInter : kCsLowMotInter;
        break;
    case 1:
        codingset2 = kCsHighMotInter;
        break;
    case 2:
        codingset2 = kCsMidRateInter;
        break;
    }
    mbIntra         = 1;
    firstSliceLine = 1;
    mbX             = 0;
    mbY             = startMbY;
    if (startMbY) {
        memset(&codedBlock[(2 * mbY - 1) * b8Stride - 2], 0,
               (1 + b8Stride) * sizeof(*codedBlock));
    }
    for (; mbY < endMbY; mbY++) {
        mbX = 0;
        initBlockIndex();
        for (;mbX < mbWidth; mbX++) {
            mquant = pq;
            updateBlockIndex();
            memset(block[curBlkIdx][0], 0, sizeof(int16_t) * 6 * 64);
            mbPos = mbX + mbY * mbStride;
            curPic.mbType[mbPos + mbOff]                         = kMbTypeIntra;
            for (int i = 0; i < 4; i++) {
                curPic.motionVal[1][blockIndex[i] + blocksOff][0] = 0;
                curPic.motionVal[1][blockIndex[i] + blocksOff][1] = 0;
            }
            if (fieldtxIsRaw)
                fieldtxPlane[mbPos] = gb.bit();
            if (gb.left() <= 1) {
                return 0;
            }
            cbp = gb.vlc(vlcs->mbI);
            if (acpredIsRaw)
                acPred = gb.bit();
            else
                acPred = acpredPlane[mbPos];
            if (condover == kCondoverSelect && overflgIsRaw)
                overFlagsPlane[mbPos] = gb.bit();
            mquant = getMquant(mquant);
            curPic.qscaleTable[mbPos] = mquant;
            yDcScale = kWmv3DcScaleTable[std::abs(mquant)];
            for (k = 0; k < 6; k++) {
                mbType[blockIndex[k]] = 1;
                val = ((cbp >> (5 - k)) & 1);
                if (k < 4)
                    val = codedBlockPred(k, val);
                cbp |= val << (5 - k);
                aAvail = !firstSliceLine || (k == 2 || k == 3);
                cAvail = !!mbX || (k == 1 || k == 3);
                decodeIBlockAdv(block[curBlkIdx][blockMap[k]], k, val,
                                       (k < 4) ? codingset : codingset2, mquant);
                vc1dsp.invTrans8x8(block[curBlkIdx][blockMap[k]]);
            }
            if (overlap && (pq >= 9 || condover != kCondoverNone))
                iOverlapFilter();
            putBlocksClamped(1);
            if (loopFilter)
                iLoopFilter();
            if (gb.left() < 0) {
                return 0;
            }
            incBlkIdx(topleftBlkIdx, nAllocatedBlks);
            incBlkIdx(topBlkIdx, nAllocatedBlks);
            incBlkIdx(leftBlkIdx, nAllocatedBlks);
            incBlkIdx(curBlkIdx, nAllocatedBlks);
        }
        firstSliceLine = 0;
    }
    return 0;
}
void Vc1::decodePBlocks()
{
    int applyLoopFilter;
    int ret;
    switch (cAcTableIndex) {
    case 0:
        codingset = (pqindex <= 8) ? kCsHighRateIntra : kCsLowMotIntra;
        break;
    case 1:
        codingset = kCsHighMotIntra;
        break;
    case 2:
        codingset = kCsMidRateIntra;
        break;
    }
    switch (cAcTableIndex) {
    case 0:
        codingset2 = (pqindex <= 8) ? kCsHighRateInter : kCsLowMotInter;
        break;
    case 1:
        codingset2 = kCsHighMotInter;
        break;
    case 2:
        codingset2 = kCsMidRateInter;
        break;
    }
    applyLoopFilter   = loopFilter;
    firstSliceLine = 1;
    memset(cbpBase, 0, sizeof(cbpBase[0]) * 3 * mbStride);
    for (mbY = startMbY; mbY < endMbY; mbY++) {
        mbX = 0;
        initBlockIndex();
        for (; mbX < mbWidth; mbX++) {
            updateBlockIndex();
            if (fcm == kIlaceField || (fcm == kProgressive && mvTypeIsRaw) || skipIsRaw)
                if (gb.left() < 1) {
                    return;
                }
            if (fcm == kIlaceField) {
                ret = decodePMbIntfi();
                if (applyLoopFilter)
                    pLoopFilter();
            } else if (fcm == kIlaceFrame) {
                ret = decodePMbIntfr();
                if (applyLoopFilter)
                    pIntfrLoopFilter();
            } else {
                ret = decodePMb();
                if (applyLoopFilter)
                    pLoopFilter();
            }
            if (ret < 0 || gb.left() < 0 || gb.count() < 0) {
                return;
            }
            incBlkIdx(topleftBlkIdx, nAllocatedBlks);
            incBlkIdx(topBlkIdx, nAllocatedBlks);
            incBlkIdx(leftBlkIdx, nAllocatedBlks);
            incBlkIdx(curBlkIdx, nAllocatedBlks);
        }
        memmove(cbpBase,
                cbp - mbStride,
                sizeof(cbpBase[0]) * 2 * mbStride);
        memmove(ttblkBase,
                ttblk - mbStride,
                sizeof(ttblkBase[0]) * 2 * mbStride);
        memmove(isIntraBase,
                isIntra - mbStride,
                sizeof(isIntraBase[0]) * 2 * mbStride);
        memmove(lumaMvBase,
                lumaMv - mbStride,
                sizeof(lumaMvBase[0]) * 2 * mbStride);
        firstSliceLine = 0;
    }
}
void Vc1::decodeBBlocks()
{
    switch (cAcTableIndex) {
    case 0:
        codingset = (pqindex <= 8) ? kCsHighRateIntra : kCsLowMotIntra;
        break;
    case 1:
        codingset = kCsHighMotIntra;
        break;
    case 2:
        codingset = kCsMidRateIntra;
        break;
    }
    switch (cAcTableIndex) {
    case 0:
        codingset2 = (pqindex <= 8) ? kCsHighRateInter : kCsLowMotInter;
        break;
    case 1:
        codingset2 = kCsHighMotInter;
        break;
    case 2:
        codingset2 = kCsMidRateInter;
        break;
    }
    firstSliceLine = 1;
    for (mbY = startMbY; mbY < endMbY; mbY++) {
        mbX = 0;
        initBlockIndex();
        for (; mbX < mbWidth; mbX++) {
            updateBlockIndex();
            if (fcm == kIlaceField || skipIsRaw || dmbIsRaw)
                if (gb.left() < 1) {
                    return;
                }
            if (fcm == kIlaceField) {
                decodeBMbIntfi();
                if (loopFilter)
                    bIntfiLoopFilter();
            } else if (fcm == kIlaceFrame) {
                decodeBMbIntfr();
                if (loopFilter)
                    pIntfrLoopFilter();
            } else {
                decodeBMb();
                if (loopFilter)
                    iLoopFilter();
            }
            if (gb.left() < 0 || gb.count() < 0) {
                return;
            }
        }
        memmove(cbpBase,
                cbp - mbStride,
                sizeof(cbpBase[0]) * 2 * mbStride);
        memmove(ttblkBase,
                ttblk - mbStride,
                sizeof(ttblkBase[0]) * 2 * mbStride);
        memmove(isIntraBase,
                isIntra - mbStride,
                sizeof(isIntraBase[0]) * 2 * mbStride);
        firstSliceLine = 0;
    }
}
void Vc1::decodeSkipBlocks()
{
    if (!lastPic.data[0])
        return;
    firstSliceLine = 1;
    for (mbY = startMbY; mbY < endMbY; mbY++) {
        mbX = 0;
        initBlockIndex();
        updateBlockIndex();
        memcpy(dest[0], lastPic.data[0] + mbY * 16 * linesize,   linesize   * 16);
        memcpy(dest[1], lastPic.data[1] + mbY *  8 * uvlinesize, uvlinesize *  8);
        memcpy(dest[2], lastPic.data[2] + mbY *  8 * uvlinesize, uvlinesize *  8);
        firstSliceLine = 0;
    }
}
void Vc1::decodeBlocks()
{
    esc3LevelLength = 0;
    if (x8Type) {
        broken = true;
    } else {
        curBlkIdx     =  0;
        leftBlkIdx    = -1;
        topleftBlkIdx =  1;
        topBlkIdx     =  2;
        switch (pictType) {
        case kPictureI:
            if (profile == kProfileAdvanced)
                decodeIBlocksAdv();
            else
                decodeIBlocks();
            break;
        case kPictureP:
            if (pFrameSkipped)
                decodeSkipBlocks();
            else
                decodePBlocks();
            break;
        case kPictureB:
            if (biType) {
                if (profile == kProfileAdvanced)
                    decodeIBlocksAdv();
                else
                    decodeIBlocks();
            } else
                decodeBBlocks();
            break;
        }
    }
}

}

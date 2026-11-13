#include "vc1.h"
#include <cstring>

namespace absent::vc1 {


int Vc1::scaleforsameX(const int n , int dir)
{
    int scaledvalue, refdist;
    int scalesame1, scalesame2;
    int scalezone1X, zone1offsetX;
    int tableIndex = dir ^ secondField;
    if (pictType != kPictureB)
        refdist = this->refdist;
    else
        refdist = dir ? brfd : frfd;
    if (refdist > 3)
        refdist = 3;
    scalesame1    = kFieldMvpredScales[tableIndex][1][refdist];
    scalesame2    = kFieldMvpredScales[tableIndex][2][refdist];
    scalezone1X  = kFieldMvpredScales[tableIndex][3][refdist];
    zone1offsetX = kFieldMvpredScales[tableIndex][5][refdist];
    if (std::abs(n) > 255)
        scaledvalue = n;
    else {
        if (std::abs(n) < scalezone1X)
            scaledvalue = (n * scalesame1) >> 8;
        else {
            if (n < 0)
                scaledvalue = ((n * scalesame2) >> 8) - zone1offsetX;
            else
                scaledvalue = ((n * scalesame2) >> 8) + zone1offsetX;
        }
    }
    return clip(scaledvalue, -rangeX, rangeX - 1);
}
int Vc1::scaleforsameY(const int n , int dir)
{
    int scaledvalue, refdist;
    int scalesame1, scalesame2;
    int scalezone1Y, zone1offsetY;
    int tableIndex = dir ^ secondField;
    if (pictType != kPictureB)
        refdist = this->refdist;
    else
        refdist = dir ? brfd : frfd;
    if (refdist > 3)
        refdist = 3;
    scalesame1    = kFieldMvpredScales[tableIndex][1][refdist];
    scalesame2    = kFieldMvpredScales[tableIndex][2][refdist];
    scalezone1Y  = kFieldMvpredScales[tableIndex][4][refdist];
    zone1offsetY = kFieldMvpredScales[tableIndex][6][refdist];
    if (std::abs(n) > 63)
        scaledvalue = n;
    else {
        if (std::abs(n) < scalezone1Y)
            scaledvalue = (n * scalesame1) >> 8;
        else {
            if (n < 0)
                scaledvalue = ((n * scalesame2) >> 8) - zone1offsetY;
            else
                scaledvalue = ((n * scalesame2) >> 8) + zone1offsetY;
        }
    }
    if (curFieldType && !refFieldType[dir])
        return clip(scaledvalue, -rangeY / 2 + 1, rangeY / 2);
    else
        return clip(scaledvalue, -rangeY / 2, rangeY / 2 - 1);
}
int Vc1::scaleforoppX(const int n )
{
    int scalezone1X, zone1offsetX;
    int scaleopp1, scaleopp2, brfd;
    int scaledvalue;
    brfd = std::min(this->brfd, 3);
    scalezone1X  = kBFieldMvpredScales[3][brfd];
    zone1offsetX = kBFieldMvpredScales[5][brfd];
    scaleopp1     = kBFieldMvpredScales[1][brfd];
    scaleopp2     = kBFieldMvpredScales[2][brfd];
    if (std::abs(n) > 255)
        scaledvalue = n;
    else {
        if (std::abs(n) < scalezone1X)
            scaledvalue = (n * scaleopp1) >> 8;
        else {
            if (n < 0)
                scaledvalue = ((n * scaleopp2) >> 8) - zone1offsetX;
            else
                scaledvalue = ((n * scaleopp2) >> 8) + zone1offsetX;
        }
    }
    return clip(scaledvalue, -rangeX, rangeX - 1);
}
int Vc1::scaleforoppY(const int n , int dir)
{
    int scalezone1Y, zone1offsetY;
    int scaleopp1, scaleopp2, brfd;
    int scaledvalue;
    brfd = std::min(this->brfd, 3);
    scalezone1Y  = kBFieldMvpredScales[4][brfd];
    zone1offsetY = kBFieldMvpredScales[6][brfd];
    scaleopp1     = kBFieldMvpredScales[1][brfd];
    scaleopp2     = kBFieldMvpredScales[2][brfd];
    if (std::abs(n) > 63)
        scaledvalue = n;
    else {
        if (std::abs(n) < scalezone1Y)
            scaledvalue = (n * scaleopp1) >> 8;
        else {
            if (n < 0)
                scaledvalue = ((n * scaleopp2) >> 8) - zone1offsetY;
            else
                scaledvalue = ((n * scaleopp2) >> 8) + zone1offsetY;
        }
    }
    if (curFieldType && !refFieldType[dir]) {
        return clip(scaledvalue, -rangeY / 2 + 1, rangeY / 2);
    } else {
        return clip(scaledvalue, -rangeY / 2, rangeY / 2 - 1);
    }
}
int Vc1::scaleforsame(int n, int dim, int dir)
{
    int brfd, scalesame;
    int hpel = 1 - quarterSample;
    n >>= hpel;
    if (pictType != kPictureB || secondField || !dir) {
        if (dim)
            n = scaleforsameY(n, dir) * (1 << hpel);
        else
            n = scaleforsameX(n, dir) * (1 << hpel);
        return n;
    }
    brfd      = std::min(this->brfd, 3);
    scalesame = kBFieldMvpredScales[0][brfd];
    n = (n * scalesame >> 8) * (1 << hpel);
    return n;
}
int Vc1::scaleforopp(int n, int dim, int dir)
{
    int refdist, scaleopp;
    int hpel = 1 - quarterSample;
    n >>= hpel;
    if (pictType == kPictureB && !secondField && dir == 1) {
        if (dim)
            n = scaleforoppY(n, dir) * (1 << hpel);
        else
            n = scaleforoppX(n)      * (1 << hpel);
        return n;
    }
    if (pictType != kPictureB)
        refdist = this->refdist;
    else
        refdist = dir ? brfd : frfd;
    refdist = std::min(refdist, 3);
    scaleopp = kFieldMvpredScales[dir ^ secondField][0][refdist];
    n = (n * scaleopp >> 8) * (1 << hpel);
    return n;
}
void Vc1::predMv(int n, int dmvX, int dmvY,
                    int mv1, int rX, int rY, uint8_t* isIntra,
                    int predFlag, int dir)
{
    int xy, wrap, off = 0;
    int px, py;
    int sum;
    int mixedmvPic, numSamefield = 0, numOppfield = 0;
    int opposite, aF, bF, cF;
    int16_t fieldPredA[2];
    int16_t fieldPredB[2];
    int16_t fieldPredC[2];
    int aValid, bValid, cValid;
    int hybridmvThresh, yBias = 0;
    if (mvMode == kMvPmodeMixedMv ||
        ((mvMode == kMvPmodeIntensityComp) && (mvMode2 == kMvPmodeMixedMv)))
        mixedmvPic = 1;
    else
        mixedmvPic = 0;
    if (!quarterSample) {
        dmvX *= 2;
        dmvY *= 2;
    }
    wrap = b8Stride;
    xy   = blockIndex[n];
    if (mbIntra) {
        mv[0][n][0] = curPic.motionVal[0][xy + blocksOff][0] = 0;
        mv[0][n][1] = curPic.motionVal[0][xy + blocksOff][1] = 0;
        curPic.motionVal[1][xy + blocksOff][0] = 0;
        curPic.motionVal[1][xy + blocksOff][1] = 0;
        if (mv1) {
            curPic.motionVal[0][xy + 1 + blocksOff][0]        = 0;
            curPic.motionVal[0][xy + 1 + blocksOff][1]        = 0;
            curPic.motionVal[0][xy + wrap + blocksOff][0]     = 0;
            curPic.motionVal[0][xy + wrap + blocksOff][1]     = 0;
            curPic.motionVal[0][xy + wrap + 1 + blocksOff][0] = 0;
            curPic.motionVal[0][xy + wrap + 1 + blocksOff][1] = 0;
            lumaMv[mbX][0] = lumaMv[mbX][1] = 0;
            curPic.motionVal[1][xy + 1 + blocksOff][0]        = 0;
            curPic.motionVal[1][xy + 1 + blocksOff][1]        = 0;
            curPic.motionVal[1][xy + wrap + blocksOff][0]     = 0;
            curPic.motionVal[1][xy + wrap + blocksOff][1]     = 0;
            curPic.motionVal[1][xy + wrap + 1 + blocksOff][0] = 0;
            curPic.motionVal[1][xy + wrap + 1 + blocksOff][1] = 0;
        }
        return;
    }
    aValid = !firstSliceLine || (n == 2 || n == 3);
    bValid = aValid;
    cValid = mbX || (n == 1 || n == 3);
    if (mv1) {
        if (fieldMode && mixedmvPic)
            off = (mbX == (mbWidth - 1)) ? -2 : 2;
        else
            off = (mbX == (mbWidth - 1)) ? -1 : 2;
        bValid = bValid && mbWidth > 1;
    } else {
        switch (n) {
        case 0:
            if (resRtmFlag)
                off = mbX ? -1 : 1;
            else
                off = mbX ? -1 : 2 * mbWidth - wrap - 1;
            break;
        case 1:
            off = (mbX == (mbWidth - 1)) ? -1 : 1;
            break;
        case 2:
            off = 1;
            break;
        case 3:
            off = -1;
        }
        if (fieldMode && mbWidth == 1)
            bValid = bValid && cValid;
    }
    if (fieldMode) {
        aValid = aValid && !isIntra[xy - wrap];
        bValid = bValid && !isIntra[xy - wrap + off];
        cValid = cValid && !isIntra[xy - 1];
    }
    if (aValid) {
        const int16_t *A = curPic.motionVal[dir][xy - wrap + blocksOff];
        aF = mvF[dir][xy - wrap + blocksOff];
        numOppfield  += aF;
        numSamefield += 1 - aF;
        fieldPredA[0] = A[0];
        fieldPredA[1] = A[1];
    } else {
        fieldPredA[0] = fieldPredA[1] = 0;
        aF = 0;
    }
    if (bValid) {
        const int16_t *B = curPic.motionVal[dir][xy - wrap + off + blocksOff];
        bF = mvF[dir][xy - wrap + off + blocksOff];
        numOppfield  += bF;
        numSamefield += 1 - bF;
        fieldPredB[0] = B[0];
        fieldPredB[1] = B[1];
    } else {
        fieldPredB[0] = fieldPredB[1] = 0;
        bF = 0;
    }
    if (cValid) {
        const int16_t *C = curPic.motionVal[dir][xy - 1 + blocksOff];
        cF = mvF[dir][xy - 1 + blocksOff];
        numOppfield  += cF;
        numSamefield += 1 - cF;
        fieldPredC[0] = C[0];
        fieldPredC[1] = C[1];
    } else {
        fieldPredC[0] = fieldPredC[1] = 0;
        cF = 0;
    }
    if (fieldMode) {
        if (!numref)
            opposite = 1 - reffield;
        else {
            if (numSamefield <= numOppfield)
                opposite = 1 - predFlag;
            else
                opposite = predFlag;
        }
    } else
        opposite = 0;
    if (opposite) {
        mvF[dir][xy + blocksOff] = 1;
        refFieldType[dir] = !curFieldType;
        if (aValid && !aF) {
            fieldPredA[0] = scaleforopp(fieldPredA[0], 0, dir);
            fieldPredA[1] = scaleforopp(fieldPredA[1], 1, dir);
        }
        if (bValid && !bF) {
            fieldPredB[0] = scaleforopp(fieldPredB[0], 0, dir);
            fieldPredB[1] = scaleforopp(fieldPredB[1], 1, dir);
        }
        if (cValid && !cF) {
            fieldPredC[0] = scaleforopp(fieldPredC[0], 0, dir);
            fieldPredC[1] = scaleforopp(fieldPredC[1], 1, dir);
        }
    } else {
        mvF[dir][xy + blocksOff] = 0;
        refFieldType[dir] = curFieldType;
        if (aValid && aF) {
            fieldPredA[0] = scaleforsame(fieldPredA[0], 0, dir);
            fieldPredA[1] = scaleforsame(fieldPredA[1], 1, dir);
        }
        if (bValid && bF) {
            fieldPredB[0] = scaleforsame(fieldPredB[0], 0, dir);
            fieldPredB[1] = scaleforsame(fieldPredB[1], 1, dir);
        }
        if (cValid && cF) {
            fieldPredC[0] = scaleforsame(fieldPredC[0], 0, dir);
            fieldPredC[1] = scaleforsame(fieldPredC[1], 1, dir);
        }
    }
    if (aValid) {
        px = fieldPredA[0];
        py = fieldPredA[1];
    } else if (cValid) {
        px = fieldPredC[0];
        py = fieldPredC[1];
    } else if (bValid) {
        px = fieldPredB[0];
        py = fieldPredB[1];
    } else {
        px = 0;
        py = 0;
    }
    if (numSamefield + numOppfield > 1) {
        px = midPred(fieldPredA[0], fieldPredB[0], fieldPredC[0]);
        py = midPred(fieldPredA[1], fieldPredB[1], fieldPredC[1]);
    }
    if (!fieldMode) {
        int qx, qy, X, Y;
        int MV = mv1 ? -60 : -28;
        qx = (mbX << 6) + ((n == 1 || n == 3) ? 32 : 0);
        qy = (mbY << 6) + ((n == 2 || n == 3) ? 32 : 0);
        X  = (mbWidth  << 6) - 4;
        Y  = (mbHeight << 6) - 4;
        if (qx + px < MV) px = MV - qx;
        if (qy + py < MV) py = MV - qy;
        if (qx + px > X) px = X - qx;
        if (qy + py > Y) py = Y - qy;
    }
    if (!fieldMode || pictType != kPictureB) {
        hybridmvThresh = 32;
        if (aValid && cValid) {
            if (isIntra[xy - wrap])
                sum = std::abs(px) + std::abs(py);
            else
                sum = std::abs(px - fieldPredA[0]) + std::abs(py - fieldPredA[1]);
            if (sum > hybridmvThresh) {
                if (gb.bit()) {
                    px = fieldPredA[0];
                    py = fieldPredA[1];
                } else {
                    px = fieldPredC[0];
                    py = fieldPredC[1];
                }
            } else {
                if (isIntra[xy - 1])
                    sum = std::abs(px) + std::abs(py);
                else
                    sum = std::abs(px - fieldPredC[0]) + std::abs(py - fieldPredC[1]);
                if (sum > hybridmvThresh) {
                    if (gb.bit()) {
                        px = fieldPredA[0];
                        py = fieldPredA[1];
                    } else {
                        px = fieldPredC[0];
                        py = fieldPredC[1];
                    }
                }
            }
        }
    }
    if (fieldMode && numref)
        rY >>= 1;
    if (fieldMode && curFieldType && refFieldType[dir] == 0)
        yBias = 1;
    mv[dir][n][0] = curPic.motionVal[dir][xy + blocksOff][0] = ((px + dmvX + rX) & ((rX << 1) - 1)) - rX;
    mv[dir][n][1] = curPic.motionVal[dir][xy + blocksOff][1] = ((py + dmvY + rY - yBias) & ((rY << 1) - 1)) - rY + yBias;
    if (mv1) {
        curPic.motionVal[dir][xy +    1 +     blocksOff][0] = curPic.motionVal[dir][xy + blocksOff][0];
        curPic.motionVal[dir][xy +    1 +     blocksOff][1] = curPic.motionVal[dir][xy + blocksOff][1];
        curPic.motionVal[dir][xy + wrap +     blocksOff][0] = curPic.motionVal[dir][xy + blocksOff][0];
        curPic.motionVal[dir][xy + wrap +     blocksOff][1] = curPic.motionVal[dir][xy + blocksOff][1];
        curPic.motionVal[dir][xy + wrap + 1 + blocksOff][0] = curPic.motionVal[dir][xy + blocksOff][0];
        curPic.motionVal[dir][xy + wrap + 1 + blocksOff][1] = curPic.motionVal[dir][xy + blocksOff][1];
        mvF[dir][xy +    1 + blocksOff] = mvF[dir][xy +            blocksOff];
        mvF[dir][xy + wrap + blocksOff] = mvF[dir][xy + wrap + 1 + blocksOff] = mvF[dir][xy + blocksOff];
    }
}
void Vc1::predMvIntfr(int n, int dmvX, int dmvY,
                          int mvn, int rX, int rY, int dir)
{
    int xy, wrap, off = 0;
    int A[2], B[2], C[2];
    int px = 0, py = 0;
    int aValid = 0, bValid = 0, cValid = 0;
    int fieldA, fieldB, fieldC;
    int totalValid, numSamefield, numOppfield;
    int posC, posB, nAdj;
    wrap = b8Stride;
    xy = blockIndex[n];
    if (mbIntra) {
        mv[0][n][0] = curPic.motionVal[0][xy][0] = 0;
        mv[0][n][1] = curPic.motionVal[0][xy][1] = 0;
        curPic.motionVal[1][xy][0] = 0;
        curPic.motionVal[1][xy][1] = 0;
        if (mvn == 1) {
            curPic.motionVal[0][xy + 1][0]        = 0;
            curPic.motionVal[0][xy + 1][1]        = 0;
            curPic.motionVal[0][xy + wrap][0]     = 0;
            curPic.motionVal[0][xy + wrap][1]     = 0;
            curPic.motionVal[0][xy + wrap + 1][0] = 0;
            curPic.motionVal[0][xy + wrap + 1][1] = 0;
            lumaMv[mbX][0] = lumaMv[mbX][1] = 0;
            curPic.motionVal[1][xy + 1][0]        = 0;
            curPic.motionVal[1][xy + 1][1]        = 0;
            curPic.motionVal[1][xy + wrap][0]     = 0;
            curPic.motionVal[1][xy + wrap][1]     = 0;
            curPic.motionVal[1][xy + wrap + 1][0] = 0;
            curPic.motionVal[1][xy + wrap + 1][1] = 0;
        }
        return;
    }
    off = ((n == 0) || (n == 1)) ? 1 : -1;
    if (mbX || (n == 1) || (n == 3)) {
        if ((blkMvType[xy])
            || (!blkMvType[xy] && !blkMvType[xy - 1])) {
            A[0] = curPic.motionVal[dir][xy - 1][0];
            A[1] = curPic.motionVal[dir][xy - 1][1];
            aValid = 1;
        } else {
            A[0] = (curPic.motionVal[dir][xy - 1][0]
                    + curPic.motionVal[dir][xy - 1 + off * wrap][0] + 1) >> 1;
            A[1] = (curPic.motionVal[dir][xy - 1][1]
                    + curPic.motionVal[dir][xy - 1 + off * wrap][1] + 1) >> 1;
            aValid = 1;
        }
        if (!(n & 1) && isIntra[mbX - 1]) {
            aValid = 0;
            A[0] = A[1] = 0;
        }
    } else
        A[0] = A[1] = 0;
    B[0] = B[1] = C[0] = C[1] = 0;
    if (n == 0 || n == 1 || blkMvType[xy]) {
        if (!firstSliceLine) {
            if (!isIntra[mbX - mbStride]) {
                bValid = 1;
                nAdj   = n | 2;
                posB   = blockIndex[nAdj] - 2 * wrap;
                if (blkMvType[posB] && blkMvType[xy]) {
                    nAdj = (n & 2) | (n & 1);
                }
                B[0] = curPic.motionVal[dir][blockIndex[nAdj] - 2 * wrap][0];
                B[1] = curPic.motionVal[dir][blockIndex[nAdj] - 2 * wrap][1];
                if (blkMvType[posB] && !blkMvType[xy]) {
                    B[0] = (B[0] + curPic.motionVal[dir][blockIndex[nAdj ^ 2] - 2 * wrap][0] + 1) >> 1;
                    B[1] = (B[1] + curPic.motionVal[dir][blockIndex[nAdj ^ 2] - 2 * wrap][1] + 1) >> 1;
                }
            }
            if (mbWidth > 1) {
                if (!isIntra[mbX - mbStride + 1]) {
                    cValid = 1;
                    nAdj   = 2;
                    posC   = blockIndex[2] - 2 * wrap + 2;
                    if (blkMvType[posC] && blkMvType[xy]) {
                        nAdj = n & 2;
                    }
                    C[0] = curPic.motionVal[dir][blockIndex[nAdj] - 2 * wrap + 2][0];
                    C[1] = curPic.motionVal[dir][blockIndex[nAdj] - 2 * wrap + 2][1];
                    if (blkMvType[posC] && !blkMvType[xy]) {
                        C[0] = (1 + C[0] + (curPic.motionVal[dir][blockIndex[nAdj ^ 2] - 2 * wrap + 2][0])) >> 1;
                        C[1] = (1 + C[1] + (curPic.motionVal[dir][blockIndex[nAdj ^ 2] - 2 * wrap + 2][1])) >> 1;
                    }
                    if (mbX == mbWidth - 1) {
                        if (!isIntra[mbX - mbStride - 1]) {
                            cValid = 1;
                            nAdj   = 3;
                            posC   = blockIndex[3] - 2 * wrap - 2;
                            if (blkMvType[posC] && blkMvType[xy]) {
                                nAdj = n | 1;
                            }
                            C[0] = curPic.motionVal[dir][blockIndex[nAdj] - 2 * wrap - 2][0];
                            C[1] = curPic.motionVal[dir][blockIndex[nAdj] - 2 * wrap - 2][1];
                            if (blkMvType[posC] && !blkMvType[xy]) {
                                C[0] = (1 + C[0] + curPic.motionVal[dir][blockIndex[1] - 2 * wrap - 2][0]) >> 1;
                                C[1] = (1 + C[1] + curPic.motionVal[dir][blockIndex[1] - 2 * wrap - 2][1]) >> 1;
                            }
                        } else
                            cValid = 0;
                    }
                }
            }
        }
    } else {
        posB   = blockIndex[1];
        bValid = 1;
        B[0]    = curPic.motionVal[dir][posB][0];
        B[1]    = curPic.motionVal[dir][posB][1];
        posC   = blockIndex[0];
        cValid = 1;
        C[0]    = curPic.motionVal[dir][posC][0];
        C[1]    = curPic.motionVal[dir][posC][1];
    }
    totalValid = aValid + bValid + cValid;
    if (!mbX && !(n == 1 || n == 3)) {
        A[0] = A[1] = 0;
    }
    if ((firstSliceLine && blkMvType[xy]) || (firstSliceLine && !(n & 2))) {
        B[0] = B[1] = C[0] = C[1] = 0;
    }
    if (!blkMvType[xy]) {
        if (mbWidth == 1) {
            px = B[0];
            py = B[1];
        } else {
            if (totalValid >= 2) {
                px = midPred(A[0], B[0], C[0]);
                py = midPred(A[1], B[1], C[1]);
            } else if (totalValid) {
                if      (aValid) { px = A[0]; py = A[1]; }
                else if (bValid) { px = B[0]; py = B[1]; }
                else              { px = C[0]; py = C[1]; }
            }
        }
    } else {
        if (aValid)
            fieldA = (A[1] & 4) ? 1 : 0;
        else
            fieldA = 0;
        if (bValid)
            fieldB = (B[1] & 4) ? 1 : 0;
        else
            fieldB = 0;
        if (cValid)
            fieldC = (C[1] & 4) ? 1 : 0;
        else
            fieldC = 0;
        numOppfield  = fieldA + fieldB + fieldC;
        numSamefield = totalValid - numOppfield;
        if (totalValid == 3) {
            if ((numSamefield == 3) || (numOppfield == 3)) {
                px = midPred(A[0], B[0], C[0]);
                py = midPred(A[1], B[1], C[1]);
            } else if (numSamefield >= numOppfield) {
                px = !fieldA ? A[0] : B[0];
                py = !fieldA ? A[1] : B[1];
            } else {
                px =  fieldA ? A[0] : B[0];
                py =  fieldA ? A[1] : B[1];
            }
        } else if (totalValid == 2) {
            if (numSamefield >= numOppfield) {
                if (!fieldA && aValid) {
                    px = A[0];
                    py = A[1];
                } else if (!fieldB && bValid) {
                    px = B[0];
                    py = B[1];
                } else  {
                    px = C[0];
                    py = C[1];
                }
            } else {
                if (fieldA && aValid) {
                    px = A[0];
                    py = A[1];
                } else  {
                    px = B[0];
                    py = B[1];
                }
            }
        } else if (totalValid == 1) {
            px = (aValid) ? A[0] : ((bValid) ? B[0] : C[0]);
            py = (aValid) ? A[1] : ((bValid) ? B[1] : C[1]);
        }
    }
    mv[dir][n][0] = curPic.motionVal[dir][xy][0] = ((px + dmvX + rX) & ((rX << 1) - 1)) - rX;
    mv[dir][n][1] = curPic.motionVal[dir][xy][1] = ((py + dmvY + rY) & ((rY << 1) - 1)) - rY;
    if (mvn == 1) {
        curPic.motionVal[dir][xy +    1    ][0] = curPic.motionVal[dir][xy][0];
        curPic.motionVal[dir][xy +    1    ][1] = curPic.motionVal[dir][xy][1];
        curPic.motionVal[dir][xy + wrap    ][0] = curPic.motionVal[dir][xy][0];
        curPic.motionVal[dir][xy + wrap    ][1] = curPic.motionVal[dir][xy][1];
        curPic.motionVal[dir][xy + wrap + 1][0] = curPic.motionVal[dir][xy][0];
        curPic.motionVal[dir][xy + wrap + 1][1] = curPic.motionVal[dir][xy][1];
    } else if (mvn == 2) {
        curPic.motionVal[dir][xy + 1][0] = curPic.motionVal[dir][xy][0];
        curPic.motionVal[dir][xy + 1][1] = curPic.motionVal[dir][xy][1];
        mv[dir][n + 1][0] = mv[dir][n][0];
        mv[dir][n + 1][1] = mv[dir][n][1];
    }
}
void Vc1::predBMv(int dmvX[2], int dmvY[2],
                      int direct, int mvtype)
{
    int xy, wrap;
    int px, py;
    int sum;
    int rX, rY;
    const uint8_t *isIntra = mbType;
    rX = rangeX;
    rY = rangeY;
    if (!quarterSample) {
        dmvX[0] *= 2;
        dmvY[0] *= 2;
        dmvX[1] *= 2;
        dmvY[1] *= 2;
    }
    wrap = b8Stride;
    xy = blockIndex[0];
    if (mbIntra) {
        curPic.motionVal[0][xy][0] =
        curPic.motionVal[0][xy][1] =
        curPic.motionVal[1][xy][0] =
        curPic.motionVal[1][xy][1] = 0;
        return;
    }
    if (direct && nextPic.ptr->fieldPicture)
        {}
    mv[0][0][0] = scaleMv(nextPic.motionVal[1][xy][0], bfraction, 0, quarterSample);
    mv[0][0][1] = scaleMv(nextPic.motionVal[1][xy][1], bfraction, 0, quarterSample);
    mv[1][0][0] = scaleMv(nextPic.motionVal[1][xy][0], bfraction, 1, quarterSample);
    mv[1][0][1] = scaleMv(nextPic.motionVal[1][xy][1], bfraction, 1, quarterSample);
    mv[0][0][0] = clip(mv[0][0][0], -60 - (mbX << 6), (mbWidth  << 6) - 4 - (mbX << 6));
    mv[0][0][1] = clip(mv[0][0][1], -60 - (mbY << 6), (mbHeight << 6) - 4 - (mbY << 6));
    mv[1][0][0] = clip(mv[1][0][0], -60 - (mbX << 6), (mbWidth  << 6) - 4 - (mbX << 6));
    mv[1][0][1] = clip(mv[1][0][1], -60 - (mbY << 6), (mbHeight << 6) - 4 - (mbY << 6));
    if (direct) {
        curPic.motionVal[0][xy][0] = mv[0][0][0];
        curPic.motionVal[0][xy][1] = mv[0][0][1];
        curPic.motionVal[1][xy][0] = mv[1][0][0];
        curPic.motionVal[1][xy][1] = mv[1][0][1];
        return;
    }
    if ((mvtype == kBmvTypeForward) || (mvtype == kBmvTypeInterpolated)) {
        int16_t       *C = curPic.motionVal[0][xy - 2];
        const int16_t *A = curPic.motionVal[0][xy - wrap * 2];
        int off          = (mbX == (mbWidth - 1)) ? -2 : 2;
        const int16_t *B = curPic.motionVal[0][xy - wrap * 2 + off];
        if (!mbX) C[0] = C[1] = 0;
        if (!firstSliceLine) {
            if (mbWidth == 1) {
                px = A[0];
                py = A[1];
            } else {
                px = midPred(A[0], B[0], C[0]);
                py = midPred(A[1], B[1], C[1]);
            }
        } else if (mbX) {
            px = C[0];
            py = C[1];
        } else {
            px = py = 0;
        }
        {
            int qx, qy, X, Y;
            int sh = profile < kProfileAdvanced ? 5 : 6;
            int MV = 4 - (1 << sh);
            qx = (mbX << sh);
            qy = (mbY << sh);
            X  = (mbWidth  << sh) - 4;
            Y  = (mbHeight << sh) - 4;
            if (qx + px < MV) px = MV - qx;
            if (qy + py < MV) py = MV - qy;
            if (qx + px > X) px = X - qx;
            if (qy + py > Y) py = Y - qy;
        }
        if (0 && !firstSliceLine && mbX) {
            if (isIntra[xy - wrap])
                sum = std::abs(px) + std::abs(py);
            else
                sum = std::abs(px - A[0]) + std::abs(py - A[1]);
            if (sum > 32) {
                if (gb.bit()) {
                    px = A[0];
                    py = A[1];
                } else {
                    px = C[0];
                    py = C[1];
                }
            } else {
                if (isIntra[xy - 2])
                    sum = std::abs(px) + std::abs(py);
                else
                    sum = std::abs(px - C[0]) + std::abs(py - C[1]);
                if (sum > 32) {
                    if (gb.bit()) {
                        px = A[0];
                        py = A[1];
                    } else {
                        px = C[0];
                        py = C[1];
                    }
                }
            }
        }
        mv[0][0][0] = ((px + dmvX[0] + rX) & ((rX << 1) - 1)) - rX;
        mv[0][0][1] = ((py + dmvY[0] + rY) & ((rY << 1) - 1)) - rY;
    }
    if ((mvtype == kBmvTypeBackward) || (mvtype == kBmvTypeInterpolated)) {
        int16_t       *C = curPic.motionVal[1][xy - 2];
        const int16_t *A = curPic.motionVal[1][xy - wrap * 2];
        int off          = (mbX == (mbWidth - 1)) ? -2 : 2;
        const int16_t *B = curPic.motionVal[1][xy - wrap * 2 + off];
        if (!mbX)
            C[0] = C[1] = 0;
        if (!firstSliceLine) {
            if (mbWidth == 1) {
                px = A[0];
                py = A[1];
            } else {
                px = midPred(A[0], B[0], C[0]);
                py = midPred(A[1], B[1], C[1]);
            }
        } else if (mbX) {
            px = C[0];
            py = C[1];
        } else {
            px = py = 0;
        }
        {
            int qx, qy, X, Y;
            int sh = profile < kProfileAdvanced ? 5 : 6;
            int MV = 4 - (1 << sh);
            qx = (mbX << sh);
            qy = (mbY << sh);
            X  = (mbWidth  << sh) - 4;
            Y  = (mbHeight << sh) - 4;
            if (qx + px < MV) px = MV - qx;
            if (qy + py < MV) py = MV - qy;
            if (qx + px > X) px = X - qx;
            if (qy + py > Y) py = Y - qy;
        }
        if (0 && !firstSliceLine && mbX) {
            if (isIntra[xy - wrap])
                sum = std::abs(px) + std::abs(py);
            else
                sum = std::abs(px - A[0]) + std::abs(py - A[1]);
            if (sum > 32) {
                if (gb.bit()) {
                    px = A[0];
                    py = A[1];
                } else {
                    px = C[0];
                    py = C[1];
                }
            } else {
                if (isIntra[xy - 2])
                    sum = std::abs(px) + std::abs(py);
                else
                    sum = std::abs(px - C[0]) + std::abs(py - C[1]);
                if (sum > 32) {
                    if (gb.bit()) {
                        px = A[0];
                        py = A[1];
                    } else {
                        px = C[0];
                        py = C[1];
                    }
                }
            }
        }
        mv[1][0][0] = ((px + dmvX[1] + rX) & ((rX << 1) - 1)) - rX;
        mv[1][0][1] = ((py + dmvY[1] + rY) & ((rY << 1) - 1)) - rY;
    }
    curPic.motionVal[0][xy][0] = mv[0][0][0];
    curPic.motionVal[0][xy][1] = mv[0][0][1];
    curPic.motionVal[1][xy][0] = mv[1][0][0];
    curPic.motionVal[1][xy][1] = mv[1][0][1];
}
void Vc1::predBMvIntfi(int n, int *dmvX, int *dmvY,
                            int mv1, int *predFlag)
{
    int dir = (bmvtype == kBmvTypeBackward) ? 1 : 0;
    int mbPos = mbX + mbY * mbStride;
    if (bmvtype == kBmvTypeDirect) {
        int totalOpp, k, f;
        if (nextPic.mbType[mbPos + mbOff] != kMbTypeIntra) {
            mv[0][0][0] = scaleMv(nextPic.motionVal[1][blockIndex[0] + blocksOff][0],
                                      bfraction, 0, quarterSample);
            mv[0][0][1] = scaleMv(nextPic.motionVal[1][blockIndex[0] + blocksOff][1],
                                      bfraction, 0, quarterSample);
            mv[1][0][0] = scaleMv(nextPic.motionVal[1][blockIndex[0] + blocksOff][0],
                                      bfraction, 1, quarterSample);
            mv[1][0][1] = scaleMv(nextPic.motionVal[1][blockIndex[0] + blocksOff][1],
                                      bfraction, 1, quarterSample);
            totalOpp = mvFNext[0][blockIndex[0] + blocksOff]
                      + mvFNext[0][blockIndex[1] + blocksOff]
                      + mvFNext[0][blockIndex[2] + blocksOff]
                      + mvFNext[0][blockIndex[3] + blocksOff];
            f = (totalOpp > 2) ? 1 : 0;
        } else {
            mv[0][0][0] = mv[0][0][1] = 0;
            mv[1][0][0] = mv[1][0][1] = 0;
            f = 0;
        }
        refFieldType[0] = refFieldType[1] = curFieldType ^ f;
        for (k = 0; k < 4; k++) {
            curPic.motionVal[0][blockIndex[k] + blocksOff][0] = mv[0][0][0];
            curPic.motionVal[0][blockIndex[k] + blocksOff][1] = mv[0][0][1];
            curPic.motionVal[1][blockIndex[k] + blocksOff][0] = mv[1][0][0];
            curPic.motionVal[1][blockIndex[k] + blocksOff][1] = mv[1][0][1];
            mvF[0][blockIndex[k] + blocksOff] = f;
            mvF[1][blockIndex[k] + blocksOff] = f;
        }
        return;
    }
    if (bmvtype == kBmvTypeInterpolated) {
        predMv(0, dmvX[0], dmvY[0],   1, rangeX, rangeY, mbType, predFlag[0], 0);
        predMv(0, dmvX[1], dmvY[1],   1, rangeX, rangeY, mbType, predFlag[1], 1);
        return;
    }
    if (dir) {
        predMv(n, dmvX[1], dmvY[1], mv1, rangeX, rangeY, mbType, predFlag[1], 1);
        if (n == 3 || mv1) {
            predMv(0, dmvX[0], dmvY[0],   1, rangeX, rangeY, mbType, 0, 0);
        }
    } else {
        predMv(n, dmvX[0], dmvY[0], mv1, rangeX, rangeY, mbType, predFlag[0], 0);
        if (n == 3 || mv1) {
            predMv(0, dmvX[1], dmvY[1],   1, rangeX, rangeY, mbType, 0, 1);
        }
    }
}

}

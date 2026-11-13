#include "vc1.h"
#include <cstring>

namespace absent::vc1 {


static void scaleLuma(uint8_t *srcY,
                                            int k, int linesize)
{
    int i, j;
    for (j = 0; j < k; j++) {
        for (i = 0; i < k; i++)
            srcY[i] = ((srcY[i] - 128) >> 1) + 128;
        srcY += linesize;
    }
}
static void scaleChroma(uint8_t *srcU, uint8_t *srcV,
                                              int k, int uvlinesize)
{
    int i, j;
    for (j = 0; j < k; j++) {
        for (i = 0; i < k; i++) {
            srcU[i] = ((srcU[i] - 128) >> 1) + 128;
            srcV[i] = ((srcV[i] - 128) >> 1) + 128;
        }
        srcU += uvlinesize;
        srcV += uvlinesize;
    }
}
static void lutScaleLuma(uint8_t *srcY,
                                                const uint8_t *lut1, const uint8_t *lut2,
                                                int k, int linesize)
{
    int i, j;
    for (j = 0; j < k; j += 2) {
        for (i = 0; i < k; i++)
            srcY[i] = lut1[srcY[i]];
        srcY += linesize;
        if (j + 1 == k)
            break;
        for (i = 0; i < k; i++)
            srcY[i] = lut2[srcY[i]];
        srcY += linesize;
    }
}
static void lutScaleChroma(uint8_t *srcU, uint8_t *srcV,
                                                  const uint8_t *lut1, const uint8_t *lut2,
                                                  int k, int uvlinesize)
{
    int i, j;
    for (j = 0; j < k; j += 2) {
        for (i = 0; i < k; i++) {
            srcU[i] = lut1[srcU[i]];
            srcV[i] = lut1[srcV[i]];
        }
        srcU += uvlinesize;
        srcV += uvlinesize;
        if (j + 1 == k)
            break;
        for (i = 0; i < k; i++) {
            srcU[i] = lut2[srcU[i]];
            srcV[i] = lut2[srcV[i]];
        }
        srcU += uvlinesize;
        srcV += uvlinesize;
    }
}
static const uint8_t popcount4[16] = { 0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4 };
int Vc1::getLumaMv(int dir, int16_t *tx, int16_t *ty)
{
    int idx = mvF[dir][blockIndex[0] + blocksOff] |
             (mvF[dir][blockIndex[1] + blocksOff] << 1) |
             (mvF[dir][blockIndex[2] + blocksOff] << 2) |
             (mvF[dir][blockIndex[3] + blocksOff] << 3);
    static const uint8_t index2[16] = { 0, 0, 0, 0x23, 0, 0x13, 0x03, 0, 0, 0x12, 0x02, 0, 0x01, 0, 0, 0 };
    int oppCount = popcount4[idx];
    switch (oppCount) {
    case 0:
    case 4:
        *tx = median4(mv[dir][0][0], mv[dir][1][0], mv[dir][2][0], mv[dir][3][0]);
        *ty = median4(mv[dir][0][1], mv[dir][1][1], mv[dir][2][1], mv[dir][3][1]);
        break;
    case 1:
        *tx = midPred(mv[dir][idx < 2][0], mv[dir][1 + (idx < 4)][0], mv[dir][2 + (idx < 8)][0]);
        *ty = midPred(mv[dir][idx < 2][1], mv[dir][1 + (idx < 4)][1], mv[dir][2 + (idx < 8)][1]);
        break;
    case 3:
        *tx = midPred(mv[dir][idx > 0xd][0], mv[dir][1 + (idx > 0xb)][0], mv[dir][2 + (idx > 0x7)][0]);
        *ty = midPred(mv[dir][idx > 0xd][1], mv[dir][1 + (idx > 0xb)][1], mv[dir][2 + (idx > 0x7)][1]);
        break;
    case 2:
        *tx = (mv[dir][index2[idx] >> 4][0] + mv[dir][index2[idx] & 0xf][0]) / 2;
        *ty = (mv[dir][index2[idx] >> 4][1] + mv[dir][index2[idx] & 0xf][1]) / 2;
        break;
    }
    return oppCount;
}
int Vc1::getChromaMv(int dir, int16_t *tx, int16_t *ty)
{
    int idx = !mbType[blockIndex[0]] |
             (!mbType[blockIndex[1]] << 1) |
             (!mbType[blockIndex[2]] << 2) |
             (!mbType[blockIndex[3]] << 3);
    static const uint8_t index2[16] = { 0, 0, 0, 0x01, 0, 0x02, 0x12, 0, 0, 0x03, 0x13, 0, 0x23, 0, 0, 0 };
    int validCount = popcount4[idx];
    switch (validCount) {
    case 4:
        *tx = median4(mv[dir][0][0], mv[dir][1][0], mv[dir][2][0], mv[dir][3][0]);
        *ty = median4(mv[dir][0][1], mv[dir][1][1], mv[dir][2][1], mv[dir][3][1]);
        break;
    case 3:
        *tx = midPred(mv[dir][idx > 0xd][0], mv[dir][1 + (idx > 0xb)][0], mv[dir][2 + (idx > 0x7)][0]);
        *ty = midPred(mv[dir][idx > 0xd][1], mv[dir][1 + (idx > 0xb)][1], mv[dir][2 + (idx > 0x7)][1]);
        break;
    case 2:
        *tx = (mv[dir][index2[idx] >> 4][0] + mv[dir][index2[idx] & 0xf][0]) / 2;
        *ty = (mv[dir][index2[idx] >> 4][1] + mv[dir][index2[idx] & 0xf][1]) / 2;
        break;
    default:
        return 0;
    }
    return validCount;
}
void Vc1::mc1mv(int dir)
{
    const Dsp* h264chroma = &vc1dsp;
    uint8_t *srcY, *srcU, *srcV;
    int dxy, mx, my, uvmx, uvmy, srcPosX, srcPosY, uvSrcPosX, uvSrcPosY;
    int vEdgePos = this->vEdgePos >> fieldMode;
    int i;
    const uint8_t (*luty)[256], (*lutuv)[256];
    int useIc;
    int interlace;
    int linesize, uvlinesize;
    if ((!fieldMode ||
         (refFieldType[dir] == 1 && curFieldType == 1)) &&
        !lastPic.data[0])
        return;
    linesize   = curPic.ptr->linesize[0];
    uvlinesize = curPic.ptr->linesize[1];
    mx = mv[dir][0][0];
    my = mv[dir][0][1];
    if (pictType == kPictureP) {
        for (i = 0; i < 4; i++) {
            curPic.motionVal[1][blockIndex[i] + blocksOff][0] = mx;
            curPic.motionVal[1][blockIndex[i] + blocksOff][1] = my;
        }
    }
    uvmx = (mx + ((mx & 3) == 3)) >> 1;
    uvmy = (my + ((my & 3) == 3)) >> 1;
    lumaMv[mbX][0] = uvmx;
    lumaMv[mbX][1] = uvmy;
    if (fieldMode &&
        curFieldType != refFieldType[dir]) {
        my   = my   - 2 + 4 * curFieldType;
        uvmy = uvmy - 2 + 4 * curFieldType;
    }
    if (fastuvmc && (fcm != kIlaceFrame)) {
        uvmx = uvmx + ((uvmx < 0) ? (uvmx & 1) : -(uvmx & 1));
        uvmy = uvmy + ((uvmy < 0) ? (uvmy & 1) : -(uvmy & 1));
    }
    if (!dir) {
        if (fieldMode && (curFieldType != refFieldType[dir]) && secondField) {
            srcY = curPic.data[0];
            srcU = curPic.data[1];
            srcV = curPic.data[2];
            luty  = currLuty;
            lutuv = currLutuv;
            useIc = *currUseIc;
            interlace = 1;
        } else {
            srcY = lastPic.data[0];
            srcU = lastPic.data[1];
            srcV = lastPic.data[2];
            luty  = lastLuty;
            lutuv = lastLutuv;
            useIc = lastUseIc;
            interlace = lastInterlaced;
        }
    } else {
        srcY = nextPic.data[0];
        srcU = nextPic.data[1];
        srcV = nextPic.data[2];
        luty  = nextLuty;
        lutuv = nextLutuv;
        useIc = nextUseIc;
        interlace = nextInterlaced;
    }
    if (!srcY || !srcU) {
        return;
    }
    srcPosX   = mbX * 16 + (mx   >> 2);
    srcPosY   = mbY * 16 + (my   >> 2);
    uvSrcPosX = mbX *  8 + (uvmx >> 2);
    uvSrcPosY = mbY *  8 + (uvmy >> 2);
    if (profile != kProfileAdvanced) {
        srcPosX   = clip(  srcPosX, -16, mbWidth  * 16);
        srcPosY   = clip(  srcPosY, -16, mbHeight * 16);
        uvSrcPosX = clip(uvSrcPosX,  -8, mbWidth  *  8);
        uvSrcPosY = clip(uvSrcPosY,  -8, mbHeight *  8);
    } else {
        srcPosX   = clip(  srcPosX, -17, codedWidth);
        uvSrcPosX = clip(uvSrcPosX,  -8, codedWidth  >> 1);
        if (fcm == kIlaceFrame) {
            srcPosY = clip(srcPosY, -18 + (srcPosY & 1), codedHeight + (srcPosY & 1));
            uvSrcPosY = clip(uvSrcPosY, -8 + (uvSrcPosY & 1), (codedHeight >> 1) + (uvSrcPosY & 1));
        } else {
            srcPosY = clip(srcPosY, -18, codedHeight + 1);
            uvSrcPosY = clip(uvSrcPosY, -8, codedHeight >> 1);
        }
    }
    srcY += srcPosY   * this->linesize   + srcPosX;
    srcU += uvSrcPosY * this->uvlinesize + uvSrcPosX;
    srcV += uvSrcPosY * this->uvlinesize + uvSrcPosX;
    if (fieldMode && refFieldType[dir]) {
        srcY += linesize;
        srcU += uvlinesize;
        srcV += uvlinesize;
    }
    if (false) {
        srcU = edgeEmuBuffer.data() + 18 * this->linesize;
        srcV = edgeEmuBuffer.data() + 18 * this->linesize;
    }
    if (rangeredfrm || useIc
        || hEdgePos < 22 || vEdgePos < 22
        || (unsigned)(srcPosX - mspel) > unsigned(hEdgePos - (mx&3) - 16 - mspel * 3)
        || (unsigned)(srcPosY - 1)        > unsigned(vEdgePos    - (my&3) - 16 - 3)) {
        uint8_t *ubuf = edgeEmuBuffer.data() + 19 * this->linesize;
        uint8_t *vbuf = ubuf + 9 * this->uvlinesize;
        const int k = 17 + mspel * 2;
        srcY -= mspel * (1 + this->linesize);
        if (interlace) {
            emulatedEdgeMc(edgeEmuBuffer.data(),
                                     srcY,
                                     linesize << 1,
                                     linesize << 1,
                                     k,
                                     fieldMode ? k : (k + 1) >> 1,
                                     srcPosX - mspel,
                                     (srcPosY - mspel) >> !fieldMode,
                                     hEdgePos,
                                     this->vEdgePos >> 1);
            if (!fieldMode)
                emulatedEdgeMc(edgeEmuBuffer.data() + linesize,
                                         srcY + linesize,
                                         linesize << 1,
                                         linesize << 1,
                                         k,
                                         k >> 1,
                                         srcPosX - mspel,
                                         (srcPosY - mspel + 1) >> 1,
                                         hEdgePos,
                                         this->vEdgePos >> 1);
        } else
            emulatedEdgeMc(edgeEmuBuffer.data(),
                                     srcY,
                                     linesize,
                                     linesize,
                                     k,
                                     fieldMode ? (k << 1) - 1 : k,
                                     srcPosX - mspel,
                                     fieldMode ? 2 * (srcPosY - mspel) + refFieldType[dir] :
                                                     srcPosY - mspel,
                                     hEdgePos,
                                     this->vEdgePos);
        srcY = edgeEmuBuffer.data();
        if (interlace) {
            emulatedEdgeMc(ubuf,
                                     srcU,
                                     uvlinesize << 1,
                                     uvlinesize << 1,
                                     9,
                                     fieldMode ? 9 : 5,
                                     uvSrcPosX,
                                     uvSrcPosY >> !fieldMode,
                                     hEdgePos >> 1,
                                     this->vEdgePos >> 2);
            emulatedEdgeMc(vbuf,
                                     srcV,
                                     uvlinesize << 1,
                                     uvlinesize << 1,
                                     9,
                                     fieldMode ? 9 : 5,
                                     uvSrcPosX,
                                     uvSrcPosY >> !fieldMode,
                                     hEdgePos >> 1,
                                     this->vEdgePos >> 2);
            if (!fieldMode) {
                emulatedEdgeMc(ubuf + uvlinesize,
                                         srcU + uvlinesize,
                                         uvlinesize << 1,
                                         uvlinesize << 1,
                                         9,
                                         4,
                                         uvSrcPosX,
                                         (uvSrcPosY + 1) >> 1,
                                         hEdgePos >> 1,
                                         this->vEdgePos >> 2);
                emulatedEdgeMc(vbuf + uvlinesize,
                                         srcV + uvlinesize,
                                         uvlinesize << 1,
                                         uvlinesize << 1,
                                         9,
                                         4,
                                         uvSrcPosX,
                                         (uvSrcPosY + 1) >> 1,
                                         hEdgePos >> 1,
                                         this->vEdgePos >> 2);
            }
        } else {
            emulatedEdgeMc(ubuf,
                                     srcU,
                                     uvlinesize,
                                     uvlinesize,
                                     9,
                                     fieldMode ? 17 : 9,
                                     uvSrcPosX,
                                     fieldMode ? 2 * uvSrcPosY + refFieldType[dir] : uvSrcPosY,
                                     hEdgePos >> 1,
                                     this->vEdgePos >> 1);
            emulatedEdgeMc(vbuf,
                                     srcV,
                                     uvlinesize,
                                     uvlinesize,
                                     9,
                                     fieldMode ? 17 : 9,
                                     uvSrcPosX,
                                     fieldMode ? 2 * uvSrcPosY + refFieldType[dir] : uvSrcPosY,
                                     hEdgePos >> 1,
                                     this->vEdgePos >> 1);
        }
        srcU = ubuf;
        srcV = vbuf;
        if (rangeredfrm) {
            scaleLuma(srcY, k, this->linesize);
            scaleChroma(srcU, srcV, 9, this->uvlinesize);
        }
        if (useIc) {
            lutScaleLuma(srcY,
                               luty[fieldMode ? refFieldType[dir] : ((0 + srcPosY - mspel) & 1)],
                               luty[fieldMode ? refFieldType[dir] : ((1 + srcPosY - mspel) & 1)],
                               k, this->linesize);
            lutScaleChroma(srcU, srcV,
                                 lutuv[fieldMode ? refFieldType[dir] : ((0 + uvSrcPosY) & 1)],
                                 lutuv[fieldMode ? refFieldType[dir] : ((1 + uvSrcPosY) & 1)],
                                 9, this->uvlinesize);
        }
        srcY += mspel * (1 + this->linesize);
    }
    if (mspel) {
        dxy = ((my & 3) << 2) | (mx & 3);
        vc1dsp.putVc1MspelPixelsTab[0][dxy](dest[0], srcY, this->linesize, rnd);
    } else {
        dxy = (my & 2) | ((mx & 2) >> 1);
        if (!rnd)
            vc1dsp.putPixelsTab[0][dxy](dest[0], srcY, this->linesize, 16);
        else
            vc1dsp.putNoRndPixelsTab[0][dxy](dest[0], srcY, this->linesize, 16);
    }
    if (false)
        return;
    uvmx = (uvmx & 3) << 1;
    uvmy = (uvmy & 3) << 1;
    if (!rnd) {
        h264chroma->putH264ChromaPixelsTab[0](dest[1], srcU, this->uvlinesize, 8, uvmx, uvmy);
        h264chroma->putH264ChromaPixelsTab[0](dest[2], srcV, this->uvlinesize, 8, uvmx, uvmy);
    } else {
        vc1dsp.putNoRndVc1ChromaPixelsTab[0](dest[1], srcU, this->uvlinesize, 8, uvmx, uvmy);
        vc1dsp.putNoRndVc1ChromaPixelsTab[0](dest[2], srcV, this->uvlinesize, 8, uvmx, uvmy);
    }
    if (fieldMode) {
        mvF[dir][blockIndex[4] + mbOff] = curFieldType != refFieldType[dir];
        mvF[dir][blockIndex[5] + mbOff] = curFieldType != refFieldType[dir];
    }
}
void Vc1::mc4mvLuma(int n, int dir, int avg)
{
    uint8_t *srcY;
    int dxy, mx, my, srcPosX, srcPosY;
    int off;
    int fieldmv = (fcm == kIlaceFrame) ? blkMvType[blockIndex[n]] : 0;
    int vEdgePos = this->vEdgePos >> fieldMode;
    const uint8_t (*luty)[256];
    int useIc;
    int interlace;
    int linesize;
    if ((!fieldMode ||
         (refFieldType[dir] == 1 && curFieldType == 1)) &&
        !lastPic.data[0])
        return;
    linesize = curPic.ptr->linesize[0];
    mx = mv[dir][n][0];
    my = mv[dir][n][1];
    if (!dir) {
        if (fieldMode && (curFieldType != refFieldType[dir]) && secondField) {
            srcY = curPic.data[0];
            luty = currLuty;
            useIc = *currUseIc;
            interlace = 1;
        } else {
            srcY = lastPic.data[0];
            luty = lastLuty;
            useIc = lastUseIc;
            interlace = lastInterlaced;
        }
    } else {
        srcY = nextPic.data[0];
        luty = nextLuty;
        useIc = nextUseIc;
        interlace = nextInterlaced;
    }
    if (!srcY) {
        return;
    }
    if (fieldMode) {
        if (curFieldType != refFieldType[dir])
            my = my - 2 + 4 * curFieldType;
    }
    if (pictType == kPictureP && n == 3 && fieldMode) {
        int oppCount = getLumaMv(0,
                                    &curPic.motionVal[1][blockIndex[0] + blocksOff][0],
                                    &curPic.motionVal[1][blockIndex[0] + blocksOff][1]);
        int k, f = oppCount > 2;
        for (k = 0; k < 4; k++)
            mvF[1][blockIndex[k] + blocksOff] = f;
    }
    if (fcm == kIlaceFrame) {
        int qx, qy;
        int width  = codedWidth;
        int height = codedHeight >> 1;
        if (pictType == kPictureP) {
            curPic.motionVal[1][blockIndex[n] + blocksOff][0] = mx;
            curPic.motionVal[1][blockIndex[n] + blocksOff][1] = my;
        }
        qx = (mbX * 16) + (mx >> 2);
        qy = (mbY *  8) + (my >> 3);
        if (qx < -17)
            mx -= 4 * (qx + 17);
        else if (qx > width)
            mx -= 4 * (qx - width);
        if (qy < -18)
            my -= 8 * (qy + 18);
        else if (qy > height + 1)
            my -= 8 * (qy - height - 1);
    }
    if ((fcm == kIlaceFrame) && fieldmv)
        off = ((n > 1) ? this->linesize : 0) + (n & 1) * 8;
    else
        off = this->linesize * 4 * (n & 2) + (n & 1) * 8;
    srcPosX = mbX * 16 + (n & 1) * 8 + (mx >> 2);
    if (!fieldmv)
        srcPosY = mbY * 16 + (n & 2) * 4 + (my >> 2);
    else
        srcPosY = mbY * 16 + ((n > 1) ? 1 : 0) + (my >> 2);
    if (profile != kProfileAdvanced) {
        srcPosX = clip(srcPosX, -16, mbWidth  * 16);
        srcPosY = clip(srcPosY, -16, mbHeight * 16);
    } else {
        srcPosX = clip(srcPosX, -17, codedWidth);
        if (fcm == kIlaceFrame)
            srcPosY = clip(srcPosY, -18 + (srcPosY & 1), codedHeight + (srcPosY & 1));
        else
            srcPosY = clip(srcPosY, -18, codedHeight + 1);
    }
    srcY += srcPosY * this->linesize + srcPosX;
    if (fieldMode && refFieldType[dir])
        srcY += linesize;
    if (rangeredfrm || useIc
        || hEdgePos < 13 || vEdgePos < 23
        || (unsigned)(srcPosX - mspel) > unsigned(hEdgePos - (mx & 3) - 8 - mspel * 2)
        || (unsigned)(srcPosY - (mspel << fieldmv)) > unsigned(vEdgePos - (my & 3) - ((8 + mspel * 2) << fieldmv))) {
        const int k = 9 + mspel * 2;
        srcY -= mspel * (1 + (this->linesize << fieldmv));
        if (interlace) {
            emulatedEdgeMc(edgeEmuBuffer.data(),
                                     srcY,
                                     linesize << 1,
                                     linesize << 1,
                                     k,
                                     fieldMode ? k : ((k << fieldmv) + 1) >> 1,
                                     srcPosX - mspel,
                                     (srcPosY - (mspel << fieldmv)) >> !fieldMode,
                                     hEdgePos,
                                     this->vEdgePos >> 1);
            if (!fieldMode && !fieldmv)
                emulatedEdgeMc(edgeEmuBuffer.data() + linesize,
                                         srcY + linesize,
                                         linesize << 1,
                                         linesize << 1,
                                         k,
                                         k >> 1,
                                         srcPosX - mspel,
                                         (srcPosY - mspel + 1) >> 1,
                                         hEdgePos,
                                         this->vEdgePos >> 1);
        } else
            emulatedEdgeMc(edgeEmuBuffer.data(),
                                     srcY,
                                     linesize,
                                     linesize,
                                     k,
                                     fieldMode ? (k << 1) - 1 : k << fieldmv,
                                     srcPosX - mspel,
                                     fieldMode ? 2 * (srcPosY - mspel) + refFieldType[dir] :
                                                     srcPosY - (mspel << fieldmv),
                                     hEdgePos,
                                     this->vEdgePos);
        srcY = edgeEmuBuffer.data();
        if (rangeredfrm) {
            scaleLuma(srcY, k, this->linesize << fieldmv);
        }
        if (useIc) {
            lutScaleLuma(srcY,
                               luty[fieldMode ? refFieldType[dir] : (((0<<fieldmv)+srcPosY - (mspel << fieldmv)) & 1)],
                               luty[fieldMode ? refFieldType[dir] : (((1<<fieldmv)+srcPosY - (mspel << fieldmv)) & 1)],
                               k, this->linesize << fieldmv);
        }
        srcY += mspel * (1 + (this->linesize << fieldmv));
    }
    if (mspel) {
        dxy = ((my & 3) << 2) | (mx & 3);
        if (avg)
            vc1dsp.avgVc1MspelPixelsTab[1][dxy](dest[0] + off, srcY, this->linesize << fieldmv, rnd);
        else
            vc1dsp.putVc1MspelPixelsTab[1][dxy](dest[0] + off, srcY, this->linesize << fieldmv, rnd);
    } else {
        dxy = (my & 2) | ((mx & 2) >> 1);
        if (!rnd)
            vc1dsp.putPixelsTab[1][dxy](dest[0] + off, srcY, this->linesize, 8);
        else
            vc1dsp.putNoRndPixelsTab[1][dxy](dest[0] + off, srcY, this->linesize, 8);
    }
}
void Vc1::mc4mvChroma(int dir)
{
    const Dsp* h264chroma = &vc1dsp;
    uint8_t *srcU, *srcV;
    int uvmx, uvmy, uvSrcPosX, uvSrcPosY;
    int16_t tx, ty;
    int chromaRefType;
    int vEdgePos = this->vEdgePos >> fieldMode;
    const uint8_t (*lutuv)[256];
    int useIc;
    int interlace;
    int uvlinesize;
    if (!fieldMode && !lastPic.data[0])
        return;
    if (false)
        return;
    if (!fieldMode || !numref) {
        int validCount = getChromaMv(dir, &tx, &ty);
        if (!validCount) {
            curPic.motionVal[1][blockIndex[0] + blocksOff][0] = 0;
            curPic.motionVal[1][blockIndex[0] + blocksOff][1] = 0;
            lumaMv[mbX][0] = lumaMv[mbX][1] = 0;
            return;
        }
        chromaRefType = refFieldType[dir];
    } else {
        int oppCount = getLumaMv(dir, &tx, &ty);
        chromaRefType = curFieldType ^ (oppCount > 2);
    }
    if (fieldMode && chromaRefType == 1 && curFieldType == 1 && !lastPic.data[0])
        return;
    curPic.motionVal[1][blockIndex[0] + blocksOff][0] = tx;
    curPic.motionVal[1][blockIndex[0] + blocksOff][1] = ty;
    uvlinesize = curPic.ptr->linesize[1];
    uvmx = (tx + ((tx & 3) == 3)) >> 1;
    uvmy = (ty + ((ty & 3) == 3)) >> 1;
    lumaMv[mbX][0] = uvmx;
    lumaMv[mbX][1] = uvmy;
    if (fastuvmc) {
        uvmx = uvmx + ((uvmx < 0) ? (uvmx & 1) : -(uvmx & 1));
        uvmy = uvmy + ((uvmy < 0) ? (uvmy & 1) : -(uvmy & 1));
    }
    if (curFieldType != chromaRefType)
        uvmy += 2 - 4 * chromaRefType;
    uvSrcPosX = mbX * 8 + (uvmx >> 2);
    uvSrcPosY = mbY * 8 + (uvmy >> 2);
    if (profile != kProfileAdvanced) {
        uvSrcPosX = clip(uvSrcPosX, -8, mbWidth  * 8);
        uvSrcPosY = clip(uvSrcPosY, -8, mbHeight * 8);
    } else {
        uvSrcPosX = clip(uvSrcPosX, -8, codedWidth  >> 1);
        uvSrcPosY = clip(uvSrcPosY, -8, codedHeight >> 1);
    }
    if (!dir) {
        if (fieldMode && (curFieldType != chromaRefType) && secondField) {
            srcU = curPic.data[1];
            srcV = curPic.data[2];
            lutuv = currLutuv;
            useIc = *currUseIc;
            interlace = 1;
        } else {
            srcU = lastPic.data[1];
            srcV = lastPic.data[2];
            lutuv = lastLutuv;
            useIc = lastUseIc;
            interlace = lastInterlaced;
        }
    } else {
        srcU = nextPic.data[1];
        srcV = nextPic.data[2];
        lutuv = nextLutuv;
        useIc = nextUseIc;
        interlace = nextInterlaced;
    }
    if (!srcU) {
        return;
    }
    srcU += uvSrcPosY * this->uvlinesize + uvSrcPosX;
    srcV += uvSrcPosY * this->uvlinesize + uvSrcPosX;
    if (fieldMode) {
        if (chromaRefType) {
            srcU += uvlinesize;
            srcV += uvlinesize;
        }
    }
    if (rangeredfrm || useIc
        || hEdgePos < 18 || vEdgePos < 18
        || (unsigned)uvSrcPosX > unsigned((hEdgePos >> 1) - 9)
        || (unsigned)uvSrcPosY > unsigned((vEdgePos    >> 1) - 9)) {
        if (interlace) {
            emulatedEdgeMc(edgeEmuBuffer.data(),
                                     srcU,
                                     uvlinesize << 1,
                                     uvlinesize << 1,
                                     9,
                                     fieldMode ? 9 : 5,
                                     uvSrcPosX,
                                     uvSrcPosY >> !fieldMode,
                                     hEdgePos >> 1,
                                     this->vEdgePos >> 2);
            emulatedEdgeMc(edgeEmuBuffer.data() + 16,
                                     srcV,
                                     uvlinesize << 1,
                                     uvlinesize << 1,
                                     9,
                                     fieldMode ? 9 : 5,
                                     uvSrcPosX,
                                     uvSrcPosY >> !fieldMode,
                                     hEdgePos >> 1,
                                     this->vEdgePos >> 2);
            if (!fieldMode) {
                emulatedEdgeMc(edgeEmuBuffer.data() + uvlinesize,
                                         srcU + uvlinesize,
                                         uvlinesize << 1,
                                         uvlinesize << 1,
                                         9,
                                         4,
                                         uvSrcPosX,
                                         (uvSrcPosY + 1) >> 1,
                                         hEdgePos >> 1,
                                         this->vEdgePos >> 2);
                emulatedEdgeMc(edgeEmuBuffer.data() + 16 + uvlinesize,
                                         srcV + uvlinesize,
                                         uvlinesize << 1,
                                         uvlinesize << 1,
                                         9,
                                         4,
                                         uvSrcPosX,
                                         (uvSrcPosY + 1) >> 1,
                                         hEdgePos >> 1,
                                         this->vEdgePos >> 2);
            }
        } else {
            emulatedEdgeMc(edgeEmuBuffer.data(),
                                     srcU,
                                     uvlinesize,
                                     uvlinesize,
                                     9,
                                     fieldMode ? 17 : 9,
                                     uvSrcPosX,
                                     fieldMode ? 2 * uvSrcPosY + chromaRefType : uvSrcPosY,
                                     hEdgePos >> 1,
                                     this->vEdgePos >> 1);
            emulatedEdgeMc(edgeEmuBuffer.data() + 16,
                                     srcV,
                                     uvlinesize,
                                     uvlinesize,
                                     9,
                                     fieldMode ? 17 : 9,
                                     uvSrcPosX,
                                     fieldMode ? 2 * uvSrcPosY + chromaRefType : uvSrcPosY,
                                     hEdgePos >> 1,
                                     this->vEdgePos >> 1);
        }
        srcU = edgeEmuBuffer.data();
        srcV = edgeEmuBuffer.data() + 16;
        if (rangeredfrm) {
            scaleChroma(srcU, srcV, 9, this->uvlinesize);
        }
        if (useIc) {
            lutScaleChroma(srcU, srcV,
                                 lutuv[fieldMode ? chromaRefType : ((0 + uvSrcPosY) & 1)],
                                 lutuv[fieldMode ? chromaRefType : ((1 + uvSrcPosY) & 1)],
                                 9, this->uvlinesize);
        }
    }
    uvmx = (uvmx & 3) << 1;
    uvmy = (uvmy & 3) << 1;
    if (!rnd) {
        h264chroma->putH264ChromaPixelsTab[0](dest[1], srcU, this->uvlinesize, 8, uvmx, uvmy);
        h264chroma->putH264ChromaPixelsTab[0](dest[2], srcV, this->uvlinesize, 8, uvmx, uvmy);
    } else {
        vc1dsp.putNoRndVc1ChromaPixelsTab[0](dest[1], srcU, this->uvlinesize, 8, uvmx, uvmy);
        vc1dsp.putNoRndVc1ChromaPixelsTab[0](dest[2], srcV, this->uvlinesize, 8, uvmx, uvmy);
    }
    if (fieldMode) {
        mvF[dir][blockIndex[4] + mbOff] = curFieldType != chromaRefType;
        mvF[dir][blockIndex[5] + mbOff] = curFieldType != chromaRefType;
    }
}
void Vc1::mc4mvChroma4(int dir, int dir2, int avg)
{
    const Dsp* h264chroma = &vc1dsp;
    uint8_t *srcU, *srcV;
    int uvSrcPosX, uvSrcPosY;
    int uvmxField[4], uvmyField[4];
    int i, off, tx, ty;
    int fieldmv = blkMvType[blockIndex[0]];
    static const uint8_t sRndtblfield[16] = { 0, 0, 1, 2, 4, 4, 5, 6, 2, 2, 3, 8, 6, 6, 7, 12 };
    int vDist = fieldmv ? 1 : 4;
    int vEdgePos = this->vEdgePos >> 1;
    int useIc;
    int interlace;
    int uvlinesize;
    const uint8_t (*lutuv)[256];
    if (false)
        return;
    uvlinesize = curPic.ptr->linesize[1];
    for (i = 0; i < 4; i++) {
        int d = i < 2 ? dir: dir2;
        tx = mv[d][i][0];
        uvmxField[i] = (tx + ((tx & 3) == 3)) >> 1;
        ty = mv[d][i][1];
        if (fieldmv)
            uvmyField[i] = (ty >> 4) * 8 + sRndtblfield[ty & 0xF];
        else
            uvmyField[i] = (ty + ((ty & 3) == 3)) >> 1;
    }
    for (i = 0; i < 4; i++) {
        off = (i & 1) * 4 + ((i & 2) ? vDist * this->uvlinesize : 0);
        uvSrcPosX = mbX * 8 +  (i & 1) * 4           + (uvmxField[i] >> 2);
        uvSrcPosY = mbY * 8 + ((i & 2) ? vDist : 0) + (uvmyField[i] >> 2);
        uvSrcPosX = clip(uvSrcPosX, -8, codedWidth  >> 1);
        if (fcm == kIlaceFrame)
            uvSrcPosY = clip(uvSrcPosY, -8 + (uvSrcPosY & 1), (codedHeight >> 1) + (uvSrcPosY & 1));
        else
            uvSrcPosY = clip(uvSrcPosY, -8, codedHeight >> 1);
        if (i < 2 ? dir : dir2) {
            srcU = nextPic.data[1];
            srcV = nextPic.data[2];
            lutuv  = nextLutuv;
            useIc = nextUseIc;
            interlace = nextInterlaced;
        } else {
            srcU = lastPic.data[1];
            srcV = lastPic.data[2];
            lutuv  = lastLutuv;
            useIc = lastUseIc;
            interlace = lastInterlaced;
        }
        if (!srcU)
            return;
        srcU += uvSrcPosY * this->uvlinesize + uvSrcPosX;
        srcV += uvSrcPosY * this->uvlinesize + uvSrcPosX;
        uvmxField[i] = (uvmxField[i] & 3) << 1;
        uvmyField[i] = (uvmyField[i] & 3) << 1;
        if (useIc
            || hEdgePos < 10 || vEdgePos < (5 << fieldmv)
            || (unsigned)uvSrcPosX > unsigned((hEdgePos >> 1) - 5)
            || (unsigned)uvSrcPosY > unsigned(vEdgePos - (5 << fieldmv))) {
            if (interlace) {
                emulatedEdgeMc(edgeEmuBuffer.data(),
                                         srcU,
                                         uvlinesize << 1,
                                         uvlinesize << 1,
                                         5,
                                         ((5 << fieldmv) + 1) >> 1,
                                         uvSrcPosX,
                                         uvSrcPosY >> 1,
                                         hEdgePos >> 1,
                                         this->vEdgePos >> 2);
                emulatedEdgeMc(edgeEmuBuffer.data() + 16,
                                         srcV,
                                         uvlinesize << 1,
                                         uvlinesize << 1,
                                         5,
                                         ((5 << fieldmv) + 1) >> 1,
                                         uvSrcPosX,
                                         uvSrcPosY >> 1,
                                         hEdgePos >> 1,
                                         this->vEdgePos >> 2);
                if (!fieldmv) {
                    emulatedEdgeMc(edgeEmuBuffer.data() + uvlinesize,
                                             srcU + uvlinesize,
                                             uvlinesize << 1,
                                             uvlinesize << 1,
                                             5,
                                             2,
                                             uvSrcPosX,
                                             (uvSrcPosY + 1) >> 1,
                                             hEdgePos >> 1,
                                             this->vEdgePos >> 2);
                    emulatedEdgeMc(edgeEmuBuffer.data() + 16 + uvlinesize,
                                             srcV + uvlinesize,
                                             uvlinesize << 1,
                                             uvlinesize << 1,
                                             5,
                                             2,
                                             uvSrcPosX,
                                             (uvSrcPosY + 1) >> 1,
                                             hEdgePos >> 1,
                                             this->vEdgePos >> 2);
                }
            } else {
                emulatedEdgeMc(edgeEmuBuffer.data(),
                                         srcU,
                                         uvlinesize,
                                         uvlinesize,
                                         5,
                                         5 << fieldmv,
                                         uvSrcPosX,
                                         uvSrcPosY,
                                         hEdgePos >> 1,
                                         this->vEdgePos >> 1);
                emulatedEdgeMc(edgeEmuBuffer.data() + 16,
                                         srcV,
                                         uvlinesize,
                                         uvlinesize,
                                         5,
                                         5 << fieldmv,
                                         uvSrcPosX,
                                         uvSrcPosY,
                                         hEdgePos >> 1,
                                         this->vEdgePos >> 1);
            }
            srcU = edgeEmuBuffer.data();
            srcV = edgeEmuBuffer.data() + 16;
            if (useIc) {
                lutScaleChroma(srcU, srcV,
                                     lutuv[(uvSrcPosY + (0 << fieldmv)) & 1],
                                     lutuv[(uvSrcPosY + (1 << fieldmv)) & 1],
                                     5, this->uvlinesize << fieldmv);
            }
        }
        if (avg) {
            if (!rnd) {
                h264chroma->avgH264ChromaPixelsTab[1](dest[1] + off, srcU, this->uvlinesize << fieldmv, 4, uvmxField[i], uvmyField[i]);
                h264chroma->avgH264ChromaPixelsTab[1](dest[2] + off, srcV, this->uvlinesize << fieldmv, 4, uvmxField[i], uvmyField[i]);
            } else {
                vc1dsp.avgNoRndVc1ChromaPixelsTab[1](dest[1] + off, srcU, this->uvlinesize << fieldmv, 4, uvmxField[i], uvmyField[i]);
                vc1dsp.avgNoRndVc1ChromaPixelsTab[1](dest[2] + off, srcV, this->uvlinesize << fieldmv, 4, uvmxField[i], uvmyField[i]);
            }
        } else {
            if (!rnd) {
                h264chroma->putH264ChromaPixelsTab[1](dest[1] + off, srcU, this->uvlinesize << fieldmv, 4, uvmxField[i], uvmyField[i]);
                h264chroma->putH264ChromaPixelsTab[1](dest[2] + off, srcV, this->uvlinesize << fieldmv, 4, uvmxField[i], uvmyField[i]);
            } else {
                vc1dsp.putNoRndVc1ChromaPixelsTab[1](dest[1] + off, srcU, this->uvlinesize << fieldmv, 4, uvmxField[i], uvmyField[i]);
                vc1dsp.putNoRndVc1ChromaPixelsTab[1](dest[2] + off, srcV, this->uvlinesize << fieldmv, 4, uvmxField[i], uvmyField[i]);
            }
        }
    }
}
void Vc1::interpMc()
{
    const Dsp* h264chroma = &vc1dsp;
    uint8_t *srcY, *srcU, *srcV;
    int dxy, mx, my, uvmx, uvmy, srcPosX, srcPosY, uvSrcPosX, uvSrcPosY;
    int vEdgePos = this->vEdgePos >> fieldMode;
    int useIc = nextUseIc;
    int interlace = nextInterlaced;
    int linesize, uvlinesize;
    if (!fieldMode && !nextPic.data[0])
        return;
    linesize   = curPic.ptr->linesize[0];
    uvlinesize = curPic.ptr->linesize[1];
    mx   = mv[1][0][0];
    my   = mv[1][0][1];
    uvmx = (mx + ((mx & 3) == 3)) >> 1;
    uvmy = (my + ((my & 3) == 3)) >> 1;
    if (fieldMode && curFieldType != refFieldType[1]) {
        my   = my   - 2 + 4 * curFieldType;
        uvmy = uvmy - 2 + 4 * curFieldType;
    }
    if (fastuvmc) {
        uvmx = uvmx + ((uvmx < 0) ? -(uvmx & 1) : (uvmx & 1));
        uvmy = uvmy + ((uvmy < 0) ? -(uvmy & 1) : (uvmy & 1));
    }
    srcY = nextPic.data[0];
    srcU = nextPic.data[1];
    srcV = nextPic.data[2];
    srcPosX   = mbX * 16 + (mx   >> 2);
    srcPosY   = mbY * 16 + (my   >> 2);
    uvSrcPosX = mbX *  8 + (uvmx >> 2);
    uvSrcPosY = mbY *  8 + (uvmy >> 2);
    if (profile != kProfileAdvanced) {
        srcPosX   = clip(  srcPosX, -16, mbWidth  * 16);
        srcPosY   = clip(  srcPosY, -16, mbHeight * 16);
        uvSrcPosX = clip(uvSrcPosX,  -8, mbWidth  *  8);
        uvSrcPosY = clip(uvSrcPosY,  -8, mbHeight *  8);
    } else {
        srcPosX   = clip(  srcPosX, -17, codedWidth);
        uvSrcPosX = clip(uvSrcPosX,  -8, codedWidth  >> 1);
        if (fcm == kIlaceFrame) {
            srcPosY = clip(srcPosY, -18 + (srcPosY & 1), codedHeight + (srcPosY & 1));
            uvSrcPosY = clip(uvSrcPosY, -8 + (uvSrcPosY & 1), (codedHeight >> 1) + (uvSrcPosY & 1));
        } else {
            srcPosY = clip(srcPosY, -18, codedHeight + 1);
            uvSrcPosY = clip(uvSrcPosY,  -8, codedHeight >> 1);
        }
    }
    srcY += srcPosY   * this->linesize   + srcPosX;
    srcU += uvSrcPosY * this->uvlinesize + uvSrcPosX;
    srcV += uvSrcPosY * this->uvlinesize + uvSrcPosX;
    if (fieldMode && refFieldType[1]) {
        srcY += linesize;
        srcU += uvlinesize;
        srcV += uvlinesize;
    }
    if (false) {
        srcU = edgeEmuBuffer.data() + 18 * this->linesize;
        srcV = edgeEmuBuffer.data() + 18 * this->linesize;
    }
    if (rangeredfrm || hEdgePos < 22 || vEdgePos < 22 || useIc
        || (unsigned)(srcPosX - 1) > unsigned(hEdgePos - (mx & 3) - 16 - 3)
        || (unsigned)(srcPosY - 1) > unsigned(vEdgePos    - (my & 3) - 16 - 3)) {
        uint8_t *ubuf = edgeEmuBuffer.data() + 19 * this->linesize;
        uint8_t *vbuf = ubuf + 9 * this->uvlinesize;
        const int k = 17 + mspel * 2;
        srcY -= mspel * (1 + this->linesize);
        if (interlace) {
            emulatedEdgeMc(edgeEmuBuffer.data(),
                                     srcY,
                                     linesize << 1,
                                     linesize << 1,
                                     k,
                                     fieldMode ? k : ((k + 1) >> 1),
                                     srcPosX - mspel,
                                     (srcPosY - mspel) >> !fieldMode,
                                     hEdgePos,
                                     this->vEdgePos >> 1);
            if (!fieldMode)
                emulatedEdgeMc(edgeEmuBuffer.data() + linesize,
                                         srcY + linesize,
                                         linesize << 1,
                                         linesize << 1,
                                         k,
                                         k >> 1,
                                         srcPosX - mspel,
                                         (srcPosY - mspel + 1) >> 1,
                                         hEdgePos,
                                         this->vEdgePos >> 1);
        } else
            emulatedEdgeMc(edgeEmuBuffer.data(),
                                     srcY,
                                     linesize,
                                     linesize,
                                     k,
                                     fieldMode ? (k << 1) - 1 : k,
                                     srcPosX - mspel,
                                     fieldMode ? 2 * (srcPosY - mspel) + refFieldType[1] :
                                                     srcPosY - mspel,
                                     hEdgePos,
                                     this->vEdgePos);
        srcY = edgeEmuBuffer.data();
        if (interlace) {
            emulatedEdgeMc(ubuf,
                                     srcU,
                                     uvlinesize << 1,
                                     uvlinesize << 1,
                                     9,
                                     fieldMode ? 9 : 5,
                                     uvSrcPosX,
                                     uvSrcPosY >> !fieldMode,
                                     hEdgePos >> 1,
                                     this->vEdgePos >> 2);
            emulatedEdgeMc(vbuf,
                                     srcV,
                                     uvlinesize << 1,
                                     uvlinesize << 1,
                                     9,
                                     fieldMode ? 9 : 5,
                                     uvSrcPosX,
                                     uvSrcPosY >> !fieldMode,
                                     hEdgePos >> 1,
                                     this->vEdgePos >> 2);
            if (!fieldMode) {
                emulatedEdgeMc(ubuf + uvlinesize,
                                         srcU + uvlinesize,
                                         uvlinesize << 1,
                                         uvlinesize << 1,
                                         9,
                                         4,
                                         uvSrcPosX,
                                         (uvSrcPosY + 1) >> 1,
                                         hEdgePos >> 1,
                                         this->vEdgePos >> 2);
                emulatedEdgeMc(vbuf + uvlinesize,
                                         srcV + uvlinesize,
                                         uvlinesize << 1,
                                         uvlinesize << 1,
                                         9,
                                         4,
                                         uvSrcPosX,
                                         (uvSrcPosY + 1) >> 1,
                                         hEdgePos >> 1,
                                         this->vEdgePos >> 2);
            }
        } else {
            emulatedEdgeMc(ubuf,
                                     srcU,
                                     uvlinesize,
                                     uvlinesize,
                                     9,
                                     fieldMode ? 17 : 9,
                                     uvSrcPosX,
                                     fieldMode ? 2 * uvSrcPosY + refFieldType[1] : uvSrcPosY,
                                     hEdgePos >> 1,
                                     this->vEdgePos >> 1);
            emulatedEdgeMc(vbuf,
                                     srcV,
                                     uvlinesize,
                                     uvlinesize,
                                     9,
                                     fieldMode ? 17 : 9,
                                     uvSrcPosX,
                                     fieldMode ? 2 * uvSrcPosY + refFieldType[1] : uvSrcPosY,
                                     hEdgePos >> 1,
                                     this->vEdgePos >> 1);
        }
        srcU = ubuf;
        srcV = vbuf;
        if (rangeredfrm) {
            scaleLuma(srcY, k, this->linesize);
            scaleChroma(srcU, srcV, 9, this->uvlinesize);
        }
        if (useIc) {
            const uint8_t (*luty )[256] = nextLuty;
            const uint8_t (*lutuv)[256] = nextLutuv;
            lutScaleLuma(srcY,
                               luty[fieldMode ? refFieldType[1] : ((0+srcPosY - mspel) & 1)],
                               luty[fieldMode ? refFieldType[1] : ((1+srcPosY - mspel) & 1)],
                               k, this->linesize);
            lutScaleChroma(srcU, srcV,
                                 lutuv[fieldMode ? refFieldType[1] : ((0+uvSrcPosY) & 1)],
                                 lutuv[fieldMode ? refFieldType[1] : ((1+uvSrcPosY) & 1)],
                                 9, this->uvlinesize);
        }
        srcY += mspel * (1 + this->linesize);
    }
    if (mspel) {
        dxy = ((my & 3) << 2) | (mx & 3);
        vc1dsp.avgVc1MspelPixelsTab[0][dxy](dest[0], srcY, this->linesize, rnd);
    } else {
        dxy = (my & 2) | ((mx & 2) >> 1);
        if (!rnd)
            vc1dsp.avgPixelsTab[0][dxy](dest[0], srcY, this->linesize, 16);
        else
            vc1dsp.avgNoRndPixelsTab[dxy](dest[0], srcY, this->linesize, 16);
    }
    if (false)
        return;
    uvmx = (uvmx & 3) << 1;
    uvmy = (uvmy & 3) << 1;
    if (!rnd) {
        h264chroma->avgH264ChromaPixelsTab[0](dest[1], srcU, this->uvlinesize, 8, uvmx, uvmy);
        h264chroma->avgH264ChromaPixelsTab[0](dest[2], srcV, this->uvlinesize, 8, uvmx, uvmy);
    } else {
        vc1dsp.avgNoRndVc1ChromaPixelsTab[0](dest[1], srcU, this->uvlinesize, 8, uvmx, uvmy);
        vc1dsp.avgNoRndVc1ChromaPixelsTab[0](dest[2], srcV, this->uvlinesize, 8, uvmx, uvmy);
    }
}

}

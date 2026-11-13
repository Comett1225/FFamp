#include "vc1.h"
#include <cstring>

namespace absent::vc1 {


void Vc1::hOverlapFilter(int16_t (*leftBlock)[64],
                                                  int16_t (*rightBlock)[64], int leftFieldtx,
                                                  int rightFieldtx, int blockNum)
{
    switch (blockNum) {
    case 0:
        vc1dsp.hSOverlap(leftBlock[2],
                                  rightBlock[0],
                                  leftFieldtx ^ rightFieldtx ? 16 - 8 * leftFieldtx : 8,
                                  leftFieldtx ^ rightFieldtx ? 16 - 8 * rightFieldtx : 8,
                                  leftFieldtx || rightFieldtx ? 0 : 1);
        break;
    case 1:
        vc1dsp.hSOverlap(rightBlock[0],
                                  rightBlock[2],
                                  8,
                                  8,
                                  rightFieldtx ? 0 : 1);
        break;
    case 2:
        vc1dsp.hSOverlap(!leftFieldtx && rightFieldtx ? leftBlock[2] + 8 : leftBlock[3],
                                  leftFieldtx && !rightFieldtx ? rightBlock[0] + 8 : rightBlock[1],
                                  leftFieldtx ^ rightFieldtx ? 16 - 8 * leftFieldtx : 8,
                                  leftFieldtx ^ rightFieldtx ? 16 - 8 * rightFieldtx : 8,
                                  leftFieldtx || rightFieldtx ? 2 : 1);
        break;
    case 3:
        vc1dsp.hSOverlap(rightBlock[1],
                                  rightBlock[3],
                                  8,
                                  8,
                                  rightFieldtx ? 2 : 1);
        break;
    case 4:
    case 5:
        vc1dsp.hSOverlap(leftBlock[blockNum], rightBlock[blockNum], 8, 8, 1);
        break;
    }
}
void Vc1::vOverlapFilter(int16_t (*topBlock)[64],
                                                  int16_t (*bottomBlock)[64], int blockNum)
{
    switch (blockNum) {
    case 0:
        vc1dsp.vSOverlap(topBlock[1], bottomBlock[0]);
        break;
    case 1:
        vc1dsp.vSOverlap(topBlock[3], bottomBlock[2]);
        break;
    case 2:
        vc1dsp.vSOverlap(bottomBlock[0], bottomBlock[1]);
        break;
    case 3:
        vc1dsp.vSOverlap(bottomBlock[2], bottomBlock[3]);
        break;
    case 4:
    case 5:
        vc1dsp.vSOverlap(topBlock[blockNum], bottomBlock[blockNum]);
        break;
    }
}
void Vc1::iOverlapFilter()
{
    int16_t (*topleftBlk)[64], (*topBlk)[64], (*leftBlk)[64], (*curBlk)[64];
    int blockCount = false ? 4 : 6;
    int mbPos = mbX + mbY * mbStride;
    int i;
    topleftBlk = block[topleftBlkIdx];
    topBlk = block[topBlkIdx];
    leftBlk = block[leftBlkIdx];
    curBlk = block[curBlkIdx];
    for (i = 0; i < blockCount; i++) {
        if (mbX == 0 && (i & 5) != 1)
            continue;
        if (pq >= 9 || (profile == kProfileAdvanced &&
                           (condover == kCondoverAll ||
                            (overFlagsPlane[mbPos] &&
                             ((i & 5) == 1 || overFlagsPlane[mbPos - 1])))))
            hOverlapFilter(mbX ? leftBlk : curBlk, curBlk,
                                 fcm == kIlaceFrame && mbX && fieldtxPlane[mbPos - 1],
                                 fcm == kIlaceFrame && fieldtxPlane[mbPos],
                                 i);
    }
    if (fcm != kIlaceFrame)
        for (i = 0; i < blockCount; i++) {
            if (firstSliceLine && !(i & 2))
                continue;
            if (mbX &&
                (pq >= 9 || (profile == kProfileAdvanced &&
                                (condover == kCondoverAll ||
                                 (overFlagsPlane[mbPos - 1] &&
                                  ((i & 2) || overFlagsPlane[mbPos - 1 - mbStride]))))))
                vOverlapFilter(firstSliceLine ? leftBlk : topleftBlk, leftBlk, i);
            if (mbX == mbWidth - 1 &&
                (pq >= 9 || (profile == kProfileAdvanced &&
                                (condover == kCondoverAll ||
                                 (overFlagsPlane[mbPos] &&
                                  ((i & 2) || overFlagsPlane[mbPos - mbStride]))))))
                vOverlapFilter(firstSliceLine ? curBlk : topBlk, curBlk, i);
        }
}
void Vc1::pOverlapFilter()
{
    int16_t (*topleftBlk)[64], (*topBlk)[64], (*leftBlk)[64], (*curBlk)[64];
    int blockCount = false ? 4 : 6;
    int mbPos = mbX + mbY * mbStride;
    int i;
    topleftBlk = block[topleftBlkIdx];
    topBlk = block[topBlkIdx];
    leftBlk = block[leftBlkIdx];
    curBlk = block[curBlkIdx];
    for (i = 0; i < blockCount; i++) {
        if (mbX == 0 && (i & 5) != 1)
            continue;
        if (mbType[blockIndex[i]] && mbType[blockIndex[i] - 1])
            hOverlapFilter(mbX ? leftBlk : curBlk, curBlk,
                                 fcm == kIlaceFrame && mbX && fieldtxPlane[mbPos - 1],
                                 fcm == kIlaceFrame && fieldtxPlane[mbPos],
                                 i);
    }
    if (fcm != kIlaceFrame)
        for (i = 0; i < blockCount; i++) {
            if (firstSliceLine && !(i & 2))
                continue;
            if (mbX && mbType[blockIndex[i] - 2 + (i > 3)] &&
                mbType[blockIndex[i] - blockWrap[i] - 2 + (i > 3)])
                vOverlapFilter(firstSliceLine ? leftBlk : topleftBlk, leftBlk, i);
            if (mbX == mbWidth - 1)
                if (mbType[blockIndex[i]] &&
                    mbType[blockIndex[i] - blockWrap[i]])
                    vOverlapFilter(firstSliceLine ? curBlk : topBlk, curBlk, i);
        }
}
constexpr uint32_t kLeftEdge = 1 << 0, kRightEdge = 1 << 1, kTopEdge = 1 << 2, kBottomEdge = 1 << 3;
void Vc1::iHLoopFilter(uint8_t *dest,
                                                 uint32_t flags, int blockNum)
{
    int pq = this->pq;
    uint8_t *dst;
    if (blockNum & 2)
        return;
    if (!(flags & kLeftEdge) || (blockNum & 5) == 1) {
        if (blockNum > 3)
            dst = dest;
        else
            dst = dest + (blockNum & 2) * 4 * linesize + (blockNum & 1) * 8;
        if (fcm == kIlaceFrame)
            if (blockNum > 3) {
                vc1dsp.hLoopFilter4(dst, 2 * uvlinesize, pq);
                vc1dsp.hLoopFilter4(dst + uvlinesize, 2 * uvlinesize, pq);
            } else {
                vc1dsp.hLoopFilter8(dst, 2 * linesize, pq);
                vc1dsp.hLoopFilter8(dst + linesize, 2 * linesize, pq);
            }
        else
            if (blockNum > 3)
                vc1dsp.hLoopFilter8(dst, uvlinesize, pq);
            else
                vc1dsp.hLoopFilter16(dst, linesize, pq);
    }
}
void Vc1::iVLoopFilter(uint8_t *dest,
                                                 uint32_t flags, uint8_t fieldtx,
                                                 int blockNum)
{
    int pq = this->pq;
    uint8_t *dst;
    if ((blockNum & 5) == 1)
        return;
    if (!(flags & kTopEdge) || blockNum & 2) {
        if (blockNum > 3)
            dst = dest;
        else
            dst = dest + (blockNum & 2) * 4 * linesize + (blockNum & 1) * 8;
        if (fcm == kIlaceFrame) {
            if (blockNum > 3) {
                vc1dsp.vLoopFilter8(dst, 2 * uvlinesize, pq);
                vc1dsp.vLoopFilter8(dst + uvlinesize, 2 * uvlinesize, pq);
            } else if (blockNum < 2 || !fieldtx) {
                vc1dsp.vLoopFilter16(dst, 2 * linesize, pq);
                vc1dsp.vLoopFilter16(dst + linesize, 2 * linesize, pq);
            }
        } else
            if (blockNum > 3)
                vc1dsp.vLoopFilter8(dst, uvlinesize, pq);
            else
                vc1dsp.vLoopFilter16(dst, linesize, pq);
    }
}
void Vc1::iLoopFilter()
{
    int blockCount = false ? 4 : 6;
    int mbPos = mbX + mbY * mbStride;
    uint8_t *dest, fieldtx;
    uint32_t flags = 0;
    int i;
    if (!firstSliceLine) {
        dest = this->dest[0] - 16 * linesize - 16;
        flags = mbY == startMbY + 1 ? kTopEdge : 0;
        if (mbX) {
            fieldtx = fieldtxPlane[mbPos - mbStride - 1];
            for (i = 0; i < blockCount; i++)
                iVLoopFilter(i > 3 ? this->dest[i - 3] - 8 * uvlinesize - 8 : dest, flags, fieldtx, i);
        }
        if (mbX == endMbX - 1) {
            dest += 16;
            fieldtx = fieldtxPlane[mbPos - mbStride];
            for (i = 0; i < blockCount; i++)
                iVLoopFilter(i > 3 ? this->dest[i - 3] - 8 * uvlinesize : dest, flags, fieldtx, i);
        }
    }
    if (mbY == endMbY - 1) {
        dest = this->dest[0] - 16;
        flags = firstSliceLine ? kTopEdge | kBottomEdge : kBottomEdge;
        if (mbX) {
            fieldtx = fieldtxPlane[mbPos - 1];
            for (i = 0; i < blockCount; i++)
                iVLoopFilter(i > 3 ? this->dest[i - 3] - 8 : dest, flags, fieldtx, i);
        }
        if (mbX == endMbX - 1) {
            dest += 16;
            fieldtx = fieldtxPlane[mbPos];
            for (i = 0; i < blockCount; i++)
                iVLoopFilter(i > 3 ? this->dest[i - 3] : dest, flags, fieldtx, i);
        }
    }
    if (mbY >= startMbY + 2) {
        dest = this->dest[0] - 32 * linesize - 16;
        if (mbX) {
            flags = mbX == 1 ? kLeftEdge : 0;
            for (i = 0; i < blockCount; i++)
                iHLoopFilter(i > 3 ? this->dest[i - 3] - 16 * uvlinesize - 8 : dest, flags, i);
        }
        if (mbX == endMbX - 1) {
            dest += 16;
            flags = mbX == 0 ? kLeftEdge | kRightEdge : kRightEdge;
            for (i = 0; i < blockCount; i++)
                iHLoopFilter(i > 3 ? this->dest[i - 3] - 16 * uvlinesize : dest, flags, i);
        }
    }
    if (mbY == endMbY - 1) {
        if (mbY >= startMbY + 1) {
            dest = this->dest[0] - 16 * linesize - 16;
            if (mbX) {
                flags = mbX == 1 ? kLeftEdge : 0;
                for (i = 0; i < blockCount; i++)
                    iHLoopFilter(i > 3 ? this->dest[i - 3] - 8 * uvlinesize - 8 : dest, flags, i);
            }
            if (mbX == endMbX - 1) {
                flags = mbX == 0 ? kLeftEdge | kRightEdge : kRightEdge;
                dest += 16;
                for (i = 0; i < blockCount; i++)
                    iHLoopFilter(i > 3 ? this->dest[i - 3] - 8 * uvlinesize : dest, flags, i);
            }
        }
        dest = this->dest[0] - 16;
        if (mbX) {
            flags = mbX == 1 ? kLeftEdge : 0;
            for (i = 0; i < blockCount; i++)
                iHLoopFilter(i > 3 ? this->dest[i - 3] - 8 : dest, flags, i);
        }
        if (mbX == endMbX - 1) {
            dest += 16;
            flags = mbX == 0 ? kLeftEdge | kRightEdge : kRightEdge;
            for (i = 0; i < blockCount; i++)
                iHLoopFilter(i > 3 ? this->dest[i - 3] : dest, flags, i);
        }
    }
}
void Vc1::pHLoopFilter(uint8_t *dest, uint32_t *cbp,
                                                 uint8_t *isIntra, int16_t (*mv)[2], uint8_t *mvF,
                                                 int *ttblk, uint32_t flags, int blockNum)
{
    int pq = this->pq;
    uint32_t leftCbp = cbp[0] >> (blockNum * 4), rightCbp;
    uint8_t leftIsIntra, rightIsIntra;
    int tt;
    int idx, linesize  = blockNum > 3 ? uvlinesize : this->linesize;
    uint8_t *dst;
    if (blockNum > 3)
        dst = dest;
    else
        dst = dest + (blockNum & 2) * 4 * this->linesize + (blockNum & 1) * 8;
    if (!(flags & kRightEdge) || !(blockNum & 5)) {
        leftIsIntra = isIntra[0] & (1 << blockNum);
        if (blockNum > 3) {
            rightIsIntra = isIntra[1] & (1 << blockNum);
            rightCbp = cbp[1] >> (blockNum * 4);
        } else if (blockNum & 1) {
            rightIsIntra = isIntra[1] & (1 << (blockNum - 1));
            rightCbp = cbp[1] >> ((blockNum - 1) * 4);
        } else {
            rightIsIntra = isIntra[0] & (1 << (blockNum + 1));
            rightCbp = cbp[0] >> ((blockNum + 1) * 4);
        }
        if (leftIsIntra || rightIsIntra ||
            mv[0][0] != mv[1][0] || mv[0][1] != mv[1][1] ||
            (fcm == kIlaceField && mvF[0] != mvF[1]))
            vc1dsp.hLoopFilter8(dst + 8, linesize, pq);
        else {
            idx = (leftCbp | (rightCbp >> 1)) & 5;
            if (idx & 1)
                vc1dsp.hLoopFilter4(dst + 4 * linesize + 8, linesize, pq);
            if (idx & 4)
                vc1dsp.hLoopFilter4(dst + 8, linesize, pq);
        }
    }
    tt = ttblk[0] >> (blockNum * 4) & 0xf;
    if (tt == kTt4x4 || tt == kTt4x8) {
        if (leftCbp & 3)
            vc1dsp.hLoopFilter4(dst + 4 * linesize + 4, linesize, pq);
        if (leftCbp & 12)
            vc1dsp.hLoopFilter4(dst + 4, linesize, pq);
    }
}
void Vc1::pVLoopFilter(uint8_t *dest, const uint32_t *cbp,
                         const uint8_t *isIntra, int16_t (*mv)[2], const uint8_t *mvF,
                         const int *ttblk, uint32_t flags, int blockNum)
{
    int pq = this->pq;
    uint32_t topCbp = cbp[0] >> (blockNum * 4), bottomCbp;
    uint8_t topIsIntra, bottomIsIntra;
    int tt;
    int idx, linesize  = blockNum > 3 ? uvlinesize : this->linesize;
    uint8_t *dst;
    if (blockNum > 3)
        dst = dest;
    else
        dst = dest + (blockNum & 2) * 4 * this->linesize + (blockNum & 1) * 8;
    if(!(flags & kBottomEdge) || blockNum < 2) {
        topIsIntra = isIntra[0] & (1 << blockNum);
        if (blockNum > 3) {
            bottomIsIntra = isIntra[mbStride] & (1 << blockNum);
            bottomCbp = cbp[mbStride] >> (blockNum * 4);
        } else if (blockNum < 2) {
            bottomIsIntra = isIntra[0] & (1 << (blockNum + 2));
            bottomCbp = cbp[0] >> ((blockNum + 2) * 4);
        } else {
            bottomIsIntra = isIntra[mbStride] & (1 << (blockNum - 2));
            bottomCbp = cbp[mbStride] >> ((blockNum - 2) * 4);
        }
        if (topIsIntra || bottomIsIntra ||
            mv[0][0] != mv[blockNum > 3 ? mbStride : b8Stride][0] ||
            mv[0][1] != mv[blockNum > 3 ? mbStride : b8Stride][1] ||
            (fcm == kIlaceField && mvF[0] != mvF[blockNum > 3 ? mbStride : b8Stride]))
            vc1dsp.vLoopFilter8(dst + 8 * linesize, linesize, pq);
        else {
            idx = (topCbp | (bottomCbp >> 2)) & 3;
            if (idx & 1)
                vc1dsp.vLoopFilter4(dst + 8 * linesize + 4, linesize, pq);
            if (idx & 2)
                vc1dsp.vLoopFilter4(dst + 8 * linesize, linesize, pq);
        }
    }
    tt = ttblk[0] >> (blockNum * 4) & 0xf;
    if (tt == kTt4x4 || tt == kTt8x4) {
        if (topCbp & 5)
            vc1dsp.vLoopFilter4(dst + 4 * linesize + 4, linesize, pq);
        if (topCbp & 10)
            vc1dsp.vLoopFilter4(dst + 4 * linesize, linesize, pq);
    }
}
void Vc1::pLoopFilter()
{
    int blockCount = false ? 4 : 6;
    uint8_t *dest;
    uint32_t *cbp;
    uint8_t *isIntra;
    int16_t (*uvmv)[2];
    int *ttblk;
    uint32_t flags;
    int i;
    if (mbY >= startMbY + 2) {
        if (mbX) {
            dest = this->dest[0] - 32 * linesize - 16;
            cbp = &this->cbp[mbX - 2 * mbStride - 1];
            isIntra = &this->isIntra[mbX - 2 * mbStride - 1];
            uvmv = &lumaMv[mbX - 2 * mbStride - 1];
            ttblk = &this->ttblk[mbX - 2 * mbStride - 1];
            flags = mbY == startMbY + 2 ? kTopEdge : 0;
            for (i = 0; i < blockCount; i++)
                pVLoopFilter(i > 3 ? this->dest[i - 3] - 16 * uvlinesize - 8 : dest,
                                    cbp,
                                    isIntra,
                                    i > 3 ? uvmv :
                                            &curPic.motionVal[0][blockIndex[i] - 4 * b8Stride - 2 + blocksOff],
                                    i > 3 ? &mvF[0][blockIndex[i] - 2 * mbStride - 1 + mbOff] :
                                            &mvF[0][blockIndex[i] - 4 * b8Stride - 2 + blocksOff],
                                    ttblk,
                                    flags,
                                    i);
        }
        if (mbX == mbWidth - 1) {
            dest = this->dest[0] - 32 * linesize;
            cbp = &this->cbp[mbX - 2 * mbStride];
            isIntra = &this->isIntra[mbX - 2 * mbStride];
            uvmv = &lumaMv[mbX - 2 * mbStride];
            ttblk = &this->ttblk[mbX - 2 * mbStride];
            flags = mbY == startMbY + 2 ? kTopEdge : 0;
            for (i = 0; i < blockCount; i++)
                pVLoopFilter(i > 3 ? this->dest[i - 3] - 16 * uvlinesize : dest,
                                    cbp,
                                    isIntra,
                                    i > 3 ? uvmv :
                                            &curPic.motionVal[0][blockIndex[i] - 4 * b8Stride + blocksOff],
                                    i > 3 ? &mvF[0][blockIndex[i] - 2 * mbStride + mbOff] :
                                            &mvF[0][blockIndex[i] - 4 * b8Stride + blocksOff],
                                    ttblk,
                                    flags,
                                    i);
        }
    }
    if (mbY == endMbY - 1) {
        if (mbX) {
            if (mbY >= startMbY + 1) {
                dest = this->dest[0] - 16 * linesize - 16;
                cbp = &this->cbp[mbX - mbStride - 1];
                isIntra = &this->isIntra[mbX - mbStride - 1];
                uvmv = &lumaMv[mbX - mbStride - 1];
                ttblk = &this->ttblk[mbX - mbStride - 1];
                flags = mbY == startMbY + 1 ? kTopEdge : 0;
                for (i = 0; i < blockCount; i++)
                    pVLoopFilter(i > 3 ? this->dest[i - 3] - 8 * uvlinesize - 8 : dest,
                                        cbp,
                                        isIntra,
                                        i > 3 ? uvmv :
                                                &curPic.motionVal[0][blockIndex[i] - 2 * b8Stride - 2 + blocksOff],
                                        i > 3 ? &mvF[0][blockIndex[i] - mbStride - 1 + mbOff] :
                                                &mvF[0][blockIndex[i] - 2 * b8Stride - 2 + blocksOff],
                                        ttblk,
                                        flags,
                                        i);
            }
            dest = this->dest[0] - 16;
            cbp = &this->cbp[mbX - 1];
            isIntra = &this->isIntra[mbX - 1];
            uvmv = &lumaMv[mbX - 1];
            ttblk = &this->ttblk[mbX - 1];
            flags = mbY == startMbY ? kTopEdge | kBottomEdge : kBottomEdge;
            for (i = 0; i < blockCount; i++)
                pVLoopFilter(i > 3 ? this->dest[i - 3] - 8 : dest,
                                    cbp,
                                    isIntra,
                                    i > 3 ? uvmv :
                                            &curPic.motionVal[0][blockIndex[i] - 2 + blocksOff],
                                    i > 3 ? &mvF[0][blockIndex[i] - 1 + mbOff] :
                                            &mvF[0][blockIndex[i] - 2 + blocksOff],
                                    ttblk,
                                    flags,
                                    i);
        }
        if (mbX == mbWidth - 1) {
            if (mbY >= startMbY + 1) {
                dest = this->dest[0] - 16 * linesize;
                cbp = &this->cbp[mbX - mbStride];
                isIntra = &this->isIntra[mbX - mbStride];
                uvmv = &lumaMv[mbX - mbStride];
                ttblk = &this->ttblk[mbX - mbStride];
                flags = mbY == startMbY + 1 ? kTopEdge : 0;
                for (i = 0; i < blockCount; i++)
                    pVLoopFilter(i > 3 ? this->dest[i - 3] - 8 * uvlinesize : dest,
                                        cbp,
                                        isIntra,
                                        i > 3 ? uvmv :
                                                &curPic.motionVal[0][blockIndex[i] - 2 * b8Stride + blocksOff],
                                        i > 3 ? &mvF[0][blockIndex[i] - mbStride + mbOff] :
                                                &mvF[0][blockIndex[i] - 2 * b8Stride + blocksOff],
                                        ttblk,
                                        flags,
                                        i);
            }
            dest = this->dest[0];
            cbp = &this->cbp[mbX];
            isIntra = &this->isIntra[mbX];
            uvmv = &lumaMv[mbX];
            ttblk = &this->ttblk[mbX];
            flags = mbY == startMbY ? kTopEdge | kBottomEdge : kBottomEdge;
            for (i = 0; i < blockCount; i++)
                pVLoopFilter(i > 3 ? this->dest[i - 3] : dest,
                                    cbp,
                                    isIntra,
                                    i > 3 ? uvmv :
                                            &curPic.motionVal[0][blockIndex[i] + blocksOff],
                                    i > 3 ? &mvF[0][blockIndex[i] + mbOff] :
                                            &mvF[0][blockIndex[i] + blocksOff],
                                    ttblk,
                                    flags,
                                    i);
        }
    }
    if (mbY >= startMbY + 2) {
        if (mbX >= 2) {
            dest = this->dest[0] - 32 * linesize - 32;
            cbp = &this->cbp[mbX - 2 * mbStride - 2];
            isIntra = &this->isIntra[mbX - 2 * mbStride - 2];
            uvmv = &lumaMv[mbX - 2 * mbStride - 2];
            ttblk = &this->ttblk[mbX - 2 * mbStride - 2];
            flags = mbX == 2 ? kLeftEdge : 0;
            for (i = 0; i < blockCount; i++)
                pHLoopFilter(i > 3 ? this->dest[i - 3] - 16 * uvlinesize - 16 : dest,
                                    cbp,
                                    isIntra,
                                    i > 3 ? uvmv :
                                            &curPic.motionVal[0][blockIndex[i] - 4 * b8Stride - 4 + blocksOff],
                                    i > 3 ? &mvF[0][blockIndex[i] - 2 * mbStride - 2 + mbOff] :
                                            &mvF[0][blockIndex[i] - 4 * b8Stride - 4 + blocksOff],
                                    ttblk,
                                    flags,
                                    i);
        }
        if (mbX == mbWidth - 1) {
            if (mbX >= 1) {
                dest = this->dest[0] - 32 * linesize - 16;
                cbp = &this->cbp[mbX - 2 * mbStride - 1];
                isIntra = &this->isIntra[mbX - 2 * mbStride - 1];
                uvmv = &lumaMv[mbX - 2 * mbStride - 1];
                ttblk = &this->ttblk[mbX - 2 * mbStride - 1];
                flags = mbX == 1 ? kLeftEdge : 0;
                for (i = 0; i < blockCount; i++)
                        pHLoopFilter(i > 3 ? this->dest[i - 3] - 16 * uvlinesize - 8 : dest,
                                            cbp,
                                            isIntra,
                                            i > 3 ? uvmv :
                                                    &curPic.motionVal[0][blockIndex[i] - 4 * b8Stride - 2 + blocksOff],
                                            i > 3 ? &mvF[0][blockIndex[i] - 2 * mbStride - 1 + mbOff] :
                                                    &mvF[0][blockIndex[i] - 4 * b8Stride - 2 + blocksOff],
                                            ttblk,
                                            flags,
                                            i);
            }
            dest = this->dest[0] - 32 * linesize;
            cbp = &this->cbp[mbX - 2 * mbStride];
            isIntra = &this->isIntra[mbX - 2 * mbStride];
            uvmv = &lumaMv[mbX - 2 * mbStride];
            ttblk = &this->ttblk[mbX - 2 * mbStride];
            flags = mbX ? kRightEdge : kLeftEdge | kRightEdge;
            for (i = 0; i < blockCount; i++)
                pHLoopFilter(i > 3 ? this->dest[i - 3] - 16 * uvlinesize : dest,
                                    cbp,
                                    isIntra,
                                    i > 3 ? uvmv :
                                            &curPic.motionVal[0][blockIndex[i] - 4 * b8Stride + blocksOff],
                                    i > 3 ? &mvF[0][blockIndex[i] - 2 * mbStride + mbOff] :
                                            &mvF[0][blockIndex[i] - 4 * b8Stride + blocksOff],
                                    ttblk,
                                    flags,
                                    i);
        }
    }
    if (mbY == endMbY - 1) {
        if (mbY >= startMbY + 1) {
            if (mbX >= 2) {
                dest = this->dest[0] - 16 * linesize - 32;
                cbp = &this->cbp[mbX - mbStride - 2];
                isIntra = &this->isIntra[mbX - mbStride - 2];
                uvmv = &lumaMv[mbX - mbStride - 2];
                ttblk = &this->ttblk[mbX - mbStride - 2];
                flags = mbX == 2 ? kLeftEdge : 0;
                for (i = 0; i < blockCount; i++)
                    pHLoopFilter(i > 3 ? this->dest[i - 3] - 8 * uvlinesize - 16 : dest,
                                        cbp,
                                        isIntra,
                                        i > 3 ? uvmv :
                                                &curPic.motionVal[0][blockIndex[i] - 2 * b8Stride - 4 + blocksOff],
                                        i > 3 ? &mvF[0][blockIndex[i] - mbStride - 2 + mbOff] :
                                                &mvF[0][blockIndex[i] - 2 * b8Stride - 4 + blocksOff],
                                        ttblk,
                                        flags,
                                        i);
            }
            if (mbX == mbWidth - 1) {
                if (mbX >= 1) {
                    dest = this->dest[0] - 16 * linesize - 16;
                    cbp = &this->cbp[mbX - mbStride - 1];
                    isIntra = &this->isIntra[mbX - mbStride - 1];
                    uvmv = &lumaMv[mbX - mbStride - 1];
                    ttblk = &this->ttblk[mbX - mbStride - 1];
                    flags = mbX == 1 ? kLeftEdge : 0;
                    for (i = 0; i < blockCount; i++)
                            pHLoopFilter(i > 3 ? this->dest[i - 3] - 8 * uvlinesize - 8 : dest,
                                                cbp,
                                                isIntra,
                                                i > 3 ? uvmv :
                                                        &curPic.motionVal[0][blockIndex[i] - 2 * b8Stride - 2 + blocksOff],
                                                i > 3 ? &mvF[0][blockIndex[i] - mbStride - 1 + mbOff] :
                                                        &mvF[0][blockIndex[i] - 2 * b8Stride - 2 + blocksOff],
                                                ttblk,
                                                flags,
                                                i);
                }
                dest = this->dest[0] - 16 * linesize;
                cbp = &this->cbp[mbX - mbStride];
                isIntra = &this->isIntra[mbX - mbStride];
                uvmv = &lumaMv[mbX - mbStride];
                ttblk = &this->ttblk[mbX - mbStride];
                flags = mbX ? kRightEdge : kLeftEdge | kRightEdge;
                for (i = 0; i < blockCount; i++)
                    pHLoopFilter(i > 3 ? this->dest[i - 3] - 8 * uvlinesize : dest,
                                        cbp,
                                        isIntra,
                                        i > 3 ? uvmv :
                                                &curPic.motionVal[0][blockIndex[i] - 2 * b8Stride + blocksOff],
                                        i > 3 ? &mvF[0][blockIndex[i] - mbStride + mbOff] :
                                                &mvF[0][blockIndex[i] - 2 * b8Stride + blocksOff],
                                        ttblk,
                                        flags,
                                        i);
            }
        }
        if (mbX >= 2) {
            dest = this->dest[0] - 32;
            cbp = &this->cbp[mbX - 2];
            isIntra = &this->isIntra[mbX - 2];
            uvmv = &lumaMv[mbX - 2];
            ttblk = &this->ttblk[mbX - 2];
            flags = mbX == 2 ? kLeftEdge : 0;
            for (i = 0; i < blockCount; i++)
                pHLoopFilter(i > 3 ? this->dest[i - 3] - 16 : dest,
                                    cbp,
                                    isIntra,
                                    i > 3 ? uvmv :
                                            &curPic.motionVal[0][blockIndex[i] - 4 + blocksOff],
                                    i > 3 ? &mvF[0][blockIndex[i] - 2 + mbOff] :
                                            &mvF[0][blockIndex[i] - 4 + blocksOff],
                                    ttblk,
                                    flags,
                                    i);
        }
        if (mbX == mbWidth - 1) {
            if (mbX >= 1) {
                dest = this->dest[0] - 16;
                cbp = &this->cbp[mbX - 1];
                isIntra = &this->isIntra[mbX - 1];
                uvmv = &lumaMv[mbX - 1];
                ttblk = &this->ttblk[mbX - 1];
                flags = mbX == 1 ? kLeftEdge : 0;
                for (i = 0; i < blockCount; i++)
                    pHLoopFilter(i > 3 ? this->dest[i - 3] - 8 : dest,
                                        cbp,
                                        isIntra,
                                        i > 3 ? uvmv :
                                                &curPic.motionVal[0][blockIndex[i] - 2 + blocksOff],
                                        i > 3 ? &mvF[0][blockIndex[i] - 1 + mbOff] :
                                                &mvF[0][blockIndex[i] - 2 + blocksOff],
                                        ttblk,
                                        flags,
                                        i);
            }
            dest = this->dest[0];
            cbp = &this->cbp[mbX];
            isIntra = &this->isIntra[mbX];
            uvmv = &lumaMv[mbX];
            ttblk = &this->ttblk[mbX];
            flags = mbX ? kRightEdge : kLeftEdge | kRightEdge;
            for (i = 0; i < blockCount; i++)
                pHLoopFilter(i > 3 ? this->dest[i - 3] : dest,
                                    cbp,
                                    isIntra,
                                    i > 3 ? uvmv :
                                            &curPic.motionVal[0][blockIndex[i] + blocksOff],
                                    i > 3 ? &mvF[0][blockIndex[i] + mbOff] :
                                            &mvF[0][blockIndex[i] + blocksOff],
                                    ttblk,
                                    flags,
                                    i);
        }
    }
}
void Vc1::pHIntfrLoopFilter(uint8_t *dest, const int *ttblk,
                                                       uint32_t flags, uint8_t fieldtx, int blockNum)
{
    int pq = this->pq;
    int tt;
    int linesize  = blockNum > 3 ? uvlinesize : this->linesize;
    uint8_t *dst;
    if (blockNum > 3)
        dst = dest;
    else
        dst = dest + (blockNum & 2) * 4 * this->linesize + (blockNum & 1) * 8;
    tt = ttblk[0] >> (blockNum * 4) & 0xf;
    if (blockNum < 4) {
        if (fieldtx) {
            if (blockNum < 2) {
                if (tt == kTt4x4 || tt == kTt4x8)
                    vc1dsp.hLoopFilter8(dst + 4, 2 * linesize, pq);
                if (!(flags & kRightEdge) || blockNum == 0)
                    vc1dsp.hLoopFilter8(dst + 8, 2 * linesize, pq);
            } else {
                if (tt == kTt4x4 || tt == kTt4x8)
                    vc1dsp.hLoopFilter8(dst - 7 * linesize + 4, 2 * linesize, pq);
                if (!(flags & kRightEdge) || blockNum == 2)
                    vc1dsp.hLoopFilter8(dst - 7 * linesize + 8, 2 * linesize, pq);
            }
        } else {
            if(tt == kTt4x4 || tt == kTt4x8) {
                vc1dsp.hLoopFilter4(dst + 4, 2 * linesize, pq);
                vc1dsp.hLoopFilter4(dst + linesize + 4, 2 * linesize, pq);
            }
            if (!(flags & kRightEdge) || !(blockNum & 5)) {
                vc1dsp.hLoopFilter4(dst + 8, 2 * linesize, pq);
                vc1dsp.hLoopFilter4(dst + linesize + 8, 2 * linesize, pq);
            }
        }
    } else {
        if (tt == kTt4x4 || tt == kTt4x8) {
            vc1dsp.hLoopFilter4(dst + 4, 2 * linesize, pq);
            vc1dsp.hLoopFilter4(dst + linesize + 4, 2 * linesize, pq);
        }
        if (!(flags & kRightEdge)) {
            vc1dsp.hLoopFilter4(dst + 8, 2 * linesize, pq);
            vc1dsp.hLoopFilter4(dst + linesize + 8, 2 * linesize, pq);
        }
    }
}
void Vc1::pVIntfrLoopFilter(uint8_t *dest, const int *ttblk,
                               uint32_t flags, uint8_t fieldtx, int blockNum)
{
    int pq = this->pq;
    int tt;
    int linesize  = blockNum > 3 ? uvlinesize : this->linesize;
    uint8_t *dst;
    if (blockNum > 3)
        dst = dest;
    else
        dst = dest + (blockNum & 2) * 4 * this->linesize + (blockNum & 1) * 8;
    tt = ttblk[0] >> (blockNum * 4) & 0xf;
    if (blockNum < 4) {
        if (fieldtx) {
            if (blockNum < 2) {
                if (tt == kTt4x4 || tt == kTt8x4)
                    vc1dsp.vLoopFilter8(dst + 8 * linesize, 2 * linesize, pq);
                if (!(flags & kBottomEdge))
                    vc1dsp.vLoopFilter8(dst + 16 * linesize, 2 * linesize, pq);
            } else {
                if (tt == kTt4x4 || tt == kTt8x4)
                    vc1dsp.vLoopFilter8(dst + linesize, 2 * linesize, pq);
                if (!(flags & kBottomEdge))
                    vc1dsp.vLoopFilter8(dst + 9 * linesize, 2 * linesize, pq);
            }
        } else {
            if (blockNum < 2) {
                if (!(flags & kTopEdge) && (tt == kTt4x4 || tt == kTt8x4)) {
                    vc1dsp.vLoopFilter8(dst + 4 * linesize, 2 * linesize, pq);
                    vc1dsp.vLoopFilter8(dst + 5 * linesize, 2 * linesize, pq);
                }
                vc1dsp.vLoopFilter8(dst + 8 * linesize, 2 * linesize, pq);
                vc1dsp.vLoopFilter8(dst + 9 * linesize, 2 * linesize, pq);
            } else if (!(flags & kBottomEdge)) {
                if (tt == kTt4x4 || tt == kTt8x4) {
                    vc1dsp.vLoopFilter8(dst + 4 * linesize, 2 * linesize, pq);
                    vc1dsp.vLoopFilter8(dst + 5 * linesize, 2 * linesize, pq);
                }
                vc1dsp.vLoopFilter8(dst + 8 * linesize, 2 * linesize, pq);
                vc1dsp.vLoopFilter8(dst + 9 * linesize, 2 * linesize, pq);
            }
        }
    } else {
        if (!(flags & kBottomEdge)) {
            if (!(flags & kTopEdge) && (tt == kTt4x4 || tt == kTt8x4)) {
                vc1dsp.vLoopFilter8(dst + 4 * linesize, 2 * linesize, pq);
                vc1dsp.vLoopFilter8(dst + 5 * linesize, 2 * linesize, pq);
            }
                vc1dsp.vLoopFilter8(dst + 8 * linesize, 2 * linesize, pq);
                vc1dsp.vLoopFilter8(dst + 9 * linesize, 2 * linesize, pq);
        }
    }
}
void Vc1::pIntfrLoopFilter()
{
    int blockCount = false ? 4 : 6;
    int mbPos = mbX + mbY * mbStride;
    uint8_t *dest;
    int *ttblk;
    uint32_t flags;
    uint8_t fieldtx;
    int i;
    if (mbX) {
        if (mbY >= startMbY + 1) {
            dest = this->dest[0] - 16 * linesize - 16;
            ttblk = &this->ttblk[mbX - mbStride - 1];
            flags = mbY == startMbY + 1 ? kTopEdge : 0;
            fieldtx = fieldtxPlane[mbPos - mbStride - 1];
            for (i = 0; i < blockCount; i++)
                pVIntfrLoopFilter(i > 3 ? this->dest[i - 3] - 8 * uvlinesize - 8 : dest,
                                          ttblk,
                                          flags,
                                          fieldtx,
                                          i);
        }
    }
    if (mbX == mbWidth - 1) {
        if (mbY >= startMbY + 1) {
            dest = this->dest[0] - 16 * linesize;
            ttblk = &this->ttblk[mbX - mbStride];
            flags = mbY == startMbY + 1 ? kTopEdge : 0;
            fieldtx = fieldtxPlane[mbPos - mbStride];
            for (i = 0; i < blockCount; i++)
                pVIntfrLoopFilter(i > 3 ? this->dest[i - 3] - 8 * uvlinesize : dest,
                                          ttblk,
                                          flags,
                                          fieldtx,
                                          i);
        }
    }
    if (mbY == endMbY - 1) {
        if (mbX) {
            dest = this->dest[0] - 16;
            ttblk = &this->ttblk[mbX - 1];
            flags = mbY == startMbY ? kTopEdge | kBottomEdge : kBottomEdge;
            fieldtx = fieldtxPlane[mbPos - 1];
            for (i = 0; i < blockCount; i++)
                pVIntfrLoopFilter(i > 3 ? this->dest[i - 3] - 8 : dest,
                                          ttblk,
                                          flags,
                                          fieldtx,
                                          i);
        }
        if (mbX == mbWidth - 1) {
            dest = this->dest[0];
            ttblk = &this->ttblk[mbX];
            flags = mbY == startMbY ? kTopEdge | kBottomEdge : kBottomEdge;
            fieldtx = fieldtxPlane[mbPos];
            for (i = 0; i < blockCount; i++)
                pVIntfrLoopFilter(i > 3 ? this->dest[i - 3] : dest,
                                          ttblk,
                                          flags,
                                          fieldtx,
                                          i);
        }
    }
    if (mbY >= startMbY + 2) {
        if (mbX >= 2) {
            dest = this->dest[0] - 32 * linesize - 32;
            ttblk = &this->ttblk[mbX - 2 * mbStride - 2];
            flags = mbX == 2 ? kLeftEdge : 0;
            fieldtx = fieldtxPlane[mbPos - 2 * mbStride - 2];
            for (i = 0; i < blockCount; i++)
                pHIntfrLoopFilter(i > 3 ? this->dest[i - 3] - 16 * uvlinesize - 16 : dest,
                                          ttblk,
                                          flags,
                                          fieldtx,
                                          i);
        }
        if (mbX == mbWidth - 1) {
            if (mbX >= 1) {
                dest = this->dest[0] - 32 * linesize - 16;
                ttblk = &this->ttblk[mbX - 2 * mbStride - 1];
                flags = mbX == 1 ? kLeftEdge : 0;
                fieldtx = fieldtxPlane[mbPos - 2 * mbStride - 1];
                for (i = 0; i < blockCount; i++)
                    pHIntfrLoopFilter(i > 3 ? this->dest[i - 3] - 16 * uvlinesize - 8 : dest,
                                              ttblk,
                                              flags,
                                              fieldtx,
                                              i);
            }
            dest = this->dest[0] - 32 * linesize;
            ttblk = &this->ttblk[mbX - 2 * mbStride];
            flags = mbX ? kRightEdge : kLeftEdge | kRightEdge;
            fieldtx = fieldtxPlane[mbPos - 2 * mbStride];
            for (i = 0; i < blockCount; i++)
                pHIntfrLoopFilter(i > 3 ? this->dest[i - 3] - 16 * uvlinesize : dest,
                                          ttblk,
                                          flags,
                                          fieldtx,
                                          i);
        }
    }
    if (mbY == endMbY - 1) {
        if (mbY >= startMbY + 1) {
            if (mbX >= 2) {
                dest = this->dest[0] - 16 * linesize - 32;
                ttblk = &this->ttblk[mbX - mbStride - 2];
                flags = mbX == 2 ? kLeftEdge : 0;
                fieldtx = fieldtxPlane[mbPos - mbStride - 2];
                for (i = 0; i < blockCount; i++)
                    pHIntfrLoopFilter(i > 3 ? this->dest[i - 3] - 8 * uvlinesize - 16 : dest,
                                              ttblk,
                                              flags,
                                              fieldtx,
                                              i);
            }
            if (mbX == mbWidth - 1) {
                if (mbX >= 1) {
                    dest = this->dest[0] - 16 * linesize - 16;
                    ttblk = &this->ttblk[mbX - mbStride - 1];
                    flags = mbX == 1 ? kLeftEdge : 0;
                    fieldtx = fieldtxPlane[mbPos - mbStride - 1];
                    for (i = 0; i < blockCount; i++)
                        pHIntfrLoopFilter(i > 3 ? this->dest[i - 3] - 8 * uvlinesize - 8 : dest,
                                                  ttblk,
                                                  flags,
                                                  fieldtx,
                                                  i);
                }
                dest = this->dest[0] - 16 * linesize;
                ttblk = &this->ttblk[mbX - mbStride];
                flags = mbX ? kRightEdge : kLeftEdge | kRightEdge;
                fieldtx = fieldtxPlane[mbPos - mbStride];
                for (i = 0; i < blockCount; i++)
                    pHIntfrLoopFilter(i > 3 ? this->dest[i - 3] - 8 * uvlinesize : dest,
                                              ttblk,
                                              flags,
                                              fieldtx,
                                              i);
            }
        }
        if (mbX >= 2) {
            dest = this->dest[0] - 32;
            ttblk = &this->ttblk[mbX - 2];
            flags = mbX == 2 ? kLeftEdge : 0;
            fieldtx = fieldtxPlane[mbPos - 2];
            for (i = 0; i < blockCount; i++)
                pHIntfrLoopFilter(i > 3 ? this->dest[i - 3] - 16 : dest,
                                          ttblk,
                                          flags,
                                          fieldtx,
                                          i);
        }
        if (mbX == mbWidth - 1) {
            if (mbX >= 1) {
                dest = this->dest[0] - 16;
                ttblk = &this->ttblk[mbX - 1];
                flags = mbX == 1 ? kLeftEdge : 0;
                fieldtx = fieldtxPlane[mbPos - 1];
                for (i = 0; i < blockCount; i++)
                    pHIntfrLoopFilter(i > 3 ? this->dest[i - 3] - 8 : dest,
                                              ttblk,
                                              flags,
                                              fieldtx,
                                              i);
            }
            dest = this->dest[0];
            ttblk = &this->ttblk[mbX];
            flags = mbX ? kRightEdge : kLeftEdge | kRightEdge;
            fieldtx = fieldtxPlane[mbPos];
            for (i = 0; i < blockCount; i++)
                pHIntfrLoopFilter(i > 3 ? this->dest[i - 3] : dest,
                                          ttblk,
                                          flags,
                                          fieldtx,
                                          i);
        }
    }
}
void Vc1::bHIntfiLoopFilter(uint8_t *dest, const uint32_t *cbp,
                               const int *ttblk, uint32_t flags, int blockNum)
{
    int pq = this->pq;
    uint8_t *dst;
    uint32_t blockCbp = cbp[0] >> (blockNum * 4);
    int tt;
    int idx, linesize  = blockNum > 3 ? uvlinesize : this->linesize;
    if (blockNum > 3)
        dst = dest;
    else
        dst = dest + (blockNum & 2) * 4 * this->linesize + (blockNum & 1) * 8;
    if (!(flags & kRightEdge) || !(blockNum & 5)) {
        vc1dsp.hLoopFilter8(dst + 8, linesize, pq);
    }
    tt = ttblk[0] >> (blockNum * 4) & 0xf;
    if (tt == kTt4x4 || tt == kTt4x8) {
        idx = (blockCbp | (blockCbp >> 1)) & 5;
        if (idx & 1)
            vc1dsp.hLoopFilter4(dst + 4 * linesize + 4, linesize, pq);
        if (idx & 4)
            vc1dsp.hLoopFilter4(dst + 4, linesize, pq);
    }
}
void Vc1::bVIntfiLoopFilter(uint8_t *dest, const uint32_t *cbp,
                               const int *ttblk, uint32_t flags, int blockNum)
{
    int pq = this->pq;
    uint8_t *dst;
    uint32_t blockCbp = cbp[0] >> (blockNum * 4);
    int tt;
    int idx, linesize  = blockNum > 3 ? uvlinesize : this->linesize;
    if (blockNum > 3)
        dst = dest;
    else
        dst = dest + (blockNum & 2) * 4 * this->linesize + (blockNum & 1) * 8;
    if(!(flags & kBottomEdge) || blockNum < 2)
        vc1dsp.vLoopFilter8(dst + 8 * linesize, linesize, pq);
    tt = ttblk[0] >> (blockNum * 4) & 0xf;
    if (tt == kTt4x4 || tt == kTt8x4) {
        idx = (blockCbp | (blockCbp >> 2)) & 3;
        if (idx & 1)
            vc1dsp.vLoopFilter4(dst + 4 * linesize + 4, linesize, pq);
        if (idx & 2)
            vc1dsp.vLoopFilter4(dst + 4 * linesize, linesize, pq);
    }
}
void Vc1::bIntfiLoopFilter()
{
    int blockCount = false ? 4 : 6;
    uint8_t *dest;
    const uint32_t *cbp;
    int *ttblk;
    uint32_t flags = 0;
    int i;
    if (!firstSliceLine) {
        dest = this->dest[0] - 16 * linesize;
        cbp = &this->cbp[mbX - mbStride];
        ttblk = &this->ttblk[mbX - mbStride];
        flags = mbY == startMbY + 1 ? kTopEdge : 0;
        for (i = 0; i < blockCount; i++)
            bVIntfiLoopFilter(i > 3 ? this->dest[i - 3] - 8 * uvlinesize : dest, cbp, ttblk, flags, i);
    }
    if (mbY == endMbY - 1) {
        dest = this->dest[0];
        cbp = &this->cbp[mbX];
        ttblk = &this->ttblk[mbX];
        flags = firstSliceLine ? kTopEdge | kBottomEdge : kBottomEdge;
        for (i = 0; i < blockCount; i++)
            bVIntfiLoopFilter(i > 3 ? this->dest[i - 3] : dest, cbp, ttblk, flags, i);
    }
    if (!firstSliceLine) {
        dest = this->dest[0] - 16 * linesize - 16;
        cbp = &this->cbp[mbX - mbStride - 1];
        ttblk = &this->ttblk[mbX - mbStride - 1];
        if (mbX) {
            flags = mbX == 1 ? kLeftEdge : 0;
            for (i = 0; i < blockCount; i++)
                bHIntfiLoopFilter(i > 3 ? this->dest[i - 3] - 8 * uvlinesize - 8 : dest, cbp, ttblk, flags, i);
        }
        if (mbX == mbWidth - 1) {
            dest += 16;
            cbp++;
            ttblk++;
            flags = mbX == 0 ? kLeftEdge | kRightEdge : kRightEdge;
            for (i = 0; i < blockCount; i++)
                bHIntfiLoopFilter(i > 3 ? this->dest[i - 3] - 8 * uvlinesize : dest, cbp, ttblk, flags, i);
        }
    }
    if (mbY == endMbY - 1) {
        dest = this->dest[0] - 16;
        cbp = &this->cbp[mbX - 1];
        ttblk = &this->ttblk[mbX - 1];
        if (mbX) {
            flags = mbX == 1 ? kLeftEdge : 0;
            for (i = 0; i < blockCount; i++)
                bHIntfiLoopFilter(i > 3 ? this->dest[i - 3] - 8 : dest, cbp, ttblk, flags, i);
        }
        if (mbX == mbWidth - 1) {
            dest += 16;
            cbp++;
            ttblk++;
            flags = mbX == 0 ? kLeftEdge | kRightEdge : kRightEdge;
            for (i = 0; i < blockCount; i++)
                bHIntfiLoopFilter(i > 3 ? this->dest[i - 3] : dest, cbp, ttblk, flags, i);
        }
    }
}

}

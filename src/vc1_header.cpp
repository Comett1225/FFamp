#include "readers.h"
#include "vc1.h"
#include <cstring>
#include <utility>

namespace absent::vc1 {

const int kTtblkToTt[3][8] = {{kTt8x4, kTt4x8, kTt8x8, kTt4x4, kTt8x4Top, kTt8x4Bottom, kTt4x8Right, kTt4x8Left},
    {kTt8x8, kTt4x8Right, kTt4x8Left, kTt4x4, kTt8x4, kTt4x8, kTt8x4Bottom, kTt8x4Top},
    {kTt8x8, kTt4x8, kTt4x4, kTt8x4Bottom, kTt4x8Right, kTt4x8Left, kTt8x4, kTt8x4Top}};
const int kTtfrmToTt[4] = {kTt8x8, kTt8x4, kTt4x8, kTt4x4};
const uint8_t kMvPmodeTable[2][5] = {{kMvPmode1mvHpelBilin, kMvPmode1mv, kMvPmode1mvHpel, kMvPmodeIntensityComp, kMvPmodeMixedMv},
    {kMvPmode1mv, kMvPmodeMixedMv, kMvPmode1mvHpel, kMvPmodeIntensityComp, kMvPmode1mvHpelBilin}};
const uint8_t kMvPmodeTable2[2][4] = {{kMvPmode1mvHpelBilin, kMvPmode1mv, kMvPmode1mvHpel, kMvPmodeMixedMv},
    {kMvPmode1mv, kMvPmodeMixedMv, kMvPmode1mvHpel, kMvPmode1mvHpelBilin}};
const uint8_t kMbmodeIntfrp[2][15][4] = {
    {{kMvPmodeIntfr1mv, 0, 1, 1}, {kMvPmodeIntfr1mv, 1, 1, 1}, {kMvPmodeIntfr1mv, 0, 1, 0}, {kMvPmodeIntfr1mv, 0, 0, 1}, {kMvPmodeIntfr1mv, 1, 0, 1},
        {kMvPmodeIntfr2mvField, 0, 0, 1}, {kMvPmodeIntfr2mvField, 1, 0, 1}, {kMvPmodeIntfr2mvField, 1, 0, 0}, {kMvPmodeIntfrIntra, 0, 0, 0}},
    {{kMvPmodeIntfr1mv, 0, 1, 1}, {kMvPmodeIntfr1mv, 1, 1, 1}, {kMvPmodeIntfr1mv, 0, 1, 0}, {kMvPmodeIntfr1mv, 0, 0, 1}, {kMvPmodeIntfr1mv, 1, 0, 1},
        {kMvPmodeIntfr2mvField, 0, 0, 1}, {kMvPmodeIntfr2mvField, 1, 0, 1}, {kMvPmodeIntfr2mvField, 1, 0, 0}, {kMvPmodeIntfr4mv, 0, 0, 1},
        {kMvPmodeIntfr4mv, 1, 0, 1}, {kMvPmodeIntfr4mv, 0, 0, 0}, {kMvPmodeIntfr4mvField, 0, 0, 1}, {kMvPmodeIntfr4mvField, 1, 0, 1},
        {kMvPmodeIntfr4mvField, 1, 0, 0}, {kMvPmodeIntfrIntra, 0, 0, 0}}};
const int16_t kBfractionLut[23] = {128, 85, 170, 64, 192, 51, 102, 153, 204, 43, 215, 37, 74, 111, 148, 185, 222, 32, 96, 160, 224, -1, 0};

namespace {

template <typename C, typename L>
void made(Vlc& vlc, int bits, int count, const C* codes, size_t codeStep, const L* lengths, size_t lengthStep) {
    std::vector<uint8_t> length(static_cast<size_t>(count));
    std::vector<uint32_t> code(static_cast<size_t>(count));
    for (size_t i = 0; i < size_t(count); i++) {
        length[i] = uint8_t(lengths[i * lengthStep]);
        code[i] = uint32_t(codes[i * codeStep]);
    }
    vlc.build(bits, size_t(count), length.data(), code.data());
}

Vlcs built() {
    Vlcs v;
    made(v.norm2, 3, 4, kNorm2Codes, 1, kNorm2Bits, 1);
    made(v.norm6, 9, 64, kNorm6Codes, 1, kNorm6Bits, 1);
    made(v.imode, 4, 7, kImodeCodes, 1, kImodeBits, 1);
    for (int i = 0; i < 3; i++) {
        made(v.ttmb[i], 9, 16, kTtmbCodes[i], 1, kTtmbBits[i], 1);
        made(v.ttblk[i], 5, 8, kTtblkCodes[i], 1, kTtblkBits[i], 1);
        made(v.subblkpat[i], 6, 15, kSubblkpatCodes[i], 1, kSubblkpatBits[i], 1);
    }
    for (int i = 0; i < 4; i++) {
        made(v.fourMvBlockPattern[i], 6, 16, k4mvBlockPatternCodes[i], 1, k4mvBlockPatternBits[i], 1);
        made(v.cbpcyP[i], 9, 64, kCbpcyPCodes[i], 1, kCbpcyPBits[i], 1);
        made(v.mvDiff[i], 9, 73, kMvDiffCodes[i], 1, kMvDiffBits[i], 1);
        made(v.intfr4mvMbmode[i], 9, 15, kIntfr4mvMbmodeCodes[i], 1, kIntfr4mvMbmodeBits[i], 1);
        made(v.intfrNon4mvMbmode[i], 6, 9, kIntfrNon4mvMbmodeCodes[i], 1, kIntfrNon4mvMbmodeBits[i], 1);
        made(v.oneRefMvdata[i], 9, 72, k1refMvdataCodes[i], 1, k1refMvdataBits[i], 1);
        made(v.twoMvBlockPattern[i], 3, 4, k2mvBlockPatternCodes[i], 1, k2mvBlockPatternBits[i], 1);
    }
    for (int i = 0; i < 8; i++) {
        made(v.ac[i], 9, kAcSizes[i], &kAcTables[i][0][0], 2, &kAcTables[i][0][1], 2);
        made(v.twoRefMvdata[i], 9, 126, k2refMvdataCodes[i], 1, k2refMvdataBits[i], 1);
        made(v.icbpcy[i], 9, 63, kIcbpcyPCodes[i], 1, kIcbpcyPBits[i], 1);
        made(v.ifMmvMbmode[i], 5, 8, kIfMmvMbmodeCodes[i], 1, kIfMmvMbmodeBits[i], 1);
        made(v.if1mvMbmode[i], 5, 6, kIf1mvMbmodeCodes[i], 1, kIf1mvMbmodeBits[i], 1);
    }
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 2; j++) made(v.dc[i][j], 9, 120, &kDcTables[i][j][0][0], 2, &kDcTables[i][j][0][1], 2);
    }
    made(v.mbI, 9, 64, &kMbITable[0][0], 2, &kMbITable[0][1], 2);
    return v;
}

void initLut(int lumscale, int lumshift, uint8_t* luty, uint8_t* lutuv, int chain) {
    int scale, shift;
    if (!lumscale) {
        scale = -64;
        shift = (255 - lumshift * 2) * 64;
        if (lumshift > 31) shift += 128 << 6;
    } else {
        scale = lumscale + 32;
        if (lumshift > 31) shift = (lumshift - 64) * 64;
        else shift = lumshift << 6;
    }
    for (int i = 0; i < 256; i++) {
        const int iy = chain ? luty[i] : i;
        const int iu = chain ? lutuv[i] : i;
        luty[i] = clipU8((scale * iy + shift + 32) >> 6);
        lutuv[i] = clipU8((scale * (iu - 128) + 128 * 64 + 32) >> 6);
    }
}

}

const Vlcs& tables() {
    static const Vlcs v = built();
    return v;
}


static void decodeRowskip(uint8_t* plane, int width, int height, int stride,
                           Bits& gb)
{
    int x, y;
    for (y = 0; y < height; y++) {
        if (!gb.bit())
            memset(plane, 0, width);
        else
            for (x = 0; x < width; x++)
                plane[x] = gb.bit();
        plane += stride;
    }
}
static void decodeColskip(uint8_t* plane, int width, int height, int stride,
                           Bits& gb)
{
    int x, y;
    for (x = 0; x < width; x++) {
        if (!gb.bit())
            for (y = 0; y < height; y++)
                plane[y*stride] = 0;
        else
            for (y = 0; y < height; y++)
                plane[y*stride] = gb.bit();
        plane ++;
    }
}
int Vc1::bitplaneDecoding(uint8_t* data, int *rawFlag)
{
    int imode, x, y, code, offset;
    uint8_t invert, *planep = data;
    int width, height, stride;
    width  = mbWidth;
    height = mbHeight >> fieldMode;
    stride = mbStride;
    invert = gb.bit();
    imode = gb.vlc(vlcs->imode);
    *rawFlag = 0;
    switch (imode) {
    case kImodeRaw:
        *rawFlag = 1;
        return invert;
    case kImodeDiff2:
    case kImodeNorm2:
        if ((height * width) & 1) {
            *planep++ = gb.bit();
            y = offset = 1;
            if (offset == width) {
                offset = 0;
                planep += stride - width;
            }
        }
        else
            y = offset = 0;
        for (; y < height * width; y += 2) {
            code = gb.vlc(vlcs->norm2);
            *planep++ = code & 1;
            offset++;
            if (offset == width) {
                offset  = 0;
                planep += stride - width;
            }
            *planep++ = code >> 1;
            offset++;
            if (offset == width) {
                offset  = 0;
                planep += stride - width;
            }
        }
        break;
    case kImodeDiff6:
    case kImodeNorm6:
        if (!(height % 3) && (width % 3)) {
            for (y = 0; y < height; y += 3) {
                for (x = width & 1; x < width; x += 2) {
                    code = gb.vlc(vlcs->norm6);
                    if (code < 0) {
                        return -1;
                    }
                    planep[x + 0]              = (code >> 0) & 1;
                    planep[x + 1]              = (code >> 1) & 1;
                    planep[x + 0 + stride]     = (code >> 2) & 1;
                    planep[x + 1 + stride]     = (code >> 3) & 1;
                    planep[x + 0 + stride * 2] = (code >> 4) & 1;
                    planep[x + 1 + stride * 2] = (code >> 5) & 1;
                }
                planep += stride * 3;
            }
            if (width & 1)
                decodeColskip(data, 1, height, stride, gb);
        } else {
            planep += (height & 1) * stride;
            for (y = height & 1; y < height; y += 2) {
                for (x = width % 3; x < width; x += 3) {
                    code = gb.vlc(vlcs->norm6);
                    if (code < 0) {
                        return -1;
                    }
                    planep[x + 0]          = (code >> 0) & 1;
                    planep[x + 1]          = (code >> 1) & 1;
                    planep[x + 2]          = (code >> 2) & 1;
                    planep[x + 0 + stride] = (code >> 3) & 1;
                    planep[x + 1 + stride] = (code >> 4) & 1;
                    planep[x + 2 + stride] = (code >> 5) & 1;
                }
                planep += stride * 2;
            }
            x = width % 3;
            if (x)
                decodeColskip(data,             x, height, stride, gb);
            if (height & 1)
                decodeRowskip(data + x, width - x,      1, stride, gb);
        }
        break;
    case kImodeRowskip:
        decodeRowskip(data, width, height, stride, gb);
        break;
    case kImodeColskip:
        decodeColskip(data, width, height, stride, gb);
        break;
    default:
        break;
    }
    if (imode == kImodeDiff2 || imode == kImodeDiff6) {
        planep = data;
        planep[0] ^= invert;
        for (x = 1; x < width; x++)
            planep[x] ^= planep[x-1];
        for (y = 1; y < height; y++) {
            planep += stride;
            planep[0] ^= planep[-stride];
            for (x = 1; x < width; x++) {
                if (planep[x-1] != planep[x-stride]) planep[x] ^= invert;
                else                                 planep[x] ^= planep[x-1];
            }
        }
    } else if (invert) {
        planep = data;
        for (x = 0; x < stride * height; x++)
            planep[x] = !planep[x];
    }
    return (imode << 1) + invert;
}
int Vc1::vopDquantDecoding()
{
    int pqdiff;
    if (dquant != 2) {
        dquantfrm = gb.bit();
        if (!dquantfrm)
            return 0;
        dqprofile = gb.bits(2);
        switch (dqprofile) {
        case kDqprofileSingleEdge:
        case kDqprofileDoubleEdges:
            dqsbedge = gb.bits(2);
            break;
        case kDqprofileAllMbs:
            dqbilevel = gb.bit();
            if (!dqbilevel) {
                halfpq = 0;
                return 0;
            }
            break;
        default:
            break;
        }
    }
    pqdiff = gb.bits(3);
    if (pqdiff == 7)
        altpq = gb.bits(5);
    else
        altpq = pq + pqdiff + 1;
    return 0;
}
int Vc1::decodeSequenceHeader(Bits& gb)
{
    profile = gb.bits(2);
    if (profile == kProfileComplex) {
    }
    if (profile == kProfileAdvanced) {
        zz8x4 = kAdvProgressive8x4Zz;
        zz4x8 = kAdvProgressive4x8Zz;
        return decodeSequenceHeaderAdv(gb);
    } else {
        chromaformat = 1;
        zz8x4 = kWmv2ScantableA;
        zz4x8 = kWmv2ScantableB;
        resY411   = gb.bit();
        resSprite = gb.bit();
        if (resY411) {
            return -1;
        }
    }
    frmrtqPostproc = gb.bits(3);
    bitrtqPostproc = gb.bits(5);
    loopFilter     = gb.bit();
    if (loopFilter == 1 && profile == kProfileSimple) {
    }
    resX8          = gb.bit();
    multires        = gb.bit();
    resFasttx      = gb.bit();
    fastuvmc        = gb.bit();
    if (!profile && !fastuvmc) {
        return -1;
    }
    extendedMv     = gb.bit();
    if (!profile && extendedMv) {
        return -1;
    }
    dquant          = gb.bits(2);
    vstransform     = gb.bit();
    resTranstab    = gb.bit();
    if (resTranstab) {
        return -1;
    }
    overlap         = gb.bit();
    resyncMarker   = gb.bit();
    rangered        = gb.bit();
    if (rangered && profile == kProfileSimple) {
    }
    maxBFrames = gb.bits(3);
    quantizerMode = gb.bits(2);
    finterpflag = gb.bit();
    if (resSprite) {
        return -1;
        resX8 = gb.bit();
        if (gb.bit()) {
            return -1;
        }
        gb.skip(3);
        resRtmFlag = 0;
    } else {
        resRtmFlag = gb.bit();
    }
    if (!resFasttx)
        gb.skip(16);
    return 0;
}
int Vc1::decodeSequenceHeaderAdv(Bits& gb)
{
    resRtmFlag = 1;
    level = gb.bits(3);
    if (level >= 5) {
    }
    chromaformat = gb.bits(2);
    if (chromaformat != 1) {
        return -1;
    }
    frmrtqPostproc       = gb.bits(3);
    bitrtqPostproc       = gb.bits(5);
    postprocflag          = gb.bit();
    maxCodedWidth       = (gb.bits(12) + 1) << 1;
    maxCodedHeight      = (gb.bits(12) + 1) << 1;
    broadcast             = gb.bit();
    interlace             = gb.bit();
    tfcntrflag            = gb.bit();
    finterpflag           = gb.bit();
    gb.skip(1);
    psf = gb.bit();
    maxBFrames = 7;
    if (gb.bit()) {
        int ar = 0;
        gb.skip(28);
        if (gb.bit())
            ar = gb.bits(4);
        if (ar == 15) {
            gb.skip(16);
        }
        if (gb.bit()) {
            if (gb.bit()) {
                gb.skip(16);
            } else {
                gb.skip(12);
            }
        }
        if (gb.bit()) {
            colorPrim    = gb.bits(8);
            transferChar = gb.bits(8);
            matrixCoef   = gb.bits(8);
        }
    }
    hrdParamFlag = gb.bit();
    if (hrdParamFlag) {
        int i;
        hrdNumLeakyBuckets = gb.bits(5);
        gb.skip(4);
        gb.skip(4);
        for (i = 0; i < hrdNumLeakyBuckets; i++) {
            gb.skip(16);
            gb.skip(16);
        }
    }
    return 0;
}
int Vc1::decodeEntryPoint(Bits& gb)
{
    int i;
    int w,h;
    brokenLink    = gb.bit();
    closedEntry   = gb.bit();
    panscanflag    = gb.bit();
    refdistFlag   = gb.bit();
    loopFilter    = gb.bit();
    fastuvmc       = gb.bit();
    extendedMv    = gb.bit();
    dquant         = gb.bits(2);
    vstransform    = gb.bit();
    overlap        = gb.bit();
    quantizerMode = gb.bits(2);
    if (hrdParamFlag) {
        for (i = 0; i < hrdNumLeakyBuckets; i++) {
            gb.skip(8);
        }
    }
    if(gb.bit()){
        w = (gb.bits(12)+1)<<1;
        h = (gb.bits(12)+1)<<1;
    } else {
        w = maxCodedWidth;
        h = maxCodedHeight;
    }
    if (w < 2 || h < 2 || w > 4096 || h > 4096) return -1;
    codedWidth = w;
    codedHeight = h;
    if (extendedMv)
        extendedDmv = gb.bit();
    if ((rangeMapyFlag = gb.bit())) {
        rangeMapy = gb.bits(3);
    }
    if ((rangeMapuvFlag = gb.bit())) {
        rangeMapuv = gb.bits(3);
    }
    return 0;
}
void Vc1::rotateLuts()
{
    if (pictType == kPictureBI || pictType == kPictureB) {
        currUseIc = &auxUseIc;
        currLuty   = auxLuty;
        currLutuv  = auxLutuv;
    } else {
        std::swap(lastUseIc, nextUseIc);
        currUseIc = &nextUseIc;
        std::swap(lastLuty, nextLuty);
        currLuty = nextLuty;
        std::swap(lastLutuv, nextLutuv);
        currLutuv = nextLutuv;
    }
    initLut(32, 0, currLuty[0], currLutuv[0], 0);
    initLut(32, 0, currLuty[1], currLutuv[1], 0);
    *currUseIc = 0;
}
int Vc1::readBfraction(Bits& gb) {
    int bfractionLutIndex = gb.bits(3);
    if (bfractionLutIndex == 7)
        bfractionLutIndex = 7 + gb.bits(4);
    if (bfractionLutIndex == 21) {
        return -1;
    }
    this->bfractionLutIndex = bfractionLutIndex;
    bfraction           = kBfractionLut[this->bfractionLutIndex];
    return 0;
}
int Vc1::parseFrameHeader(Bits& gb)
{
    int pqindex, lowquant, status;
    fieldMode = 0;
    fcm = kProgressive;
    if (finterpflag)
        interpfrm = gb.bit();
    gb.skip(2);
    rangeredfrm = 0;
    if (rangered)
        rangeredfrm = gb.bit();
    if (gb.bit()) {
        pictType = kPictureP;
    } else {
        if (maxBFrames && !gb.bit()) {
            pictType = kPictureB;
        } else
            pictType = kPictureI;
    }
    biType = 0;
    if (pictType == kPictureB) {
        if (readBfraction(gb) < 0)
            return -1;
        if (bfraction == 0) {
            pictType = kPictureBI;
        }
    }
    if (pictType == kPictureI || pictType == kPictureBI)
        gb.skip(7);
    if (pictType == kPictureI || pictType == kPictureBI)
        rnd = 1;
    if (pictType == kPictureP)
        rnd ^= 1;
    if (gb.left() < 5)
        return -1;
    pqindex = gb.bits(5);
    if (!pqindex)
        return -1;
    if (quantizerMode == kQuantFrameImplicit)
        pq = kPquantTable[0][pqindex];
    else
        pq = kPquantTable[1][pqindex];
    this->pqindex = pqindex;
    if (pqindex < 9)
        halfpq = gb.bit();
    else
        halfpq = 0;
    switch (quantizerMode) {
    case kQuantFrameImplicit:
        pquantizer = pqindex < 9;
        break;
    case kQuantNonUniform:
        pquantizer = 0;
        break;
    case kQuantFrameExplicit:
        pquantizer = gb.bit();
        break;
    default:
        pquantizer = 1;
        break;
    }
    dquantfrm = 0;
    if (extendedMv == 1)
        mvrange = gb.unary(0, 3);
    kX = mvrange + 9 + (mvrange >> 1);
    kY = mvrange + 8;
    rangeX = 1 << (kX - 1);
    rangeY = 1 << (kY - 1);
    if (multires && pictType != kPictureB)
        respic = gb.bits(2);
    if (resX8 && (pictType == kPictureI || pictType == kPictureBI)) {
        x8Type = gb.bit();
    } else
        x8Type = 0;
    if (firstPicHeaderFlag)
        rotateLuts();
    switch (pictType) {
    case kPictureP:
        ttIndex = (pq > 4) + (pq > 12);
        lowquant = (pq > 12) ? 0 : 1;
        mvMode = kMvPmodeTable[lowquant][gb.unary(1, 4)];
        if (mvMode == kMvPmodeIntensityComp) {
            mvMode2 = kMvPmodeTable2[lowquant][gb.unary(1, 3)];
            lumscale = gb.bits(6);
            lumshift = gb.bits(6);
            lastUseIc = 1;
            initLut(lumscale, lumshift, lastLuty[0], lastLutuv[0], 1);
            initLut(lumscale, lumshift, lastLuty[1], lastLutuv[1], 1);
        }
        if (mvMode == kMvPmodeIntensityComp) {
            quarterSample = (mvMode2 != kMvPmode1mvHpel &&
                                   mvMode2 != kMvPmode1mvHpelBilin);
            mspel          = (mvMode2 != kMvPmode1mvHpelBilin);
        } else {
            quarterSample = (mvMode != kMvPmode1mvHpel &&
                                   mvMode != kMvPmode1mvHpelBilin);
            mspel          = (mvMode != kMvPmode1mvHpelBilin);
        }
        if ((mvMode  == kMvPmodeIntensityComp &&
             mvMode2 == kMvPmodeMixedMv)      ||
            mvMode   == kMvPmodeMixedMv) {
            status = bitplaneDecoding(mvTypeMbPlane, &mvTypeIsRaw);
            if (status < 0)
                return -1;
        } else {
            mvTypeIsRaw = 0;
            memset(mvTypeMbPlane, 0, mbStride * mbHeight);
        }
        status = bitplaneDecoding(mbskipTable, &skipIsRaw);
        if (status < 0)
            return -1;
        if (gb.left() < 4)
            return -1;
        mvTableIndex = gb.bits(2);
        cbptab = gb.bits(2);
        cbpcyVlc = &vlcs->cbpcyP[cbptab];
        if (dquant) {
            vopDquantDecoding();
        }
        if (vstransform) {
            ttmbf = gb.bit();
            if (ttmbf) {
                ttfrm = kTtfrmToTt[gb.bits(2)];
            } else
                ttfrm = 0;
        } else {
            ttmbf = 1;
            ttfrm = kTt8x8;
        }
        break;
    case kPictureB:
        ttIndex = (pq > 4) + (pq > 12);
        mvMode          = gb.bit() ? kMvPmode1mv : kMvPmode1mvHpelBilin;
        quarterSample = (mvMode == kMvPmode1mv);
        mspel          = quarterSample;
        status = bitplaneDecoding(directMbPlane, &dmbIsRaw);
        if (status < 0)
            return -1;
        status = bitplaneDecoding(mbskipTable, &skipIsRaw);
        if (status < 0)
            return -1;
        mvTableIndex   = gb.bits(2);
        cbptab           = gb.bits(2);
        cbpcyVlc        = &vlcs->cbpcyP[cbptab];
        if (dquant) {
            vopDquantDecoding();
        }
        if (vstransform) {
            ttmbf = gb.bit();
            if (ttmbf) {
                ttfrm = kTtfrmToTt[gb.bits(2)];
            } else
                ttfrm = 0;
        } else {
            ttmbf = 1;
            ttfrm = kTt8x8;
        }
        break;
    }
    if (!x8Type) {
        cAcTableIndex = gb.decode012();
        if (pictType == kPictureI || pictType == kPictureBI) {
            yAcTableIndex = gb.decode012();
        }
        dcTableIndex = gb.bit();
    }
    if (pictType == kPictureBI) {
        pictType = kPictureB;
        biType     = 1;
    }
    return 0;
}
int Vc1::parseFrameHeaderAdv(Bits& gb)
{
    int pqindex, lowquant;
    int status;
    int fieldMode, fcm;
    numref          = 0;
    pFrameSkipped = 0;
    if (secondField) {
        if (this->fcm != kIlaceField || this->fieldMode!=1)
            return -1;
        if (fptype & 4)
            pictType = (fptype & 1) ? kPictureBI : kPictureB;
        else
            pictType = (fptype & 1) ? kPictureP : kPictureI;
        if (!picHeaderFlag)
            goto parseCommonInfo;
    }
    fieldMode = 0;
    if (interlace) {
        fcm = gb.decode012();
        if (fcm) {
            if (fcm == kIlaceField)
                fieldMode = 1;
        }
    } else {
        fcm = kProgressive;
    }
    if (!firstPicHeaderFlag && this->fieldMode != fieldMode)
        return -1;
    this->fieldMode = fieldMode;
    this->fcm = fcm;
    if (this->fieldMode) {
        mbHeight = (((height + 15) >> 4) + 1) & ~1;
        fptype = gb.bits(3);
        if (fptype & 4)
            pictType = (fptype & 2) ? kPictureBI : kPictureB;
        else
            pictType = (fptype & 2) ? kPictureP : kPictureI;
    } else {
        mbHeight = (height + 15) >> 4;
        switch (gb.unary(0, 4)) {
        case 0:
            pictType = kPictureP;
            break;
        case 1:
            pictType = kPictureB;
            break;
        case 2:
            pictType = kPictureI;
            break;
        case 3:
            pictType = kPictureBI;
            break;
        case 4:
            pictType = kPictureP;
            pFrameSkipped = 1;
            break;
        }
    }
    if (tfcntrflag)
        gb.skip(8);
    if (broadcast) {
        if (!interlace || psf) {
            rptfrm = gb.bits(2);
        } else {
            tff = gb.bit();
            rff = gb.bit();
        }
    } else {
        tff = 1;
    }
    if (panscanflag && gb.bit()) {
        const int windows = interlace && !psf ? 2 + (broadcast ? rff : 0) : 1 + (broadcast ? rptfrm : 0);
        gb.skip(windows * 64);
    }
    if (pFrameSkipped) {
        return 0;
    }
    rnd = gb.bit();
    if (interlace)
        uvsamp = gb.bit();
    if (this->fieldMode) {
        if (!refdistFlag)
            refdist = 0;
        else if ((pictType != kPictureB) && (pictType != kPictureBI)) {
            refdist = gb.bits(2);
            if (refdist == 3)
                refdist += gb.unary(0, 14);
            if (refdist > 16)
                return -1;
        }
        if ((pictType == kPictureB) || (pictType == kPictureBI)) {
            if (readBfraction(gb) < 0)
                return -1;
            frfd = (bfraction * refdist) >> 8;
            brfd = refdist - frfd - 1;
            if (brfd < 0)
                brfd = 0;
        }
        goto parseCommonInfo;
    }
    if (this->fcm == kProgressive) {
        if (finterpflag)
            interpfrm = gb.bit();
        if (pictType == kPictureB) {
            if (readBfraction(gb) < 0)
                return -1;
            if (bfraction == 0) {
                pictType = kPictureBI;
            }
        }
    }
    parseCommonInfo:
    if (this->fieldMode)
        curFieldType = !(tff ^ secondField);
    pqindex = gb.bits(5);
    if (!pqindex)
        return -1;
    if (quantizerMode == kQuantFrameImplicit)
        pq = kPquantTable[0][pqindex];
    else
        pq = kPquantTable[1][pqindex];
    this->pqindex = pqindex;
    if (pqindex < 9)
        halfpq = gb.bit();
    else
        halfpq = 0;
    switch (quantizerMode) {
    case kQuantFrameImplicit:
        pquantizer = pqindex < 9;
        break;
    case kQuantNonUniform:
        pquantizer = 0;
        break;
    case kQuantFrameExplicit:
        pquantizer = gb.bit();
        break;
    default:
        pquantizer = 1;
        break;
    }
    dquantfrm = 0;
    if (postprocflag)
        postproc = gb.bits(2);
    if (firstPicHeaderFlag)
        rotateLuts();
    switch (pictType) {
    case kPictureI:
    case kPictureBI:
        if (this->fcm == kIlaceFrame) {
            status = bitplaneDecoding(fieldtxPlane, &fieldtxIsRaw);
            if (status < 0)
                return -1;
        } else
            fieldtxIsRaw = 0;
        status = bitplaneDecoding(acpredPlane, &acpredIsRaw);
        if (status < 0)
            return -1;
        condover = kCondoverNone;
        if (overlap && pq <= 8) {
            condover = gb.decode012();
            if (condover == kCondoverSelect) {
                status = bitplaneDecoding(overFlagsPlane, &overflgIsRaw);
                if (status < 0)
                    return -1;
            }
        }
        break;
    case kPictureP:
        if (this->fieldMode) {
            numref = gb.bit();
            if (!numref) {
                reffield          = gb.bit();
                refFieldType[0] = reffield ^ !curFieldType;
            }
        }
        if (extendedMv)
            mvrange = gb.unary(0, 3);
        else
            mvrange = 0;
        if (interlace) {
            if (extendedDmv)
                dmvrange = gb.unary(0, 3);
            else
                dmvrange = 0;
            if (this->fcm == kIlaceFrame) {
                fourmvswitch = gb.bit();
                intcomp      = gb.bit();
                if (intcomp) {
                    lumscale = gb.bits(6);
                    lumshift = gb.bits(6);
                    initLut(lumscale, lumshift, lastLuty[0], lastLutuv[0], 1);
                    initLut(lumscale, lumshift, lastLuty[1], lastLutuv[1], 1);
                    lastUseIc = 1;
                }
                status = bitplaneDecoding(mbskipTable, &skipIsRaw);
                if (status < 0)
                    return -1;
                mbmodetab = gb.bits(2);
                if (fourmvswitch)
                    mbmodeVlc = &vlcs->intfr4mvMbmode[mbmodetab];
                else
                    mbmodeVlc = &vlcs->intfrNon4mvMbmode[mbmodetab];
                imvtab      = gb.bits(2);
                imvVlc     = &vlcs->oneRefMvdata[imvtab];
                icbptab     = gb.bits(3);
                cbpcyVlc   = &vlcs->icbpcy[icbptab];
                twomvbptab     = gb.bits(2);
                twomvbpVlc = &vlcs->twoMvBlockPattern[twomvbptab];
                if (fourmvswitch) {
                    fourmvbptab     = gb.bits(2);
                    fourmvbpVlc = &vlcs->fourMvBlockPattern[fourmvbptab];
                }
            }
        }
        kX = mvrange + 9 + (mvrange >> 1);
        kY = mvrange + 8;
        rangeX = 1 << (kX - 1);
        rangeY = 1 << (kY - 1);
        ttIndex = (pq > 4) + (pq > 12);
        if (this->fcm != kIlaceFrame) {
            int mvmode;
            mvmode     = gb.unary(1, 4);
            lowquant   = (pq > 12) ? 0 : 1;
            mvMode = kMvPmodeTable[lowquant][mvmode];
            if (mvMode == kMvPmodeIntensityComp) {
                int mvmode2;
                mvmode2 = gb.unary(1, 3);
                mvMode2 = kMvPmodeTable2[lowquant][mvmode2];
                if (this->fieldMode) {
                    intcompfield = gb.decode210() ^ 3;
                } else
                    intcompfield = 3;
                lumscale2 = lumscale = 32;
                lumshift2 = lumshift =  0;
                if (intcompfield & 1) {
                    lumscale = gb.bits(6);
                    lumshift = gb.bits(6);
                }
                if ((intcompfield & 2) && this->fieldMode) {
                    lumscale2 = gb.bits(6);
                    lumshift2 = gb.bits(6);
                } else if(!this->fieldMode) {
                    lumscale2 = lumscale;
                    lumshift2 = lumshift;
                }
                if (this->fieldMode && secondField) {
                    if (curFieldType) {
                        initLut(lumscale , lumshift , currLuty[curFieldType^1], currLutuv[curFieldType^1], 0);
                        initLut(lumscale2, lumshift2, lastLuty[curFieldType  ], lastLutuv[curFieldType  ], 1);
                    } else {
                        initLut(lumscale2, lumshift2, currLuty[curFieldType^1], currLutuv[curFieldType^1], 0);
                        initLut(lumscale , lumshift , lastLuty[curFieldType  ], lastLutuv[curFieldType  ], 1);
                    }
                    nextUseIc = *currUseIc = 1;
                } else {
                    initLut(lumscale , lumshift , lastLuty[0], lastLutuv[0], 1);
                    initLut(lumscale2, lumshift2, lastLuty[1], lastLutuv[1], 1);
                }
                lastUseIc = 1;
            }
            if (mvMode == kMvPmodeIntensityComp) {
                quarterSample = (mvMode2 != kMvPmode1mvHpel &&
                                       mvMode2 != kMvPmode1mvHpelBilin);
                mspel          = (mvMode2 != kMvPmode1mvHpelBilin);
            } else {
                quarterSample = (mvMode != kMvPmode1mvHpel &&
                                       mvMode != kMvPmode1mvHpelBilin);
                mspel          = (mvMode != kMvPmode1mvHpelBilin);
            }
        }
        if (this->fcm == kProgressive) {
            if ((mvMode == kMvPmodeIntensityComp &&
                 mvMode2 == kMvPmodeMixedMv)
                || mvMode == kMvPmodeMixedMv) {
                status = bitplaneDecoding(mvTypeMbPlane, &mvTypeIsRaw);
                if (status < 0)
                    return -1;
            } else {
                mvTypeIsRaw = 0;
                memset(mvTypeMbPlane, 0, mbStride * mbHeight);
            }
            status = bitplaneDecoding(mbskipTable, &skipIsRaw);
            if (status < 0)
                return -1;
            mvTableIndex   = gb.bits(2);
            cbptab           = gb.bits(2);
            cbpcyVlc        = &vlcs->cbpcyP[cbptab];
        } else if (this->fcm == kIlaceFrame) {
            quarterSample = 1;
            mspel          = 1;
        } else {
            mbmodetab = gb.bits(3);
            imvtab = gb.bits(2 + numref);
            if (!numref)
                imvVlc = &vlcs->oneRefMvdata[imvtab];
            else
                imvVlc = &vlcs->twoRefMvdata[imvtab];
            icbptab = gb.bits(3);
            cbpcyVlc = &vlcs->icbpcy[icbptab];
            if ((mvMode == kMvPmodeIntensityComp &&
                mvMode2 == kMvPmodeMixedMv) || mvMode == kMvPmodeMixedMv) {
                fourmvbptab     = gb.bits(2);
                fourmvbpVlc = &vlcs->fourMvBlockPattern[fourmvbptab];
                mbmodeVlc   = &vlcs->ifMmvMbmode[mbmodetab];
            } else {
                mbmodeVlc   = &vlcs->if1mvMbmode[mbmodetab];
            }
        }
        if (dquant) {
            vopDquantDecoding();
        }
        if (vstransform) {
            ttmbf = gb.bit();
            if (ttmbf) {
                ttfrm = kTtfrmToTt[gb.bits(2)];
            } else
                ttfrm = 0;
        } else {
            ttmbf = 1;
            ttfrm = kTt8x8;
        }
        break;
    case kPictureB:
        if (this->fcm == kIlaceFrame) {
            if (readBfraction(gb) < 0)
                return -1;
            if (bfraction == 0) {
                return -1;
            }
        }
        if (extendedMv)
            mvrange = gb.unary(0, 3);
        else
            mvrange = 0;
        kX     = mvrange + 9 + (mvrange >> 1);
        kY     = mvrange + 8;
        rangeX = 1 << (kX - 1);
        rangeY = 1 << (kY - 1);
        ttIndex = (pq > 4) + (pq > 12);
        if (this->fieldMode) {
            int mvmode;
            if (extendedDmv)
                dmvrange = gb.unary(0, 3);
            mvmode = gb.unary(1, 3);
            lowquant = (pq > 12) ? 0 : 1;
            mvMode          = kMvPmodeTable2[lowquant][mvmode];
            quarterSample = (mvMode == kMvPmode1mv || mvMode == kMvPmodeMixedMv);
            mspel          = (mvMode != kMvPmode1mvHpelBilin);
            status = bitplaneDecoding(forwardMbPlane, &fmbIsRaw);
            if (status < 0)
                return -1;
            mbmodetab = gb.bits(3);
            if (mvMode == kMvPmodeMixedMv)
                mbmodeVlc = &vlcs->ifMmvMbmode[mbmodetab];
            else
                mbmodeVlc = &vlcs->if1mvMbmode[mbmodetab];
            imvtab     = gb.bits(3);
            imvVlc   = &vlcs->twoRefMvdata[imvtab];
            icbptab   = gb.bits(3);
            cbpcyVlc = &vlcs->icbpcy[icbptab];
            if (mvMode == kMvPmodeMixedMv) {
                fourmvbptab     = gb.bits(2);
                fourmvbpVlc = &vlcs->fourMvBlockPattern[fourmvbptab];
            }
            numref = 1;
        } else if (this->fcm == kIlaceFrame) {
            if (extendedDmv)
                dmvrange = gb.unary(0, 3);
            if (gb.bit())
                {}
            intcomp          = 0;
            mvMode          = kMvPmode1mv;
            fourmvswitch     = 0;
            quarterSample = 1;
            mspel          = 1;
            status              = bitplaneDecoding(directMbPlane, &dmbIsRaw);
            if (status < 0)
                return -1;
            status = bitplaneDecoding(mbskipTable, &skipIsRaw);
            if (status < 0)
                return -1;
            mbmodetab       = gb.bits(2);
            mbmodeVlc   = &vlcs->intfrNon4mvMbmode[mbmodetab];
            imvtab       = gb.bits(2);
            imvVlc      = &vlcs->oneRefMvdata[imvtab];
            icbptab      = gb.bits(3);
            cbpcyVlc    = &vlcs->icbpcy[icbptab];
            twomvbptab      = gb.bits(2);
            twomvbpVlc  = &vlcs->twoMvBlockPattern[twomvbptab];
            fourmvbptab     = gb.bits(2);
            fourmvbpVlc = &vlcs->fourMvBlockPattern[fourmvbptab];
        } else {
            mvMode          = gb.bit() ? kMvPmode1mv : kMvPmode1mvHpelBilin;
            quarterSample = (mvMode == kMvPmode1mv);
            mspel          = quarterSample;
            status              = bitplaneDecoding(directMbPlane, &dmbIsRaw);
            if (status < 0)
                return -1;
            status = bitplaneDecoding(mbskipTable, &skipIsRaw);
            if (status < 0)
                return -1;
            mvTableIndex = gb.bits(2);
            cbptab = gb.bits(2);
            cbpcyVlc = &vlcs->cbpcyP[cbptab];
        }
        if (dquant) {
            vopDquantDecoding();
        }
        if (vstransform) {
            ttmbf = gb.bit();
            if (ttmbf) {
                ttfrm = kTtfrmToTt[gb.bits(2)];
            } else
                ttfrm = 0;
        } else {
            ttmbf = 1;
            ttfrm = kTt8x8;
        }
        break;
    }
    cAcTableIndex = gb.decode012();
    if (pictType == kPictureI || pictType == kPictureBI) {
        yAcTableIndex = gb.decode012();
    }
    else if (this->fcm != kProgressive && !quarterSample) {
        rangeX <<= 1;
        rangeY <<= 1;
    }
    dcTableIndex = gb.bit();
    if ((pictType == kPictureI || pictType == kPictureBI)
        && dquant) {
        vopDquantDecoding();
    }
    biType = (pictType == kPictureBI);
    if (biType)
        pictType = kPictureB;
    return 0;
}

int pictureType(uint32_t codec, const std::vector<uint8_t>& config, const uint8_t* sample, size_t size) {
    if (config.size() < 40 || !size) return -1;
    Vc1 v;
    std::vector<uint8_t> buffer(std::max(config.size(), size) + Bits::kPadding, 0);
    Bits bits;
    if (codec == fourcc("WMV3")) {
        memcpy(buffer.data(), config.data() + 40, config.size() - 40);
        bits.reset(buffer.data(), config.size() - 40);
        if (v.decodeSequenceHeader(bits) < 0 || v.profile == kProfileAdvanced) return -1;
        std::fill(buffer.begin(), buffer.end(), uint8_t(0));
        memcpy(buffer.data(), sample, size);
        bits.reset(buffer.data(), size);
        if (v.finterpflag) bits.skip(1);
        bits.skip(2);
        if (v.rangered) bits.skip(1);
        if (bits.bit()) return 1;
        return v.maxBFrames && !bits.bit() ? 2 : 0;
    }
    const uint8_t* end = config.data() + config.size();
    for (const uint8_t* at = findNextMarker(config.data() + 40, end); at < end;) {
        const uint8_t* next = findNextMarker(at + 4, end);
        if (readBe32(at) == kCodeSequence && next - at > 4) {
            std::fill(buffer.begin(), buffer.end(), uint8_t(0));
            const int length = unescapeBuffer(at + 4, int(next - at - 4), buffer.data());
            bits.reset(buffer.data(), size_t(length));
            if (v.decodeSequenceHeader(bits) < 0) return -1;
        }
        at = next;
    }
    if (v.profile != kProfileAdvanced) return -1;
    const uint8_t* picture = sample;
    size_t left = size;
    if (size >= 4 && (readBe32(sample) & ~0xFFu) == 0x100) {
        const uint8_t* frame = sample;
        while (frame < sample + size && readBe32(frame) != kCodeFrame) {
            frame = findNextMarker(frame + 4, sample + size);
            if (frame + 4 > sample + size) return -1;
        }
        picture = frame + 4;
        left = size_t(sample + size - picture);
    }
    std::fill(buffer.begin(), buffer.end(), uint8_t(0));
    bits.reset(buffer.data(), size_t(unescapeBuffer(picture, int(std::min<size_t>(left, 64)), buffer.data())));
    if (v.interlace && bits.decode012() == kIlaceField) {
        const int type = int(bits.bits(3));
        return type <= 1 ? 0 : type < 4 ? 1 : 2;
    }
    switch (bits.unary(0, 4)) {
    case 1:
    case 3: return 2;
    case 2: return 0;
    default: return 1;
    }
}

}

namespace absent {

int vc1PictureType(uint32_t codec, const std::vector<uint8_t>& config, const uint8_t* sample, size_t size) {
    return vc1::pictureType(codec, config, sample, size);
}

}

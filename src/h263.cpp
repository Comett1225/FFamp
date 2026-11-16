#include "h263.h"
#include <algorithm>
#include <cstring>

namespace absent::h263 {

namespace {

template <typename T>
void pairs(Vlc& vlc, int bits, size_t count, const T (*table)[2], const int32_t* symbols = nullptr) {
    std::vector<uint8_t> lengths(count);
    std::vector<uint32_t> codes(count);
    for (size_t i = 0; i < count; i++) {
        codes[i] = uint32_t(table[i][0]);
        lengths[i] = uint8_t(table[i][1]);
    }
    vlc.build(bits, count, lengths.data(), codes.data(), symbols);
}

}

const uint8_t kIdentityQscale[32] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};

void buildRl(RlTable& rl, int n, int last, const uint16_t (*codes)[2], const int8_t* run, const int8_t* level) {
    rl.n = n;
    rl.last = last;
    rl.run = run;
    rl.level = level;
    std::vector<int32_t> symbols(size_t(n + 1), 66 << 8);
    for (int i = 0; i < n; i++) symbols[size_t(i)] = (run[i] + 1 + (i >= last ? 192 : 0)) << 8 | level[i];
    pairs(rl.vlc, 9, size_t(n + 1), codes, symbols.data());
    for (int l = 0; l < 2; l++) {
        for (int i = l ? last : 0; i < (l ? n : last); i++) {
            rl.maxLevel[l][run[i]] = std::max(rl.maxLevel[l][run[i]], level[i]);
            rl.maxRun[l][level[i]] = std::max(rl.maxRun[l][level[i]], run[i]);
        }
    }
}

namespace {

void v2DcTable(Vlc& vlc, const uint8_t (*tab)[2]) {
    std::vector<uint8_t> lengths(512);
    std::vector<uint32_t> codes(512);
    for (int level = -256; level < 256; level++) {
        int size = 0;
        for (int v = std::abs(level); v; v >>= 1) size++;
        const int l = level < 0 ? (-level) ^ ((1 << size) - 1) : level;
        int code = tab[size][0] ^ ((1 << tab[size][1]) - 1);
        int length = tab[size][1];
        if (size > 0) {
            code = (code << size) | l;
            length += size;
            if (size > 8) {
                code = (code << 1) | 1;
                length++;
            }
        }
        codes[size_t(level + 256)] = uint32_t(code);
        lengths[size_t(level + 256)] = uint8_t(length);
    }
    vlc.build(9, 512, lengths.data(), codes.data());
}

void hLoopFilter(uint8_t* src, ptrdiff_t stride, int qscale, ptrdiff_t step) {
    const int strength = kLoopFilterStrength[qscale];
    for (int y = 0; y < 8; y++, src += stride) {
        int p0 = src[-2 * step], p1 = src[-step], p2 = src[0], p3 = src[step];
        const int d = (p0 - p3 + 4 * (p2 - p1)) / 8;
        int d1;
        if (d < -2 * strength) d1 = 0;
        else if (d < -strength) d1 = -2 * strength - d;
        else if (d < strength) d1 = d;
        else if (d < 2 * strength) d1 = 2 * strength - d;
        else d1 = 0;
        p1 += d1;
        p2 -= d1;
        if (p1 & 256) p1 = ~(p1 >> 31);
        if (p2 & 256) p2 = ~(p2 >> 31);
        src[-step] = uint8_t(p1);
        src[0] = uint8_t(p2);
        const int ad1 = std::abs(d1) >> 1;
        const int d2 = std::clamp((p0 - p3) / 4, -ad1, ad1);
        src[-2 * step] = uint8_t(p0 - d2);
        src[step] = uint8_t(p3 + d2);
    }
}

void hFilter(uint8_t* src, ptrdiff_t stride, int qscale) { hLoopFilter(src, stride, qscale, 1); }
void vFilter(uint8_t* src, ptrdiff_t stride, int qscale) { hLoopFilter(src, 1, qscale, stride); }

}

const Tables& tables() {
    static const Tables made = [] {
        Tables t;
        std::vector<uint32_t> codes(kIntraMcbpcCode, kIntraMcbpcCode + 9);
        t.intraMcbpc.build(6, 9, kIntraMcbpcBits, codes.data());
        codes.assign(kInterMcbpcCode, kInterMcbpcCode + 28);
        t.interMcbpc.build(7, 28, kInterMcbpcBits, codes.data());
        pairs(t.cbpy, 6, 16, kCbpyTab);
        pairs(t.mv, 9, 33, kMvTab);
        v2DcTable(t.v2DcLum, kDcTabLum);
        v2DcTable(t.v2DcChroma, kDcTabChrom);
        pairs(t.v2IntraCbpc, 3, 4, kV2IntraCbpc);
        pairs(t.v2MbType, 7, 8, kV2MbType);
        pairs(t.interIntra, 3, 4, kInterIntra);
        t.msMv[0].buildInOrder(9, 1100, kMvTable0Lens, kMvTable0, 0);
        t.msMv[1].buildInOrder(9, 1100, kMvTable1Lens, kMvTable1, 0);
        const uint32_t (*inter[4])[2] = {kMbNonIntra2, kMbNonIntra3, kMbNonIntra4, kMbNonIntra1};
        for (int i = 0; i < 4; i++) pairs(t.mbNonIntra[i], 9, 128, inter[i]);
        buildRl(t.rl[0], 132, 85, kTable0Vlc, kTable0Run, kTable0Level);
        buildRl(t.rl[1], 185, 119, kTable2Vlc, kTable2Run, kTable2Level);
        buildRl(t.rl[2], 102, 67, kMpeg4IntraVlc, kMpeg4IntraRun, kMpeg4IntraLevel);
        buildRl(t.rl[3], 148, 81, kTable1Vlc, kTable1Run, kTable1Level);
        buildRl(t.rl[4], 168, 99, kTable4Vlc, kTable4Run, kTable4Level);
        buildRl(t.rl[5], 102, 58, kInterVlc, kInterRun, kInterLevel);
        return t;
    }();
    return made;
}

bool Mpv::allocate(int w, int h) {
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return false;
    width = w;
    height = h;
    mbWidth = (w + 15) / 16;
    mbHeight = (h + 15) / 16;
    mbStride = mbWidth + 1;
    b8Stride = mbWidth * 2 + 1;
    mbNum = mbWidth * mbHeight;
    hEdgePos = mbWidth * 16;
    vEdgePos = mbHeight * 16;
    blockWrap[0] = blockWrap[1] = blockWrap[2] = blockWrap[3] = b8Stride;
    blockWrap[4] = blockWrap[5] = mbStride;
    const size_t ySize = size_t(b8Stride) * (2 * mbHeight + 1);
    const size_t cSize = size_t(mbStride) * (mbHeight + 1);
    const size_t ycSize = ySize + 2 * cSize;
    dcValBase.assign(ycSize, 1024);
    dcVal = dcValBase.data() + b8Stride + 1;
    acValBase.assign(ycSize * 16, 0);
    acVal = reinterpret_cast<int16_t (*)[16]>(acValBase.data()) + b8Stride + 1;
    codedBlockBase.assign(ySize, 0);
    codedBlock = codedBlockBase.data() + b8Stride + 1;
    mbIntraTable.assign(size_t(mbHeight) * mbStride, 0);
    pool.resize(mbWidth, mbHeight);
    cur.set(nullptr);
    last.set(nullptr);
    next.set(nullptr);
    return true;
}

void Mpv::setScans(const uint8_t* intra, const uint8_t* inter, const uint8_t* horizontal, const uint8_t* vertical) {
    memcpy(intraScan, intra, 64);
    memcpy(interScan, inter, 64);
    memcpy(intraHScan, horizontal, 64);
    memcpy(intraVScan, vertical, 64);
    int end = -1;
    for (int i = 0; i < 64; i++) {
        end = std::max<int>(end, intraScan[i]);
        intraRasterEnd[i] = uint8_t(end);
    }
}

void Mpv::frameStart() {
    cur.set(pool.get());
    last.set(next.ptr);
    next.set(cur.ptr);
    if (!last.ptr && pictType != kPictureI) {
        auto dummy = pool.get();
        pool.grey(*dummy);
        last.set(dummy);
    }
    linesize = cur.linesize[0];
    uvlinesize = cur.linesize[1];
    if (edgeEmu.size() < size_t(linesize) * 40) edgeEmu.assign(size_t(linesize) * 40, 0);
}

void Mpv::initBlockIndex() {
    blockIndex[0] = b8Stride * (mbY * 2) - 2 + mbX * 2;
    blockIndex[1] = b8Stride * (mbY * 2) - 1 + mbX * 2;
    blockIndex[2] = b8Stride * (mbY * 2 + 1) - 2 + mbX * 2;
    blockIndex[3] = b8Stride * (mbY * 2 + 1) - 1 + mbX * 2;
    blockIndex[4] = mbStride * (mbY + 1) + b8Stride * mbHeight * 2 + mbX - 1;
    blockIndex[5] = mbStride * (mbY + mbHeight + 2) + b8Stride * mbHeight * 2 + mbX - 1;
    dest[0] = cur.data[0] + (mbX - 1) * 16 + ptrdiff_t(mbY) * linesize * 16;
    dest[1] = cur.data[1] + (mbX - 1) * 8 + ptrdiff_t(mbY) * uvlinesize * 8;
    dest[2] = cur.data[2] + (mbX - 1) * 8 + ptrdiff_t(mbY) * uvlinesize * 8;
}

void Mpv::setQscale(int q) {
    qscale = std::clamp(q, 1, 31);
    chromaQscale = chromaQscaleTable[qscale];
    yDcScale = yDcScaleTable[qscale];
    cDcScale = cDcScaleTable[chromaQscale];
}

void Mpv::predMotion(int& px, int& py) {
    const int wrap = b8Stride;
    int16_t (*mot)[2] = cur.motionVal[0] + blockIndex[0];
    const int16_t* a = mot[-1];
    if (firstSliceLine) {
        if (mbX == resyncMbX) {
            px = py = 0;
        } else {
            px = a[0];
            py = a[1];
        }
        return;
    }
    const int16_t* b = mot[-wrap];
    const int16_t* c = mot[2 - wrap];
    px = midPred(a[0], b[0], c[0]);
    py = midPred(a[1], b[1], c[1]);
}

void Mpv::updateMotionVal() {
    const int xy = blockIndex[0], wrap = b8Stride;
    const int16_t x = int16_t(mbIntra ? 0 : mv[0]), y = int16_t(mbIntra ? 0 : mv[1]);
    for (int at : {xy, xy + 1, xy + wrap, xy + 1 + wrap}) {
        cur.motionVal[0][at][0] = x;
        cur.motionVal[0][at][1] = y;
    }
}

void Mpv::cleanIntraTableEntries() {
    const int wrap = b8Stride, xy = blockIndex[0], uxy = blockIndex[4], vxy = blockIndex[5];
    dcVal[xy] = dcVal[xy + 1] = dcVal[xy + wrap] = dcVal[xy + 1 + wrap] = 1024;
    dcVal[uxy] = dcVal[vxy] = 1024;
    memset(acVal[xy + 1], 0, sizeof acVal[0]);
    memset(acVal[xy + wrap], 0, 2 * sizeof acVal[0]);
    memset(acVal[uxy], 0, sizeof acVal[0]);
    memset(acVal[vxy], 0, sizeof acVal[0]);
}

void Mpv::cleanBuffers() {
    const int lWrap = b8Stride, cWrap = mbStride;
    const int lXy = (2 * mbY - 1) * lWrap + mbX * 2 - 1;
    const int uXy = 2 * mbHeight * lWrap + mbY * cWrap + mbX - 1;
    const int vXy = uXy + cWrap * (mbHeight + 1);
    memset(acVal + lXy, 0, size_t(lWrap * 2 + 1) * sizeof acVal[0]);
    memset(acVal + uXy, 0, size_t(cWrap + 1) * sizeof acVal[0]);
    memset(acVal + vXy, 0, size_t(cWrap + 1) * sizeof acVal[0]);
}

void Mpv::predAc(int16_t* b, int n, int dir) {
    auto roundedDiv = [](int a, int d) { return (a >= 0 ? a + (d >> 1) : a - (d >> 1)) / d; };
    int16_t* ac = acVal[blockIndex[n]];
    int16_t* ac1 = ac;
    const int8_t* qt = cur.qscaleTable;
    if (acPred) {
        if (dir == 0) {
            const int xy = mbX - 1 + mbY * mbStride;
            ac -= 16;
            if (mbX == 0 || qscale == qt[xy] || n == 1 || n == 3) {
                for (int i = 1; i < 8; i++) b[i << 3] = int16_t(b[i << 3] + ac[i]);
            } else {
                for (int i = 1; i < 8; i++) b[i << 3] = int16_t(b[i << 3] + roundedDiv(ac[i] * qt[xy], qscale));
            }
        } else {
            const int xy = mbX + mbY * mbStride - mbStride;
            ac -= 16 * blockWrap[n];
            if (mbY == 0 || qscale == qt[xy] || n == 2 || n == 3) {
                for (int i = 1; i < 8; i++) b[i] = int16_t(b[i] + ac[i + 8]);
            } else {
                for (int i = 1; i < 8; i++) b[i] = int16_t(b[i] + roundedDiv(ac[i + 8] * qt[xy], qscale));
            }
        }
    }
    for (int i = 1; i < 8; i++) {
        ac1[i] = b[i << 3];
        ac1[8 + i] = b[i];
    }
}

void Mpv::mpegMotion(const uint8_t* const ref[3], PixelsOp const (*ops)[4], int motionX, int motionY) {
    const int dxy = ((motionY & 1) << 1) | (motionX & 1);
    const int srcX = mbX * 16 + (motionX >> 1);
    const int srcY = mbY * 16 + (motionY >> 1);
    const int uvdxy = dxy | (motionY & 2) | ((motionX & 2) >> 1);
    const int uvSrcX = srcX >> 1, uvSrcY = srcY >> 1;
    const uint8_t* ptrY = ref[0] + ptrdiff_t(srcY) * linesize + srcX;
    const uint8_t* ptrCb = ref[1] + ptrdiff_t(uvSrcY) * uvlinesize + uvSrcX;
    const uint8_t* ptrCr = ref[2] + ptrdiff_t(uvSrcY) * uvlinesize + uvSrcX;
    if (unsigned(srcX) >= unsigned(std::max(hEdgePos - (motionX & 1) - 15, 0)) || unsigned(srcY) >= unsigned(std::max(vEdgePos - (motionY & 1) - 15, 0))) {
        uint8_t* buffer = edgeEmu.data();
        vc1::emulatedEdgeMc(buffer, ptrY, linesize, linesize, 17, 17, srcX, srcY, hEdgePos, vEdgePos);
        ptrY = buffer;
        uint8_t* u = buffer + 18 * linesize;
        uint8_t* v = u + 10 * uvlinesize;
        vc1::emulatedEdgeMc(u, ptrCb, uvlinesize, uvlinesize, 9, 9, uvSrcX, uvSrcY, hEdgePos >> 1, vEdgePos >> 1);
        vc1::emulatedEdgeMc(v, ptrCr, uvlinesize, uvlinesize, 9, 9, uvSrcX, uvSrcY, hEdgePos >> 1, vEdgePos >> 1);
        ptrCb = u;
        ptrCr = v;
    }
    ops[0][dxy](dest[0], ptrY, linesize, 16);
    ops[1][uvdxy](dest[1], ptrCb, uvlinesize, 8);
    ops[1][uvdxy](dest[2], ptrCr, uvlinesize, 8);
}

void Mpv::putDct(int n, uint8_t* dst, int stride, int q) {
    int16_t* b = block[n];
    const int qmul = q << 1;
    const int qadd = (q - 1) | 1;
    b[0] = int16_t(b[0] * (n < 4 ? yDcScale : cDcScale));
    const int count = acPred ? 63 : intraRasterEnd[blockLastIndex[n] & 63];
    for (int i = 1; i <= count; i++) {
        const int level = b[i];
        if (level) b[i] = int16_t(level < 0 ? level * qmul - qadd : level * qmul + qadd);
    }
    idctPut(dst, stride, b);
}

void Mpv::loopFilter() {
    const int xy = mbY * mbStride + mbX;
    uint8_t* y = dest[0];
    uint8_t* cb = dest[1];
    uint8_t* cr = dest[2];
    const uint32_t* types = cur.mbType;
    const int8_t* qt = cur.qscaleTable;
    auto skipped = [&](int at) { return (types[at] & kMbSkip) != 0; };
    int qpC = 0;
    if (!skipped(xy)) {
        qpC = qscale;
        vFilter(y + 8 * linesize, linesize, qpC);
        vFilter(y + 8 * linesize + 8, linesize, qpC);
    }
    if (mbY) {
        const int qpTt = skipped(xy - mbStride) ? 0 : qt[xy - mbStride];
        const int qpTc = qpC ? qpC : qpTt;
        if (qpTc) {
            vFilter(y, linesize, qpTc);
            vFilter(y + 8, linesize, qpTc);
            vFilter(cb, uvlinesize, chromaQscaleTable[qpTc]);
            vFilter(cr, uvlinesize, chromaQscaleTable[qpTc]);
        }
        if (qpTt) hFilter(y - 8 * linesize + 8, linesize, qpTt);
        if (mbX) {
            const int qpDt = (qpTt || skipped(xy - 1 - mbStride)) ? qpTt : qt[xy - 1 - mbStride];
            if (qpDt) {
                hFilter(y - 8 * linesize, linesize, qpDt);
                hFilter(cb - 8 * uvlinesize, uvlinesize, chromaQscaleTable[qpDt]);
                hFilter(cr - 8 * uvlinesize, uvlinesize, chromaQscaleTable[qpDt]);
            }
        }
    }
    if (qpC) {
        hFilter(y + 8, linesize, qpC);
        if (mbY + 1 == mbHeight) hFilter(y + 8 * linesize + 8, linesize, qpC);
    }
    if (mbX) {
        const int qpLc = (qpC || skipped(xy - 1)) ? qpC : qt[xy - 1];
        if (qpLc) {
            hFilter(y, linesize, qpLc);
            if (mbY + 1 == mbHeight) {
                hFilter(y + 8 * linesize, linesize, qpLc);
                hFilter(cb, uvlinesize, chromaQscaleTable[qpLc]);
                hFilter(cr, uvlinesize, chromaQscaleTable[qpLc]);
            }
        }
    }
}

}

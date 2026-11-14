#include "soft_video.h"
#include "h263.h"
#include "readers.h"
#include <array>
#include <cstring>

namespace absent {

namespace h263 {

namespace {

constexpr int kDcMax = 119;
constexpr int kMbacBitrate = 50 * 1024, kIiBitrate = 128 * 1024;
constexpr int kSkipMpeg = 1, kSkipRow = 2, kSkipCol = 3;
constexpr int kFrameSkipped = 1;

constexpr auto kInverse = [] {
    std::array<uint32_t, 257> table{};
    table[1] = 0xFFFFFFFFu;
    for (uint64_t b = 2; b < 257; b++) table[b] = uint32_t(((uint64_t(1) << 32) + b - 1) / b);
    return table;
}();

int fastDiv(int a, int b) {
    const uint64_t inverse = b < 257 ? kInverse[size_t(b)] : ((uint64_t(1) << 32) + uint64_t(b) - 1) / uint64_t(b);
    return int(uint32_t((uint64_t(int64_t(a)) * inverse) >> 32));
}

int blockDc(const uint8_t* src, int stride, int scale) {
    int sum = 0;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) sum += src[x + y * stride];
    return fastDiv(sum + (scale >> 1), scale);
}

void wmv2LowpassH(uint8_t* dst, const uint8_t* src, int dstStride, int srcStride, int h) {
    for (int i = 0; i < h; i++, dst += dstStride, src += srcStride)
        for (int x = 0; x < 8; x++) dst[x] = clipU8((9 * (src[x] + src[x + 1]) - (src[x - 1] + src[x + 2]) + 8) >> 4);
}

void wmv2LowpassV(uint8_t* dst, const uint8_t* src, int dstStride, int srcStride, int w) {
    for (int i = 0; i < w; i++, src++, dst++)
        for (int y = 0; y < 8; y++)
            dst[y * dstStride] = clipU8((9 * (src[y * srcStride] + src[(y + 1) * srcStride]) - (src[(y - 1) * srcStride] + src[(y + 2) * srcStride]) + 8) >> 4);
}

void average8(uint8_t* dst, const uint8_t* a, const uint8_t* b, ptrdiff_t dstStride, ptrdiff_t aStride, ptrdiff_t bStride) {
    for (int y = 0; y < 8; y++, dst += dstStride, a += aStride, b += bStride)
        for (int x = 0; x < 8; x++) dst[x] = uint8_t((a[x] + b[x] + 1) >> 1);
}

void mspel8(int dxy, uint8_t* dst, const uint8_t* src, ptrdiff_t stride) {
    uint8_t halfH[88], halfV[64], halfHV[64];
    const int s = int(stride);
    switch (dxy) {
    case 0:
        for (int y = 0; y < 8; y++) memcpy(dst + y * stride, src + y * stride, 8);
        break;
    case 1:
        wmv2LowpassH(halfH, src, 8, s, 8);
        average8(dst, src, halfH, stride, stride, 8);
        break;
    case 2:
        wmv2LowpassH(dst, src, s, s, 8);
        break;
    case 3:
        wmv2LowpassH(halfH, src, 8, s, 8);
        average8(dst, src + 1, halfH, stride, stride, 8);
        break;
    case 4:
        wmv2LowpassV(dst, src, s, s, 8);
        break;
    case 5:
    case 7:
        wmv2LowpassH(halfH, src - stride, 8, s, 11);
        wmv2LowpassV(halfV, src + (dxy == 7), 8, s, 8);
        wmv2LowpassV(halfHV, halfH + 8, 8, 8, 8);
        average8(dst, halfV, halfHV, stride, 8, 8);
        break;
    default:
        wmv2LowpassH(halfH, src - stride, 8, s, 11);
        wmv2LowpassV(dst, halfH + 8, s, 8, 8);
        break;
    }
}

struct Msmpeg4 : Mpv {
    Bits gb;
    int bitRate = 0, flipflopRounding = 0, mvTableIndex = 0, rlTableIndex = 0, rlChromaTableIndex = 0, dcTableIndex = 0;
    int useSkipMbCode = 0, perMbRlTable = 0, esc3LevelLength = 0, esc3RunLength = 0;
    int sliceHeight = 0;
    int lastDc[3] = {};
    int loopFilterOn = 0;
    int jTypeBit = 0, jType = 0, abtFlag = 0, abtType = 0, abtTypeTable[6] = {}, perMbAbt = 0, perBlockAbt = 0;
    int mspelBit = 0, cbpTableIndex = 0, topLeftMvFlag = 0, perMbRlBit = 0, hshift = 0;
    alignas(16) int16_t abtBlock2[6][64] = {};
    IntraX8 x8;
    const Tables& t = tables();

    bool open(int ver, const uint8_t* extra, size_t extraSize, int w, int h) {
        version = ver;
        if (!allocate(w, h)) return false;
        if (version <= kMsmpeg4v2) {
            static const uint8_t eight[32] = {8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8};
            yDcScaleTable = cDcScaleTable = eight;
        } else if (version == kMsmpeg4v3) {
            yDcScaleTable = kOldYDcScale;
            cDcScaleTable = kWmv1CDcScale;
        } else {
            yDcScaleTable = kWmv1YDcScale;
            cDcScaleTable = kWmv1CDcScale;
        }
        if (version >= kWmv1) setScans(vc1::kWmv1Scantable[1], vc1::kWmv1Scantable[0], vc1::kWmv1Scantable[2], vc1::kWmv1Scantable[3]);
        else setScans(kZigzagDirect, kZigzagDirect, kAlternateHorizontalScan, kAlternateVerticalScan);
        if (version == kWmv2) {
            idctPut = wmv2IdctPut;
            idctAdd = wmv2IdctAdd;
        }
        sliceHeight = mbHeight;
        setQscale(1);
        if (version == kWmv2) {
            if (extraSize >= 4) {
                Bits bits;
                bits.reset(extra, 4);
                bits.skip(5);
                bitRate = int(bits.bits(11)) * 1024;
                mspelBit = bits.bit();
                loopFilterOn = bits.bit();
                abtFlag = bits.bit();
                jTypeBit = bits.bit();
                topLeftMvFlag = bits.bit();
                perMbRlBit = bits.bit();
                const int code = int(bits.bits(3));
                if (code) sliceHeight = mbHeight / code;
            }
            x8.setSize(mbWidth, mbHeight);
        }
        return true;
    }

    void extHeader(int bufSize) {
        const int64_t left = int64_t(bufSize) * 8 - gb.count();
        const int length = version >= kMsmpeg4v3 ? 17 : 16;
        if (left >= length && left < length + 8) {
            gb.skip(5);
            bitRate = int(gb.bits(11)) * 1024;
            flipflopRounding = version >= kMsmpeg4v3 ? gb.bit() : 0;
        } else if (left < length + 8) {
            flipflopRounding = 0;
        }
    }

    int pictureHeader() {
        if (int64_t(gb.left()) * 8 < int64_t((width + 15) / 16) * ((height + 15) / 16)) return -1;
        if (version == kMsmpeg4v1) {
            if (gb.bits(32) != 0x100) return -1;
            gb.skip(5);
        }
        pictType = int(gb.bits(2)) + 1;
        if (pictType != kPictureI && pictType != kPictureP) return -1;
        qscale = chromaQscale = int(gb.bits(5));
        if (!qscale) return -1;
        if (pictType == kPictureI) {
            const int code = int(gb.bits(5));
            if (version == kMsmpeg4v1) {
                if (code == 0 || code > mbHeight) return -1;
                sliceHeight = code;
            } else {
                if (code < 0x17) return -1;
                sliceHeight = mbHeight / (code - 0x16);
            }
            switch (version) {
            case kMsmpeg4v1:
            case kMsmpeg4v2:
                rlChromaTableIndex = rlTableIndex = 2;
                dcTableIndex = 0;
                break;
            case kMsmpeg4v3:
                rlChromaTableIndex = gb.decode012();
                rlTableIndex = gb.decode012();
                dcTableIndex = gb.bit();
                break;
            default:
                extHeader((2 + 5 + 5 + 17 + 7) / 8);
                perMbRlTable = bitRate > kMbacBitrate ? gb.bit() : 0;
                if (!perMbRlTable) {
                    rlChromaTableIndex = gb.decode012();
                    rlTableIndex = gb.decode012();
                }
                dcTableIndex = gb.bit();
                interIntraPred = 0;
                break;
            }
            noRounding = 1;
        } else {
            switch (version) {
            case kMsmpeg4v1:
            case kMsmpeg4v2:
                useSkipMbCode = version == kMsmpeg4v1 ? 1 : gb.bit();
                rlTableIndex = rlChromaTableIndex = 2;
                dcTableIndex = 0;
                mvTableIndex = 0;
                break;
            case kMsmpeg4v3:
                useSkipMbCode = gb.bit();
                rlTableIndex = rlChromaTableIndex = gb.decode012();
                dcTableIndex = gb.bit();
                mvTableIndex = gb.bit();
                break;
            default:
                useSkipMbCode = gb.bit();
                perMbRlTable = bitRate > kMbacBitrate ? gb.bit() : 0;
                if (!perMbRlTable) rlTableIndex = rlChromaTableIndex = gb.decode012();
                dcTableIndex = gb.bit();
                mvTableIndex = gb.bit();
                interIntraPred = width * height < 320 * 240 && bitRate <= kIiBitrate;
                break;
            }
            noRounding = flipflopRounding ? noRounding ^ 1 : 0;
        }
        esc3LevelLength = esc3RunLength = 0;
        return 0;
    }

    int wmv2PictureHeader() {
        pictType = gb.bit() + 1;
        if (pictType == kPictureI) gb.skip(7);
        qscale = chromaQscale = int(gb.bits(5));
        if (qscale <= 0) return -1;
        if (pictType != kPictureI && gb.show(1)) {
            Bits copy = gb;
            const int skipType = int(copy.bits(2));
            int run = skipType == kSkipCol ? mbWidth : mbHeight;
            while (run > 0) {
                const int span = std::min(run, 25);
                if (copy.bits(span) + 1 != (1u << span)) break;
                run -= span;
            }
            if (!run) return kFrameSkipped;
        }
        return 0;
    }

    int parseMbSkip() {
        uint32_t* types = cur.mbType;
        const int skipType = int(gb.bits(2));
        auto at = [&](int x, int y) -> uint32_t& { return types[y * mbStride + x]; };
        switch (skipType) {
        case kSkipMpeg:
            if (gb.left() < mbHeight * mbWidth) return -1;
            for (int y = 0; y < mbHeight; y++)
                for (int x = 0; x < mbWidth; x++) at(x, y) = gb.bit() ? kMbSkip : 0;
            break;
        case kSkipRow:
            for (int y = 0; y < mbHeight; y++) {
                if (gb.left() < 1) return -1;
                if (gb.bit()) {
                    for (int x = 0; x < mbWidth; x++) at(x, y) = kMbSkip;
                } else {
                    if (gb.left() < mbWidth) return -1;
                    for (int x = 0; x < mbWidth; x++) at(x, y) = gb.bit() ? kMbSkip : 0;
                }
            }
            break;
        case kSkipCol:
            for (int x = 0; x < mbWidth; x++) {
                if (gb.left() < 1) return -1;
                if (gb.bit()) {
                    for (int y = 0; y < mbHeight; y++) at(x, y) = kMbSkip;
                } else {
                    if (gb.left() < mbHeight) return -1;
                    for (int y = 0; y < mbHeight; y++) at(x, y) = gb.bit() ? kMbSkip : 0;
                }
            }
            break;
        default:
            for (int y = 0; y < mbHeight; y++)
                for (int x = 0; x < mbWidth; x++) at(x, y) = 0;
            break;
        }
        int coded = 0;
        for (int y = 0; y < mbHeight; y++)
            for (int x = 0; x < mbWidth; x++) coded += !(at(x, y) & kMbSkip);
        return coded > gb.left() ? -1 : 0;
    }

    int wmv2SecondaryHeader() {
        if (pictType == kPictureI) {
            memset(cur.mbType, 0, sizeof(uint32_t) * size_t(mbHeight) * mbStride);
            jType = jTypeBit ? gb.bit() : 0;
            if (!jType) {
                perMbRlTable = perMbRlBit ? gb.bit() : 0;
                if (!perMbRlTable) {
                    rlChromaTableIndex = gb.decode012();
                    rlTableIndex = gb.decode012();
                }
                dcTableIndex = gb.bit();
                if (int64_t(gb.left()) * 8 < int64_t((width + 15) / 16) * ((height + 15) / 16)) return -1;
            }
            interIntraPred = 0;
            noRounding = 1;
        } else {
            jType = 0;
            if (parseMbSkip() < 0) return -1;
            static const uint8_t map[3][3] = {{0, 2, 1}, {1, 0, 2}, {2, 1, 0}};
            cbpTableIndex = map[(qscale > 10) + (qscale > 20)][gb.decode012()];
            mspel = mspelBit ? gb.bit() : 0;
            if (abtFlag) {
                perMbAbt = gb.bit() ^ 1;
                if (!perMbAbt) abtType = gb.decode012();
            }
            perMbRlTable = perMbRlBit ? gb.bit() : 0;
            if (!perMbRlTable) rlTableIndex = rlChromaTableIndex = gb.decode012();
            if (gb.left() < 2) return -1;
            dcTableIndex = gb.bit();
            mvTableIndex = gb.bit();
            interIntraPred = 0;
            noRounding ^= 1;
        }
        esc3LevelLength = esc3RunLength = 0;
        if (jType) {
            x8.decode(gb, cur.data, cur.linesize, cur.qscaleTable, 2 * qscale, (qscale - 1) | 1, loopFilterOn);
            return 1;
        }
        return 0;
    }

    int codedBlockPred(int n, uint8_t*& codedValue) {
        const int xy = blockIndex[n], wrap = b8Stride;
        const int a = codedBlock[xy - 1], b = codedBlock[xy - 1 - wrap], c = codedBlock[xy - wrap];
        codedValue = &codedBlock[xy];
        return b == c ? a : c;
    }

    int predDc(int n, int16_t*& dcValuePtr, int& dir) {
        const int scale = n < 4 ? yDcScale : cDcScale;
        int16_t* const dc = dcVal + blockIndex[n];
        const int wrap = blockWrap[n];
        int a = dc[-1], b = dc[-1 - wrap], c = dc[-wrap];
        if (firstSliceLine && !(n & 2) && version < kWmv1) b = c = 1024;
        if (scale == 8) {
            a = (a + 4) / 8;
            b = (b + 4) / 8;
            c = (c + 4) / 8;
        } else {
            a = fastDiv(a + (scale >> 1), scale);
            b = fastDiv(b + (scale >> 1), scale);
            c = fastDiv(c + (scale >> 1), scale);
        }
        int pred;
        if (version > kMsmpeg4v3) {
            if (interIntraPred) {
                if (n == 1) {
                    pred = a;
                    dir = 0;
                } else if (n == 2) {
                    pred = c;
                    dir = 1;
                } else if (n == 3) {
                    if (std::abs(a - b) < std::abs(b - c)) {
                        pred = c;
                        dir = 1;
                    } else {
                        pred = a;
                        dir = 0;
                    }
                } else {
                    uint8_t* pixels;
                    int stride;
                    if (n < 4) {
                        stride = linesize;
                        pixels = cur.data[0] + ((n >> 1) + 2 * mbY) * 8 * stride + ((n & 1) + 2 * mbX) * 8;
                    } else {
                        stride = uvlinesize;
                        pixels = cur.data[n - 3] + mbY * 8 * stride + mbX * 8;
                    }
                    a = mbX == 0 ? (1024 + (scale >> 1)) / scale : blockDc(pixels - 8, stride, scale * 8);
                    c = mbY == 0 ? (1024 + (scale >> 1)) / scale : blockDc(pixels - 8 * stride, stride, scale * 8);
                    if (aicDir == 0) {
                        pred = a;
                        dir = 0;
                    } else if (aicDir == 1) {
                        pred = n == 0 ? c : a;
                        dir = n == 0;
                    } else if (aicDir == 2) {
                        pred = n == 0 ? a : c;
                        dir = n != 0;
                    } else {
                        pred = c;
                        dir = 1;
                    }
                }
            } else if (std::abs(a - b) < std::abs(b - c)) {
                pred = c;
                dir = 1;
            } else {
                pred = a;
                dir = 0;
            }
        } else if (std::abs(a - b) <= std::abs(b - c)) {
            pred = c;
            dir = 1;
        } else {
            pred = a;
            dir = 0;
        }
        dcValuePtr = dc;
        return pred;
    }

    int decodeDc(int n, int& dir) {
        int level;
        if (version <= kMsmpeg4v2) {
            level = (n < 4 ? t.v2DcLum : t.v2DcChroma).read(gb);
            if (level < 0) {
                dir = 0;
                return -1;
            }
            level -= 256;
        } else {
            level = vc1::tables().dc[dcTableIndex][n >= 4].read(gb);
            if (level == kDcMax) {
                level = int(gb.bits(8));
                if (gb.bit()) level = -level;
            } else if (level != 0) {
                if (gb.bit()) level = -level;
            }
        }
        if (version == kMsmpeg4v1) {
            int& last = lastDc[n < 4 ? 0 : n - 3];
            level += last;
            last = level;
        } else {
            int16_t* dc = nullptr;
            level += predDc(n, dc, dir);
            *dc = int16_t(level * (n < 4 ? yDcScale : cDcScale));
        }
        return level;
    }

    int decodeBlock(int16_t* b, int n, int coded, const uint8_t* scan) {
        int dir = -1, qmul, qadd, i, runDiff;
        const RlTable* rl;
        if (mbIntra) {
            qmul = 1;
            qadd = 0;
            int level = decodeDc(n, dir);
            if (level < 0 && interIntraPred) level = 0;
            if (n < 4) {
                rl = &t.rl[rlTableIndex];
                if (level > 256 * yDcScale && !interIntraPred) return -1;
            } else {
                rl = &t.rl[3 + rlChromaTableIndex];
                if (level > 256 * cDcScale && !interIntraPred) return -1;
            }
            b[0] = int16_t(level);
            runDiff = version >= kWmv1;
            i = 0;
            if (!coded) {
                predAc(b, n, dir);
                blockLastIndex[n] = i;
                return 0;
            }
            scan = acPred ? (dir == 0 ? intraVScan : intraHScan) : intraScan;
        } else {
            qmul = qscale << 1;
            qadd = (qscale - 1) | 1;
            i = -1;
            rl = &t.rl[3 + rlTableIndex];
            runDiff = version != kMsmpeg4v2;
            if (!coded) {
                blockLastIndex[n] = i;
                return 0;
            }
            if (!scan) scan = interScan;
        }
        const int64_t left = gb.left();
        for (;;) {
            int level, run, used = 0;
            const uint64_t window = gb.cache();
            const int v = rl->vlc.lookup(window, used);
            if (v > 0 && (v & 0xFF)) {
                level = (v & 0xFF) * qmul + qadd;
                if (int64_t(window << used) < 0) level = -level;
                gb.skip(used + 1);
                i += v >> 8;
                if (i > 62) {
                    i -= 192;
                    if (i & ~63) {
                        if (left >= 0) {
                            i = 63;
                            break;
                        }
                        return -1;
                    }
                    b[scan[i]] = int16_t(level);
                    break;
                }
                b[scan[i]] = int16_t(level);
                continue;
            }
            gb.skip(used);
            level = v < 0 ? 64 : 0;
            run = 66;
            if (level == 0) {
                const uint32_t cache = gb.show(2);
                if (version == kMsmpeg4v1 || !(cache & 2)) {
                    if (version == kMsmpeg4v1 || !(cache & 1)) {
                        int last;
                        if (version != kMsmpeg4v1) gb.skip(2);
                        if (version <= kMsmpeg4v3) {
                            last = gb.bit();
                            run = int(gb.bits(6));
                            level = int8_t(gb.bits(8));
                        } else {
                            last = gb.bit();
                            if (!esc3LevelLength) {
                                int ll;
                                if (qscale < 8) {
                                    ll = int(gb.bits(3));
                                    if (ll == 0) ll = 8 + gb.bit();
                                } else {
                                    ll = 2;
                                    while (ll < 8 && gb.show(1) == 0) {
                                        ll++;
                                        gb.skip(1);
                                    }
                                    if (ll < 8) gb.skip(1);
                                }
                                esc3LevelLength = ll;
                                esc3RunLength = int(gb.bits(2)) + 3;
                            }
                            run = int(gb.bits(esc3RunLength));
                            const int sign = gb.bit();
                            level = int(gb.bits(esc3LevelLength));
                            if (sign) level = -level;
                        }
                        level = level > 0 ? level * qmul + qadd : level * qmul - qadd;
                        i += run + 1;
                        if (last) i += 192;
                    } else {
                        gb.skip(2);
                        rl->read(gb, qmul, qadd, level, run);
                        i += run + rl->maxRun[run >> 7][level / qmul] + runDiff;
                        if (gb.bit()) level = -level;
                    }
                } else {
                    gb.skip(1);
                    rl->read(gb, qmul, qadd, level, run);
                    i += run;
                    level = level + rl->maxLevel[run >> 7][(run - 1) & 63] * qmul;
                    if (gb.bit()) level = -level;
                }
            } else {
                i += run;
                if (gb.bit()) level = -level;
            }
            if (i > 62) {
                i -= 192;
                if (i & ~63) {
                    if (left >= 0) {
                        i = 63;
                        break;
                    }
                    return -1;
                }
                b[scan[i]] = int16_t(level);
                break;
            }
            b[scan[i]] = int16_t(level);
        }
        if (mbIntra) predAc(b, n, dir);
        blockLastIndex[n] = i;
        return 0;
    }

    void decodeMotion(int& mx, int& my) {
        const int sym = t.msMv[mvTableIndex].read(gb);
        int x, y;
        if (sym) {
            x = sym >> 8;
            y = sym & 0xFF;
        } else {
            x = int(gb.bits(6));
            y = int(gb.bits(6));
        }
        x += mx - 32;
        y += my - 32;
        if (x <= -64) x += 64;
        else if (x >= 64) x -= 64;
        if (y <= -64) y += 64;
        else if (y >= 64) y -= 64;
        mx = x;
        my = y;
    }

    int v2Motion(int pred) {
        const int code = t.mv.read(gb);
        if (code < 0) return 0xffff;
        if (code == 0) return pred;
        int value = code;
        if (gb.bit()) value = -value;
        value += pred;
        if (value <= -64) value += 64;
        else if (value >= 64) value -= 64;
        return value;
    }

    void skipMb() {
        mbIntra = 0;
        for (int& last : blockLastIndex) last = -1;
        mv[0] = mv[1] = 0;
        mbSkipped = 1;
    }

    int decodeBlocks(int cbp) {
        memset(block, 0, sizeof block);
        for (int i = 0; i < 6; i++)
            if (decodeBlock(block[i], i, (cbp >> (5 - i)) & 1, nullptr) < 0) return -1;
        return 0;
    }

    int v12Mb() {
        int cbp;
        if (pictType == kPictureP) {
            if (useSkipMbCode && gb.bit()) {
                skipMb();
                return 0;
            }
            const int code = (version == kMsmpeg4v2 ? t.v2MbType : t.interMcbpc).read(gb);
            if (code < 0 || code > 7) return -1;
            mbIntra = code >> 2;
            cbp = code & 3;
        } else {
            mbIntra = 1;
            cbp = (version == kMsmpeg4v2 ? t.v2IntraCbpc : t.intraMcbpc).read(gb);
            if (cbp < 0 || cbp > 3) return -1;
        }
        if (!mbIntra) {
            const int cbpy = t.cbpy.read(gb);
            if (cbpy < 0) return -1;
            cbp |= cbpy << 2;
            if (version == kMsmpeg4v1 || (cbp & 3) != 3) cbp ^= 0x3C;
            int mx, my;
            predMotion(mx, my);
            mv[0] = v2Motion(mx);
            mv[1] = v2Motion(my);
        } else {
            acPred = version == kMsmpeg4v2 ? gb.bit() : 0;
            const int v = t.cbpy.read(gb);
            if (v < 0) return -1;
            cbp |= v << 2;
            if (version != kMsmpeg4v2 && pictType == kPictureP) cbp ^= 0x3C;
        }
        return decodeBlocks(cbp);
    }

    int intraCbp(int code) {
        int cbp = 0;
        for (int i = 0; i < 6; i++) {
            int value = (code >> (5 - i)) & 1;
            if (i < 4) {
                uint8_t* coded = nullptr;
                value ^= codedBlockPred(i, coded);
                *coded = uint8_t(value);
            }
            cbp |= value << (5 - i);
        }
        return cbp;
    }

    int v34Mb() {
        if (gb.left() <= 0) return -1;
        int cbp;
        if (pictType == kPictureP) {
            if (useSkipMbCode && gb.bit()) {
                skipMb();
                return 0;
            }
            const int code = t.mbNonIntra[3].read(gb);
            mbIntra = (~code & 0x40) >> 6;
            cbp = code & 0x3f;
        } else {
            mbIntra = 1;
            cbp = intraCbp(vc1::tables().mbI.read(gb));
        }
        if (!mbIntra) {
            if (perMbRlTable && cbp) rlTableIndex = rlChromaTableIndex = gb.decode012();
            int mx, my;
            predMotion(mx, my);
            decodeMotion(mx, my);
            mv[0] = mx;
            mv[1] = my;
        } else {
            acPred = gb.bit();
            if (interIntraPred) aicDir = t.interIntra.read(gb);
            if (perMbRlTable && cbp) rlTableIndex = rlChromaTableIndex = gb.decode012();
        }
        return decodeBlocks(cbp);
    }

    void wmv2PredMotion(int& px, int& py) {
        const int wrap = b8Stride, xy = blockIndex[0];
        const int16_t* a = cur.motionVal[0][xy - 1];
        const int16_t* b = cur.motionVal[0][xy - wrap];
        const int16_t* c = cur.motionVal[0][xy + 2 - wrap];
        const int diff = mbX && !firstSliceLine && !mspel && topLeftMvFlag ? std::max(std::abs(a[0] - b[0]), std::abs(a[1] - b[1])) : 0;
        const int type = diff >= 8 ? gb.bit() : 2;
        if (type == 0) {
            px = a[0];
            py = a[1];
        } else if (type == 1) {
            px = b[0];
            py = b[1];
        } else if (firstSliceLine) {
            px = a[0];
            py = a[1];
        } else {
            px = midPred(a[0], b[0], c[0]);
            py = midPred(a[1], b[1], c[1]);
        }
    }

    int wmv2InterBlock(int16_t* b, int n, int cbp) {
        static const int subCbpTable[3] = {2, 3, 1};
        if (!cbp) {
            blockLastIndex[n] = -1;
            return 0;
        }
        if (perBlockAbt) abtType = gb.decode012();
        abtTypeTable[n] = abtType;
        if (abtType) {
            const uint8_t* scan = abtType == 1 ? vc1::kWmv2ScantableA : vc1::kWmv2ScantableB;
            const int subCbp = subCbpTable[gb.decode012()];
            if ((subCbp & 1) && decodeBlock(b, n, 1, scan) < 0) return -1;
            if ((subCbp & 2) && decodeBlock(abtBlock2[n], n, 1, scan) < 0) return -1;
            blockLastIndex[n] = 63;
            return 0;
        }
        return decodeBlock(b, n, 1, interScan);
    }

    int wmv2Mb() {
        if (jType) return 0;
        int cbp;
        if (pictType == kPictureP) {
            if (cur.mbType[mbY * mbStride + mbX] & kMbSkip) {
                skipMb();
                hshift = 0;
                return 0;
            }
            if (gb.left() <= 0) return -1;
            const int code = t.mbNonIntra[cbpTableIndex].read(gb);
            mbIntra = (~code & 0x40) >> 6;
            cbp = code & 0x3f;
        } else {
            mbIntra = 1;
            if (gb.left() <= 0) return -1;
            cbp = intraCbp(vc1::tables().mbI.read(gb));
        }
        if (!mbIntra) {
            int mx, my;
            wmv2PredMotion(mx, my);
            if (cbp) {
                memset(block, 0, sizeof block);
                if (perMbRlTable) rlTableIndex = rlChromaTableIndex = gb.decode012();
                if (abtFlag && perMbAbt) {
                    perBlockAbt = gb.bit();
                    if (!perBlockAbt) abtType = gb.decode012();
                } else {
                    perBlockAbt = 0;
                }
            }
            decodeMotion(mx, my);
            hshift = ((mx | my) & 1) && mspel ? gb.bit() : 0;
            mv[0] = mx;
            mv[1] = my;
            for (int i = 0; i < 6; i++)
                if (wmv2InterBlock(block[i], i, (cbp >> (5 - i)) & 1) < 0) return -1;
            return 0;
        }
        acPred = gb.bit();
        if (perMbRlTable && cbp) rlTableIndex = rlChromaTableIndex = gb.decode012();
        return decodeBlocks(cbp);
    }

    int decodeMb() {
        if (version <= kMsmpeg4v2) return v12Mb();
        if (version == kWmv2) return wmv2Mb();
        return v34Mb();
    }

    void mspelMotion(PixelsOp const (*ops)[4]) {
        const uint8_t* const* ref = last.data;
        const int motionX = mv[0], motionY = mv[1];
        int dxy = 2 * (((motionY & 1) << 1) | (motionX & 1)) + hshift;
        int srcX = std::clamp(mbX * 16 + (motionX >> 1), -16, width);
        int srcY = std::clamp(mbY * 16 + (motionY >> 1), -16, height);
        if (srcX <= -16 || srcX >= width) dxy &= ~3;
        if (srcY <= -16 || srcY >= height) dxy &= ~4;
        const uint8_t* ptr = ref[0] + ptrdiff_t(srcY) * linesize + srcX;
        bool emu = false;
        if (srcX < 1 || srcY < 1 || srcX + 17 >= hEdgePos || srcY + 16 + 1 >= vEdgePos) {
            vc1::emulatedEdgeMc(edgeEmu.data(), ptr - 1 - linesize, linesize, linesize, 19, 19, srcX - 1, srcY - 1, hEdgePos, vEdgePos);
            ptr = edgeEmu.data() + 1 + linesize;
            emu = true;
        }
        mspel8(dxy, dest[0], ptr, linesize);
        mspel8(dxy, dest[0] + 8, ptr + 8, linesize);
        mspel8(dxy, dest[0] + 8 * linesize, ptr + 8 * linesize, linesize);
        mspel8(dxy, dest[0] + 8 + 8 * linesize, ptr + 8 + 8 * linesize, linesize);
        dxy = ((motionX & 3) != 0) | (((motionY & 3) != 0) << 1);
        srcX = std::clamp(mbX * 8 + (motionX >> 2), -8, width >> 1);
        if (srcX == (width >> 1)) dxy &= ~1;
        srcY = std::clamp(mbY * 8 + (motionY >> 2), -8, height >> 1);
        if (srcY == (height >> 1)) dxy &= ~2;
        const ptrdiff_t offset = ptrdiff_t(srcY) * uvlinesize + srcX;
        for (int p = 1; p < 3; p++) {
            const uint8_t* source = ref[p] + offset;
            if (emu) {
                vc1::emulatedEdgeMc(edgeEmu.data(), source, uvlinesize, uvlinesize, 9, 9, srcX, srcY, hEdgePos >> 1, vEdgePos >> 1);
                source = edgeEmu.data();
            }
            ops[1][dxy](dest[p], source, uvlinesize, 8);
        }
    }

    void wmv2AddBlock(int n, uint8_t* dst, int stride) {
        if (blockLastIndex[n] < 0) return;
        switch (abtTypeTable[n]) {
        case 0:
            idctAdd(dst, stride, block[n]);
            break;
        case 1:
            vc1::simpleIdct84Add(dst, stride, block[n]);
            vc1::simpleIdct84Add(dst + 4 * stride, stride, abtBlock2[n]);
            memset(abtBlock2[n], 0, sizeof abtBlock2[n]);
            break;
        default:
            vc1::simpleIdct48Add(dst, stride, block[n]);
            vc1::simpleIdct48Add(dst + 4, stride, abtBlock2[n]);
            memset(abtBlock2[n], 0, sizeof abtBlock2[n]);
            break;
        }
    }

    void reconstructMb() {
        cur.qscaleTable[mbY * mbStride + mbX] = int8_t(qscale);
        mbSkipped = 0;
        uint8_t* y = dest[0];
        const int offsets[4] = {0, 8, 8 * linesize, 8 * linesize + 8};
        if (!mbIntra) {
            PixelsOp const (*ops)[4] = noRounding ? dsp->putNoRndPixelsTab : dsp->putPixelsTab;
            if (mspel && version == kWmv2) mspelMotion(ops);
            else mpegMotion(last.data, ops, mv[0], mv[1]);
            if (version == kWmv2) {
                for (int i = 0; i < 4; i++) wmv2AddBlock(i, y + offsets[i], linesize);
                wmv2AddBlock(4, dest[1], uvlinesize);
                wmv2AddBlock(5, dest[2], uvlinesize);
            } else {
                for (int i = 0; i < 4; i++)
                    if (blockLastIndex[i] >= 0) idctAdd(y + offsets[i], linesize, block[i]);
                if (blockLastIndex[4] >= 0) idctAdd(dest[1], uvlinesize, block[4]);
                if (blockLastIndex[5] >= 0) idctAdd(dest[2], uvlinesize, block[5]);
            }
        } else {
            for (int i = 0; i < 4; i++) putDct(i, y + offsets[i], linesize, qscale);
            putDct(4, dest[1], uvlinesize, chromaQscale);
            putDct(5, dest[2], uvlinesize, chromaQscale);
        }
    }

    int decodeSlice() {
        firstSliceLine = 1;
        resyncMbX = mbX;
        resyncMbY = mbY;
        setQscale(qscale);
        for (; mbY < mbHeight; mbY++) {
            if (resyncMbY + sliceHeight == mbY) return 0;
            if (version == kMsmpeg4v1) lastDc[0] = lastDc[1] = lastDc[2] = 128;
            initBlockIndex();
            for (; mbX < mbWidth; mbX++) {
                updateBlockIndex();
                if (resyncMbX == mbX && resyncMbY + 1 == mbY) firstSliceLine = 0;
                const int ret = decodeMb();
                const int xy = mbY * mbStride + mbX;
                if (!mbIntra) {
                    if (mbIntraTable[size_t(xy)]) {
                        mbIntraTable[size_t(xy)] = 0;
                        cleanIntraTableEntries();
                    }
                } else {
                    mbIntraTable[size_t(xy)] = 1;
                }
                updateMotionVal();
                if (ret < 0) return -1;
                reconstructMb();
                if (loopFilterOn) loopFilter();
            }
            mbX = 0;
        }
        return 0;
    }

    void conceal(int first) {
        for (int index = first; index < mbNum; index++) {
            const int x = index % mbWidth, y = index / mbWidth;
            for (int p = 0; p < 3; p++) {
                const int size = p ? 8 : 16;
                const int stride = cur.linesize[p];
                uint8_t* to = cur.data[p] + ptrdiff_t(y) * size * stride + x * size;
                const uint8_t* source = last.ptr && last.ptr != cur.ptr ? last.data[p] + ptrdiff_t(y) * size * stride + x * size : nullptr;
                for (int row = 0; row < size; row++) {
                    if (source) memcpy(to + row * stride, source + row * stride, size_t(size));
                    else memset(to + row * stride, 0x80, size_t(size));
                }
            }
        }
    }

    int decodeFrame(const uint8_t* data, size_t size) {
        gb.reset(data, size);
        const int header = version == kWmv2 ? wmv2PictureHeader() : pictureHeader();
        if (header == kFrameSkipped) return kFrameSkipped;
        if (header < 0) return -1;
        frameStart();
        if (version == kWmv2) {
            const int ret = wmv2SecondaryHeader();
            if (ret < 0) return -1;
            if (ret == 1) return 0;
        }
        mbX = mbY = 0;
        int sliceRet = decodeSlice();
        while (mbY < mbHeight) {
            if (sliceHeight == 0 || mbX != 0 || sliceRet < 0 || (mbY % sliceHeight) != 0 || gb.left() < 0) break;
            if (version < kWmv1) cleanBuffers();
            if (decodeSlice() < 0) sliceRet = -1;
        }
        if (version < kWmv1 && pictType == kPictureI) extHeader(int(size));
        if (mbY < mbHeight) conceal(std::min(mbNum, mbY * mbWidth + mbX));
        return 0;
    }
};

const char* backendName(int version) {
    static const char* const names[] = {"msmpeg4v1", "msmpeg4v2", "msmpeg4v3", "wmv1", "wmv2"};
    return names[version - 1];
}

struct Msmpeg4Decoder : SoftDecoder {
    std::unique_ptr<Msmpeg4> m;
    std::vector<uint8_t> extra;
    int version = 0, width = 0, height = 0;

    const char* name() const override { return backendName(version); }

    bool open() {
        m = std::make_unique<Msmpeg4>();
        return m->open(version, extra.data(), extra.size(), width, height);
    }

    void reset() override { open(); }

    void emit(const Pic& pic, int64_t stamp, const PictureSink& shown) {
        Picture p;
        p.stamp = stamp;
        p.width = width;
        p.height = height;
        p.chromaWidth = (width + 1) / 2;
        p.chromaHeight = (height + 1) / 2;
        for (int i = 0; i < 3; i++) {
            p.plane[i] = pic.data[i];
            p.stride[i] = size_t(pic.linesize[i]);
        }
        shown(p);
    }

    bool decode(const uint8_t* data, size_t size, int64_t stamp, const PictureSink& shown) override {
        if (size > (64u << 20)) return false;
        const int ret = m->decodeFrame(data, size);
        if (ret < 0) return false;
        const Pic& pic = ret == kFrameSkipped ? m->next : m->cur;
        if (!pic.ptr) return false;
        emit(pic, stamp, shown);
        return true;
    }
};

}

}

namespace {

int msmpeg4Version(uint32_t codec) {
    if (codec == fourcc("MPG4")) return h263::kMsmpeg4v1;
    if (codec == fourcc("MP42")) return h263::kMsmpeg4v2;
    if (codec == fourcc("MP43")) return h263::kMsmpeg4v3;
    if (codec == fourcc("WMV1")) return h263::kWmv1;
    if (codec == fourcc("WMV2")) return h263::kWmv2;
    return 0;
}

}

int msmpeg4PictureType(uint32_t codec, const uint8_t* data, size_t size) {
    const int version = msmpeg4Version(codec);
    if (!version || !size) return -1;
    BitReader in(data, size);
    if (version == h263::kWmv2) return int(in.read(1));
    if (version == h263::kMsmpeg4v1) {
        if (in.read(32) != 0x100) return -1;
        in.skip(5);
    }
    const int type = int(in.read(2));
    return type <= 1 ? type : -1;
}

std::unique_ptr<SoftDecoder> openMsmpeg4(uint32_t codec, const std::vector<uint8_t>& config, int width, int height, std::string* error) {
    auto decoder = std::make_unique<h263::Msmpeg4Decoder>();
    decoder->version = msmpeg4Version(codec);
    decoder->width = width;
    decoder->height = height;
    if (config.size() > 40) decoder->extra.assign(config.begin() + 40, config.end());
    if (!decoder->version || !decoder->open()) {
        if (error) *error = "the video size or codec is not supported by this decoder";
        return nullptr;
    }
    return decoder;
}

}

#include "soft_video.h"
#include "h263.h"
#include "readers.h"
#include <cstring>

namespace absent {

namespace h263 {

namespace {

constexpr int kPictureB = 3;
constexpr int kSliceOk = 0, kSliceError = -1, kSliceEnd = -2;
constexpr uint32_t kMbIntra = 2, kMb8x8 = 4;

const uint16_t kFormat[8][2] = {{0, 0}, {128, 96}, {176, 144}, {352, 288}, {704, 576}, {1408, 1152}, {0, 0}, {0, 0}};
const uint8_t kMpeg1DcScale[32] = {8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8};
const uint8_t kAicDcScale[32] = {0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30, 32, 34, 36, 38, 40, 42, 44, 46, 48, 50, 52, 54, 56, 58, 60, 62};
const uint8_t kModifiedQuant[2][32] = {
    {0, 3, 1, 2, 3, 4, 5, 6, 7, 8, 9, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28},
    {0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 24, 25, 26, 27, 28, 29, 30, 31, 31, 31, 26}};
const uint8_t kChromaQscale[32] = {0, 1, 2, 3, 4, 5, 6, 6, 7, 8, 9, 9, 10, 10, 11, 11, 12, 12, 12, 13, 13, 13, 14, 14, 14, 14, 14, 15, 15, 15, 15, 15};
const uint16_t kMbaMax[6] = {47, 98, 395, 1583, 6335, 9215};
const uint8_t kMbaLength[7] = {6, 7, 9, 11, 13, 14, 14};
const uint8_t kChromaRound[16] = {0, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1};

const uint16_t kAicVlc[103][2] = {
    {0x2, 2}, {0x6, 3}, {0xe, 4}, {0xc, 5}, {0xd, 5}, {0x10, 6}, {0x11, 6}, {0x12, 6}, {0x16, 7}, {0x1b, 8}, {0x20, 9}, {0x21, 9},
    {0x1a, 9}, {0x1b, 9}, {0x1c, 9}, {0x1d, 9}, {0x1e, 9}, {0x1f, 9}, {0x23, 11}, {0x22, 11}, {0x57, 12}, {0x56, 12}, {0x55, 12}, {0x54, 12},
    {0x53, 12}, {0xf, 4}, {0x14, 6}, {0x14, 7}, {0x1e, 8}, {0xf, 10}, {0x21, 11}, {0x50, 12}, {0xb, 5}, {0x15, 7}, {0xe, 10}, {0x9, 10},
    {0x15, 6}, {0x1d, 8}, {0xd, 10}, {0x51, 12}, {0x13, 6}, {0x23, 9}, {0x7, 11}, {0x17, 7}, {0x22, 9}, {0x52, 12}, {0x1c, 8}, {0xc, 10},
    {0x1f, 8}, {0xb, 10}, {0x25, 9}, {0xa, 10}, {0x24, 9}, {0x6, 11}, {0x21, 10}, {0x20, 10}, {0x8, 10}, {0x20, 11}, {0x7, 4}, {0xc, 6},
    {0x10, 7}, {0x13, 8}, {0x11, 9}, {0x12, 9}, {0x4, 10}, {0x27, 11}, {0x26, 11}, {0x5f, 12}, {0xf, 6}, {0x13, 9}, {0x5, 10}, {0x25, 11},
    {0xe, 6}, {0x14, 9}, {0x24, 11}, {0xd, 6}, {0x6, 10}, {0x5e, 12}, {0x11, 7}, {0x7, 10}, {0x13, 7}, {0x5d, 12}, {0x12, 7}, {0x5c, 12},
    {0x14, 8}, {0x5b, 12}, {0x15, 8}, {0x1a, 8}, {0x19, 8}, {0x18, 8}, {0x17, 8}, {0x16, 8}, {0x19, 9}, {0x15, 9}, {0x16, 9}, {0x18, 9},
    {0x17, 9}, {0x4, 11}, {0x5, 11}, {0x58, 12}, {0x59, 12}, {0x5a, 12}, {0x3, 7}};
const int8_t kAicRun[102] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
    4, 4, 4, 5, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 11, 12, 13, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4,
    5, 5, 6, 6, 7, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23};
const int8_t kAicLevel[102] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 1, 2, 3, 4, 5, 6, 7, 1, 2, 3, 4, 1, 2, 3, 4,
    1, 2, 3, 1, 2, 3, 1, 2, 1, 2, 1, 2, 1, 2, 1, 1, 1, 1, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 1, 2, 3, 4, 1, 2, 3, 1, 2, 3, 1, 2,
    1, 2, 1, 2, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};

const RlTable& aicTable() {
    static const RlTable made = [] {
        RlTable rl;
        buildRl(rl, 102, 58, kAicVlc, kAicRun, kAicLevel);
        return rl;
    }();
    return made;
}

int signExtend(int value, int bits) { return int(unsigned(value) << (32 - bits)) >> (32 - bits); }


void putObmc(uint8_t* dst, uint8_t* const src[5], int stride) {
    const uint8_t* top = src[1];
    const uint8_t* left = src[2];
    const uint8_t* mid = src[0];
    const uint8_t* right = src[3];
    const uint8_t* bottom = src[4];
    auto f = [&](int x, int t, int l, int m, int r, int b) { dst[x] = uint8_t((t * top[x] + l * left[x] + m * mid[x] + r * right[x] + b * bottom[x] + 4) >> 3); };
    auto f4 = [&](int x, int t, int l, int m, int r, int b) {
        f(x, t, l, m, r, b);
        f(x + 1, t, l, m, r, b);
        f(x + stride, t, l, m, r, b);
        f(x + 1 + stride, t, l, m, r, b);
    };
    int x = 0;
    f(x, 2, 2, 4, 0, 0);
    f(x + 1, 2, 1, 5, 0, 0);
    f4(x + 2, 2, 1, 5, 0, 0);
    f4(x + 4, 2, 0, 5, 1, 0);
    f(x + 6, 2, 0, 5, 1, 0);
    f(x + 7, 2, 0, 4, 2, 0);
    x += stride;
    f(x, 1, 2, 5, 0, 0);
    f(x + 1, 1, 2, 5, 0, 0);
    f(x + 6, 1, 0, 5, 2, 0);
    f(x + 7, 1, 0, 5, 2, 0);
    x += stride;
    f4(x, 1, 2, 5, 0, 0);
    f4(x + 2, 1, 1, 6, 0, 0);
    f4(x + 4, 1, 0, 6, 1, 0);
    f4(x + 6, 1, 0, 5, 2, 0);
    x += 2 * stride;
    f4(x, 0, 2, 5, 0, 1);
    f4(x + 2, 0, 1, 6, 0, 1);
    f4(x + 4, 0, 0, 6, 1, 1);
    f4(x + 6, 0, 0, 5, 2, 1);
    x += 2 * stride;
    f(x, 0, 2, 5, 0, 1);
    f(x + 1, 0, 2, 5, 0, 1);
    f4(x + 2, 0, 1, 5, 0, 2);
    f4(x + 4, 0, 0, 5, 1, 2);
    f(x + 6, 0, 0, 5, 2, 1);
    f(x + 7, 0, 0, 5, 2, 1);
    x += stride;
    f(x, 0, 2, 4, 0, 2);
    f(x + 1, 0, 1, 5, 0, 2);
    f(x + 6, 0, 0, 5, 1, 2);
    f(x + 7, 0, 0, 4, 2, 2);
}

struct H263 : Mpv {
    Bits gb, resyncGb;
    const Tables& t = tables();
    int pictureNumber = 0, gobIndex = 1;
    int longVectors = 0, obmc = 0, pbFrame = 0, customPcf = 0, umvplus = 0, aic = 0, loop = 0, sliced = 0, altInter = 0, modifiedQuant = 0;
    int fourMv = 0;
    int mvs[4][2] = {};
    std::vector<uint8_t> obmcScratch;
    std::vector<uint8_t> decoded;

    H263() {
        setScans(kZigzagDirect, kZigzagDirect, kAlternateHorizontalScan, kAlternateVerticalScan);
        yDcScaleTable = cDcScaleTable = kMpeg1DcScale;
    }

    int decodeMba() {
        int i = 0;
        const int count = ((width + 15) / 16) * ((height + 15) / 16);
        while (i < 6 && count - 1 > kMbaMax[i]) i++;
        return int(gb.bits(kMbaLength[i]));
    }

    int pictureHeader() {
        gb.align();
        uint32_t startcode = gb.bits(14);
        for (int i = gb.left(); i > 24; i -= 8) {
            startcode = ((startcode << 8) | gb.bits(8)) & 0x3FFFFF;
            if (startcode == 0x20) break;
        }
        if (startcode != 0x20) return -1;
        int reference = int(gb.bits(8));
        reference -= (reference - (pictureNumber & 0xFF) + 128) & ~0xFF;
        pictureNumber = (pictureNumber & ~0xFF) + reference;
        if (gb.bit() != 1) return -1;
        if (gb.bit() != 0) return -1;
        gb.skip(3);
        int format = int(gb.bits(3));
        if (format != 7 && format != 6) {
            const int w = kFormat[format][0], h = kFormat[format][1];
            if (!w) return -1;
            pictType = kPictureI + gb.bit();
            longVectors = gb.bit();
            if (gb.bit()) return -1;
            obmc = gb.bit();
            pbFrame = gb.bit();
            qscale = chromaQscale = int(gb.bits(5));
            gb.skip(1);
            width = w;
            height = h;
        } else {
            const int ufep = int(gb.bits(3));
            if (ufep == 1) {
                format = int(gb.bits(3));
                customPcf = gb.bit();
                umvplus = gb.bit();
                gb.skip(1);
                obmc = gb.bit();
                aic = gb.bit();
                loop = gb.bit();
                sliced = gb.bit();
                gb.skip(2);
                altInter = gb.bit();
                modifiedQuant = gb.bit();
                if (modifiedQuant) chromaQscaleTable = kChromaQscale;
                gb.skip(4);
            } else if (ufep != 0) {
                return -1;
            }
            switch (gb.bits(3)) {
            case 0:
            case 7:
                pictType = kPictureI;
                break;
            case 1:
                pictType = kPictureP;
                break;
            case 2:
                pictType = kPictureP;
                pbFrame = 3;
                break;
            case 3:
                pictType = kPictureB;
                break;
            default:
                return -1;
            }
            gb.skip(2);
            noRounding = gb.bit();
            gb.skip(4);
            if (ufep) {
                int w, h;
                if (format == 6) {
                    const int aspect = int(gb.bits(4));
                    w = (int(gb.bits(9)) + 1) * 4;
                    gb.skip(1);
                    h = int(gb.bits(9)) * 4;
                    if (aspect == 15) gb.skip(16);
                } else {
                    w = kFormat[format][0];
                    h = kFormat[format][1];
                }
                if (!w || !h) return -1;
                width = w;
                height = h;
                if (customPcf) {
                    gb.skip(1);
                    if (!gb.bits(7)) return -1;
                }
            }
            if (customPcf) gb.skip(2);
            if (ufep) {
                if (umvplus && gb.bit() == 0) gb.skip(1);
                if (sliced) gb.skip(2);
                if (pictType == kPictureB) gb.skip(ufep == 1 ? 8 : 4);
            }
            qscale = int(gb.bits(5));
        }
        if (width <= 0 || height <= 0 || width > 4096 || height > 4096) return -1;
        if (width * height / 256 / 8 > gb.left()) return -1;
        gobIndex = height <= 400 ? 1 : height <= 800 ? 2 : 4;
        if (pbFrame) gb.skip(customPcf ? 7 : 5);
        if (gb.left() <= 0) return -1;
        while (gb.bit()) {
            gb.skip(8);
            if (gb.left() <= 0) return -1;
        }
        if (sliced) {
            if (gb.bit() != 1) return -1;
            decodeMba();
            if (gb.bit() != 1) return -1;
        }
        yDcScaleTable = cDcScaleTable = aic ? kAicDcScale : kMpeg1DcScale;
        return 0;
    }

    int gobHeader() {
        if (gb.show(16)) return -1;
        gb.skip(16);
        int left = std::min(gb.left(), 32);
        for (; left > 13; left--)
            if (gb.bit()) break;
        if (left <= 13) return -1;
        if (sliced) {
            if (gb.bit() == 0) return -1;
            const int position = decodeMba();
            mbX = position % mbWidth;
            mbY = position / mbWidth;
            if (mbNum > 1583 && gb.bit() == 0) return -1;
            qscale = int(gb.bits(5));
            if (gb.bit() == 0) return -1;
            gb.skip(2);
        } else {
            const int number = int(gb.bits(5));
            mbX = 0;
            mbY = gobIndex * number;
            gb.skip(2);
            qscale = int(gb.bits(5));
        }
        if (mbY >= mbHeight) return -1;
        if (qscale == 0) return -1;
        return 0;
    }

    int resync() {
        if (gb.show(16) == 0 && gobHeader() >= 0) return 0;
        gb = resyncGb;
        gb.align();
        for (int left = gb.left(); left > 16 + 1 + 5 + 5; left -= 8) {
            if (gb.show(16) == 0) {
                const Bits saved = gb;
                if (gobHeader() >= 0) return 0;
                gb = saved;
            }
            gb.skip(8);
        }
        return -1;
    }

    int readMotion(int pred) {
        const int code = t.mv.read(gb);
        if (code == 0) return pred;
        if (code < 0) return 0xffff;
        int value = gb.bit() ? -code : code;
        value += pred;
        if (!longVectors) {
            value = signExtend(value, 6);
        } else {
            if (pred < -31 && value < -63) value += 64;
            if (pred > 32 && value > 63) value -= 64;
        }
        return value;
    }

    int readUmotion(int pred) {
        if (gb.bit()) return pred;
        int code = 2 + gb.bit();
        while (gb.bit()) {
            code = (code << 1) + gb.bit();
            if (code >= 32768) return 0xffff;
        }
        const int sign = code & 1;
        code >>= 1;
        return sign ? pred - code : pred + code;
    }

    int readVector(int pred) { return umvplus ? readUmotion(pred) : readMotion(pred); }

    int16_t* predMotion(int block, int& px, int& py) {
        static const int off[4] = {2, 1, 1, -1};
        const int wrap = b8Stride;
        int16_t (*mot)[2] = cur.motionVal[0] + blockIndex[block];
        int16_t* a = mot[-1];
        if (firstSliceLine && block < 3) {
            if (block == 0 && mbX == resyncMbX) {
                px = py = 0;
            } else if (block < 2) {
                px = a[0];
                py = a[1];
            } else {
                const int16_t* b = mot[-wrap];
                const int16_t* c = mot[off[block] - wrap];
                if (mbX == resyncMbX) a[0] = a[1] = 0;
                px = midPred(a[0], b[0], c[0]);
                py = midPred(a[1], b[1], c[1]);
            }
        } else {
            const int16_t* b = mot[-wrap];
            const int16_t* c = mot[off[block] - wrap];
            px = midPred(a[0], b[0], c[0]);
            py = midPred(a[1], b[1], c[1]);
        }
        return mot[0];
    }

    void previewObmc() {
        const Bits saved = gb;
        const int xy = mbX + 1 + mbY * mbStride;
        const int stride = b8Stride;
        for (int i = 0; i < 4; i++) blockIndex[i] += 2;
        blockIndex[4]++;
        blockIndex[5]++;
        mbX++;
        int cbpc;
        bool skipped = false;
        do {
            if (gb.bit()) {
                int16_t (*mot)[2] = cur.motionVal[0] + blockIndex[0];
                for (int at : {0, 1, stride, stride + 1}) mot[at][0] = mot[at][1] = 0;
                cur.mbType[xy] = kMbSkip;
                skipped = true;
                break;
            }
            cbpc = t.interMcbpc.read(gb);
        } while (cbpc == 20);
        if (!skipped) {
            if (cbpc & 4) {
                cur.mbType[xy] = kMbIntra;
            } else {
                t.cbpy.read(gb);
                if (cbpc & 8) gb.skip(modifiedQuant ? (gb.bit() ? 1 : 5) : 2);
                int px, py;
                if ((cbpc & 16) == 0) {
                    cur.mbType[xy] = 0;
                    int16_t (*mot)[2] = reinterpret_cast<int16_t (*)[2]>(predMotion(0, px, py));
                    const int16_t mx = int16_t(readVector(px));
                    const int16_t my = int16_t(readVector(py));
                    for (int at : {0, 1, stride, stride + 1}) {
                        mot[at][0] = mx;
                        mot[at][1] = my;
                    }
                } else {
                    cur.mbType[xy] = kMb8x8;
                    for (int i = 0; i < 4; i++) {
                        int16_t* mot = predMotion(i, px, py);
                        const int mx = readVector(px);
                        const int my = readVector(py);
                        if (umvplus && mx - px == 1 && my - py == 1) gb.skip(1);
                        mot[0] = int16_t(mx);
                        mot[1] = int16_t(my);
                    }
                }
            }
        }
        for (int i = 0; i < 4; i++) blockIndex[i] -= 2;
        blockIndex[4]--;
        blockIndex[5]--;
        mbX--;
        gb = saved;
    }

    void decodeDquant() {
        static const int8_t step[4] = {-1, -2, 1, 2};
        int q;
        if (modifiedQuant) q = gb.bit() ? kModifiedQuant[gb.bit()][qscale] : int(gb.bits(5));
        else q = qscale + step[gb.bits(2)];
        setQscale(q);
    }

    void predAcdc(int16_t* b, int n) {
        const int xy = blockIndex[n];
        int16_t* dc = dcVal + xy;
        int16_t* ac = acVal[xy];
        const int wrap = n < 4 ? b8Stride : mbStride;
        const int scale = n < 4 ? yDcScale : cDcScale;
        int a = dc[-1], c = dc[-wrap];
        if (firstSliceLine && n != 3) {
            if (n != 2) c = 1024;
            if (n != 1 && mbX == resyncMbX) a = 1024;
        }
        int pred;
        if (acPred) {
            pred = 1024;
            if (aicDir) {
                if (a != 1024) {
                    const int16_t* left = ac - 16;
                    for (int i = 1; i < 8; i++) b[i << 3] = int16_t(b[i << 3] + left[i]);
                    pred = a;
                }
            } else if (c != 1024) {
                const int16_t* above = ac - 16 * wrap;
                for (int i = 1; i < 8; i++) b[i] = int16_t(b[i] + above[i + 8]);
                pred = c;
            }
        } else if (a != 1024 && c != 1024) {
            pred = (a + c) >> 1;
        } else {
            pred = a != 1024 ? a : c;
        }
        int value = int16_t(b[0] * scale + pred);
        value = value < 0 ? 0 : value | 1;
        b[0] = int16_t(value);
        *dc = int16_t(value);
        for (int i = 1; i < 8; i++) {
            ac[i] = b[i << 3];
            ac[8 + i] = b[i];
        }
    }

    int decodeBlock(int16_t* b, int n, int coded) {
        const RlTable* rl = &t.rl[5];
        const uint8_t* scan = intraScan;
        const Bits start = gb;
        int i;
        if (aic && mbIntra) {
            i = 0;
            if (!coded) {
                predAcdc(b, n);
                blockLastIndex[n] = 0;
                return 0;
            }
            rl = &aicTable();
            if (acPred) scan = aicDir ? intraVScan : intraHScan;
        } else if (mbIntra) {
            int level = int(gb.bits(8));
            if (level == 255) level = 128;
            b[0] = int16_t(level);
            i = 1;
        } else {
            i = 0;
        }
        if (!coded) {
            blockLastIndex[n] = i - 1;
            return 0;
        }
        for (;;) {
            i--;
            bool again = false;
            for (;;) {
                const int v = rl->vlc.read(gb);
                if (v < 0) return -1;
                int run = v >> 8, level = v & 0xFF;
                if (run == 66) {
                    run = int(gb.bits(7)) + 1;
                    level = int8_t(gb.bits(8));
                    if (level == -128) {
                        const int low = int(gb.bits(5));
                        level = low | signExtend(int(gb.bits(6)), 6) * 32;
                    }
                } else if (gb.bit()) {
                    level = -level;
                }
                i += run;
                if (i >= 64) {
                    i = i - run + ((run - 1) & 63) + 1;
                    if (i < 64) {
                        b[scan[i]] = int16_t(level);
                        break;
                    }
                    if (altInter && rl == &t.rl[5] && !mbIntra) {
                        rl = &aicTable();
                        i = 0;
                        gb = start;
                        memset(b, 0, 64 * sizeof(int16_t));
                        again = true;
                        break;
                    }
                    return -1;
                }
                b[scan[i]] = int16_t(level);
            }
            if (!again) break;
        }
        if (mbIntra && aic) predAcdc(b, n);
        blockLastIndex[n] = i;
        return 0;
    }

    int modb(int& cbpb) {
        int coded, mv = 1;
        if (pbFrame < 3) {
            coded = gb.bit();
            if (pbFrame == 2 && coded) mv = !gb.bit();
        } else {
            mv = gb.unary(0, 4) + 1;
            coded = mv & 1;
            mv = !!(mv & 2);
        }
        if (coded) cbpb = int(gb.bits(6));
        return mv;
    }

    int skipBPart(int cbp) {
        alignas(16) int16_t scratch[64];
        const int intra = mbIntra;
        int last[6];
        memcpy(last, blockLastIndex, sizeof last);
        mbIntra = 0;
        for (int i = 0; i < 6; i++) {
            if (decodeBlock(scratch, i, cbp & 32) < 0) return -1;
            cbp += cbp;
        }
        mbIntra = intra;
        memcpy(blockLastIndex, last, sizeof last);
        return 0;
    }

    int endOfMb() {
        if (gb.left() < 0) return kSliceError;
        int v = int(gb.show(16));
        if (gb.left() < 16) v >>= 16 - gb.left();
        return v == 0 ? kSliceEnd : kSliceOk;
    }

    int decodeMb() {
        const int xy = mbX + mbY * mbStride;
        int cbpc, cbpy, cbp, dquant, cbpb = 0, mvCount = 0;
        if (pictType == kPictureP) {
            do {
                if (gb.bit()) {
                    mbIntra = 0;
                    for (int& last : blockLastIndex) last = -1;
                    fourMv = 0;
                    cur.mbType[xy] = kMbSkip;
                    mv[0] = mv[1] = 0;
                    return endOfMb();
                }
                cbpc = t.interMcbpc.read(gb);
                if (cbpc < 0) return kSliceError;
            } while (cbpc == 20);
            memset(block, 0, sizeof block);
            dquant = cbpc & 8;
            mbIntra = (cbpc & 4) != 0;
        } else {
            do {
                cbpc = t.intraMcbpc.read(gb);
                if (cbpc < 0) return kSliceError;
            } while (cbpc == 8);
            memset(block, 0, sizeof block);
            dquant = cbpc & 4;
            mbIntra = 1;
        }
        if (!mbIntra) {
            if (pbFrame && gb.bit()) mvCount = modb(cbpb);
            cbpy = t.cbpy.read(gb);
            if (cbpy < 0) return kSliceError;
            if (!altInter || (cbpc & 3) != 3) cbpy ^= 0xF;
            cbp = (cbpc & 3) | (cbpy << 2);
            if (dquant) decodeDquant();
            int px, py;
            if ((cbpc & 16) == 0) {
                cur.mbType[xy] = 0;
                fourMv = 0;
                predMotion(0, px, py);
                const int mx = readVector(px);
                if (mx >= 0xffff) return kSliceError;
                const int my = readVector(py);
                if (my >= 0xffff) return kSliceError;
                mv[0] = mx;
                mv[1] = my;
                if (umvplus && mx - px == 1 && my - py == 1) gb.skip(1);
            } else {
                cur.mbType[xy] = kMb8x8;
                fourMv = 1;
                for (int i = 0; i < 4; i++) {
                    int16_t* mot = predMotion(i, px, py);
                    const int mx = readVector(px);
                    if (mx >= 0xffff) return kSliceError;
                    const int my = readVector(py);
                    if (my >= 0xffff) return kSliceError;
                    mvs[i][0] = mx;
                    mvs[i][1] = my;
                    if (umvplus && mx - px == 1 && my - py == 1) gb.skip(1);
                    mot[0] = int16_t(mx);
                    mot[1] = int16_t(my);
                }
            }
        } else {
            cur.mbType[xy] = kMbIntra;
            fourMv = 0;
            acPred = 0;
            if (aic) {
                acPred = gb.bit();
                if (acPred) aicDir = gb.bit();
            }
            if (pbFrame && gb.bit()) mvCount = modb(cbpb);
            cbpy = t.cbpy.read(gb);
            if (cbpy < 0) return kSliceError;
            cbp = (cbpc & 3) | (cbpy << 2);
            if (dquant) decodeDquant();
            mvCount += pbFrame ? 1 : 0;
        }
        while (mvCount-- > 0) {
            readMotion(0);
            readMotion(0);
        }
        for (int i = 0; i < 6; i++) {
            if (decodeBlock(block[i], i, cbp & 32) < 0) return kSliceError;
            cbp += cbp;
        }
        if (pbFrame && skipBPart(cbpb) < 0) return kSliceError;
        if (obmc && !mbIntra && pictType == kPictureP && mbX + 1 < mbWidth) previewObmc();
        return endOfMb();
    }

    void hpelMotion(uint8_t* dst, const uint8_t* src, int srcX, int srcY, PixelsOp const* op, int mx, int my) {
        int dxy = 0;
        srcX += mx >> 1;
        srcY += my >> 1;
        srcX = std::clamp(srcX, -16, width);
        if (srcX != width) dxy |= mx & 1;
        srcY = std::clamp(srcY, -16, height);
        if (srcY != height) dxy |= (my & 1) << 1;
        src += ptrdiff_t(srcY) * linesize + srcX;
        if (unsigned(srcX) >= unsigned(std::max(hEdgePos - (mx & 1) - 7, 0)) || unsigned(srcY) >= unsigned(std::max(vEdgePos - (my & 1) - 7, 0))) {
            vc1::emulatedEdgeMc(edgeEmu.data(), src, linesize, linesize, 9, 9, srcX, srcY, hEdgePos, vEdgePos);
            src = edgeEmu.data();
        }
        op[dxy](dst, src, linesize, 8);
    }

    void chroma4mv(PixelsOp const* op, int mx, int my) {
        mx = kChromaRound[mx & 15] + (mx >> 3);
        my = kChromaRound[my & 15] + (my >> 3);
        int dxy = ((my & 1) << 1) | (mx & 1);
        mx >>= 1;
        my >>= 1;
        int srcX = std::clamp(mbX * 8 + mx, -8, width >> 1);
        if (srcX == (width >> 1)) dxy &= ~1;
        int srcY = std::clamp(mbY * 8 + my, -8, height >> 1);
        if (srcY == (height >> 1)) dxy &= ~2;
        const ptrdiff_t offset = ptrdiff_t(srcY) * uvlinesize + srcX;
        const bool emulate = unsigned(srcX) >= unsigned(std::max((hEdgePos >> 1) - (dxy & 1) - 7, 0)) || unsigned(srcY) >= unsigned(std::max((vEdgePos >> 1) - (dxy >> 1) - 7, 0));
        for (int p = 1; p < 3; p++) {
            const uint8_t* ptr = last.data[p] + offset;
            if (emulate) {
                vc1::emulatedEdgeMc(edgeEmu.data(), ptr, uvlinesize, uvlinesize, 9, 9, srcX, srcY, hEdgePos >> 1, vEdgePos >> 1);
                ptr = edgeEmu.data();
            }
            op[dxy](dest[p], ptr, uvlinesize, 8);
        }
    }

    void apply8x8(PixelsOp const (*ops)[4]) {
        int mx = 0, my = 0;
        for (int i = 0; i < 4; i++) {
            hpelMotion(dest[0] + (i & 1) * 8 + (i >> 1) * 8 * linesize, last.data[0], mbX * 16 + (i & 1) * 8, mbY * 16 + (i >> 1) * 8, ops[1], mvs[i][0], mvs[i][1]);
            mx += mvs[i][0];
            my += mvs[i][1];
        }
        chroma4mv(ops[1], mx, my);
    }

    void applyObmc(PixelsOp const (*ops)[4]) {
        int16_t cache[4][4][2];
        const int xy = mbX + mbY * mbStride;
        const int stride = b8Stride;
        const int at = mbX * 2 + mbY * 2 * stride;
        const int16_t (*motion)[2] = cur.motionVal[0];
        auto copy = [](int16_t* to, const int16_t* from) {
            to[0] = from[0];
            to[1] = from[1];
        };
        auto intra = [&](int index) { return (cur.mbType[index] & kMbIntra) != 0; };
        copy(cache[1][1], motion[at]);
        copy(cache[1][2], motion[at + 1]);
        copy(cache[2][1], motion[at + stride]);
        copy(cache[2][2], motion[at + stride + 1]);
        copy(cache[3][1], motion[at + stride]);
        copy(cache[3][2], motion[at + stride + 1]);
        if (mbY == 0 || intra(xy - mbStride)) {
            copy(cache[0][1], cache[1][1]);
            copy(cache[0][2], cache[1][2]);
        } else {
            copy(cache[0][1], motion[at - stride]);
            copy(cache[0][2], motion[at - stride + 1]);
        }
        if (mbX == 0 || intra(xy - 1)) {
            copy(cache[1][0], cache[1][1]);
            copy(cache[2][0], cache[2][1]);
        } else {
            copy(cache[1][0], motion[at - 1]);
            copy(cache[2][0], motion[at - 1 + stride]);
        }
        if (mbX + 1 >= mbWidth || intra(xy + 1)) {
            copy(cache[1][3], cache[1][2]);
            copy(cache[2][3], cache[2][2]);
        } else {
            copy(cache[1][3], motion[at + 2]);
            copy(cache[2][3], motion[at + 2 + stride]);
        }
        int mx = 0, my = 0;
        for (int i = 0; i < 4; i++) {
            const int x = (i & 1) + 1, y = (i >> 1) + 1;
            const int16_t m[5][2] = {{cache[y][x][0], cache[y][x][1]}, {cache[y - 1][x][0], cache[y - 1][x][1]}, {cache[y][x - 1][0], cache[y][x - 1][1]},
                                     {cache[y][x + 1][0], cache[y][x + 1][1]}, {cache[y + 1][x][0], cache[y + 1][x][1]}};
            uint8_t* ptr[5];
            for (int k = 0; k < 5; k++) {
                if (k && m[k][0] == m[0][0] && m[k][1] == m[0][1]) {
                    ptr[k] = ptr[0];
                } else {
                    ptr[k] = obmcScratch.data() + 8 * (k & 1) + ptrdiff_t(linesize) * 8 * (k >> 1);
                    hpelMotion(ptr[k], last.data[0], mbX * 16 + (i & 1) * 8, mbY * 16 + (i >> 1) * 8, ops[1], m[k][0], m[k][1]);
                }
            }
            putObmc(dest[0] + (i & 1) * 8 + (i >> 1) * 8 * linesize, ptr, linesize);
            mx += m[0][0];
            my += m[0][1];
        }
        chroma4mv(ops[1], mx, my);
    }

    void addDequant(int n, uint8_t* dst, int stride, int q) {
        if (blockLastIndex[n] < 0) return;
        int16_t* b = block[n];
        const int qmul = q << 1, qadd = (q - 1) | 1;
        const int count = intraRasterEnd[blockLastIndex[n] & 63];
        for (int i = 0; i <= count; i++) {
            const int level = b[i];
            if (level) b[i] = int16_t(level < 0 ? level * qmul - qadd : level * qmul + qadd);
        }
        idctAdd(dst, stride, b);
    }

    void putIntra(int n, uint8_t* dst, int stride, int q) {
        int16_t* b = block[n];
        const int qmul = q << 1;
        int qadd = 0;
        if (!aic) {
            b[0] = int16_t(b[0] * (n < 4 ? yDcScale : cDcScale));
            qadd = (q - 1) | 1;
        }
        const int count = acPred ? 63 : intraRasterEnd[blockLastIndex[n] & 63];
        for (int i = 1; i <= count; i++) {
            const int level = b[i];
            if (level) b[i] = int16_t(level < 0 ? level * qmul - qadd : level * qmul + qadd);
        }
        idctPut(dst, stride, b);
    }

    void reconstructMb() {
        const int xy = mbY * mbStride + mbX;
        cur.qscaleTable[xy] = int8_t(qscale);
        uint8_t* y = dest[0];
        const int offsets[4] = {0, 8, 8 * linesize, 8 * linesize + 8};
        if (!mbIntra) {
            PixelsOp const (*ops)[4] = noRounding ? pixelOps().noRnd : pixelOps().put;
            if (obmc) applyObmc(ops);
            else if (fourMv) apply8x8(ops);
            else mpegMotion(last.data, ops, mv[0], mv[1]);
            for (int i = 0; i < 4; i++) addDequant(i, y + offsets[i], linesize, qscale);
            addDequant(4, dest[1], uvlinesize, chromaQscale);
            addDequant(5, dest[2], uvlinesize, chromaQscale);
        } else {
            for (int i = 0; i < 4; i++) putIntra(i, y + offsets[i], linesize, qscale);
            putIntra(4, dest[1], uvlinesize, chromaQscale);
            putIntra(5, dest[2], uvlinesize, chromaQscale);
        }
        decoded[size_t(mbY * mbWidth + mbX)] = 1;
    }

    void finishMb() {
        reconstructMb();
        if (loop) loopFilter();
    }

    int decodeSlice() {
        resyncGb = gb;
        firstSliceLine = 1;
        resyncMbX = mbX;
        resyncMbY = mbY;
        setQscale(qscale);
        for (; mbY < mbHeight; mbY++) {
            initBlockIndex();
            for (; mbX < mbWidth; mbX++) {
                updateBlockIndex();
                if (resyncMbX == mbX && resyncMbY + 1 == mbY) firstSliceLine = 0;
                fourMv = 0;
                const int ret = decodeMb();
                if (aic) {
                    const size_t xy = size_t(mbY * mbStride + mbX);
                    if (!mbIntra) {
                        if (mbIntraTable[xy]) {
                            mbIntraTable[xy] = 0;
                            cleanIntraTableEntries();
                        }
                    } else {
                        mbIntraTable[xy] = 1;
                    }
                }
                if (!fourMv) updateMotionVal();
                if (ret == kSliceEnd) {
                    finishMb();
                    if (++mbX >= mbWidth) {
                        mbX = 0;
                        mbY++;
                    }
                    return 0;
                }
                if (ret < 0) return -1;
                finishMb();
            }
            mbX = 0;
        }
        return 0;
    }

    void start() {
        cur.set(pool.get());
        last.set(next.ptr);
        next.set(cur.ptr);
        if (!last.ptr && pictType != kPictureI) {
            auto dummy = pool.get();
            const int rows = (mbHeight + 1) & ~1;
            for (int y = 0; y < rows * 16; y++) memset(dummy->data[0] + ptrdiff_t(y) * dummy->linesize[0], 16, size_t(mbWidth) * 16);
            for (int p = 1; p < 3; p++)
                for (int y = 0; y < rows * 8; y++) memset(dummy->data[p] + ptrdiff_t(y) * dummy->linesize[p], 0x80, size_t(mbWidth) * 8);
            last.set(dummy);
        }
        linesize = cur.linesize[0];
        uvlinesize = cur.linesize[1];
        if (edgeEmu.size() < size_t(linesize) * 40) edgeEmu.assign(size_t(linesize) * 40, 0);
        if (obmcScratch.size() < size_t(linesize) * 24) obmcScratch.assign(size_t(linesize) * 24, 0);
        decoded.assign(size_t(mbNum), 0);
    }

    void conceal() {
        const bool source = last.ptr && last.ptr != cur.ptr;
        for (int index = 0; index < mbNum; index++) {
            if (decoded[size_t(index)]) continue;
            const int x = index % mbWidth, y = index / mbWidth;
            for (int p = 0; p < 3; p++) {
                const int size = p ? 8 : 16;
                const int stride = cur.linesize[p];
                uint8_t* to = cur.data[p] + ptrdiff_t(y) * size * stride + x * size;
                const uint8_t* from = source ? last.data[p] + ptrdiff_t(y) * size * stride + x * size : nullptr;
                for (int row = 0; row < size; row++) {
                    if (from) memcpy(to + row * stride, from + row * stride, size_t(size));
                    else memset(to + row * stride, 0x80, size_t(size));
                }
            }
        }
    }

    int decodeFrame(const uint8_t* data, size_t size) {
        gb.reset(data, size);
        const int oldWidth = width, oldHeight = height;
        if (pictureHeader() < 0) {
            width = oldWidth;
            height = oldHeight;
            return -1;
        }
        if (width != oldWidth || height != oldHeight || !pool.mbWidth) {
            const int w = width, h = height;
            if (!allocate(w, h)) {
                width = oldWidth;
                height = oldHeight;
                return -1;
            }
        }
        if (pictType == kPictureB) return -1;
        start();
        mbX = mbY = 0;
        decodeSlice();
        while (mbY < mbHeight) {
            if (resync() < 0) break;
            decodeSlice();
        }
        conceal();
        return 0;
    }
};

struct H263Decoder : SoftDecoder {
    std::unique_ptr<H263> h = std::make_unique<H263>();

    const char* name() const override { return "h263"; }

    void reset() override { h = std::make_unique<H263>(); }

    bool decode(const uint8_t* data, size_t size, int64_t stamp, const PictureSink& shown) override {
        if (!size || size > (64u << 20)) return false;
        if (h->decodeFrame(data, size) < 0 || !h->cur.ptr) return false;
        Picture p;
        p.stamp = stamp;
        p.width = h->width;
        p.height = h->height;
        p.chromaWidth = (h->width + 1) / 2;
        p.chromaHeight = (h->height + 1) / 2;
        for (int i = 0; i < 3; i++) {
            p.plane[i] = h->cur.data[i];
            p.stride[i] = size_t(h->cur.linesize[i]);
        }
        shown(p);
        return true;
    }
};

}

}

std::unique_ptr<SoftDecoder> openH263(std::string*) { return std::make_unique<h263::H263Decoder>(); }

int h263PictureType(const uint8_t* data, size_t size) {
    BitReader in(data, size);
    uint32_t code = in.read(14);
    for (int64_t left = in.left(); left > 24; left -= 8) {
        code = ((code << 8) | in.read(8)) & 0x3FFFFF;
        if (code == 0x20) break;
    }
    if (code != 0x20) return -1;
    in.skip(8);
    if (in.read(2) != 2) return -1;
    in.skip(3);
    const uint32_t format = in.read(3);
    if (format != 6 && format != 7) return int(in.read(1));
    const uint32_t ufep = in.read(3);
    if (ufep == 1) in.skip(18);
    else if (ufep) return -1;
    switch (in.read(3)) {
    case 0:
    case 7:
        return 0;
    case 1:
    case 2:
        return 1;
    case 3:
        return 2;
    default:
        return -1;
    }
}

}

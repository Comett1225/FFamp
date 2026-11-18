#include "intrax8.h"
#include "h263.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace absent {

namespace {

inline uint8_t clip8(int value) { return uint8_t(value < 0 ? 0 : value > 255 ? 255 : value); }

constexpr int kW0 = 2048, kW1 = 2841, kW2 = 2676, kW3 = 2408, kW5 = 1609, kW6 = 1108, kW7 = 565;

void wmv2Row(int16_t* b) {
    const int a1 = kW1 * b[1] + kW7 * b[7];
    const int a7 = kW7 * b[1] - kW1 * b[7];
    const int a5 = kW5 * b[5] + kW3 * b[3];
    const int a3 = kW3 * b[5] - kW5 * b[3];
    const int a2 = kW2 * b[2] + kW6 * b[6];
    const int a6 = kW6 * b[2] - kW2 * b[6];
    const int a0 = kW0 * b[0] + kW0 * b[4];
    const int a4 = kW0 * b[0] - kW0 * b[4];
    const int s1 = int(181u * unsigned(a1 - a5 + a7 - a3) + 128) >> 8;
    const int s2 = int(181u * unsigned(a1 - a5 - a7 + a3) + 128) >> 8;
    b[0] = int16_t((a0 + a2 + a1 + a5 + (1 << 7)) >> 8);
    b[1] = int16_t((a4 + a6 + s1 + (1 << 7)) >> 8);
    b[2] = int16_t((a4 - a6 + s2 + (1 << 7)) >> 8);
    b[3] = int16_t((a0 - a2 + a7 + a3 + (1 << 7)) >> 8);
    b[4] = int16_t((a0 - a2 - a7 - a3 + (1 << 7)) >> 8);
    b[5] = int16_t((a4 - a6 - s2 + (1 << 7)) >> 8);
    b[6] = int16_t((a4 + a6 - s1 + (1 << 7)) >> 8);
    b[7] = int16_t((a0 + a2 - a1 - a5 + (1 << 7)) >> 8);
}

void wmv2Column(int16_t* b) {
    const int a1 = (kW1 * b[8] + kW7 * b[56] + 4) >> 3;
    const int a7 = (kW7 * b[8] - kW1 * b[56] + 4) >> 3;
    const int a5 = (kW5 * b[40] + kW3 * b[24] + 4) >> 3;
    const int a3 = (kW3 * b[40] - kW5 * b[24] + 4) >> 3;
    const int a2 = (kW2 * b[16] + kW6 * b[48] + 4) >> 3;
    const int a6 = (kW6 * b[16] - kW2 * b[48] + 4) >> 3;
    const int a0 = (kW0 * b[0] + kW0 * b[32]) >> 3;
    const int a4 = (kW0 * b[0] - kW0 * b[32]) >> 3;
    const int s1 = int(181u * unsigned(a1 - a5 + a7 - a3) + 128) >> 8;
    const int s2 = int(181u * unsigned(a1 - a5 - a7 + a3) + 128) >> 8;
    b[0] = int16_t((a0 + a2 + a1 + a5 + (1 << 13)) >> 14);
    b[8] = int16_t((a4 + a6 + s1 + (1 << 13)) >> 14);
    b[16] = int16_t((a4 - a6 + s2 + (1 << 13)) >> 14);
    b[24] = int16_t((a0 - a2 + a7 + a3 + (1 << 13)) >> 14);
    b[32] = int16_t((a0 - a2 - a7 - a3 + (1 << 13)) >> 14);
    b[40] = int16_t((a4 - a6 - s2 + (1 << 13)) >> 14);
    b[48] = int16_t((a4 + a6 - s1 + (1 << 13)) >> 14);
    b[56] = int16_t((a0 + a2 - a1 - a5 + (1 << 13)) >> 14);
}

void wmv2Idct(int16_t* block) {
    for (int i = 0; i < 64; i += 8) wmv2Row(block + i);
    for (int i = 0; i < 8; i++) wmv2Column(block + i);
}

struct X8Tables {
    Vlc ac[2][2][8], dc[2][8], orient[2][4];
};

void buildPairs(Vlc& vlc, int bits, int count, const uint8_t (*pairs)[2]) {
    uint8_t lengths[80], symbols[80];
    for (int i = 0; i < count; i++) {
        symbols[i] = pairs[i][0];
        lengths[i] = pairs[i][1];
    }
    vlc.buildInOrder(bits, size_t(count), lengths, symbols, 0);
}

const X8Tables& x8Tables() {
    static const X8Tables tables = [] {
        X8Tables t;
        for (int i = 0; i < 2; i++)
            for (int j = 0; j < 2; j++)
                for (int k = 0; k < 8; k++) buildPairs(t.ac[i][j][k], 9, 77, h263::kX8AcQuant[i][j][k]);
        for (int i = 0; i < 2; i++)
            for (int j = 0; j < 8; j++) buildPairs(t.dc[i][j], 9, 34, h263::kX8DcQuant[i][j]);
        for (int i = 0; i < 2; i++) buildPairs(t.orient[0][i], 7, 12, h263::kX8OrientHighquant[i]);
        for (int i = 0; i < 4; i++) buildPairs(t.orient[1][i], 7, 12, h263::kX8OrientLowquant[i]);
        return t;
    }();
    return tables;
}

constexpr uint32_t acDecode(int bits, bool run, int runOffset, int levelOffset) {
    return uint32_t(bits) | (run ? 0xFFu << 8 : 0u) | uint32_t(runOffset) << 16 | uint32_t(levelOffset) << 24;
}

constexpr uint32_t kAcDecodeTable[] = {
    acDecode(3, true, 16, 0), acDecode(3, true, 24, 0), acDecode(2, true, 4, 1), acDecode(3, true, 8, 1),
    acDecode(5, true, 32, 0), acDecode(4, true, 16, 1),
    acDecode(2, false, 0, 4), acDecode(2, false, 0, 8), acDecode(2, false, 0, 12), acDecode(3, false, 0, 16), acDecode(3, false, 0, 24),
    acDecode(2, false, 1, 3), acDecode(3, false, 1, 7),
    acDecode(2, true, 16, 0), acDecode(2, true, 20, 0), acDecode(2, true, 24, 0), acDecode(2, true, 28, 0), acDecode(4, true, 32, 0), acDecode(4, true, 48, 0),
    acDecode(2, true, 4, 1), acDecode(3, true, 8, 1), acDecode(4, true, 16, 1),
    acDecode(2, false, 0, 4), acDecode(3, false, 0, 8), acDecode(4, false, 0, 16),
    acDecode(2, false, 1, 3), acDecode(3, false, 1, 7),
};

constexpr uint8_t kCrazyMixRunLevel[32] = {
    0x22, 0x32, 0x33, 0x53, 0x23, 0x42, 0x43, 0x63, 0x24, 0x52, 0x34, 0x73, 0x25, 0x62, 0x44, 0x83,
    0x26, 0x72, 0x35, 0x54, 0x27, 0x82, 0x45, 0x64, 0x28, 0x92, 0x36, 0x74, 0x29, 0xa2, 0x46, 0x84,
};

constexpr uint8_t kDcIndexOffset[] = {0, 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193};

constexpr int16_t kQuantTable[64] = {
    256, 256, 256, 256, 256, 256, 259, 262, 265, 269, 272, 275, 278, 282, 285, 288,
    292, 295, 299, 303, 306, 310, 314, 317, 321, 325, 329, 333, 337, 341, 345, 349,
    353, 358, 362, 366, 371, 375, 379, 384, 389, 393, 398, 403, 408, 413, 417, 422,
    428, 433, 438, 443, 448, 454, 459, 465, 470, 476, 482, 488, 493, 499, 505, 511,
};

constexpr uint16_t kZeroPredictionWeights[128] = {
    640, 640, 669, 480, 708, 354, 748, 257, 792, 198, 760, 143, 808, 101, 772, 72,
    480, 669, 537, 537, 598, 416, 661, 316, 719, 250, 707, 185, 768, 134, 745, 97,
    354, 708, 416, 598, 488, 488, 564, 388, 634, 317, 642, 241, 716, 179, 706, 132,
    257, 748, 316, 661, 388, 564, 469, 469, 543, 395, 571, 311, 655, 238, 660, 180,
    198, 792, 250, 719, 317, 634, 395, 543, 469, 469, 507, 380, 597, 299, 616, 231,
    161, 855, 206, 788, 266, 710, 340, 623, 411, 548, 455, 455, 548, 366, 576, 288,
    122, 972, 159, 914, 211, 842, 276, 758, 341, 682, 389, 584, 483, 483, 520, 390,
    110, 1172, 144, 1107, 193, 1028, 254, 932, 317, 846, 366, 731, 458, 611, 499, 499,
};

constexpr int kArea1 = 0, kArea2 = 8, kArea3 = 16, kArea4 = 17, kArea5 = 25, kArea6 = 33;

void setupSpatialCompensation(const uint8_t* src, uint8_t* dst, ptrdiff_t stride, int& range, int& psum, int edges) {
    if ((edges & 3) == 3) {
        psum = 0x80 * (8 + 1 + 8 + 2);
        range = 0;
        memset(dst, 0x80, 16 + 1 + 16 + 8);
        return;
    }
    int minPix = 256, maxPix = -1, sum = 0;
    if (!(edges & 1)) {
        const uint8_t* ptr = src - 1;
        for (int i = 7; i >= 0; i--) {
            dst[kArea1 + i] = *(ptr - 1);
            const uint8_t c = *ptr;
            sum += c;
            minPix = std::min<int>(minPix, c);
            maxPix = std::max<int>(maxPix, c);
            dst[kArea2 + i] = c;
            ptr += stride;
        }
    }
    if (!(edges & 2)) {
        const uint8_t* ptr = src - stride;
        uint8_t c = 0;
        for (int i = 0; i < 8; i++) {
            c = ptr[i];
            sum += c;
            minPix = std::min<int>(minPix, c);
            maxPix = std::max<int>(maxPix, c);
        }
        if (edges & 4) {
            memset(dst + kArea5, c, 8);
            memcpy(dst + kArea4, ptr, 8);
        } else {
            memcpy(dst + kArea4, ptr, 16);
        }
        memcpy(dst + kArea6, ptr - stride, 8);
    }
    if (edges & 3) {
        const int avg = (sum + 4) >> 3;
        if (edges & 1) memset(dst + kArea1, avg, 8 + 8 + 1);
        else memset(dst + kArea3, avg, 1 + 16 + 8);
        sum += avg * 9;
    } else {
        const uint8_t c = *(src - 1 - stride);
        dst[kArea3] = c;
        sum += c;
    }
    range = maxPix - minPix;
    sum += dst[kArea5] + dst[kArea5 + 1];
    psum = sum;
}

void spatialCompensation(int mode, const uint8_t* src, uint8_t* dst, ptrdiff_t stride) {
    switch (mode) {
    case 0: {
        uint16_t leftSum[2][8] = {}, topSum[2][8] = {};
        for (int i = 0; i < 8; i++) {
            const int a = src[kArea2 + 7 - i] << 4;
            for (int j = 0; j < 8; j++) {
                const int p = std::abs(i - j);
                leftSum[p & 1][j] += uint16_t(a >> (p >> 1));
            }
        }
        for (int i = 0; i < 12; i++) {
            const int a = src[kArea4 + i] << 4;
            for (int j = i < 8 ? 0 : i < 10 ? 5 : 7; j < 8; j++) {
                const int p = std::abs(i - j);
                topSum[p & 1][j] += uint16_t(a >> (p >> 1));
            }
        }
        for (int i = 0; i < 8; i++) {
            topSum[0][i] += uint16_t((topSum[1][i] * 181 + 128) >> 8);
            leftSum[0][i] += uint16_t((leftSum[1][i] * 181 + 128) >> 8);
        }
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                dst[x] = uint8_t((uint32_t(topSum[0][x]) * kZeroPredictionWeights[y * 16 + x * 2] + uint32_t(leftSum[0][y]) * kZeroPredictionWeights[y * 16 + x * 2 + 1] + 0x8000) >> 16);
            }
            dst += stride;
        }
        return;
    }
    case 1:
        for (int y = 0; y < 8; y++, dst += stride)
            for (int x = 0; x < 8; x++) dst[x] = src[kArea4 + std::min(2 * y + x + 2, 15)];
        return;
    case 2:
        for (int y = 0; y < 8; y++, dst += stride) memcpy(dst, src + kArea4 + 1 + y, 8);
        return;
    case 3:
        for (int y = 0; y < 8; y++, dst += stride) memcpy(dst, src + kArea4 + ((y + 1) >> 1), 8);
        return;
    case 4:
        for (int y = 0; y < 8; y++, dst += stride)
            for (int x = 0; x < 8; x++) dst[x] = uint8_t((src[kArea4 + x] + src[kArea6 + x] + 1) >> 1);
        return;
    case 5:
        for (int y = 0; y < 8; y++, dst += stride)
            for (int x = 0; x < 8; x++) dst[x] = 2 * x - y < 0 ? src[kArea2 + 9 + 2 * x - y] : src[kArea4 + x - ((y + 1) >> 1)];
        return;
    case 6:
        for (int y = 0; y < 8; y++, dst += stride) memcpy(dst, src + kArea3 - y, 8);
        return;
    case 7:
        for (int y = 0; y < 8; y++, dst += stride)
            for (int x = 0; x < 8; x++)
                dst[x] = x - 2 * y > 0 ? uint8_t((src[kArea3 - 1 + x - 2 * y] + src[kArea3 + x - 2 * y] + 1) >> 1) : src[kArea2 + 8 - y + (x >> 1)];
        return;
    case 8:
        for (int y = 0; y < 8; y++, dst += stride)
            for (int x = 0; x < 8; x++) dst[x] = uint8_t((src[kArea1 + 7 - y] + src[kArea2 + 7 - y] + 1) >> 1);
        return;
    case 9:
        for (int y = 0; y < 8; y++, dst += stride)
            for (int x = 0; x < 8; x++) dst[x] = src[kArea2 + 6 - std::min(x + y, 6)];
        return;
    case 10:
        for (int y = 0; y < 8; y++, dst += stride)
            for (int x = 0; x < 8; x++) dst[x] = uint8_t((src[kArea2 + 7 - y] * (8 - x) + src[kArea4 + x] * x + 4) >> 3);
        return;
    default:
        for (int y = 0; y < 8; y++, dst += stride)
            for (int x = 0; x < 8; x++) dst[x] = uint8_t((src[kArea2 + 7 - y] * y + src[kArea4 + x] * (8 - y) + 4) >> 3);
        return;
    }
}

void x8LoopFilter(uint8_t* ptr, ptrdiff_t a, ptrdiff_t b, int quant) {
    const int ql = (quant + 10) >> 3;
    for (int i = 0; i < 8; i++, ptr += b) {
        const int p0 = ptr[-5 * a], p1 = ptr[-4 * a], p2 = ptr[-3 * a], p3 = ptr[-2 * a], p4 = ptr[-1 * a];
        const int p5 = ptr[0], p6 = ptr[a], p7 = ptr[2 * a], p8 = ptr[3 * a], p9 = ptr[4 * a];
        int t = (std::abs(p1 - p2) <= ql) + (std::abs(p2 - p3) <= ql) + (std::abs(p3 - p4) <= ql) + (std::abs(p4 - p5) <= ql);
        if (t > 0) {
            t += (std::abs(p5 - p6) <= ql) + (std::abs(p6 - p7) <= ql) + (std::abs(p7 - p8) <= ql) + (std::abs(p8 - p9) <= ql) + (std::abs(p0 - p1) <= ql);
            if (t >= 6) {
                int low = std::min({p1, p3, p5, p8}), high = std::max({p1, p3, p5, p8});
                if (high - low < 2 * quant) {
                    low = std::min({low, p2, p4, p6, p7});
                    high = std::max({high, p2, p4, p6, p7});
                    if (high - low < 2 * quant) {
                        ptr[-2 * a] = uint8_t((4 * p2 + 3 * p3 + 1 * p7 + 4) >> 3);
                        ptr[-1 * a] = uint8_t((3 * p2 + 3 * p4 + 2 * p7 + 4) >> 3);
                        ptr[0] = uint8_t((2 * p2 + 3 * p5 + 3 * p7 + 4) >> 3);
                        ptr[1 * a] = uint8_t((1 * p2 + 3 * p6 + 4 * p7 + 4) >> 3);
                        continue;
                    }
                }
            }
        }
        const int x0 = (2 * p3 - 5 * p4 + 5 * p5 - 2 * p6 + 4) >> 3;
        if (std::abs(x0) < quant) {
            const int x1 = (2 * p1 - 5 * p2 + 5 * p3 - 2 * p4 + 4) >> 3;
            const int x2 = (2 * p5 - 5 * p6 + 5 * p7 - 2 * p8 + 4) >> 3;
            int x = std::abs(x0) - std::min(std::abs(x1), std::abs(x2));
            int m = p4 - p5;
            if (x > 0 && (m ^ x0) < 0) {
                const int sign = m >> 31;
                m = ((m ^ sign) - sign) >> 1;
                x = 5 * x >> 3;
                if (x > m) x = m;
                x = (x ^ sign) - sign;
                ptr[-1 * a] = uint8_t(ptr[-1 * a] - x);
                ptr[0] = uint8_t(ptr[0] + x);
            }
        }
    }
}

}

void wmv2IdctAdd(uint8_t* dest, ptrdiff_t stride, int16_t* block) {
    wmv2Idct(block);
    for (int i = 0; i < 8; i++, dest += stride, block += 8)
        for (int j = 0; j < 8; j++) dest[j] = clip8(dest[j] + block[j]);
}

void wmv2IdctPut(uint8_t* dest, ptrdiff_t stride, int16_t* block) {
    wmv2Idct(block);
    for (int i = 0; i < 8; i++, dest += stride, block += 8)
        for (int j = 0; j < 8; j++) dest[j] = clip8(block[j]);
}

void IntraX8::setSize(int width, int height) {
    mbWidth = width;
    mbHeight = height;
    predictionTable.assign(size_t(mbWidth) * 4, 0);
}

void IntraX8::selectAcTable(int mode) {
    if (acTable[mode]) return;
    acTable[mode] = &x8Tables().ac[quant < 13][mode >> 1][gb->read(3)];
}

int IntraX8::orientation() {
    if (!orientTable) orientTable = &x8Tables().orient[quant < 13][gb->read(1 + (quant < 13))];
    return orientTable->read(*gb);
}

void IntraX8::acRunLevel(int mode, int& run, int& level, int& final) {
    int i = acTable[mode]->read(*gb);
    if (i < 46) {
        if (i < 0) {
            level = final = run = 64;
            return;
        }
        int t = i > 22;
        final = t;
        i -= 23 * t;
        const int l = (0xE50000 >> (i & 0x1E)) & 3;
        t = 0x01030F >> (l << 3);
        run = i & t;
        level = l;
    } else if (i < 73) {
        i -= 46;
        uint32_t sm = kAcDecodeTable[i];
        const uint32_t e = gb->read(int(sm & 0xF));
        sm >>= 8;
        const uint32_t mask = sm & 0xff;
        sm >>= 8;
        run = int((sm & 0xff) + (e & mask));
        level = int((sm >> 8) + (e & ~mask));
        final = i > (58 - 46);
    } else if (i < 75) {
        final = !(i & 1);
        const uint32_t e = gb->read(5);
        run = kCrazyMixRunLevel[e] >> 4;
        level = kCrazyMixRunLevel[e] & 0x0F;
    } else {
        level = int(gb->read(7 - 3 * (i & 1)));
        run = int(gb->read(6));
        final = int(gb->read(1));
    }
}

int IntraX8::dcLevel(int mode, int& level, int& final) {
    if (!dcTable[mode]) dcTable[mode] = &x8Tables().dc[quant < 13][gb->read(3)];
    int i = dcTable[mode]->read(*gb);
    int c = i > 16;
    final = c;
    i -= 17 * c;
    if (i <= 0) {
        level = 0;
        return -i;
    }
    c = (i + 1) >> 1;
    c -= c > 1;
    int e = int(gb->read(c));
    i = kDcIndexOffset[i] + (e >> 1);
    e = -(e & 1);
    level = (i ^ e) - e;
    return 0;
}

bool IntraX8::spatialPredictor(int chroma) {
    int range = 0, sum = 0, q = 0;
    setupSpatialCompensation(dest[chroma], scratchpad, lines[chroma], range, sum, edges);
    if (chroma) {
        orient = chromaOrient;
        q = quantDcChroma;
    } else {
        q = quant;
    }
    flatDc = 0;
    if (range < q || range < 3) {
        orient = 0;
        if (range < 3) {
            flatDc = 1;
            sum += 9;
            predictedDc = sum * 6899 >> 17;
        }
    }
    if (chroma) return true;
    if (range < 2 * quant) {
        if ((edges & 3) == 0) {
            if (orient == 1) orient = 11;
            if (orient == 2) orient = 10;
        } else {
            orient = 0;
        }
        rawOrient = 0;
    } else {
        static const uint8_t table[3][12] = {
            {0, 8, 4, 10, 11, 2, 6, 9, 1, 3, 5, 7},
            {4, 0, 8, 11, 10, 3, 5, 2, 6, 9, 1, 7},
            {8, 0, 4, 10, 11, 1, 7, 2, 6, 9, 3, 5},
        };
        rawOrient = orientation();
        if (rawOrient < 0 || rawOrient >= 12 || orient < 0 || orient > 2) return false;
        orient = table[orient][rawOrient];
    }
    return true;
}

void IntraX8::chromaPrediction() {
    edges = 1 * !(mbX >> 1);
    edges |= 2 * !(mbY >> 1);
    edges |= 4 * (mbX >= (2 * mbWidth - 1));
    rawOrient = 0;
    if (edges & 3) {
        chromaOrient = 4 << ((0xCC >> edges) & 1);
        return;
    }
    chromaOrient = (predictionTable[size_t(2 * mbX - 2)] & 0x03) << 2;
}

void IntraX8::prediction() {
    edges = 1 * !mbX;
    edges |= 2 * !mbY;
    edges |= 4 * (mbX >= (2 * mbWidth - 1));
    switch (edges & 3) {
    case 1:
        estRun = predictionTable[!(mbY & 1)] >> 2;
        orient = 1;
        return;
    case 2:
        estRun = predictionTable[size_t(2 * mbX - 2)] >> 2;
        orient = 2;
        return;
    case 3:
        estRun = 16;
        orient = 0;
        return;
    }
    int b = predictionTable[size_t(2 * mbX + !(mbY & 1))];
    int a = predictionTable[size_t(2 * mbX - 2 + (mbY & 1))];
    int c = predictionTable[size_t(2 * mbX - 2 + !(mbY & 1))];
    estRun = std::min(b, a);
    if ((mbX & mbY) != 0) estRun = std::min(c, estRun);
    estRun >>= 2;
    a &= 3;
    b &= 3;
    c &= 3;
    const int i = int((0xFFEAF4C4u >> (2 * b + 8 * a)) & 3);
    if (i != 3) orient = i;
    else orient = (0xFFEAD8 >> (2 * c + 8 * (quant > 12))) & 3;
}

void IntraX8::acCompensation(int direction, int dc) {
    auto t = [dc](int x) { return (x * dc + 0x8000) >> 16; };
    auto B = [this](int x, int y) -> int16_t& { return block[x + y * 8]; };
    int v;
    switch (direction) {
    case 0:
        v = t(3811);
        B(1, 0) = int16_t(B(1, 0) - v);
        B(0, 1) = int16_t(B(0, 1) - v);
        v = t(487);
        B(2, 0) = int16_t(B(2, 0) - v);
        B(0, 2) = int16_t(B(0, 2) - v);
        v = t(506);
        B(3, 0) = int16_t(B(3, 0) - v);
        B(0, 3) = int16_t(B(0, 3) - v);
        v = t(135);
        B(4, 0) = int16_t(B(4, 0) - v);
        B(0, 4) = int16_t(B(0, 4) - v);
        B(2, 1) = int16_t(B(2, 1) + v);
        B(1, 2) = int16_t(B(1, 2) + v);
        B(3, 1) = int16_t(B(3, 1) + v);
        B(1, 3) = int16_t(B(1, 3) + v);
        v = t(173);
        B(5, 0) = int16_t(B(5, 0) - v);
        B(0, 5) = int16_t(B(0, 5) - v);
        v = t(61);
        B(6, 0) = int16_t(B(6, 0) - v);
        B(0, 6) = int16_t(B(0, 6) - v);
        B(5, 1) = int16_t(B(5, 1) + v);
        B(1, 5) = int16_t(B(1, 5) + v);
        v = t(42);
        B(7, 0) = int16_t(B(7, 0) - v);
        B(0, 7) = int16_t(B(0, 7) - v);
        B(4, 1) = int16_t(B(4, 1) + v);
        B(1, 4) = int16_t(B(1, 4) + v);
        B(4, 4) = int16_t(B(4, 4) + v);
        v = t(1084);
        B(1, 1) = int16_t(B(1, 1) + v);
        break;
    case 1:
        B(0, 1) = int16_t(B(0, 1) - t(6269));
        B(0, 3) = int16_t(B(0, 3) - t(708));
        B(0, 5) = int16_t(B(0, 5) - t(172));
        B(0, 7) = int16_t(B(0, 7) - t(73));
        break;
    case 2:
        B(1, 0) = int16_t(B(1, 0) - t(6269));
        B(3, 0) = int16_t(B(3, 0) - t(708));
        B(5, 0) = int16_t(B(5, 0) - t(172));
        B(7, 0) = int16_t(B(7, 0) - t(73));
        break;
    }
}

bool IntraX8::intraBlock(int chroma) {
    memset(block, 0, sizeof block);
    const int dcMode = chroma ? 2 : !!estRun;
    int dc = 0, final = 0;
    if (dcLevel(dcMode, dc, final)) return false;
    int n = 0, zerosOnly = 0;
    const int stride = lines[chroma];
    bool placed = false;
    if (!final) {
        int quantMatrix = useQuantMatrix, acMode, runLimit;
        if (chroma) {
            acMode = 1;
            runLimit = 64;
        } else {
            if (rawOrient < 3) quantMatrix = 0;
            if (rawOrient > 4) {
                acMode = 0;
                runLimit = 64;
            } else if (estRun > 1) {
                acMode = 2;
                runLimit = estRun;
            } else {
                acMode = 3;
                runLimit = 64;
            }
        }
        selectAcTable(acMode);
        static const int kScans[3] = {0, 2, 3};
        const uint8_t* scan = vc1::kWmv1Scantable[kScans[(0x928548 >> (2 * orient)) & 3]];
        int pos = 0;
        do {
            n++;
            if (n >= runLimit) {
                acMode = 3;
                selectAcTable(3);
            }
            int run = 0, level = 0;
            acRunLevel(acMode, run, level, final);
            pos += run + 1;
            if (pos > 63) return false;
            level = (level + 1) * dquant;
            level += qsum;
            const int sign = -int(gb->read(1));
            level = (level ^ sign) - sign;
            if (quantMatrix) level = (level * kQuantTable[pos]) >> 8;
            block[scan[pos]] = int16_t(level);
        } while (!final);
    } else {
        if (flatDc && unsigned(dc + 1) < 3) {
            const int divide = !chroma ? divideQuantDcLuma : divideQuantDcChroma;
            const int dcQuant = !chroma ? quant : quantDcChroma;
            dc += (predictedDc * divide + (1 << 12)) >> 13;
            const uint8_t value = clip8((dc * dcQuant + 4) >> 3);
            for (int k = 0; k < 8; k++) memset(dest[chroma] + k * stride, value, 8);
            placed = true;
        } else {
            zerosOnly = dc == 0;
        }
    }
    if (!placed) {
        block[0] = int16_t(dc * (!chroma ? quant : quantDcChroma));
        if (unsigned(dc + 1) >= 3 && (edges & 3) != 3) {
            const int direction = (0x6A017C >> (orient * 2)) & 3;
            if (direction != 3) acCompensation(direction, block[0]);
        }
        if (flatDc) {
            for (int k = 0; k < 8; k++) memset(dest[chroma] + k * stride, predictedDc, 8);
        } else {
            spatialCompensation(orient, scratchpad, dest[chroma], stride);
        }
        if (!zerosOnly) wmv2IdctAdd(dest[chroma], stride, block);
    }
    if (!chroma) predictionTable[size_t(mbX * 2 + (mbY & 1))] = uint8_t((n << 2) + 1 * (orient == 4) + 2 * (orient == 8));
    if (loopFilter) {
        uint8_t* ptr = dest[chroma];
        if (!((edges & 2) || (zerosOnly && (orient | 4) == 4))) x8LoopFilter(ptr, stride, 1, quant);
        if (!((edges & 1) || (zerosOnly && (orient | 8) == 8))) x8LoopFilter(ptr, 1, stride, quant);
    }
    return true;
}

int IntraX8::decode(BitReader& in, uint8_t* const data[3], const int linesize[3], int8_t* qscaleTable, int dq, int quantOffset, bool filter) {
    gb = &in;
    dquant = dq;
    quant = dq >> 1;
    qsum = quantOffset;
    loopFilter = filter;
    useQuantMatrix = int(in.read(1));
    if (quant <= 0) return 0;
    for (int i = 0; i < 3; i++) lines[i] = linesize[i];
    divideQuantDcLuma = ((1 << 16) + (quant >> 1)) / quant;
    if (quant < 5) {
        quantDcChroma = quant;
        divideQuantDcChroma = divideQuantDcLuma;
    } else {
        quantDcChroma = quant + ((quant + 3) >> 3);
        divideQuantDcChroma = ((1 << 16) + (quantDcChroma >> 1)) / quantDcChroma;
    }
    std::fill(std::begin(acTable), std::end(acTable), nullptr);
    std::fill(std::begin(dcTable), std::end(dcTable), nullptr);
    orientTable = nullptr;
    auto stopped = [&] { return (mbY >> 1) * mbWidth + ((mbY & 1) ? mbX >> 1 : 0); };
    for (mbY = 0; mbY < mbHeight * 2; mbY++) {
        dest[0] = data[0] + ptrdiff_t(mbY) * linesize[0] * 8;
        dest[1] = data[1] + ptrdiff_t(mbY & ~1) * linesize[1] * 4;
        dest[2] = data[2] + ptrdiff_t(mbY & ~1) * linesize[2] * 4;
        int mbXy = (mbY >> 1) * (mbWidth + 1);
        mbX = 0;
        if (in.left() < 1) return stopped();
        for (; mbX < mbWidth * 2; mbX++) {
            prediction();
            if (!spatialPredictor(0) || !intraBlock(0)) return stopped();
            if (mbX & mbY & 1) {
                chromaPrediction();
                spatialPredictor(1);
                if (!intraBlock(1)) return stopped();
                spatialPredictor(2);
                if (!intraBlock(2)) return stopped();
                dest[1] += 8;
                dest[2] += 8;
                if (qscaleTable) qscaleTable[mbXy] = int8_t(quant);
                mbXy++;
            }
            dest[0] += 8;
        }
    }
    return mbWidth * mbHeight;
}

}

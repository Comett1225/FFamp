#include "soft_video.h"
#include "bits.h"
#include "parallel.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace absent {

namespace {

enum Format { k411, k420, k422 };

constexpr uint8_t kBlocksSd[8] = {112, 112, 112, 112, 80, 80, 0, 0};
constexpr uint8_t kBlocksHd[8] = {80, 80, 80, 80, 80, 80, 64, 64};

struct Profile {
    int dsf, stype, frameSize, segments, channels, height, width;
    Format format;
    int bpm;
    const uint8_t* blockSizes;
};

constexpr Profile kProfiles[] = {
    {0, 0x0, 120000, 10, 1, 480, 720, k411, 6, kBlocksSd},
    {1, 0x0, 144000, 12, 1, 576, 720, k420, 6, kBlocksSd},
    {1, 0x0, 144000, 12, 1, 576, 720, k411, 6, kBlocksSd},
    {0, 0x4, 240000, 10, 2, 480, 720, k422, 6, kBlocksSd},
    {1, 0x4, 288000, 12, 2, 576, 720, k422, 6, kBlocksSd},
    {0, 0x14, 480000, 10, 4, 1080, 1280, k422, 8, kBlocksHd},
    {1, 0x14, 576000, 12, 4, 1080, 1440, k422, 8, kBlocksHd},
    {0, 0x18, 240000, 10, 2, 720, 960, k422, 8, kBlocksHd},
    {1, 0x18, 288000, 12, 2, 720, 960, k422, 8, kBlocksHd},
    {1, 0x1, 144000, 12, 1, 576, 720, k420, 6, kBlocksSd},
};

bool isHd(const Profile& p) { return p.stype & 0x10; }
bool is1080i50(const Profile& p) { return p.stype == 0x14 && p.dsf == 1; }
bool is720p50(const Profile& p) { return p.stype == 0x18 && p.dsf == 1; }

const Profile* frameProfile(const uint8_t* frame, size_t size, const Profile* current) {
    if (size < 480) return nullptr;
    const int dsf = (frame[3] & 0x80) >> 7, stype = frame[451] & 0x1F, pal = (frame[451] & 0x20) != 0;
    if (dsf == 1 && stype == 0 && (frame[4] & 0x07)) return &kProfiles[2];
    if (dsf == 0 && pal && stype == kProfiles[1].stype && size == size_t(kProfiles[1].frameSize)) return &kProfiles[1];
    for (const Profile& p : kProfiles)
        if (dsf == p.dsf && stype == p.stype) return &p;
    if (current && size == size_t(current->frameSize)) return current;
    if ((frame[3] & 0x7F) == 0x3F && frame[451] == 0xFF) return &kProfiles[dsf];
    return nullptr;
}

constexpr uint8_t kZigzagOrder[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
constexpr auto kZigzag = [] {
    std::array<uint8_t, 64> out{};
    for (size_t i = 0; i < 64; i++) out[i] = uint8_t((kZigzagOrder[i] & 7) << 3 | kZigzagOrder[i] >> 3);
    return out;
}();
constexpr uint8_t kZigzag248[64] = {
    0, 8, 1, 9, 16, 24, 2, 10, 17, 25, 32, 40, 48, 56, 33, 41, 18, 26, 3, 11, 4, 12, 19, 27, 34, 42, 49, 57, 50, 58, 35, 43,
    20, 28, 5, 13, 6, 14, 21, 29, 36, 44, 51, 59, 52, 60, 37, 45, 22, 30, 7, 15, 23, 31, 38, 46, 53, 61, 54, 62, 39, 47, 55, 63};

constexpr uint8_t kQuantShifts[22][4] = {
    {3, 3, 4, 4}, {3, 3, 4, 4}, {2, 3, 3, 4}, {2, 3, 3, 4}, {2, 2, 3, 3}, {2, 2, 3, 3}, {1, 2, 2, 3}, {1, 2, 2, 3},
    {1, 1, 2, 2}, {1, 1, 2, 2}, {0, 1, 1, 2}, {0, 1, 1, 2}, {0, 0, 1, 1}, {0, 0, 1, 1}, {0, 0, 0, 1}, {0, 0, 0, 0},
    {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
constexpr uint8_t kQuantOffset[4] = {6, 3, 0, 1};

constexpr uint16_t kWeight88[64] = {
    32768, 16705, 16705, 17734, 17032, 17734, 18205, 18081, 18081, 18205, 18725, 18562, 19195, 18562, 18725, 19266,
    19091, 19705, 19705, 19091, 19266, 21407, 19643, 20267, 20228, 20267, 19643, 21407, 22725, 21826, 20853, 20806,
    20806, 20853, 21826, 22725, 23170, 23170, 21407, 21400, 21407, 23170, 23170, 24598, 23786, 22018, 22018, 23786,
    24598, 25251, 24465, 22654, 24465, 25251, 25972, 25172, 25172, 25972, 26722, 27969, 26722, 29692, 29692, 31521};
constexpr uint16_t kWeight248[64] = {
    32768, 16384, 16705, 16705, 17734, 17734, 17734, 17734, 18081, 18081, 18725, 18725, 21407, 21407, 19091, 19091,
    19195, 19195, 18205, 18205, 18725, 18725, 19705, 19705, 20267, 20267, 21826, 21826, 23170, 23170, 20806, 20806,
    20267, 20267, 19266, 19266, 21407, 21407, 20853, 20853, 21400, 21400, 23786, 23786, 24465, 24465, 22018, 22018,
    23170, 23170, 22725, 22725, 24598, 24598, 24465, 24465, 25172, 25172, 27969, 27969, 25972, 25972, 29692, 29692};
constexpr uint16_t kWeight1080Y[64] = {
    128, 16, 16, 17, 17, 17, 18, 18, 18, 18, 18, 18, 19, 18, 18, 19, 19, 19, 19, 19, 19, 42, 38, 40, 40, 40, 38, 42, 44, 43, 41, 41,
    41, 41, 43, 44, 45, 45, 42, 42, 42, 45, 45, 48, 46, 43, 43, 46, 48, 49, 48, 44, 48, 49, 101, 98, 98, 101, 104, 109, 104, 116, 116, 123};
constexpr uint16_t kWeight1080C[64] = {
    128, 16, 16, 17, 17, 17, 25, 25, 25, 25, 26, 25, 26, 25, 26, 26, 26, 27, 27, 26, 26, 42, 38, 40, 40, 40, 38, 42, 44, 43, 41, 41,
    41, 41, 43, 44, 91, 91, 84, 84, 84, 91, 91, 96, 93, 86, 86, 93, 96, 197, 191, 177, 191, 197, 203, 197, 197, 203, 209, 219, 209, 232, 232, 246};
constexpr uint16_t kWeight720Y[64] = {
    128, 16, 16, 17, 17, 17, 18, 18, 18, 18, 18, 18, 19, 18, 18, 19, 19, 19, 19, 19, 19, 42, 38, 40, 40, 40, 38, 42, 44, 43, 41, 41,
    41, 41, 43, 44, 68, 68, 63, 63, 63, 68, 68, 96, 92, 86, 86, 92, 96, 98, 96, 88, 96, 98, 202, 196, 196, 202, 208, 218, 208, 232, 232, 246};
constexpr uint16_t kWeight720C[64] = {
    128, 24, 24, 26, 26, 26, 36, 36, 36, 36, 36, 36, 38, 36, 36, 38, 38, 38, 38, 38, 38, 84, 76, 80, 80, 80, 76, 84, 88, 86, 82, 82,
    82, 82, 86, 88, 182, 182, 168, 168, 168, 182, 182, 192, 186, 192, 172, 186, 192, 394, 382, 354, 382, 394, 406, 394, 394, 406, 418, 438, 418, 464, 464, 492};

constexpr uint8_t kVlcLength[89] = {
    2, 3, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7, 7, 7, 7, 7, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 9, 9, 9, 9, 9, 9, 9,
    9, 9, 9, 9, 9, 9, 9, 9, 9, 10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 11, 11, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 12, 12, 12, 12, 12, 12};
constexpr uint8_t kVlcRun[89] = {
    0, 0, 127, 1, 0, 0, 2, 1, 0, 0, 3, 4, 0, 0, 5, 6, 2, 1, 1, 0, 0, 0, 7, 8, 9, 10, 3, 4, 2, 1, 1, 1, 0, 0, 0, 0, 0, 0, 11, 12, 13, 14, 5,
    6, 3, 4, 2, 2, 1, 0, 0, 0, 0, 0, 5, 3, 3, 2, 1, 1, 1, 0, 1, 6, 4, 3, 1, 1, 1, 2, 3, 4, 5, 7, 8, 9, 10, 7, 8, 4, 3, 2, 2, 2, 2, 2, 1, 1, 1};
constexpr uint8_t kVlcLevel[89] = {
    1, 2, 0, 1, 3, 4, 1, 2, 5, 6, 1, 1, 7, 8, 1, 1, 2, 3, 4, 9, 10, 11, 1, 1, 1, 1, 2, 2, 3, 5, 6, 7, 12, 13, 14, 15, 16, 17, 1, 1, 1, 1, 2,
    2, 3, 3, 4, 5, 8, 18, 19, 20, 21, 22, 3, 4, 5, 6, 9, 10, 11, 0, 0, 3, 4, 6, 12, 13, 14, 0, 0, 0, 0, 2, 2, 2, 2, 3, 3, 5, 7, 7, 8, 9, 10,
    11, 15, 16, 17};

struct Code {
    int8_t length = 0;
    uint8_t run = 0;
    int16_t level = 0;
};

struct Vlc {
    std::array<Code, 1024> first;
    std::vector<Code> rest;

    Vlc() {
        struct Symbol {
            int length, run, level;
        };
        std::vector<Symbol> symbols;
        auto add = [&](int length, int run, int level) {
            if (level) {
                symbols.push_back({length + 1, run, level});
                symbols.push_back({length + 1, run, -level});
            } else {
                symbols.push_back({length, run, 0});
            }
        };
        for (int i = 0; i < 89; i++) add(kVlcLength[i], kVlcRun[i], kVlcLevel[i]);
        for (int i = 0; i < 64; i++) add(13, i, 0);
        for (int i = 0; i < 256; i++) add(15, 0, i);
        std::array<int, 1024> longest{};
        uint32_t code = 0;
        std::vector<uint32_t> codes;
        for (const Symbol& s : symbols) {
            codes.push_back(code >> (32 - s.length));
            if (s.length > 10) longest[size_t(codes.back() >> (s.length - 10))] = std::max(longest[size_t(codes.back() >> (s.length - 10))], s.length);
            code += 1u << (32 - s.length);
        }
        std::array<int, 1024> start{};
        for (size_t p = 0; p < 1024; p++) {
            if (!longest[p]) continue;
            start[p] = int(rest.size());
            rest.resize(rest.size() + (size_t(1) << (longest[p] - 10)));
            first[p].length = int8_t(-(longest[p] - 10));
            first[p].level = int16_t(start[p]);
        }
        for (size_t i = 0; i < symbols.size(); i++) {
            const Symbol& s = symbols[i];
            const Code entry{0, uint8_t(s.run + 1), int16_t(s.level)};
            if (s.length <= 10) {
                for (uint32_t k = 0; k < (1u << (10 - s.length)); k++) {
                    first[(codes[i] << (10 - s.length)) + k] = entry;
                    first[(codes[i] << (10 - s.length)) + k].length = int8_t(s.length);
                }
                continue;
            }
            const size_t p = codes[i] >> (s.length - 10);
            const int extra = longest[p] - 10, own = s.length - 10;
            const uint32_t low = codes[i] & ((1u << own) - 1);
            for (uint32_t k = 0; k < (1u << (extra - own)); k++) {
                Code& slot = rest[size_t(start[p]) + (low << (extra - own)) + k];
                slot = entry;
                slot.length = int8_t(longest[p]);
            }
        }
    }

    Code look(uint64_t cache) const {
        const Code& c = first[size_t(cache >> 54)];
        if (c.length >= 0) return c;
        return rest[size_t(c.level) + size_t((cache << 10) >> (64 + c.length))];
    }
};

const Vlc& vlc() {
    static const Vlc table;
    return table;
}

struct BitWriter {
    uint8_t* data;
    int64_t capacity;
    int64_t count = 0;

    void put(uint32_t value, int n) {
        for (int i = n - 1; i >= 0; i--, count++) {
            if (count >= capacity) return;
            if ((value >> i) & 1) data[count >> 3] |= uint8_t(0x80 >> (count & 7));
        }
    }
};

void copyBits(BitWriter& out, BitReader& in) {
    for (int64_t left = in.left(); left > 0;) {
        const int n = int(std::min<int64_t>(left, 25));
        out.put(in.read(n), n);
        left -= n;
    }
}

constexpr int kW1 = 22725, kW2 = 21407, kW3 = 19266, kW4 = 16383, kW5 = 12873, kW6 = 8867, kW7 = 4520;

inline int16_t saturate16(int value) { return int16_t(std::clamp(value, -32768, 32767)); }
inline uint8_t clip8(int value) { return uint8_t(std::clamp(value, 0, 255)); }

void idctRow(int16_t* row) {
    if (!(row[1] | row[2] | row[3] | row[4] | row[5] | row[6] | row[7])) {
        const int16_t value = int16_t(uint16_t(row[0] * 8));
        for (int i = 0; i < 8; i++) row[i] = value;
        return;
    }
    const int r0 = row[0], r1 = row[1], r2 = row[2], r3 = row[3], r4 = row[4], r5 = row[5], r6 = row[6], r7 = row[7];
    const unsigned base = unsigned(kW4 * r0) + (1u << 10);
    const unsigned a0 = base + unsigned(kW2 * r2) + unsigned(kW4 * r4 + kW6 * r6);
    const unsigned a1 = base + unsigned(kW6 * r2) - unsigned(kW4 * r4 + kW2 * r6);
    const unsigned a2 = base - unsigned(kW6 * r2) - unsigned(kW4 * r4 - kW2 * r6);
    const unsigned a3 = base - unsigned(kW2 * r2) + unsigned(kW4 * r4 - kW6 * r6);
    const unsigned b0 = unsigned(kW1 * r1 + kW3 * r3) + unsigned(kW5 * r5 + kW7 * r7);
    const unsigned b1 = unsigned(kW3 * r1 - kW7 * r3) - unsigned(kW1 * r5 + kW5 * r7);
    const unsigned b2 = unsigned(kW5 * r1 - kW1 * r3) + unsigned(kW7 * r5 + kW3 * r7);
    const unsigned b3 = unsigned(kW7 * r1 - kW5 * r3) + unsigned(kW3 * r5 - kW1 * r7);
    row[0] = int16_t(int(a0 + b0) >> 11);
    row[7] = int16_t(int(a0 - b0) >> 11);
    row[1] = int16_t(int(a1 + b1) >> 11);
    row[6] = int16_t(int(a1 - b1) >> 11);
    row[2] = int16_t(int(a2 + b2) >> 11);
    row[5] = int16_t(int(a2 - b2) >> 11);
    row[3] = int16_t(int(a3 + b3) >> 11);
    row[4] = int16_t(int(a3 - b3) >> 11);
}

template <bool First>
void idctPass(int16_t* b) {
    for (int i = 0; i < 8; i++) {
        const int r0 = First ? b[i] : int16_t(b[i] + 32), r1 = b[8 + i], r2 = b[16 + i], r3 = b[24 + i], r4 = b[32 + i], r5 = b[40 + i], r6 = b[48 + i], r7 = b[56 + i];
        const unsigned base = unsigned(kW4 * r0) + (First ? 1u << 10 : 0u);
        const unsigned a0 = base + unsigned(kW2 * r2) + unsigned(kW4 * r4 + kW6 * r6);
        const unsigned a1 = base + unsigned(kW6 * r2) - unsigned(kW4 * r4 + kW2 * r6);
        const unsigned a2 = base - unsigned(kW6 * r2) - unsigned(kW4 * r4 - kW2 * r6);
        const unsigned a3 = base - unsigned(kW2 * r2) + unsigned(kW4 * r4 - kW6 * r6);
        const unsigned b0 = unsigned(kW1 * r1 + kW3 * r3) + unsigned(kW5 * r5 + kW7 * r7);
        const unsigned b1 = unsigned(kW3 * r1 - kW7 * r3) - unsigned(kW1 * r5 + kW5 * r7);
        const unsigned b2 = unsigned(kW5 * r1 - kW1 * r3) + unsigned(kW7 * r5 + kW3 * r7);
        const unsigned b3 = unsigned(kW7 * r1 - kW5 * r3) + unsigned(kW3 * r5 - kW1 * r7);
        const unsigned sums[8] = {a0 + b0, a1 + b1, a2 + b2, a3 + b3, a3 - b3, a2 - b2, a1 - b1, a0 - b0};
        const bool flat = First && !(r1 | r2 | r3 | r4 | r5 | r6 | r7);
        for (int k = 0; k < 8; k++) b[k * 8 + i] = flat ? int16_t(uint16_t(r0 * 8)) : saturate16(int(sums[k]) >> (First ? 11 : 20));
    }
}

void idct88(int16_t* block) {
    idctPass<true>(block);
    for (int i = 0; i < 8; i++)
        for (int j = i + 1; j < 8; j++) std::swap(block[i * 8 + j], block[j * 8 + i]);
    idctPass<false>(block);
}

void idctPut88(uint8_t* dest, ptrdiff_t stride, int16_t* block) {
    idct88(block);
    for (int y = 0; y < 8; y++, dest += stride)
        for (int x = 0; x < 8; x++) dest[x] = clip8(block[y * 8 + x]);
}

constexpr int kC1 = 2676, kC2 = 1108;

void idct4ColumnPut(uint8_t* dest, ptrdiff_t stride, const int16_t* col) {
    const int a0 = col[0], a1 = col[16], a2 = col[32], a3 = col[48];
    const int c0 = (a0 + a2) * 2048 + (1 << 16), c2 = (a0 - a2) * 2048 + (1 << 16);
    const int c1 = a1 * kC1 + a3 * kC2, c3 = a1 * kC2 - a3 * kC1;
    dest[0] = clip8((c0 + c1) >> 17);
    dest[stride] = clip8((c2 + c3) >> 17);
    dest[2 * stride] = clip8((c2 - c3) >> 17);
    dest[3 * stride] = clip8((c0 - c1) >> 17);
}

void idctPut248(uint8_t* dest, ptrdiff_t stride, int16_t* block) {
    for (int i = 0; i < 4; i++) {
        int16_t* p = block + i * 16;
        for (int k = 0; k < 8; k++) {
            const int a0 = p[k], a1 = p[8 + k];
            p[k] = int16_t(a0 + a1);
            p[8 + k] = int16_t(a0 - a1);
        }
    }
    for (int i = 0; i < 8; i++) idctRow(block + i * 8);
    for (int i = 0; i < 8; i++) {
        idct4ColumnPut(dest + i, 2 * stride, block + i);
        idct4ColumnPut(dest + stride + i, 2 * stride, block + 8 + i);
    }
}

void putBlock8x4(const int16_t* block, uint8_t* p, ptrdiff_t stride) {
    for (int i = 0; i < 4; i++, block += 8, p += stride)
        for (int j = 0; j < 8; j++) p[j] = clip8(block[j]);
}

struct Chunk {
    uint16_t offset = 0;
    uint16_t places[5] = {};
};

void placeChunk(const Profile& d, int chan, int seq, int slot, uint16_t* table) {
    static const uint8_t off[] = {2, 6, 8, 0, 4};
    static const uint8_t shuf1[] = {36, 18, 54, 0, 72};
    static const uint8_t shuf2[] = {24, 12, 36, 0, 48};
    static const uint8_t shuf3[] = {18, 9, 27, 0, 36};
    static const uint8_t lStart[] = {0, 4, 9, 13, 18, 22, 27, 31, 36, 40};
    static const uint8_t lStartShuffled[] = {9, 4, 13, 0, 18};
    static const uint8_t serpent1[] = {0, 1, 2, 2, 1, 0, 0, 1, 2, 2, 1, 0, 0, 1, 2, 2, 1, 0, 0, 1, 2, 2, 1, 0, 0, 1, 2};
    static const uint8_t serpent2[] = {0, 1, 2, 3, 4, 5, 5, 4, 3, 2, 1, 0, 0, 1, 2, 3, 4, 5, 5, 4, 3, 2, 1, 0, 0, 1, 2, 3, 4, 5};
    static const uint8_t remap[][2] = {
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 1}, {0, 2}, {0, 3}, {10, 0}, {10, 1}, {10, 2}, {10, 3}, {20, 0}, {20, 1},
        {20, 2}, {20, 3}, {30, 0}, {30, 1}, {30, 2}, {30, 3}, {40, 0}, {40, 1}, {40, 2}, {40, 3}, {50, 0}, {50, 1}, {50, 2}, {50, 3},
        {60, 0}, {60, 1}, {60, 2}, {60, 3}, {70, 0}, {70, 1}, {70, 2}, {70, 3}, {0, 64}, {0, 65}, {0, 66}, {10, 64}, {10, 65}, {10, 66},
        {20, 64}, {20, 65}, {20, 66}, {30, 64}, {30, 65}, {30, 66}, {40, 64}, {40, 65}, {40, 66}, {50, 64}, {50, 65}, {50, 66}, {60, 64},
        {60, 65}, {60, 66}, {70, 64}, {70, 65}, {70, 66}, {0, 67}, {20, 67}, {40, 67}, {60, 67}};
    for (int m = 0; m < 5; m++) {
        int x = 0, y = 0;
        if (d.width == 1440) {
            const int blk = (chan * 11 + seq) * 27 + slot;
            if (chan == 0 && seq == 11) {
                x = m * 27 + slot;
                if (x < 90) {
                    y = 0;
                } else {
                    x = (x - 90) * 2;
                    y = 67;
                }
            } else {
                const int i = (4 * chan + blk + off[m]) % 11, k = (blk / 11) % 27;
                x = shuf1[m] + (chan & 1) * 9 + k % 9;
                y = (i * 3 + k / 9) * 2 + (chan >> 1) + 1;
            }
            table[m] = uint16_t((x << 1) | (y << 9));
        } else if (d.width == 1280) {
            const int blk = (chan * 10 + seq) * 27 + slot;
            const int i = (4 * chan + (seq / 5) + 2 * blk + off[m]) % 10, k = (blk / 5) % 27;
            x = shuf1[m] + (chan & 1) * 9 + k % 9;
            y = (i * 3 + k / 9) * 2 + (chan >> 1) + 4;
            if (x >= 80) {
                x = remap[y][0] + ((x - 80) << (y > 59));
                y = remap[y][1];
            }
            table[m] = uint16_t((x << 1) | (y << 9));
        } else if (d.width == 960) {
            const int blk = (chan * 10 + seq) * 27 + slot;
            const int i = (4 * chan + (seq / 5) + 2 * blk + off[m]) % 10, k = (blk / 5) % 27 + (i & 1) * 3;
            x = shuf2[m] + k % 6 + 6 * (chan & 1);
            y = lStart[i] + k / 6 + 45 * (chan >> 1);
            table[m] = uint16_t((x << 1) | (y << 9));
        } else if (d.format == k422) {
            x = shuf3[m] + slot / 3;
            y = serpent1[slot] + ((((seq + off[m]) % d.segments) << 1) + chan) * 3;
            table[m] = uint16_t((x << 1) | (y << 8));
        } else if (d.format == k420) {
            x = shuf3[m] + slot / 3;
            y = serpent1[slot] + ((seq + off[m]) % d.segments) * 3;
            table[m] = uint16_t((x << 1) | (y << 9));
        } else {
            const int i = (seq + off[m]) % d.segments, k = slot + ((m == 1 || m == 2) ? 3 : 0);
            x = lStartShuffled[m] + k / 6;
            y = serpent2[k] + i * 6;
            if (x > 21) y = y * 2 - i * 6;
            table[m] = uint16_t((x << 2) | (y << 8));
        }
    }
}

struct Block {
    const uint32_t* factor = nullptr;
    const uint8_t* scan = nullptr;
    int pos = 0;
    bool wide = false;
    int partialCount = 0;
    uint32_t partialBits = 0;
};

void decodeAc(BitReader& in, Block& mb, int16_t* block) {
    const Vlc& table = vlc();
    const int64_t last = in.position() + in.left();
    uint32_t prefix = mb.partialBits;
    int prefixCount = mb.partialCount;
    mb.partialCount = 0;
    int pos = mb.pos;
    for (;;) {
        uint64_t cache = in.cache();
        if (prefixCount) cache = (uint64_t(prefix) << 32) | (cache >> prefixCount);
        const Code code = table.look(cache);
        const int64_t index = in.position() - prefixCount;
        if (index + code.length > last) {
            mb.partialCount = int(last - index);
            mb.partialBits = uint32_t(cache >> 32) & ~(0xFFFFFFFFu >> mb.partialCount);
            in.skip(last - in.position());
            break;
        }
        if (prefixCount) {
            if (code.length >= prefixCount) {
                in.skip(code.length - prefixCount);
                prefixCount = 0;
            } else {
                prefix <<= code.length;
                prefixCount -= code.length;
            }
        } else {
            in.skip(code.length);
        }
        pos += code.run;
        if (pos >= 64) break;
        block[mb.scan[pos]] = int16_t((uint32_t(code.level) * mb.factor[pos] + 8192u) >> 14);
    }
    mb.pos = pos;
}

class DvDecoder : public SoftDecoder {
public:
    const char* name() const override { return "dv"; }
    void reset() override {}

    bool decode(const uint8_t* data, size_t size, int64_t stamp, const PictureSink& shown) override {
        const Profile* found = frameProfile(data, size, sys);
        if (!found || size < size_t(found->frameSize)) return false;
        if (found != sys) setProfile(*found);
        frame = data;
        frameBytes = size;
        parallelFor(chunks.size(), [&](size_t i) { decodeChunk(chunks[i]); });
        Picture p;
        p.stamp = stamp;
        p.width = sys->width;
        p.height = sys->height;
        p.chromaWidth = sys->format == k411 ? (sys->width + 3) / 4 : sys->width / 2;
        p.chromaHeight = sys->format == k420 ? sys->height / 2 : sys->height;
        for (int i = 0; i < 3; i++) {
            p.plane[i] = planes[i].data();
            p.stride[i] = size_t(pitch[i]);
        }
        shown(p);
        return true;
    }

private:
    const Profile* sys = nullptr;
    std::vector<Chunk> chunks;
    std::vector<uint32_t> factors;
    std::vector<uint8_t> planes[3];
    int pitch[3] = {};
    const uint8_t* frame = nullptr;
    size_t frameBytes = 0;

    void setProfile(const Profile& d) {
        sys = &d;
        chunks.clear();
        int p = 0;
        for (int c = 0; c < d.channels; c++) {
            for (int s = 0; s < d.segments; s++) {
                p += 6;
                for (int j = 0; j < 27; j++) {
                    p += !(j % 3);
                    if (!(is1080i50(d) && c != 0 && s == 11) && !(is720p50(d) && s > 9)) {
                        Chunk chunk;
                        placeChunk(d, c, s, j, chunk.places);
                        chunk.offset = uint16_t(p);
                        chunks.push_back(chunk);
                    }
                    p += 5;
                }
            }
        }
        factors.assign(2 * 4 * 16 * 64, 0);
        uint32_t* first = factors.data();
        uint32_t* second = factors.data() + (isHd(d) ? 4096 : 2816);
        if (isHd(d)) {
            static const uint8_t step[16] = {1, 1, 2, 3, 4, 5, 6, 7, 8, 16, 18, 20, 22, 24, 28, 52};
            const uint16_t* one = d.height == 720 ? kWeight720Y : kWeight1080Y;
            const uint16_t* two = d.height == 720 ? kWeight720C : kWeight1080C;
            for (int c = 0; c < 4; c++)
                for (int s = 0; s < 16; s++)
                    for (int i = 0; i < 64; i++) {
                        *first++ = uint32_t(step[s] << (c + 9)) * one[i];
                        *second++ = uint32_t(step[s] << (c + 9)) * two[i];
                    }
        } else {
            static const uint8_t areas[4] = {6, 21, 43, 64};
            const uint16_t* weight = kWeight88;
            for (int j = 0; j < 2; j++, weight = kWeight248)
                for (int s = 0; s < 22; s++)
                    for (int i = 0, c = 0; c < 4; c++)
                        for (; i < areas[c]; i++) {
                            *first = uint32_t(weight[i]) << (kQuantShifts[s][c] + 1);
                            *second++ = *first++ << 1;
                        }
        }
        const int chromaWidth = d.format == k411 ? (d.width + 3) / 4 : d.width / 2;
        const int chromaHeight = d.format == k420 ? d.height / 2 : d.height;
        pitch[0] = d.width;
        pitch[1] = pitch[2] = chromaWidth;
        planes[0].assign(size_t(d.width) * size_t(d.height + 16), 0);
        planes[1].assign(size_t(chromaWidth) * size_t(chromaHeight + 16), 0);
        planes[2].assign(size_t(chromaWidth) * size_t(chromaHeight + 16), 0);
    }

    void put(const Block& mb, uint8_t* dest, ptrdiff_t stride, int16_t* block) const {
        if (mb.wide) idctPut248(dest, stride, block);
        else idctPut88(dest, stride, block);
    }

    void decodeChunk(const Chunk& chunk) {
        const Profile& d = *sys;
        const bool hd = isHd(d);
        const int bpm = d.bpm;
        alignas(16) int16_t blocks[5 * 8][64];
        Block info[5 * 8];
        alignas(8) uint8_t mbBuffer[80 + 64];
        alignas(8) uint8_t vsBuffer[80 * 5 + 64];
        bool fieldMode[5] = {};
        bool vsDamaged = false;
        bool mbDamaged[5] = {};
        for (bool retried = false;; retried = true) {
            memset(blocks, 0, sizeof(blocks));
            memset(vsBuffer, 0, sizeof(vsBuffer));
            BitWriter vs{vsBuffer, 80 * 5 * 8};
            const uint8_t* at = frame + size_t(chunk.offset) * 80;
            int sta = 0;
            for (int m = 0; m < 5; m++) {
                const int quant = at[3] & 0x0F;
                if ((at[3] >> 4) == 0x0E) vsDamaged = true;
                if (!m) sta = at[3] >> 4;
                else if (sta != (at[3] >> 4)) vsDamaged = true;
                at += 4;
                memset(mbBuffer, 0, sizeof(mbBuffer));
                BitWriter mbBits{mbBuffer, 80 * 8};
                Block* mb = info + m * bpm;
                int16_t* block = blocks[m * bpm];
                fieldMode[m] = false;
                for (int j = 0; j < bpm; j++, mb++, block += 64) {
                    const int last = d.blockSizes[j];
                    BitReader in(at, frameBytes - size_t(at - frame), last);
                    int dc = int(in.read(9));
                    dc = dc >= 256 ? dc - 512 : dc;
                    const int dctMode = int(in.read(1));
                    const int class1 = int(in.read(2));
                    if (hd) {
                        mb->wide = false;
                        mb->scan = kZigzag.data();
                        mb->factor = &factors[size_t((j >= 4) * 4 * 16 * 64 + class1 * 16 * 64 + quant * 64)];
                        fieldMode[m] = fieldMode[m] || (!j && dctMode);
                    } else {
                        mb->wide = dctMode != 0;
                        mb->scan = dctMode ? kZigzag248 : kZigzag.data();
                        mb->factor = &factors[size_t((class1 == 3) * 2 * 22 * 64 + dctMode * 22 * 64 + (quant + kQuantOffset[class1]) * 64)];
                    }
                    block[0] = int16_t(dc * 4 + 1024);
                    at += last >> 3;
                    mb->pos = 0;
                    mb->partialCount = 0;
                    decodeAc(in, *mb, block);
                    if (mb->pos >= 64) copyBits(mbBits, in);
                    if (mb->pos >= 64 && mb->pos < 127) vsDamaged = mbDamaged[m] = true;
                }
                if (mbDamaged[m]) continue;
                mb = info + m * bpm;
                block = blocks[m * bpm];
                BitReader in(mbBuffer, sizeof(mbBuffer), mbBits.count);
                int j = 0;
                for (; j < bpm; j++, block += 64, mb++) {
                    if (mb->pos < 64 && in.left() > 0) {
                        decodeAc(in, *mb, block);
                        if (mb->pos < 64) break;
                        if (mb->pos < 127) vsDamaged = mbDamaged[m] = true;
                    }
                }
                if (j >= bpm) copyBits(vs, in);
            }
            BitReader in(vsBuffer, sizeof(vsBuffer), vs.count);
            for (int m = 0; m < 5; m++) {
                for (int j = 0; j < bpm; j++) {
                    Block& mb = info[m * bpm + j];
                    if (mb.pos < 64 && in.left() > 0 && !vsDamaged) decodeAc(in, mb, blocks[m * bpm + j]);
                    if (mb.pos >= 64 && mb.pos < 127) vsDamaged = true;
                }
            }
            if (!vsDamaged || retried) break;
        }
        place(chunk, blocks, info, fieldMode);
    }

    void place(const Chunk& chunk, int16_t (*blocks)[64], const Block* info, const bool* fieldMode) {
        const Profile& d = *sys;
        const ptrdiff_t lumaPitch = pitch[0], chromaPitch = pitch[1];
        int16_t* block = blocks[0];
        const Block* mb = info;
        for (int m = 0; m < 5; m++) {
            int mbX = chunk.places[m] & 0xFF, mbY = chunk.places[m] >> 8;
            if (d.height == 720 && !(frame[1] & 0x0C)) mbY -= mbY > 17 ? 18 : -72;
            const int field = fieldMode[m] ? 1 : 0;
            const ptrdiff_t yStride = d.format == k420 || (d.format == k411 && mbX >= 88) || (d.height >= 720 && mbY != 134) ? lumaPitch << (field ? 0 : 3) : 16;
            uint8_t* y = planes[0].data() + ((ptrdiff_t(mbY) * lumaPitch + mbX) << 3);
            if (mbY == 134 && field) {
                idct88(block);
                idct88(block + 64);
                idct88(block + 128);
                idct88(block + 192);
                const ptrdiff_t two = lumaPitch << 1;
                putBlock8x4(block, y, two);
                putBlock8x4(block + 32, y + 16, two);
                putBlock8x4(block + 64, y + 8, two);
                putBlock8x4(block + 96, y + 24, two);
                putBlock8x4(block + 128, y + lumaPitch, two);
                putBlock8x4(block + 160, y + 16 + lumaPitch, two);
                putBlock8x4(block + 192, y + 8 + lumaPitch, two);
                putBlock8x4(block + 224, y + 24 + lumaPitch, two);
            } else {
                const ptrdiff_t line = lumaPitch << field;
                put(mb[0], y, line, block);
                if (d.stype == 4) {
                    put(mb[2], y + 8, line, block + 128);
                } else {
                    put(mb[1], y + 8, line, block + 64);
                    put(mb[2], y + yStride, line, block + 128);
                    put(mb[3], y + 8 + yStride, line, block + 192);
                }
            }
            mb += 4;
            block += 4 * 64;
            const ptrdiff_t chromaOffset = ((ptrdiff_t(mbY >> (d.format == k420 ? 1 : 0)) * chromaPitch + (mbX >> (d.format == k411 ? 2 : 1))) << 3);
            for (int j = 2; j; j--) {
                uint8_t* c = planes[j].data() + chromaOffset;
                if (d.format == k411 && mbX >= 88) {
                    alignas(8) uint8_t pixels[64];
                    put(*mb, pixels, 8, block);
                    for (int row = 0; row < 8; row++)
                        for (int x = 0; x < 4; x++) {
                            c[row * chromaPitch + x] = pixels[row * 8 + x];
                            c[(row + 8) * chromaPitch + x] = pixels[row * 8 + 4 + x];
                        }
                    block += 64;
                    mb++;
                } else if (mbY == 134 && field) {
                    idct88(block);
                    idct88(block + 64);
                    const ptrdiff_t two = chromaPitch << 1;
                    putBlock8x4(block, c, two);
                    putBlock8x4(block + 32, c + 8, two);
                    putBlock8x4(block + 64, c + chromaPitch, two);
                    putBlock8x4(block + 96, c + 8 + chromaPitch, two);
                    mb += 2;
                    block += 128;
                } else {
                    const ptrdiff_t cStride = mbY == 134 ? 8 : chromaPitch << (field ? 0 : 3);
                    const ptrdiff_t line = chromaPitch << field;
                    put(*mb++, c, line, block);
                    block += 64;
                    if (d.bpm == 8) {
                        put(*mb++, c + cStride, line, block);
                        block += 64;
                    }
                }
            }
        }
    }
};

}

std::unique_ptr<SoftDecoder> openDv() { return std::make_unique<DvDecoder>(); }

}

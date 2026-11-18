#include "own_video.h"
#include "bits.h"
#include "read_common.h"

namespace absent {

namespace {

const uint8_t kLog2Run[41] = {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24};

int midPred(int a, int b, int c) { return std::max(std::min(a, b), std::min(std::max(a, b), c)); }

struct Range {
    int low = 0;
    int range = 0;
    const uint8_t* at = nullptr;
    const uint8_t* start = nullptr;
    const uint8_t* end = nullptr;
    int overread = 0;
    uint8_t zero[256] = {};
    uint8_t one[256] = {};

    void begin(const uint8_t* data, size_t size) {
        start = at = data;
        end = data + size;
        range = 0xFF00;
        overread = 0;
        low = size >= 2 ? int(be16(data)) : 0xFF00;
        at += std::min<size_t>(2, size);
        if (low >= 0xFF00) {
            low = 0xFF00;
            end = at;
        }
    }

    void states(int factor, int most) {
        const int64_t unit = int64_t(1) << 32;
        memset(zero, 0, sizeof zero);
        memset(one, 0, sizeof one);
        int last = 0;
        int64_t p = unit / 2;
        for (int i = 0; i < 128; i++) {
            int p8 = int((256 * p + unit / 2) >> 32);
            if (p8 <= last) p8 = last + 1;
            if (last && last < 256 && p8 <= most) one[last] = uint8_t(p8);
            p += ((unit - p) * factor + unit / 2) >> 32;
            last = p8;
        }
        for (int i = 256 - most; i <= most; i++) {
            if (one[i]) continue;
            p = (int64_t(i) * unit + 128) >> 8;
            p += ((unit - p) * factor + unit / 2) >> 32;
            int p8 = int((256 * p + unit / 2) >> 32);
            if (p8 <= i) p8 = i + 1;
            if (p8 > most) p8 = most;
            one[i] = uint8_t(p8);
        }
        for (int i = 1; i < 255; i++) zero[i] = uint8_t(256 - one[256 - i]);
    }

    void refill() {
        range <<= 8;
        low <<= 8;
        if (at < end) low += *at++;
        else ++overread;
    }

    int bit(uint8_t* state) {
        const int part = (range * *state) >> 8;
        range -= part;
        if (low < range) {
            *state = zero[*state];
            if (range < 0x100) refill();
            return 0;
        }
        low -= range;
        *state = one[*state];
        range = part;
        if (range < 0x100) refill();
        return 1;
    }

    int symbol(uint8_t* state, bool signedValue) {
        if (bit(state)) return 0;
        int e = 0;
        while (bit(state + 1 + std::min(e, 9))) {
            if (++e > 31) return -1;
        }
        unsigned a = 1;
        for (int i = e - 1; i >= 0; i--) a += a + unsigned(bit(state + 22 + std::min(i, 9)));
        const int sign = -(signedValue && bit(state + 11 + std::min(e, 10)));
        return int((a ^ unsigned(sign)) - unsigned(sign));
    }
};

const int kDefaultFactor = int(0.05 * double(int64_t(1) << 32));

struct VlcState {
    uint32_t errorSum = 4;
    int16_t drift = 0;
    int8_t bias = 0;
    uint8_t count = 1;
};

struct PlaneState {
    int table = 0;
    int contexts = 0;
    std::vector<uint8_t> states;
    std::vector<VlcState> vlc;
};

struct Slice {
    int x = 0, y = 0, width = 0, height = 0;
    int codingMode = 0, rctBy = 1, rctRy = 1;
    bool reset = false;
    int runIndex = 0;
    PlaneState planes[4];
    Range c;
    BitReader gb;
};

struct Ffv1Decoder : SoftDecoder {
    int width = 0;
    int height = 0;
    int version = 0;
    int combined = 0;
    int ac = 0;
    int colorspace = 0;
    int bits = 8;
    bool chroma = true;
    int shiftX = 1, shiftY = 1;
    bool transparency = false;
    int planeCount = 2;
    int hSlices = 1, vSlices = 1;
    int tableCount = 1;
    bool ec = false;
    bool flt = false;
    bool keyOk = false;
    int16_t tables[8][5][256] = {};
    int contextCount[8] = {};
    uint8_t transition[256] = {};
    std::vector<std::vector<uint8_t>> initial;
    std::vector<Slice> slices;
    int sliceCount = 1;
    Color trackColor;
    Planes out;
    std::vector<uint8_t> grey;

    const char* name() const override { return "ffv1"; }
    void reset() override {}

    static int readTable(Range& c, int16_t* table, int scale) {
        uint8_t state[32];
        memset(state, 128, sizeof state);
        int v = 0, i = 0;
        for (; i < 128; v++) {
            const unsigned length = unsigned(c.symbol(state, false)) + 1u;
            if (length > unsigned(128 - i) || !length) return -1;
            for (unsigned k = 0; k < length; ++k) table[i++] = int16_t(scale * v);
        }
        for (i = 1; i < 128; i++) table[256 - i] = int16_t(-table[i]);
        table[128] = int16_t(-table[127]);
        return 2 * v - 1;
    }

    static int readTables(Range& c, int16_t table[5][256]) {
        int count = 1;
        for (int i = 0; i < 5; i++) {
            const int got = readTable(c, table[i], count);
            if (got < 0) return -1;
            count *= got;
            if (count > 32768) return -1;
        }
        return (count + 1) / 2;
    }

    void defaultTransition() {
        Range rc;
        rc.states(kDefaultFactor, 256 - 8);
        for (int i = 1; i < 256; i++) transition[i] = rc.one[i];
    }

    bool extra(const std::vector<uint8_t>& data) {
        if (data.size() < 4) return false;
        Range c;
        c.begin(data.data(), data.size());
        c.states(kDefaultFactor, 256 - 8);
        uint8_t state[32];
        memset(state, 128, sizeof state);
        version = c.symbol(state, false);
        if (version < 2 || version > 4) return false;
        combined = version << 16;
        if (version > 2) {
            c.end -= 4;
            const int micro = c.symbol(state, false);
            if (micro < 0 || micro > 65535) return false;
            combined += micro;
        }
        ac = c.symbol(state, false);
        if (ac == 2) {
            for (int i = 1; i < 256; i++) transition[i] = uint8_t(c.symbol(state, true) + c.one[i]);
        } else {
            defaultTransition();
        }
        colorspace = c.symbol(state, false);
        bits = c.symbol(state, false);
        chroma = c.bit(state);
        shiftX = c.symbol(state, false);
        shiftY = c.symbol(state, false);
        transparency = c.bit(state);
        if (colorspace < 0 || colorspace > 1) return false;
        planeCount = 1 + (chroma || version < 4) + transparency;
        hSlices = 1 + c.symbol(state, false);
        vSlices = 1 + c.symbol(state, false);
        if (shiftX > 4 || shiftY > 4 || shiftX < 0 || shiftY < 0) return false;
        if (hSlices < 1 || vSlices < 1 || hSlices > width || vSlices > height || hSlices > 1024 / vSlices) return false;
        tableCount = c.symbol(state, false);
        if (tableCount < 1 || tableCount > 8) return false;
        for (int i = 0; i < tableCount; i++) {
            contextCount[i] = readTables(c, tables[i]);
            if (contextCount[i] < 0) return false;
        }
        initial.assign(size_t(tableCount), {});
        uint8_t state2[32][32];
        memset(state2, 128, sizeof state2);
        for (int i = 0; i < tableCount; i++) {
            initial[size_t(i)].assign(size_t(contextCount[i]) * 32, 128);
            if (c.bit(state)) {
                for (int j = 0; j < contextCount[i]; j++)
                    for (int k = 0; k < 32; k++) {
                        const int pred = j ? initial[size_t(i)][size_t(j - 1) * 32 + size_t(k)] : 128;
                        initial[size_t(i)][size_t(j) * 32 + size_t(k)] = uint8_t((pred + c.symbol(state2[k], true)) & 0xFF);
                    }
            }
        }
        if (version > 2) {
            ec = c.symbol(state, false) != 0;
            if (combined >= 0x30003) c.symbol(state, false);
            if (combined >= 0x40004) flt = c.symbol(state, false) != 0;
        }
        return true;
    }

    int sliceCoord(int size, int s, int count, int shift) const {
        const int unit = 1 << shift, aligned = (size + unit - 1) & ~(unit - 1);
        if (combined <= 0x40002) return int(int64_t(size) * s / count);
        int v = int((2LL * aligned * s + count * unit) / (2LL * count * unit) * unit);
        return v == aligned ? size : v;
    }

    void layoutSlices() {
        slices.assign(size_t(hSlices * vSlices), Slice());
        for (int i = 0; i < hSlices * vSlices; ++i) {
            Slice& s = slices[size_t(i)];
            const int sx = i % hSlices, sy = i / hSlices;
            s.x = sliceCoord(width, sx, hSlices, shiftX);
            s.width = sliceCoord(width, sx + 1, hSlices, shiftX) - s.x;
            s.y = sliceCoord(height, sy, vSlices, shiftY);
            s.height = sliceCoord(height, sy + 1, vSlices, shiftY) - s.y;
        }
    }

    bool header(Range& c) {
        uint8_t state[32];
        memset(state, 128, sizeof state);
        int contexts = -1;
        if (version < 2) {
            const int v = c.symbol(state, false);
            if (v < 0 || v >= 2) return false;
            version = v;
            ac = c.symbol(state, false);
            if (ac == 2) {
                for (int i = 1; i < 256; i++) {
                    const int st = c.symbol(state, true) + c.one[i];
                    if (st < 1 || st > 255) return false;
                    transition[i] = uint8_t(st);
                }
            } else {
                defaultTransition();
            }
            colorspace = c.symbol(state, false);
            bits = version > 0 ? c.symbol(state, false) : (bits ? bits : 8);
            chroma = c.bit(state);
            shiftX = c.symbol(state, false);
            shiftY = c.symbol(state, false);
            transparency = c.bit(state);
            planeCount = 2 + transparency;
            if (shiftX > 4 || shiftY > 4 || shiftX < 0 || shiftY < 0) return false;
        }
        if (bits == 0) bits = 8;
        if (bits < 0 || bits > 16 || colorspace < 0 || colorspace > 1 || flt) return false;
        if (colorspace == 1 && (shiftX || shiftY)) return false;
        if (colorspace == 0 && transparency && !chroma) return false;
        if (colorspace == 0 && bits > 8 && bits != 9 && bits != 10 && bits != 12 && bits != 14 && bits != 16) return false;
        if (colorspace == 1 && bits > 8 && bits != 9 && bits != 10 && bits != 12 && bits != 14 && bits != 16) return false;
        if (colorspace == 1 && bits == 9 && transparency) return false;
        if (version < 2) {
            contexts = readTables(c, tables[0]);
            if (contexts < 0) return false;
            tableCount = 1;
            contextCount[0] = contexts;
            initial.assign(1, std::vector<uint8_t>(size_t(contexts) * 32, 128));
            sliceCount = hSlices * vSlices;
        } else if (version < 3) {
            sliceCount = c.symbol(state, false);
        } else {
            const uint8_t* p = c.end;
            const ptrdiff_t trailer = 3 + 5 * ec;
            for (sliceCount = 0; sliceCount < 1024 && trailer < p - c.start; sliceCount++) {
                const ptrdiff_t size = ptrdiff_t(be24(p - trailer));
                if (size + trailer > p - c.start) break;
                p -= size + trailer;
            }
        }
        if (sliceCount <= 0 || sliceCount > int(slices.size())) return false;
        for (int j = 0; j < sliceCount; j++) {
            Slice& s = slices[size_t(j)];
            if (version == 2) {
                const int sx = c.symbol(state, false), sy = c.symbol(state, false);
                const int sw = c.symbol(state, false) + 1, sh = c.symbol(state, false) + 1;
                if (sx < 0 || sy < 0 || sw <= 0 || sh <= 0 || sx > hSlices - sw || sy > vSlices - sh) return false;
                s.x = int(int64_t(sx) * width / hSlices);
                s.y = int(int64_t(sy) * height / vSlices);
                s.width = int(int64_t(sx + sw) * width / hSlices) - s.x;
                s.height = int(int64_t(sy + sh) * height / vSlices) - s.y;
            }
            for (int i = 0; i < planeCount; i++) {
                PlaneState& p = s.planes[i];
                if (version == 2) {
                    const int index = c.symbol(state, false);
                    if (index < 0 || index >= tableCount) return false;
                    p.table = index;
                    contexts = contextCount[index];
                }
                if (version <= 2) {
                    p.contexts = contexts;
                    p.states.clear();
                    p.vlc.clear();
                }
            }
        }
        return true;
    }

    void prepare(Slice& s) {
        for (int j = 0; j < planeCount; j++) {
            PlaneState& p = s.planes[j];
            if (ac != 0 && p.states.size() != size_t(p.contexts) * 32) p.states.assign(size_t(p.contexts) * 32, 128);
            if (ac == 0 && p.vlc.size() != size_t(p.contexts)) p.vlc.assign(size_t(p.contexts), VlcState());
        }
        if (ac == 2) {
            for (int j = 1; j < 256; j++) {
                s.c.one[j] = transition[j];
                s.c.zero[256 - j] = uint8_t(256 - s.c.one[j]);
            }
        }
    }

    void clear(Slice& s) {
        for (int i = 0; i < planeCount; i++) {
            PlaneState& p = s.planes[i];
            if (ac != 0) {
                if (size_t(p.table) < initial.size() && initial[size_t(p.table)].size() >= p.states.size()) memcpy(p.states.data(), initial[size_t(p.table)].data(), p.states.size());
                else std::fill(p.states.begin(), p.states.end(), 128);
            } else {
                std::fill(p.vlc.begin(), p.vlc.end(), VlcState());
            }
        }
    }

    bool sliceHeader(Slice& s) {
        Range& c = s.c;
        uint8_t state[32];
        memset(state, 128, sizeof state);
        const int sx = c.symbol(state, false), sy = c.symbol(state, false);
        const int sw = c.symbol(state, false) + 1, sh = c.symbol(state, false) + 1;
        if (sx < 0 || sy < 0 || sw <= 0 || sh <= 0 || sx > hSlices - sw || sy > vSlices - sh) return false;
        s.x = sliceCoord(width, sx, hSlices, shiftX);
        s.y = sliceCoord(height, sy, vSlices, shiftY);
        s.width = sliceCoord(width, sx + sw, hSlices, shiftX) - s.x;
        s.height = sliceCoord(height, sy + sh, vSlices, shiftY) - s.y;
        if (s.width <= 0 || s.height <= 0 || s.x + s.width > width || s.y + s.height > height) return false;
        if (ac == 0 && s.width >= (1 << 23)) return false;
        for (int i = 0; i < planeCount; i++) {
            const int index = c.symbol(state, false);
            if (index < 0 || index >= tableCount) return false;
            PlaneState& p = s.planes[i];
            p.table = index;
            if (p.contexts < contextCount[index]) {
                p.states.clear();
                p.vlc.clear();
            }
            p.contexts = contextCount[index];
        }
        c.symbol(state, false);
        c.symbol(state, false);
        c.symbol(state, false);
        s.reset = false;
        s.codingMode = 0;
        if (version > 3) {
            s.reset = c.bit(state);
            s.codingMode = c.symbol(state, false);
            if (s.codingMode != 1 && colorspace != 0) {
                s.rctBy = c.symbol(state, false);
                s.rctRy = c.symbol(state, false);
                if (s.rctBy < 0 || s.rctRy < 0 || s.rctBy + s.rctRy > 4) return false;
            }
            if (combined >= 0x40004 && c.symbol(state, false) != 0) return false;
        }
        return true;
    }

    int vlcSymbol(BitReader& gb, VlcState& st, int depth) {
        int k = 0;
        for (int i = st.count; i < int(st.errorSum); i += i) k++;
        if (k > depth) k = depth;
        const uint32_t window = gb.peek(32);
        const int log = window ? 31 - __builtin_clz(window) : 0;
        unsigned v;
        if (log > 31 - 12) {
            v = (window >> (log - k)) + (unsigned(30 - log) << k);
            gb.skip(32 + k - log);
        } else {
            gb.skip(12);
            v = gb.read(depth) + 11;
        }
        int value = int((v >> 1) ^ -(v & 1));
        value ^= (2 * st.drift + st.count) >> 31;
        int folded = value + st.bias;
        folded = depth == 8 ? int(int8_t(folded)) : int(unsigned(folded) << (32 - depth)) >> (32 - depth);
        int drift = st.drift, count = st.count;
        st.errorSum += uint32_t(std::abs(value));
        drift += value;
        if (count == 128) {
            count >>= 1;
            drift >>= 1;
            st.errorSum >>= 1;
        }
        count++;
        if (drift <= -count) {
            st.bias = int8_t(std::max(st.bias - 1, -128));
            drift = std::max(drift + count, -count + 1);
        } else if (drift > 0) {
            st.bias = int8_t(std::min(st.bias + 1, 127));
            drift = std::min(drift - count, 0);
        }
        st.drift = int16_t(drift);
        st.count = uint8_t(count);
        return folded;
    }

    bool inputEnd(Slice& s, int mode) const { return mode != 0 ? s.c.overread > 2 : s.gb.left() < 1; }

    template <typename T>
    bool line(Slice& s, int w, T* sample[2], int planeIndex, int depth, int mode) {
        PlaneState& p = s.planes[planeIndex];
        const int16_t (*q)[256] = tables[p.table];
        if (depth == 0) {
            for (int x = 0; x < w; x++) sample[1][x] = 0;
            return true;
        }
        if (inputEnd(s, mode)) return false;
        if (s.codingMode == 1) {
            for (int x = 0; x < w; x++) {
                int v = 0;
                for (int i = 0; i < depth; i++) {
                    uint8_t state = 128;
                    v += v + s.c.bit(&state);
                }
                sample[1][x] = T(v);
            }
            return true;
        }
        int runCount = 0, runMode = 0, runIndex = s.runIndex;
        const bool five = q[3][127] || q[4][127];
        for (int x = 0; x < w; x++) {
            if (!(x & 1023) && inputEnd(s, mode)) return false;
            const T* cur = sample[1] + x;
            const T* last = sample[0] + x;
            const int LT = last[-1], Tp = last[0], RT = last[1], L = cur[-1];
            int context = q[0][(L - LT) & 255] + q[1][(LT - Tp) & 255] + q[2][(Tp - RT) & 255];
            if (five) context += q[3][(cur[-2] - L) & 255] + q[4][(cur[0] - Tp) & 255];
            int sign = 0;
            if (context < 0) {
                context = -context;
                sign = 1;
            }
            if (context >= p.contexts) return false;
            int diff;
            if (mode != 0) {
                diff = s.c.symbol(&p.states[size_t(context) * 32], true);
            } else {
                if (context == 0 && runMode == 0) runMode = 1;
                if (runMode) {
                    if (runCount == 0 && runMode == 1) {
                        if (s.gb.read(1)) {
                            runCount = 1 << kLog2Run[runIndex];
                            if (x + runCount <= w) runIndex++;
                        } else {
                            runCount = kLog2Run[runIndex] ? int(s.gb.read(kLog2Run[runIndex])) : 0;
                            if (runIndex) runIndex--;
                            runMode = 2;
                        }
                        if (runIndex > 40) return false;
                    }
                    if (sample[1][x - 1] == sample[0][x - 1]) {
                        while (runCount > 1 && w - x > 1) {
                            sample[1][x] = sample[0][x];
                            x++;
                            runCount--;
                        }
                    } else {
                        while (runCount > 1 && w - x > 1) {
                            sample[1][x] = T(midPred(sample[1][x - 1], sample[1][x - 1] + sample[0][x] - sample[0][x - 1], sample[0][x]));
                            x++;
                            runCount--;
                        }
                    }
                    runCount--;
                    if (runCount < 0) {
                        runMode = 0;
                        runCount = 0;
                        diff = vlcSymbol(s.gb, p.vlc[size_t(context)], depth);
                        if (diff >= 0) diff++;
                    } else {
                        diff = 0;
                    }
                } else {
                    diff = vlcSymbol(s.gb, p.vlc[size_t(context)], depth);
                }
            }
            if (sign) diff = int(-unsigned(diff));
            const int predicted = midPred(sample[1][x - 1], sample[1][x - 1] + sample[0][x] - sample[0][x - 1], sample[0][x]);
            sample[1][x] = T((unsigned(predicted) + unsigned(diff)) & ((1u << depth) - 1));
        }
        s.runIndex = runIndex;
        return true;
    }

    template <typename T>
    void swapRows(T* sample[2], int w) {
        std::swap(sample[0], sample[1]);
        sample[1][-1] = sample[0][0];
        sample[0][w] = sample[0][w - 1];
    }

    bool plane(Slice& s, int planeIndex, int outPlane, int x0, int y0, int w, int h, int mode) {
        std::vector<int16_t> buffer(size_t(2 * (w + 6)), 0);
        int16_t* sample[2] = {buffer.data() + 3, buffer.data() + w + 6 + 3};
        s.runIndex = 0;
        for (int y = 0; y < h; y++) {
            swapRows(sample, w);
            if (!line(s, w, sample, planeIndex, bits <= 8 ? 8 : bits, mode)) return false;
            if (bits <= 8) {
                uint8_t* row = out.row(outPlane, y0 + y) + x0;
                for (int x = 0; x < w; x++) row[x] = uint8_t(sample[1][x]);
            } else {
                uint16_t* row = out.wide(outPlane, y0 + y) + x0;
                for (int x = 0; x < w; x++) row[x] = uint16_t(sample[1][x]);
            }
        }
        return true;
    }

    template <typename T>
    bool rgb(Slice& s, int mode) {
        const int w = s.width, h = s.height;
        int depth[4], offset = 1 << bits;
        for (int& d : depth) d = bits;
        if (s.codingMode == 0) {
            depth[0] = bits;
            depth[1] = depth[2] = bits + 1;
            if (combined < 0x40008) {
                depth[0]++;
                if (transparency) depth[3]++;
            }
        }
        if (s.codingMode == 1) mode = 1;
        std::vector<T> buffer(size_t(8 * (w + 6)), 0);
        T* sample[4][2];
        for (int k = 0; k < 4; k++) {
            sample[k][0] = buffer.data() + size_t(k * 2) * size_t(w + 6) + 3;
            sample[k][1] = buffer.data() + size_t(k * 2 + 1) * size_t(w + 6) + 3;
        }
        s.runIndex = 0;
        const bool narrow = bits <= 8, swapped = !narrow && sizeof(T) == 2 && !transparency;
        for (int y = 0; y < h; y++) {
            for (int p = 0; p < 3 + transparency; p++) {
                swapRows(sample[p], w);
                if (!line(s, w, sample[p], (p + 1) / 2, depth[p], mode)) return false;
            }
            for (int x = 0; x < w; x++) {
                int g = sample[0][1][x], b = sample[1][1][x], r = sample[2][1][x];
                const int a = sample[3][1][x];
                if (s.codingMode != 1) {
                    b -= offset;
                    r -= offset;
                    g -= (b * s.rctBy + r * s.rctRy) >> 2;
                    b += g;
                    r += g;
                }
                const int px = s.x + x, py = s.y + y;
                if (narrow) {
                    out.row(0, py)[px] = uint8_t(g);
                    out.row(1, py)[px] = uint8_t(b);
                    out.row(2, py)[px] = uint8_t(r);
                    if (transparency) out.row(3, py)[px] = uint8_t(a);
                } else {
                    out.wide(0, py)[px] = uint16_t(swapped ? b : g);
                    out.wide(1, py)[px] = uint16_t(swapped ? g : b);
                    out.wide(2, py)[px] = uint16_t(r);
                    if (transparency) out.wide(3, py)[px] = uint16_t(a);
                }
            }
        }
        return true;
    }

    bool slice(Slice& s, bool key) {
        int mode = ac;
        if (version > 2) {
            prepare(s);
            if (!sliceHeader(s)) return false;
        }
        if (s.codingMode == 1) mode = 1;
        prepare(s);
        if (key || s.reset) clear(s);
        if (mode == 0) {
            if (combined >= 0x30002) {
                uint8_t state = 129;
                s.c.bit(&state);
            }
            const ptrdiff_t used = version > 2 || (!s.x && !s.y) ? s.c.at - s.c.start - 1 : 0;
            if (used < 0 || used > s.c.end - s.c.start) return false;
            s.gb = BitReader(s.c.start + used, size_t(s.c.end - s.c.start - used));
        }
        if (colorspace == 0) {
            const int cw = -((-s.width) >> shiftX), ch = -((-s.height) >> shiftY), cx = s.x >> shiftX, cy = s.y >> shiftY;
            if (!plane(s, 0, 0, s.x, s.y, s.width, s.height, mode)) return false;
            if (chroma) {
                if (!plane(s, 1, 1, cx, cy, cw, ch, mode) || !plane(s, 1, 2, cx, cy, cw, ch, mode)) return false;
            }
            if (transparency && !plane(s, version >= 4 && !chroma ? 1 : 2, 3, s.x, s.y, s.width, s.height, mode)) return false;
            return true;
        }
        return bits == 16 ? rgb<int32_t>(s, mode) : rgb<int16_t>(s, mode);
    }

    bool decode(const uint8_t* data, size_t size, int64_t stamp, const PictureSink& sink) override {
        if (size < 2) return false;
        Range c;
        c.begin(data, size);
        c.states(kDefaultFactor, 256 - 8);
        uint8_t keystate = 128;
        const bool key = c.bit(&keystate);
        if (key) {
            keyOk = false;
            if (!header(c)) return false;
            keyOk = true;
        } else if (!keyOk) {
            return false;
        }
        if (colorspace == 1) {
            out.rgb(width, height, bits <= 8 ? 8 : bits, transparency);
        } else {
            const int cw = chroma ? -((-width) >> shiftX) : (width + 1) / 2, ch = chroma ? -((-height) >> shiftY) : (height + 1) / 2;
            out.shape(width, height, cw, ch, bits <= 8 ? 8 : bits, transparency);
            out.color = trackColor;
            if (!out.color.matrix) out.color.matrix = 1;
            if (!chroma) {
                out.color.full = true;
                const uint16_t middle = uint16_t(1 << ((bits <= 8 ? 8 : bits) - 1));
                for (int plane = 1; plane <= 2; ++plane) {
                    if (bits <= 8) std::fill(out.data[plane].begin(), out.data[plane].end(), uint8_t(128));
                    else {
                        uint16_t* v = reinterpret_cast<uint16_t*>(out.data[plane].data());
                        std::fill(v, v + out.data[plane].size() / 2, middle);
                    }
                }
            }
        }
        const uint8_t* end = data + size;
        for (int i = sliceCount - 1; i >= 0; i--) {
            Slice& s = slices[size_t(i)];
            size_t length = size_t(end - data);
            if (i || version > 2) {
                const size_t trailer = 3 + 5 * ec;
                if (trailer > size_t(end - data)) return false;
                length = be24(end - trailer) + trailer;
            }
            if (size_t(end - data) < length) return false;
            const uint8_t* place = i ? end - length : data;
            end -= length;
            if (i) {
                s.c.begin(place, length);
                s.c.states(kDefaultFactor, 256 - 8);
            } else {
                s.c = c;
                s.c.end = place + length;
            }
        }
        std::vector<char> ok(size_t(sliceCount), 0);
        inParallel(sliceCount * 64, [&](int from, int to) {
            for (int i = (from + 63) / 64; i < (to + 63) / 64 && i < sliceCount; ++i) ok[size_t(i)] = slice(slices[size_t(i)], key);
        });
        for (char good : ok)
            if (!good) return false;
        sink(out.picture(stamp));
        return true;
    }
};

}

std::unique_ptr<SoftDecoder> openFfv1(const Track& track, std::string* error) {
    auto d = std::make_unique<Ffv1Decoder>();
    d->width = track.width;
    d->height = track.height;
    d->trackColor = track.color;
    std::vector<uint8_t> extra = track.config;
    if (track.configType == fourcc("bmih")) extra.erase(extra.begin(), extra.begin() + std::min<size_t>(40, extra.size()));
    if (!extra.empty() && !d->extra(extra)) {
        if (error) *error = "this FFV1 configuration is not decoded";
        return nullptr;
    }
    d->layoutSlices();
    return d;
}

}

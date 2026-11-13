#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace absent {

struct VlcEntry {
    int32_t symbol = -1;
    int32_t length = 0;
};

class Vlc {
public:
    int bits = 0;
    std::vector<VlcEntry> table;

    template <typename C, typename L>
    void build(int indexBits, int count, const C* codes, size_t codeStep, const L* lengths, size_t lengthStep) {
        std::vector<Code> longer, shorter;
        for (int i = 0; i < count; ++i) {
            const int length = int(lengths[size_t(i) * lengthStep]);
            if (length <= 0 || length > 32) continue;
            Code c{uint32_t(codes[size_t(i) * codeStep]) << (32 - length), length, i};
            (length > indexBits ? longer : shorter).push_back(c);
        }
        std::sort(longer.begin(), longer.end(), [](const Code& a, const Code& b) { return a.code < b.code; });
        longer.insert(longer.end(), shorter.begin(), shorter.end());
        bits = indexBits;
        table.clear();
        fill(indexBits, longer.data(), int(longer.size()));
    }

private:
    struct Code {
        uint32_t code;
        int length;
        int symbol;
    };

    int fill(int indexBits, Code* codes, int count) {
        const int start = int(table.size());
        const int size = 1 << indexBits;
        table.resize(table.size() + size_t(size), VlcEntry{0, 0});
        for (int i = 0; i < count; ++i) {
            int n = codes[i].length;
            const uint32_t code = codes[i].code;
            if (n <= indexBits) {
                const int j = int(code >> (32 - indexBits));
                const int many = 1 << (indexBits - n);
                for (int k = 0; k < many; ++k) table[size_t(start + j + k)] = VlcEntry{codes[i].symbol, n};
                continue;
            }
            n -= indexBits;
            const uint32_t prefix = code >> (32 - indexBits);
            int subBits = n;
            codes[i].length = n;
            codes[i].code = code << indexBits;
            int k = i + 1;
            for (; k < count; ++k) {
                const int m = codes[k].length - indexBits;
                if (m <= 0 || codes[k].code >> (32 - indexBits) != prefix) break;
                codes[k].length = m;
                codes[k].code <<= indexBits;
                subBits = std::max(subBits, m);
            }
            subBits = std::min(subBits, indexBits);
            const int at = fill(subBits, codes + i, k - i);
            table[size_t(start) + prefix] = VlcEntry{at, -subBits};
            i = k - 1;
        }
        for (int i = 0; i < size; ++i) {
            if (table[size_t(start + i)].length == 0) table[size_t(start + i)].symbol = -1;
        }
        return start;
    }
};

class Bits {
public:
    static constexpr size_t kPadding = 32;

    void reset(const uint8_t* bytes, size_t count) {
        data = bytes;
        total = count * 8;
        at = 0;
    }

    uint32_t show(int n) const {
        if (n <= 0) return 0;
        uint64_t word;
        memcpy(&word, data + (at >> 3), 8);
        word = __builtin_bswap64(word) << (at & 7);
        return uint32_t(word >> (64 - n));
    }
    void skip(int n) { at = std::min(total + 8, at + size_t(n)); }
    uint32_t bits(int n) {
        const uint32_t value = show(n);
        skip(n);
        return value;
    }
    int bit() { return int(bits(1)); }
    int left() const { return int(int64_t(total) - int64_t(at)); }
    int count() const { return int(at); }
    int unary(int stop, int length) {
        int i = 0;
        while (i < length && bit() != stop) ++i;
        return i;
    }
    int decode012() { return bit() ? bit() + 1 : 0; }
    int decode210() { return bit() ? 0 : 2 - bit(); }
    int vlc(const Vlc& t) {
        int index = int(show(t.bits));
        int code = t.table[size_t(index)].symbol;
        int n = t.table[size_t(index)].length;
        if (n < 0) {
            skip(t.bits);
            int more = -n;
            index = int(show(more)) + code;
            code = t.table[size_t(index)].symbol;
            n = t.table[size_t(index)].length;
            if (n < 0) {
                skip(more);
                more = -n;
                index = int(show(more)) + code;
                code = t.table[size_t(index)].symbol;
                n = t.table[size_t(index)].length;
            }
        }
        skip(n);
        return code;
    }
    int vlc(const Vlc* t) { return vlc(*t); }

private:
    const uint8_t* data = nullptr;
    size_t total = 0;
    size_t at = 0;
};

}

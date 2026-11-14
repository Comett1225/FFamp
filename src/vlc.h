#pragma once
#include "bits.h"
#include <vector>

namespace absent {

class Vlc {
public:
    bool build(int rootBits, size_t count, const uint8_t* lengths, const uint32_t* codes, const int32_t* symbols = nullptr);
    bool buildInOrder(int rootBits, size_t count, const uint8_t* lengths, const uint8_t* symbols, int offset);
    bool buildInOrder(int rootBits, size_t count, const uint8_t* lengths, const uint16_t* symbols, int offset);

    [[gnu::always_inline]] int read(BitReader& in) const {
        Entry e = table[in.peek(root)];
        if (e.length >= 0) {
            in.skip(e.length);
            return e.value;
        }
        in.skip(root);
        for (;;) {
            const int bits = -e.length;
            e = table[size_t(e.value) + in.peek(bits)];
            if (e.length >= 0) {
                in.skip(e.length);
                return e.value;
            }
            in.skip(bits);
        }
    }

    int lookup(uint64_t window, int& used) const {
        Entry e = table[window >> (64 - root)];
        if (e.length >= 0) {
            used = e.length;
            return e.value;
        }
        int total = root;
        window <<= root;
        for (;;) {
            const int bits = -e.length;
            e = table[size_t(e.value) + (window >> (64 - bits))];
            if (e.length >= 0) {
                used = total + e.length;
                return e.value;
            }
            total += bits;
            window <<= bits;
        }
    }

private:
    struct Entry {
        int32_t value = -1;
        int8_t length = 0;
    };
    struct Code {
        uint32_t bits;
        int length;
        int32_t symbol;
    };
    size_t fill(const std::vector<Code>& codes, int bits);

    std::vector<Entry> table;
    int root = 0;
};

}

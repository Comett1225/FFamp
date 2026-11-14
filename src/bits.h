#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace absent {

class BitReader {
public:
    BitReader() = default;
    BitReader(const uint8_t* data, size_t bytes) : data(data), size(bytes), end(int64_t(bytes) * 8) {}
    BitReader(const uint8_t* data, size_t bytes, int64_t bits) : data(data), size(bytes), end(bits) {}

    uint32_t peek(int n) const { return n ? uint32_t(window() >> (64 - n)) : 0; }
    uint32_t read(int n) {
        uint32_t value = peek(n);
        at += n;
        return value;
    }
    bool bit() { return read(1) != 0; }
    uint64_t cache() const { return window(); }
    void skip(int64_t n) { at += n; }
    void align() { at = (at + 7) & ~int64_t(7); }
    int64_t position() const { return at; }
    int64_t left() const { return end - at; }

private:
    uint64_t window() const {
        const size_t byte = size_t(at >> 3);
        uint64_t value = 0;
        if (byte < size && size - byte >= 8) {
            memcpy(&value, data + byte, 8);
            value = __builtin_bswap64(value);
        } else {
            for (size_t i = 0; i < 8; ++i) value = value << 8 | (byte < size && i < size - byte ? data[byte + i] : 0);
        }
        return value << (at & 7);
    }

    const uint8_t* data = nullptr;
    size_t size = 0;
    int64_t end = 0;
    int64_t at = 0;
};

}

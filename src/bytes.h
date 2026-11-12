#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace absent {

struct View {
    const uint8_t* p = nullptr;
    size_t n = 0;
    size_t at = 0;
    bool ok = true;

    size_t left() const { return ok ? n - at : 0; }
    bool need(size_t k) {
        if (!ok || n - at < k) { ok = false; return false; }
        return true;
    }
    uint8_t u8() { return need(1) ? p[at++] : 0; }
    uint16_t u16() {
        if (!need(2)) return 0;
        uint16_t v = uint16_t((p[at] << 8) | p[at + 1]);
        at += 2;
        return v;
    }
    uint32_t u24() {
        if (!need(3)) return 0;
        uint32_t v = (uint32_t(p[at]) << 16) | (uint32_t(p[at + 1]) << 8) | p[at + 2];
        at += 3;
        return v;
    }
    uint32_t u32() {
        if (!need(4)) return 0;
        uint32_t v = (uint32_t(p[at]) << 24) | (uint32_t(p[at + 1]) << 16) | (uint32_t(p[at + 2]) << 8) | p[at + 3];
        at += 4;
        return v;
    }
    uint64_t u64() {
        uint64_t high = u32();
        return (high << 32) | u32();
    }
    void skip(size_t k) { if (need(k)) at += k; }
    View sub(size_t k) {
        if (!need(k)) return {};
        View v{p + at, k};
        at += k;
        return v;
    }
};

struct Bytes {
    std::vector<uint8_t> d;

    void u8(uint32_t v) { d.push_back(uint8_t(v)); }
    void u16(uint32_t v) { u8(v >> 8); u8(v); }
    void u24(uint32_t v) { u8(v >> 16); u16(v); }
    void u32(uint32_t v) { u16(v >> 16); u16(v); }
    void u64(uint64_t v) { u32(uint32_t(v >> 32)); u32(uint32_t(v)); }
    void zero(size_t k) { d.insert(d.end(), k, 0); }
    void raw(const void* p, size_t k) { d.insert(d.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + k); }
    size_t open(uint32_t type) {
        size_t at = d.size();
        u32(0);
        u32(type);
        return at;
    }
    size_t open(uint32_t type, uint32_t version, uint32_t flags) {
        size_t at = open(type);
        u8(version);
        u24(flags);
        return at;
    }
    void close(size_t at) {
        uint32_t size = uint32_t(d.size() - at);
        d[at] = uint8_t(size >> 24);
        d[at + 1] = uint8_t(size >> 16);
        d[at + 2] = uint8_t(size >> 8);
        d[at + 3] = uint8_t(size);
    }
};

}

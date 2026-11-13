#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace absent {

struct BitReader {
    const uint8_t* p = nullptr;
    size_t n = 0;
    size_t at = 0;
    bool ok = true;

    size_t left() const { return ok ? n * 8 - at : 0; }
    uint32_t bit() {
        if (at >= n * 8) { ok = false; return 0; }
        uint32_t v = (p[at >> 3] >> (7 - (at & 7))) & 1;
        ++at;
        return v;
    }
    uint32_t bits(int count) {
        uint32_t v = 0;
        for (int i = 0; i < count; ++i) v = (v << 1) | bit();
        return v;
    }
    void skip(size_t count) {
        if (count > left()) { ok = false; return; }
        at += count;
    }
    uint32_t ue() {
        int zeros = 0;
        while (ok && bit() == 0) if (++zeros > 32) { ok = false; return 0; }
        if (!ok) return 0;
        if (zeros == 32) return 0xFFFFFFFFu;
        return ((1u << zeros) - 1) + (zeros ? bits(zeros) : 0);
    }
    int32_t se() {
        uint32_t v = ue();
        return (v & 1) ? int32_t((v + 1) / 2) : -int32_t(v / 2);
    }
};

std::vector<uint8_t> unescape(const uint8_t* p, size_t n);

struct VideoInfo {
    bool valid = false;
    int profile = 0;
    int compatibility = 0;
    int level = 0;
    int chroma = 1;
    int bitDepth = 8;
    int bitDepthChroma = 8;
    int width = 0;
    int height = 0;
    bool interlaced = false;
    bool rangeKnown = false;
    bool full = false;
    bool colorKnown = false;
    int primaries = 2;
    int transfer = 2;
    int matrix = 2;
    uint32_t ticks = 0;
    uint32_t timeScale = 0;
    int frameNumBits = 4;
    int orderType = -1;
    int orderBits = 0;
    bool separatePlanes = false;
    int profileSpace = 0;
    int tier = 0;
    uint32_t compatibilityFlags = 0;
    uint64_t constraintFlags = 0;
    int subLayers = 1;
    bool nested = false;
};

VideoInfo parseAvcSps(const uint8_t* nal, size_t n);
VideoInfo parseHevcSps(const uint8_t* nal, size_t n);
VideoInfo videoInfo(uint32_t configType, const std::vector<uint8_t>& config);

using NalList = std::vector<std::vector<uint8_t>>;
std::vector<uint8_t> buildAvcConfig(const NalList& sps, const NalList& pps);
std::vector<uint8_t> buildHevcConfig(const NalList& vps, const NalList& sps, const NalList& pps);

template <class F>
void annexNals(const uint8_t* p, size_t n, F each) {
    size_t start = 0;
    bool inside = false;
    size_t zeros = 0;
    for (size_t i = 0; i < n; ++i) {
        if (zeros >= 2 && p[i] == 1) {
            if (inside && i - zeros > start) each(p + start, i - zeros - start);
            inside = true;
            start = i + 1;
            zeros = 0;
            continue;
        }
        zeros = p[i] == 0 ? zeros + 1 : 0;
    }
    if (inside && n - zeros > start) each(p + start, n - zeros - start);
}

}

#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace absent {

struct SincKernel {
    int64_t up = 1;
    int64_t down = 1;
    int half = 0;
    int phases = 0;
    bool blended = false;
    std::vector<float> table;
    mutable std::vector<float> planes;

    SincKernel(int inRate, int outRate);
    bool identity() const { return up == down; }
    int64_t centre(int64_t outIndex) const;
    int64_t firstInput(int64_t outIndex) const { return centre(outIndex) - half + 1; }
    int64_t endInput(int64_t outIndex) const { return centre(outIndex) + half + 1; }
    int64_t outputLength(int64_t inFrames) const;
    void renderPart(const float* planes, size_t span, int64_t first, int channels, int64_t outStart, size_t count, float* out) const;
    void render(const float* in, int64_t inBase, int channels, int64_t outStart, size_t count, float* out) const;
};

struct ChannelMix {
    int from = 0;
    int to = 0;
    bool identity = true;
    std::vector<float> matrix;

    ChannelMix(int inChannels, int outChannels);
    double peak() const;
    void apply(const float* in, size_t frames, float* out) const;
};

}

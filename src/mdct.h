#pragma once
#include <cstdint>
#include <vector>

namespace absent {

class InverseMdct {
public:
    InverseMdct(int bits, double scale);
    int size() const { return n; }
    void half(const float* in, float* out);
    void full(const float* in, float* out);

private:
    void transform();

    int n = 0;
    std::vector<float> turnRe, turnIm, twiddleRe, twiddleIm, re, im;
    std::vector<uint32_t> order;
};

const float* sineWindow(int bits);

}

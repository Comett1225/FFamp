#pragma once
#include <cstdint>
#include <vector>

namespace absent {

template <class T>
class InverseMdctOf {
public:
    InverseMdctOf(int bits, double scale);
    int size() const { return n; }
    void half(const T* in, T* out);
    void full(const T* in, T* out);

private:
    void transform();

    int n = 0;
    std::vector<T> turnRe, turnIm, twiddleRe, twiddleIm, re, im;
    std::vector<uint32_t> order;
};

using InverseMdct = InverseMdctOf<float>;
using PreciseMdct = InverseMdctOf<double>;

const float* sineWindow(int bits);
void kbdWindow(float* window, float alpha, int n);

}

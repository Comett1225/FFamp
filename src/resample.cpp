#include "resample.h"
#include "absent/audio.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

namespace absent {

namespace {

const double kPi = 3.14159265358979323846;
const double kCutoff = 0.95;
const int kCrossings = 80;
const double kBeta = 12.57;
const int kExactPhases = 1024;
const int kBlendPhases = 256;
const int kMostTableValues = 1 << 24;

double bessel(double x) {
    double sum = 1;
    double term = 1;
    for (int k = 1; k < 64; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

enum Place { FL, FR, FC, LFE, BL, BR, BC, SL, SR };

const std::vector<Place>& layout(int channels) {
    static const std::vector<Place> known[9] = {{}, {FC}, {FL, FR}, {FL, FR, LFE}, {FL, FR, FC, BC}, {FL, FR, FC, BL, BR},
        {FL, FR, FC, LFE, BL, BR}, {FL, FR, FC, LFE, BC, SL, SR}, {FL, FR, FC, LFE, BL, BR, SL, SR}};
    return known[channels >= 1 && channels <= 8 ? channels : 0];
}

void stereoShare(Place place, float& left, float& right) {
    const float side = float(std::sqrt(0.5));
    left = right = 0;
    switch (place) {
        case FL: left = 1; break;
        case FR: right = 1; break;
        case FC: left = right = side; break;
        case BL: case SL: left = side; break;
        case BR: case SR: right = side; break;
        case BC: left = right = 0.5f; break;
        case LFE: break;
    }
}

}

SincKernel::SincKernel(int inRate, int outRate) {
    inRate = std::max(1, inRate);
    outRate = std::max(1, outRate);
    int64_t common = std::gcd(inRate, outRate);
    up = outRate / common;
    down = inRate / common;
    if (identity()) return;
    double scale = std::min(1.0, double(outRate) / inRate);
    half = (int(std::ceil(kCrossings / scale)) + 3) & ~3;
    blended = up > kExactPhases;
    phases = blended ? kBlendPhases : int(up);
    int rows = blended ? phases + 3 : phases;
    half = std::max(4, std::min(half, (kMostTableValues / 2 / rows) & ~3));
    int taps = 2 * half;
    table.resize(size_t(rows) * size_t(taps));
    row.resize(size_t(taps));
    double window = bessel(kBeta);
    double pass = scale * kCutoff;
    std::vector<double> exact(table.size() / size_t(rows));
    for (int r = 0; r < rows; ++r) {
        double shift = double(blended ? r - 1 : r) / phases;
        double sum = 0;
        for (int k = 0; k < taps; ++k) {
            double x = double(k - half + 1) - shift;
            double edge = x / half;
            double shape = std::fabs(edge) >= 1 ? 0 : bessel(kBeta * std::sqrt(1 - edge * edge)) / window;
            double angle = kPi * pass * x;
            exact[size_t(k)] = (std::fabs(angle) < 1e-12 ? 1.0 : std::sin(angle) / angle) * pass * shape;
            sum += exact[size_t(k)];
        }
        float* h = table.data() + size_t(r) * size_t(taps);
        for (int k = 0; k < taps; ++k) h[k] = float(exact[size_t(k)] / sum);
    }
}

int64_t SincKernel::centre(int64_t outIndex) const {
    __int128 product = __int128(outIndex) * down;
    int64_t whole = int64_t(product / up);
    if (product % up < 0) --whole;
    return whole;
}

int64_t SincKernel::outputLength(int64_t inFrames) const {
    if (inFrames <= 0) return 0;
    return int64_t((__int128(inFrames) * up + down - 1) / down);
}

void SincKernel::render(const float* in, int64_t inBase, int channels, int64_t outStart, size_t count, float* out) const {
    if (!count) return;
    const size_t taps = size_t(2 * half);
    const size_t width = size_t(channels);
    const int64_t first = firstInput(outStart);
    const size_t span = size_t(endInput(outStart + int64_t(count) - 1) - first);
    planes.resize(span * width);
    const float* source = in + size_t(first - inBase) * width;
    for (size_t c = 0; c < width; ++c) {
        float* plane = planes.data() + c * span;
        for (size_t i = 0; i < span; ++i) plane[i] = source[i * width + c];
    }
    for (size_t n = 0; n < count; ++n) {
        int64_t index = outStart + int64_t(n);
        int64_t whole = centre(index);
        int64_t phase = int64_t(__int128(index) * down - __int128(whole) * up);
        const float* h;
        if (blended) {
            double position = double(phase) / double(up) * phases;
            int cell = std::min(int(position), phases - 1);
            float t = float(position - cell);
            const float* a = table.data() + size_t(cell) * taps;
            const float* b = a + taps;
            const float* c = b + taps;
            const float* d = c + taps;
            float wa = ((-t + 2) * t - 1) * t * 0.5f;
            float wb = ((3 * t - 5) * t * t + 2) * 0.5f;
            float wc = ((-3 * t + 4) * t + 1) * t * 0.5f;
            float wd = (t - 1) * t * t * 0.5f;
            for (size_t k = 0; k < taps; ++k) row[k] = wa * a[k] + wb * b[k] + wc * c[k] + wd * d[k];
            h = row.data();
        } else {
            h = table.data() + size_t(phase) * taps;
        }
        const size_t offset = size_t(whole - half + 1 - first);
        for (size_t c = 0; c < width; ++c) {
            const float* x = planes.data() + c * span + offset;
            float sum[8] = {0, 0, 0, 0, 0, 0, 0, 0};
            for (size_t k = 0; k < taps; k += 8) {
                for (size_t j = 0; j < 8; ++j) sum[j] += x[k + j] * h[k + j];
            }
            out[n * width + c] = ((sum[0] + sum[4]) + (sum[1] + sum[5])) + ((sum[2] + sum[6]) + (sum[3] + sum[7]));
        }
    }
}

ChannelMix::ChannelMix(int inChannels, int outChannels) : from(std::max(1, inChannels)), to(std::max(1, outChannels)) {
    matrix.assign(size_t(from) * size_t(to), 0.0f);
    identity = from == to;
    const std::vector<Place>& source = layout(from);
    const std::vector<Place>& target = layout(to);
    for (int o = 0; o < to; ++o) {
        for (int i = 0; i < from; ++i) {
            float& gain = matrix[size_t(o) * size_t(from) + size_t(i)];
            if (identity || source.empty() || target.empty()) {
                gain = i == o ? 1.0f : 0.0f;
            } else if (to <= 2) {
                float left, right;
                stereoShare(source[size_t(i)], left, right);
                gain = to == 2 ? (o == 0 ? left : right) : (left + right) * float(std::sqrt(0.5));
            } else {
                gain = source[size_t(i)] == target[size_t(o)] ? 1.0f : 0.0f;
            }
        }
    }
}

double ChannelMix::peak() const {
    double most = 0;
    for (int o = 0; o < to; ++o) {
        double sum = 0;
        for (int i = 0; i < from; ++i) sum += std::fabs(matrix[size_t(o) * size_t(from) + size_t(i)]);
        most = std::max(most, sum);
    }
    return most;
}

void ChannelMix::apply(const float* in, size_t frames, float* out) const {
    if (identity) {
        if (in != out) memcpy(out, in, frames * size_t(from) * sizeof(float));
        return;
    }
    for (size_t f = 0; f < frames; ++f) {
        const float* x = in + f * size_t(from);
        float* y = out + f * size_t(to);
        for (int o = 0; o < to; ++o) {
            const float* g = matrix.data() + size_t(o) * size_t(from);
            float sum = 0;
            for (int i = 0; i < from; ++i) sum += g[i] * x[i];
            y[o] = sum;
        }
    }
}

double mixPeak(int inChannels, int outChannels) { return ChannelMix(inChannels, outChannels).peak(); }

struct Resampler::Impl {
    SincKernel kernel;
    ChannelMix mix;
    int inChannels;
    int outChannels;
    int width;
    bool mixFirst;
    std::vector<float> held;
    int64_t heldBase = 0;
    int64_t received = 0;
    int64_t produced = 0;
    bool finished = false;
    std::vector<float> scratch;

    Impl(int inRate, int inCh, int outRate, int outCh)
        : kernel(inRate, outRate), mix(inCh, outCh), inChannels(std::max(1, inCh)), outChannels(std::max(1, outCh)) {
        mixFirst = outChannels <= inChannels;
        width = mixFirst ? outChannels : inChannels;
        reset();
    }

    void reset() {
        heldBase = kernel.identity() ? 0 : kernel.firstInput(0);
        held.assign(size_t(-heldBase) * size_t(width), 0.0f);
        received = 0;
        produced = 0;
        finished = false;
    }
};

Resampler::Resampler(int inRate, int inChannels, int outRate, int outChannels) : impl(new Impl(inRate, inChannels, outRate, outChannels)) {}
Resampler::~Resampler() = default;

void Resampler::reset() { impl->reset(); }

void Resampler::push(const float* in, size_t frames) {
    Impl& r = *impl;
    if (r.finished) return;
    size_t at = r.held.size();
    r.held.resize(at + frames * size_t(r.width));
    if (r.mixFirst) r.mix.apply(in, frames, r.held.data() + at);
    else memcpy(r.held.data() + at, in, frames * size_t(r.width) * sizeof(float));
    r.received += int64_t(frames);
}

void Resampler::finish() {
    Impl& r = *impl;
    if (r.finished) return;
    r.finished = true;
    if (!r.kernel.identity()) r.held.resize(r.held.size() + size_t(r.kernel.half + 1) * size_t(r.width), 0.0f);
}

size_t Resampler::pull(float* out, size_t frames) {
    Impl& r = *impl;
    int64_t heldEnd = r.heldBase + int64_t(r.held.size() / size_t(r.width));
    int64_t ready;
    if (r.kernel.identity()) {
        ready = r.received - r.produced;
    } else {
        int64_t total = r.finished ? r.kernel.outputLength(r.received) : INT64_MAX;
        __int128 reach = __int128(heldEnd - r.kernel.half) * r.kernel.up;
        int64_t covered = reach <= 0 ? 0 : int64_t((reach - 1) / r.kernel.down) + 1;
        ready = std::min(total, covered) - r.produced;
    }
    size_t count = size_t(std::clamp<int64_t>(ready, 0, int64_t(frames)));
    if (!count) return 0;
    const float* source = r.held.data();
    float* target = out;
    if (!r.mixFirst) {
        r.scratch.resize(count * size_t(r.width));
        target = r.scratch.data();
    }
    int64_t keepFrom;
    if (r.kernel.identity()) {
        memcpy(target, source + size_t(r.produced - r.heldBase) * size_t(r.width), count * size_t(r.width) * sizeof(float));
        keepFrom = r.produced + int64_t(count);
    } else {
        r.kernel.render(source, r.heldBase, r.width, r.produced, count, target);
        keepFrom = r.kernel.firstInput(r.produced + int64_t(count));
    }
    if (!r.mixFirst) r.mix.apply(target, count, out);
    r.produced += int64_t(count);
    keepFrom = std::min(keepFrom, heldEnd);
    if (keepFrom > r.heldBase) {
        r.held.erase(r.held.begin(), r.held.begin() + long(size_t(keepFrom - r.heldBase) * size_t(r.width)));
        r.heldBase = keepFrom;
    }
    return count;
}

}

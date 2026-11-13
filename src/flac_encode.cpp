#include "flac_encode.h"
#include "dot.h"
#include "parallel.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>

namespace absent {

namespace {

const uint32_t kMd5Sines[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122,
    0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6,
    0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60,
    0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
    0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
const int kMd5Turns[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

void md5Round(uint32_t state[4], const uint8_t block[64]) {
    uint32_t words[16];
    for (int i = 0; i < 16; ++i) words[i] = uint32_t(block[i * 4]) | (uint32_t(block[i * 4 + 1]) << 8) | (uint32_t(block[i * 4 + 2]) << 16) | (uint32_t(block[i * 4 + 3]) << 24);
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    for (int i = 0; i < 64; ++i) {
        uint32_t mix;
        int pick;
        if (i < 16) {
            mix = (b & c) | (~b & d);
            pick = i;
        } else if (i < 32) {
            mix = (d & b) | (~d & c);
            pick = (5 * i + 1) & 15;
        } else if (i < 48) {
            mix = b ^ c ^ d;
            pick = (3 * i + 5) & 15;
        } else {
            mix = c ^ (b | ~d);
            pick = (7 * i) & 15;
        }
        uint32_t next = d;
        d = c;
        c = b;
        b += std::rotl(a + mix + kMd5Sines[i] + words[pick], kMd5Turns[i]);
        a = next;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

const int kMaxOrder = 12;
const int kPrecision = 15;
const int kMaxPartitionOrder = 8;
const size_t kBatchBlocks = 32;

struct Tables {
    uint8_t crc8[256];
    uint16_t crc16[256];
    Tables() {
        for (int i = 0; i < 256; ++i) {
            uint32_t small = uint32_t(i), big = uint32_t(i) << 8;
            for (int k = 0; k < 8; ++k) {
                small = (small << 1) ^ ((small & 0x80) ? 0x07 : 0);
                big = (big << 1) ^ ((big & 0x8000) ? 0x8005 : 0);
            }
            crc8[i] = uint8_t(small);
            crc16[i] = uint16_t(big);
        }
    }
};

const Tables& tables() {
    static const Tables made;
    return made;
}

struct BitWriter {
    std::vector<uint8_t> d;
    size_t at = 0;
    uint64_t hold = 0;
    int used = 0;

    void put(uint32_t value, int bits) {
        hold = (hold << bits) | (value & ((1ull << bits) - 1));
        used += bits;
        if (used < 32) return;
        used -= 32;
        if (at + 4 > d.size()) d.resize(std::max<size_t>(d.size() * 2, 4096));
        uint32_t word = uint32_t(hold >> used);
        d[at] = uint8_t(word >> 24);
        d[at + 1] = uint8_t(word >> 16);
        d[at + 2] = uint8_t(word >> 8);
        d[at + 3] = uint8_t(word);
        at += 4;
    }
    void unary(uint32_t zeros) {
        while (zeros >= 32) {
            put(0, 32);
            zeros -= 32;
        }
        put(1, int(zeros) + 1);
    }
    void rice(uint32_t folded, int k) {
        uint32_t high = folded >> k;
        if (high + 1 + uint32_t(k) <= 32) {
            put((1u << k) | (folded & ((1u << k) - 1)), int(high) + 1 + k);
        } else {
            unary(high);
            put(folded, k);
        }
    }
    void align() {
        if (used & 7) put(0, 8 - (used & 7));
        if (at + 4 > d.size()) d.resize(d.size() + 4);
        for (; used > 0; used -= 8) d[at++] = uint8_t(hold >> (used - 8));
        d.resize(at);
    }
};

enum class Shape { Constant, Verbatim, Fixed, Linear };

struct Plan {
    Shape shape = Shape::Verbatim;
    int order = 0;
    int shift = 0;
    int partitionOrder = 0;
    int32_t coefficients[kMaxOrder] = {};
    std::vector<uint8_t> rice;
    std::vector<int32_t> residual;
    uint64_t bits = 0;
};

struct Study {
    std::vector<int32_t> samples;
    int wasted = 0;
    int depth = 0;
    bool constant = false;
    uint32_t most = 0;
    int best = 0;
    double bits = 0;
    double predictor[kMaxOrder + 1][kMaxOrder] = {};
    Plan plan;
};

struct RiceChoice {
    int partitionOrder = 0;
    std::vector<uint8_t> parameters;
    uint64_t bits = ~0ull;
};

inline uint32_t fold(int32_t value) { return (uint32_t(value) << 1) ^ uint32_t(value >> 31); }

RiceChoice chooseRice(const int32_t* residual, int count, int order, bool wide) {
    const int blockSize = count + order;
    const int parameterBits = wide ? 5 : 4;
    const int largest = wide ? 30 : 14;
    int top = 0;
    while (top < kMaxPartitionOrder && !(blockSize & ((1 << (top + 1)) - 1)) && (blockSize >> (top + 1)) > order) ++top;
    uint64_t sums[1 << kMaxPartitionOrder];
    int at = 0;
    for (int p = 0; p < (1 << top); ++p) {
        int length = (blockSize >> top) - (p ? 0 : order);
        uint64_t sum = 0;
        for (int i = 0; i < length; ++i) sum += fold(residual[at + i]);
        sums[p] = sum;
        at += length;
    }
    RiceChoice best;
    uint8_t parameters[1 << kMaxPartitionOrder];
    for (int level = top; level >= 0; --level) {
        int parts = 1 << level;
        uint64_t total = 6;
        for (int p = 0; p < parts; ++p) {
            uint64_t length = uint64_t((blockSize >> level) - (p ? 0 : order));
            uint64_t sum = sums[p];
            uint64_t trimmed = sum > (length >> 1) ? sum - (length >> 1) : 0;
            int guess = length && trimmed > length ? std::min(largest, int(std::bit_width(trimmed / length)) - 1) : 0;
            uint64_t least = ~0ull;
            int pick = 0;
            for (int k = std::max(0, guess - 1); k <= std::min(largest, guess + 1); ++k) {
                uint64_t cost = length * uint64_t(k + 1) + (trimmed >> k);
                if (cost < least) {
                    least = cost;
                    pick = k;
                }
            }
            parameters[p] = uint8_t(pick);
            total += uint64_t(parameterBits) + least;
        }
        if (total < best.bits) {
            best.bits = total;
            best.partitionOrder = level;
            best.parameters.assign(parameters, parameters + parts);
        }
        for (int p = 0; p < parts / 2; ++p) sums[p] = sums[2 * p] + sums[2 * p + 1];
    }
    return best;
}

template <class Sum>
bool predict(const int32_t* s, int count, const int32_t* quantised, int order, int shift, int32_t* out) {
    const int kTile = 256;
    Sum sums[kTile];
    bool fits = true;
    for (int from = order; from < count; from += kTile) {
        const int n = std::min(kTile, count - from);
        std::fill(sums, sums + n, Sum(0));
        for (int j = 0; j < order; ++j) {
            const Sum weight = quantised[j];
            const int32_t* past = s + from - 1 - j;
            for (int i = 0; i < n; ++i) sums[i] += weight * Sum(past[i]);
        }
        for (int i = 0; i < n; ++i) {
            int64_t value = int64_t(s[from + i]) - int64_t(sums[i] >> shift);
            fits = fits && value > INT32_MIN && value <= INT32_MAX;
            out[from + i] = int32_t(value);
        }
    }
    return fits;
}

void study(Study& lane, int depth) {
    const int count = int(lane.samples.size());
    int32_t* s = lane.samples.data();
    uint32_t any = 0;
    bool same = true;
    for (int i = 0; i < count; ++i) {
        any |= uint32_t(s[i]);
        same = same && s[i] == s[0];
    }
    lane.wasted = 0;
    if (any && depth > 1) {
        lane.wasted = std::min(std::countr_zero(any), depth - 1);
        if (lane.wasted) for (int i = 0; i < count; ++i) s[i] >>= lane.wasted;
    }
    depth -= lane.wasted;
    lane.depth = depth;
    lane.constant = same;
    lane.best = 0;
    lane.bits = same ? double(depth) : double(count) * depth;
    int highest = std::min(kMaxOrder, count - 1);
    if (same || highest < 1) return;
    std::vector<double> windowed(static_cast<size_t>(count));
    const double middle = (count - 1) / 2.0;
    double weightEnergy = 0;
    uint32_t most = 0;
    for (int i = 0; i < count; ++i) {
        double x = middle > 0 ? (i - middle) / (middle + 1) : 0;
        double weight = 1.0 - x * x;
        windowed[size_t(i)] = s[i] * weight;
        weightEnergy += weight * weight;
        most = std::max(most, uint32_t(s[i] < 0 ? -int64_t(s[i]) : int64_t(s[i])));
    }
    lane.most = most;
    double correlation[kMaxOrder + 1] = {};
    for (int lag = 0; lag <= highest; ++lag) correlation[lag] = dotWide(windowed.data() + lag, windowed.data(), size_t(count - lag));
    if (!(correlation[0] > 0) || !(weightEnergy > 0)) return;
    double predictor[kMaxOrder + 1] = {};
    double previous[kMaxOrder + 1] = {};
    double error = correlation[0];
    for (int order = 1; order <= highest && error > 0; ++order) {
        double reflection = correlation[order];
        for (int j = 1; j < order; ++j) reflection -= predictor[j] * correlation[order - j];
        reflection /= error;
        std::copy(predictor, predictor + order, previous);
        predictor[order] = reflection;
        for (int j = 1; j < order; ++j) predictor[j] = previous[j] - reflection * previous[order - j];
        error *= 1.0 - reflection * reflection;
        if (!std::isfinite(error)) break;
        std::copy(predictor + 1, predictor + order + 1, lane.predictor[order]);
        double each = error > 0 ? std::max(1.0, 1.94 + 0.5 * std::log2(error / weightEnergy)) : 1.0;
        double bits = each * (count - order) + double(order) * (depth + kPrecision) + 9;
        if (bits < lane.bits) {
            lane.bits = bits;
            lane.best = order;
        }
    }
}

void consider(Study& lane, Shape shape, int order, const int32_t* work, uint64_t fixedBits, int shift, const int32_t* quantised) {
    const int count = int(lane.samples.size());
    RiceChoice choice = chooseRice(work + order, count - order, order, lane.depth > 16);
    if (fixedBits + choice.bits >= lane.plan.bits) return;
    Plan& plan = lane.plan;
    plan.shape = shape;
    plan.order = order;
    plan.shift = shift;
    if (quantised) std::copy(quantised, quantised + order, plan.coefficients);
    plan.residual.assign(work + order, work + count);
    plan.partitionOrder = choice.partitionOrder;
    plan.rice = std::move(choice.parameters);
    plan.bits = fixedBits + choice.bits;
}

void linear(Study& lane, int order, int32_t* work) {
    const int count = int(lane.samples.size());
    const double* predictor = lane.predictor[order];
    double largest = 0;
    for (int j = 0; j < order; ++j) largest = std::max(largest, std::fabs(predictor[j]));
    if (!(largest > 0) || !std::isfinite(largest)) return;
    int exponent = 0;
    std::frexp(largest, &exponent);
    int shift = std::min(kPrecision - 1 - exponent, 15);
    if (shift < 0) return;
    int32_t quantised[kMaxOrder];
    double carried = 0;
    const int32_t limit = (1 << (kPrecision - 1)) - 1;
    uint64_t reach = 0;
    for (int j = 0; j < order; ++j) {
        carried += predictor[j] * double(1 << shift);
        int32_t q = int32_t(std::clamp<long>(std::lround(carried), -limit - 1, limit));
        quantised[j] = q;
        carried -= q;
        reach += uint64_t(q < 0 ? -q : q);
    }
    const int32_t* s = lane.samples.data();
    bool fits = reach * lane.most < (1ull << 31) ? predict<int32_t>(s, count, quantised, order, shift, work) : predict<int64_t>(s, count, quantised, order, shift, work);
    if (!fits) return;
    consider(lane, Shape::Linear, order, work, 8 + uint64_t(lane.wasted) + uint64_t(order) * uint64_t(lane.depth) + 9 + uint64_t(order) * kPrecision, shift, quantised);
}

void settle(Study& lane) {
    const int count = int(lane.samples.size());
    const int32_t* s = lane.samples.data();
    const uint64_t head = 8 + uint64_t(lane.wasted);
    Plan& plan = lane.plan;
    plan = Plan();
    if (lane.constant) {
        plan.shape = Shape::Constant;
        plan.bits = head + uint64_t(lane.depth);
        return;
    }
    plan.bits = head + uint64_t(count) * uint64_t(lane.depth);
    std::vector<int32_t> work(static_cast<size_t>(count));
    std::vector<int64_t> wide(s, s + count);
    int fixedOrder = 0;
    uint64_t fixedSum = ~0ull;
    bool fixedFits = false;
    for (int order = 0; order <= 4 && order < count; ++order) {
        uint64_t sum = 0;
        bool fits = true;
        for (int i = order; i < count; ++i) {
            int64_t value = wide[size_t(i)];
            fits = fits && value > INT32_MIN && value <= INT32_MAX;
            sum += uint64_t(value < 0 ? -value : value);
        }
        if (fits && sum < fixedSum) {
            fixedSum = sum;
            fixedOrder = order;
            fixedFits = true;
        }
        for (int i = count - 1; i > order; --i) wide[size_t(i)] -= wide[size_t(i - 1)];
    }
    if (fixedFits) {
        wide.assign(s, s + count);
        for (int order = 0; order < fixedOrder; ++order) for (int i = count - 1; i > order; --i) wide[size_t(i)] -= wide[size_t(i - 1)];
        for (int i = fixedOrder; i < count; ++i) work[size_t(i)] = int32_t(wide[size_t(i)]);
        consider(lane, Shape::Fixed, fixedOrder, work.data(), head + uint64_t(fixedOrder) * uint64_t(lane.depth), 0, nullptr);
    }
    if (lane.best > 0) linear(lane, lane.best, work.data());
}

void writeSubframe(BitWriter& w, const Study& lane) {
    const Plan& plan = lane.plan;
    const int count = int(lane.samples.size());
    const int depth = lane.depth;
    const bool wideRice = depth > 16;
    int code = plan.shape == Shape::Constant ? 0 : plan.shape == Shape::Verbatim ? 1 : plan.shape == Shape::Fixed ? 8 + plan.order : 32 + plan.order - 1;
    w.put(0, 1);
    w.put(uint32_t(code), 6);
    w.put(lane.wasted ? 1 : 0, 1);
    if (lane.wasted) w.unary(uint32_t(lane.wasted - 1));
    if (plan.shape == Shape::Constant) {
        w.put(uint32_t(lane.samples[0]), depth);
        return;
    }
    if (plan.shape == Shape::Verbatim) {
        for (int32_t value : lane.samples) w.put(uint32_t(value), depth);
        return;
    }
    for (int i = 0; i < plan.order; ++i) w.put(uint32_t(lane.samples[size_t(i)]), depth);
    if (plan.shape == Shape::Linear) {
        w.put(kPrecision - 1, 4);
        w.put(uint32_t(plan.shift), 5);
        for (int i = 0; i < plan.order; ++i) w.put(uint32_t(plan.coefficients[i]), kPrecision);
    }
    w.put(wideRice ? 1 : 0, 2);
    w.put(uint32_t(plan.partitionOrder), 4);
    int at = 0;
    for (int p = 0; p < (1 << plan.partitionOrder); ++p) {
        int length = (count >> plan.partitionOrder) - (p ? 0 : plan.order);
        int k = plan.rice[size_t(p)];
        w.put(uint32_t(k), wideRice ? 5 : 4);
        const int32_t* residual = plan.residual.data() + at;
        for (int i = 0; i < length; ++i) w.rice(fold(residual[i]), k);
        at += length;
    }
}

int blockSizeCode(int size) {
    if (size == 192) return 1;
    for (int i = 0; i < 4; ++i) if (size == 576 << i) return 2 + i;
    for (int i = 0; i < 8; ++i) if (size == 256 << i) return 8 + i;
    return size <= 256 ? 6 : 7;
}

int sampleRateCode(int rate) {
    static const int known[] = {88200, 176400, 192000, 8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000};
    for (int i = 0; i < 11; ++i) if (rate == known[i]) return i + 1;
    if (rate % 1000 == 0 && rate / 1000 < 256) return 12;
    if (rate < 65536) return 13;
    if (rate % 10 == 0 && rate / 10 < 65536) return 14;
    return 0;
}

int sampleSizeCode(int bits) {
    switch (bits) {
        case 8: return 1;
        case 12: return 2;
        case 16: return 4;
        case 20: return 5;
        case 24: return 6;
        default: return 0;
    }
}

}

void Md5::add(const uint8_t* data, size_t size) {
    size_t filled = size_t(length & 63);
    length += size;
    while (size) {
        size_t part = std::min(size, 64 - filled);
        memcpy(block + filled, data, part);
        filled += part;
        data += part;
        size -= part;
        if (filled == 64) {
            md5Round(state, block);
            filled = 0;
        }
    }
}

void Md5::finish(uint8_t out[16]) {
    uint64_t bitCount = length * 8;
    const uint8_t one = 0x80, none = 0;
    add(&one, 1);
    while ((length & 63) != 56) add(&none, 1);
    uint8_t tail[8];
    for (int i = 0; i < 8; ++i) tail[i] = uint8_t(bitCount >> (8 * i));
    add(tail, 8);
    for (int i = 0; i < 16; ++i) out[i] = uint8_t(state[i / 4] >> (8 * (i % 4)));
}

struct FlacEncoder::Impl {
    int rate = 0;
    int channels = 0;
    int bits = 0;
    std::vector<int32_t> pending;
    uint64_t total = 0;
    uint32_t frameNumber = 0;
    uint32_t smallest = 0;
    uint32_t largest = 0;
    Md5 digest;

    std::vector<uint8_t> frame(const int32_t* interleaved, int count, uint32_t number) const {
        std::vector<Study> lanes(size_t(channels == 2 ? 4 : channels));
        for (Study& lane : lanes) lane.samples.resize(size_t(count));
        std::vector<const Study*> chosen;
        int assignment = channels - 1;
        if (channels == 2) {
            for (int i = 0; i < count; ++i) {
                int32_t left = interleaved[i * 2], right = interleaved[i * 2 + 1];
                lanes[0].samples[size_t(i)] = left;
                lanes[1].samples[size_t(i)] = right;
                lanes[2].samples[size_t(i)] = (left + right) >> 1;
                lanes[3].samples[size_t(i)] = left - right;
            }
            for (int c = 0; c < 4; ++c) study(lanes[size_t(c)], c == 3 ? bits + 1 : bits);
            double costs[4] = {lanes[0].bits + lanes[1].bits, lanes[0].bits + lanes[3].bits, lanes[3].bits + lanes[1].bits, lanes[2].bits + lanes[3].bits};
            int pick = int(std::min_element(costs, costs + 4) - costs);
            assignment = pick == 0 ? 1 : 7 + pick;
            static const int first[4] = {0, 0, 3, 2}, second[4] = {1, 3, 1, 3};
            chosen = {&lanes[size_t(first[pick])], &lanes[size_t(second[pick])]};
        } else {
            for (int c = 0; c < channels; ++c) {
                for (int i = 0; i < count; ++i) lanes[size_t(c)].samples[size_t(i)] = interleaved[i * channels + c];
                study(lanes[size_t(c)], bits);
                chosen.push_back(&lanes[size_t(c)]);
            }
        }
        for (const Study* lane : chosen) settle(*const_cast<Study*>(lane));

        BitWriter w;
        w.d.resize(size_t(count) * size_t(channels) * 2 + 64);
        int sizeCode = blockSizeCode(count);
        int rateCode = sampleRateCode(rate);
        uint8_t head[16];
        size_t n = 0;
        head[n++] = 0xFF;
        head[n++] = 0xF8;
        head[n++] = uint8_t((sizeCode << 4) | rateCode);
        head[n++] = uint8_t((assignment << 4) | (sampleSizeCode(bits) << 1));
        if (number < 0x80) {
            head[n++] = uint8_t(number);
        } else {
            int extra = number < 0x800 ? 1 : number < 0x10000 ? 2 : number < 0x200000 ? 3 : number < 0x4000000 ? 4 : 5;
            head[n++] = uint8_t((0xFF00u >> (extra + 1)) | (number >> (6 * extra)));
            for (int i = extra - 1; i >= 0; --i) head[n++] = uint8_t(0x80 | ((number >> (6 * i)) & 0x3F));
        }
        if (sizeCode == 6) head[n++] = uint8_t(count - 1);
        if (sizeCode == 7) {
            head[n++] = uint8_t((count - 1) >> 8);
            head[n++] = uint8_t(count - 1);
        }
        if (rateCode == 12) head[n++] = uint8_t(rate / 1000);
        if (rateCode == 13 || rateCode == 14) {
            int value = rateCode == 13 ? rate : rate / 10;
            head[n++] = uint8_t(value >> 8);
            head[n++] = uint8_t(value);
        }
        uint8_t headCheck = 0;
        for (size_t i = 0; i < n; ++i) headCheck = tables().crc8[headCheck ^ head[i]];
        head[n++] = headCheck;
        for (size_t i = 0; i < n; ++i) w.put(head[i], 8);
        for (const Study* lane : chosen) writeSubframe(w, *lane);
        w.align();
        uint16_t check = 0;
        for (uint8_t byte : w.d) check = uint16_t((check << 8) ^ tables().crc16[(check >> 8) ^ byte]);
        w.d.push_back(uint8_t(check >> 8));
        w.d.push_back(uint8_t(check));
        return std::move(w.d);
    }

    void digestOf(const int32_t* interleaved, size_t values) {
        const size_t width = size_t((bits + 7) / 8);
        const size_t kStep = 16384;
        std::vector<uint8_t> packed(kStep * width);
        for (size_t from = 0; from < values; from += kStep) {
            size_t n = std::min(kStep, values - from);
            size_t at = 0;
            for (size_t i = 0; i < n; ++i) for (size_t k = 0; k < width; ++k) packed[at++] = uint8_t(uint32_t(interleaved[from + i]) >> (8 * k));
            digest.add(packed.data(), at);
        }
    }

    void flush(size_t frames, const FlacSink& sink) {
        const size_t blocks = (frames + size_t(kBlock) - 1) / size_t(kBlock);
        std::vector<std::vector<uint8_t>> made(blocks);
        parallelFor(blocks + 1, [&](size_t i) {
            if (i == blocks) return digestOf(pending.data(), frames * size_t(channels));
            size_t from = i * size_t(kBlock);
            made[i] = frame(pending.data() + from * size_t(channels), int(std::min(size_t(kBlock), frames - from)), frameNumber + uint32_t(i));
        });
        for (size_t i = 0; i < blocks; ++i) {
            uint32_t count = uint32_t(std::min(size_t(kBlock), frames - i * size_t(kBlock)));
            uint32_t size = uint32_t(made[i].size());
            smallest = smallest ? std::min(smallest, size) : size;
            largest = std::max(largest, size);
            total += count;
            ++frameNumber;
            sink(made[i].data(), made[i].size(), count);
        }
        pending.erase(pending.begin(), pending.begin() + long(frames * size_t(channels)));
    }
};

FlacEncoder::FlacEncoder(int rate, int channels, int bits) : impl(new Impl) {
    impl->rate = rate;
    impl->channels = channels;
    impl->bits = bits;
}

FlacEncoder::~FlacEncoder() = default;

void FlacEncoder::encode(const int32_t* interleaved, size_t frames, const FlacSink& sink) {
    Impl& f = *impl;
    f.pending.insert(f.pending.end(), interleaved, interleaved + frames * size_t(f.channels));
    const size_t batch = size_t(kBlock) * kBatchBlocks;
    while (f.pending.size() / size_t(f.channels) >= batch) f.flush(batch, sink);
}

void FlacEncoder::finish(const FlacSink& sink) {
    Impl& f = *impl;
    if (!f.pending.empty()) f.flush(f.pending.size() / size_t(f.channels), sink);
}

std::vector<uint8_t> FlacEncoder::streamInfo() const {
    const Impl& f = *impl;
    std::vector<uint8_t> info;
    auto put = [&](uint64_t value, int bytes) { for (int i = bytes - 1; i >= 0; --i) info.push_back(uint8_t(value >> (8 * i))); };
    put(kBlock, 2);
    put(kBlock, 2);
    put(f.smallest, 3);
    put(f.largest, 3);
    uint64_t packedFields = (uint64_t(f.rate) << 44) | (uint64_t(f.channels - 1) << 41) | (uint64_t(f.bits - 1) << 36) | (f.total & 0xFFFFFFFFFull);
    put(packedFields, 8);
    Md5 copy = f.digest;
    uint8_t sum[16];
    copy.finish(sum);
    info.insert(info.end(), sum, sum + 16);
    return info;
}

}

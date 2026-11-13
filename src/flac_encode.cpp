#include "flac_encode.h"
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
    uint64_t hold = 0;
    int used = 0;

    void put(uint32_t value, int bits) {
        if (bits <= 0) return;
        uint64_t mask = bits >= 32 ? 0xFFFFFFFFull : ((1ull << bits) - 1);
        hold = (hold << bits) | (value & mask);
        used += bits;
        while (used >= 8) {
            used -= 8;
            d.push_back(uint8_t(hold >> used));
        }
    }
    void unary(uint32_t zeros) {
        while (zeros >= 32) {
            put(0, 32);
            zeros -= 32;
        }
        put(1, int(zeros) + 1);
    }
    void align() { if (used) put(0, 8 - used); }
};

enum class Shape { Constant, Verbatim, Fixed, Linear };

struct Plan {
    Shape shape = Shape::Verbatim;
    int order = 0;
    int shift = 0;
    int wasted = 0;
    int depth = 0;
    int partitionOrder = 0;
    bool wideRice = false;
    int32_t coefficients[kMaxOrder] = {};
    std::vector<uint8_t> rice;
    std::vector<int32_t> residual;
    std::vector<int32_t> samples;
    uint64_t bits = 0;
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
    std::vector<uint64_t> sums(size_t(1) << top);
    int at = 0;
    for (int p = 0; p < (1 << top); ++p) {
        int length = (blockSize >> top) - (p ? 0 : order);
        uint64_t sum = 0;
        for (int i = 0; i < length; ++i) sum += fold(residual[at + i]);
        sums[size_t(p)] = sum;
        at += length;
    }
    RiceChoice best;
    std::vector<uint8_t> parameters;
    for (int level = top; level >= 0; --level) {
        int parts = 1 << level;
        uint64_t total = 6;
        parameters.assign(size_t(parts), 0);
        for (int p = 0; p < parts; ++p) {
            uint64_t length = uint64_t((blockSize >> level) - (p ? 0 : order));
            uint64_t sum = sums[size_t(p)];
            uint64_t trimmed = sum > (length >> 1) ? sum - (length >> 1) : 0;
            uint64_t least = ~0ull;
            int pick = 0;
            for (int k = 0; k <= largest; ++k) {
                uint64_t cost = length * uint64_t(k + 1) + (trimmed >> k);
                if (cost < least) {
                    least = cost;
                    pick = k;
                }
            }
            parameters[size_t(p)] = uint8_t(pick);
            total += uint64_t(parameterBits) + least;
        }
        if (total < best.bits) {
            best.bits = total;
            best.partitionOrder = level;
            best.parameters = parameters;
        }
        for (int p = 0; p < parts / 2; ++p) sums[size_t(p)] = sums[size_t(2 * p)] + sums[size_t(2 * p + 1)];
    }
    return best;
}

void take(Plan& plan, RiceChoice& choice, uint64_t headBits) {
    plan.partitionOrder = choice.partitionOrder;
    plan.rice = std::move(choice.parameters);
    plan.bits = headBits + choice.bits;
}

Plan analyse(const int32_t* input, int count, int depth) {
    Plan plan;
    plan.samples.assign(input, input + count);
    int32_t* s = plan.samples.data();
    uint32_t any = 0;
    bool same = true;
    for (int i = 0; i < count; ++i) {
        any |= uint32_t(s[i]);
        same = same && s[i] == s[0];
    }
    if (any && depth > 1) {
        plan.wasted = std::min(std::countr_zero(any), depth - 1);
        if (plan.wasted) for (int i = 0; i < count; ++i) s[i] >>= plan.wasted;
    }
    depth -= plan.wasted;
    plan.depth = depth;
    plan.wideRice = depth > 16;
    const uint64_t head = 8 + uint64_t(plan.wasted);
    if (same) {
        plan.shape = Shape::Constant;
        plan.bits = head + uint64_t(depth);
        return plan;
    }
    plan.shape = Shape::Verbatim;
    plan.bits = head + uint64_t(count) * uint64_t(depth);

    std::vector<int32_t> work(static_cast<size_t>(count));
    std::vector<int64_t> wide(s, s + count);
    for (int order = 0; order <= 4 && order < count; ++order) {
        bool fits = true;
        for (int i = order; i < count; ++i) {
            int64_t value = wide[size_t(i)];
            fits = fits && value > INT32_MIN && value <= INT32_MAX;
            work[size_t(i)] = int32_t(value);
        }
        if (fits) {
            RiceChoice choice = chooseRice(work.data() + order, count - order, order, plan.wideRice);
            uint64_t bits = head + uint64_t(order) * uint64_t(depth) + choice.bits;
            if (bits < plan.bits) {
                plan.shape = Shape::Fixed;
                plan.order = order;
                plan.residual.assign(work.begin() + order, work.end());
                take(plan, choice, head + uint64_t(order) * uint64_t(depth));
            }
        }
        for (int i = count - 1; i > order; --i) wide[size_t(i)] -= wide[size_t(i - 1)];
    }

    int highest = std::min(kMaxOrder, count - 1);
    if (highest < 1) return plan;
    std::vector<double> windowed(static_cast<size_t>(count));
    const double middle = (count - 1) / 2.0;
    for (int i = 0; i < count; ++i) {
        double x = middle > 0 ? (i - middle) / (middle + 1) : 0;
        windowed[size_t(i)] = s[i] * (1.0 - x * x);
    }
    double correlation[kMaxOrder + 1] = {};
    for (int lag = 0; lag <= highest; ++lag) {
        double sum = 0;
        for (int i = lag; i < count; ++i) sum += windowed[size_t(i)] * windowed[size_t(i - lag)];
        correlation[lag] = sum;
    }
    if (correlation[0] <= 0) return plan;
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

        double largest = 0;
        for (int j = 1; j <= order; ++j) largest = std::max(largest, std::fabs(predictor[j]));
        if (!(largest > 0) || !std::isfinite(largest)) continue;
        int exponent = 0;
        std::frexp(largest, &exponent);
        int shift = std::min(kPrecision - 1 - exponent, 15);
        if (shift < 0) continue;
        int32_t quantised[kMaxOrder];
        double carried = 0;
        const int32_t limit = (1 << (kPrecision - 1)) - 1;
        for (int j = 0; j < order; ++j) {
            carried += predictor[j + 1] * double(1 << shift);
            int32_t q = int32_t(std::clamp<long>(std::lround(carried), -limit - 1, limit));
            quantised[j] = q;
            carried -= q;
        }
        bool fits = true;
        for (int i = order; i < count; ++i) {
            int64_t sum = 0;
            for (int j = 0; j < order; ++j) sum += int64_t(quantised[j]) * s[i - 1 - j];
            int64_t value = int64_t(s[i]) - (sum >> shift);
            fits = fits && value > INT32_MIN && value <= INT32_MAX;
            work[size_t(i)] = int32_t(value);
        }
        if (!fits) continue;
        RiceChoice choice = chooseRice(work.data() + order, count - order, order, plan.wideRice);
        uint64_t fixedBits = head + uint64_t(order) * uint64_t(depth) + 9 + uint64_t(order) * kPrecision;
        if (fixedBits + choice.bits < plan.bits) {
            plan.shape = Shape::Linear;
            plan.order = order;
            plan.shift = shift;
            std::copy(quantised, quantised + order, plan.coefficients);
            plan.residual.assign(work.begin() + order, work.end());
            take(plan, choice, fixedBits);
        }
    }
    return plan;
}

void writeSubframe(BitWriter& w, const Plan& plan) {
    int count = int(plan.samples.size());
    int code = plan.shape == Shape::Constant ? 0 : plan.shape == Shape::Verbatim ? 1 : plan.shape == Shape::Fixed ? 8 + plan.order : 32 + plan.order - 1;
    w.put(0, 1);
    w.put(uint32_t(code), 6);
    w.put(plan.wasted ? 1 : 0, 1);
    if (plan.wasted) w.unary(uint32_t(plan.wasted - 1));
    if (plan.shape == Shape::Constant) {
        w.put(uint32_t(plan.samples[0]), plan.depth);
        return;
    }
    if (plan.shape == Shape::Verbatim) {
        for (int32_t value : plan.samples) w.put(uint32_t(value), plan.depth);
        return;
    }
    for (int i = 0; i < plan.order; ++i) w.put(uint32_t(plan.samples[size_t(i)]), plan.depth);
    if (plan.shape == Shape::Linear) {
        w.put(kPrecision - 1, 4);
        w.put(uint32_t(plan.shift), 5);
        for (int i = 0; i < plan.order; ++i) w.put(uint32_t(plan.coefficients[i]), kPrecision);
    }
    w.put(plan.wideRice ? 1 : 0, 2);
    w.put(uint32_t(plan.partitionOrder), 4);
    int at = 0;
    for (int p = 0; p < (1 << plan.partitionOrder); ++p) {
        int length = (count >> plan.partitionOrder) - (p ? 0 : plan.order);
        int k = plan.rice[size_t(p)];
        w.put(uint32_t(k), plan.wideRice ? 5 : 4);
        for (int i = 0; i < length; ++i) {
            uint32_t folded = fold(plan.residual[size_t(at + i)]);
            w.unary(folded >> k);
            w.put(folded, k);
        }
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
    std::vector<uint8_t> packed;
    std::vector<int32_t> lanes[4];

    void block(const int32_t* interleaved, int count, const FlacSink& sink) {
        std::vector<Plan> plans;
        int assignment = channels - 1;
        if (channels == 2) {
            for (auto& lane : lanes) lane.resize(size_t(count));
            for (int i = 0; i < count; ++i) {
                int32_t left = interleaved[i * 2], right = interleaved[i * 2 + 1];
                lanes[0][size_t(i)] = left;
                lanes[1][size_t(i)] = right;
                lanes[2][size_t(i)] = (left + right) >> 1;
                lanes[3][size_t(i)] = left - right;
            }
            Plan left = analyse(lanes[0].data(), count, bits);
            Plan right = analyse(lanes[1].data(), count, bits);
            Plan mid = analyse(lanes[2].data(), count, bits);
            Plan side = analyse(lanes[3].data(), count, bits + 1);
            uint64_t costs[4] = {left.bits + right.bits, left.bits + side.bits, side.bits + right.bits, mid.bits + side.bits};
            int pick = int(std::min_element(costs, costs + 4) - costs);
            assignment = pick == 0 ? 1 : 7 + pick;
            if (pick == 0) {
                plans.push_back(std::move(left));
                plans.push_back(std::move(right));
            } else if (pick == 1) {
                plans.push_back(std::move(left));
                plans.push_back(std::move(side));
            } else if (pick == 2) {
                plans.push_back(std::move(side));
                plans.push_back(std::move(right));
            } else {
                plans.push_back(std::move(mid));
                plans.push_back(std::move(side));
            }
        } else {
            for (int c = 0; c < channels; ++c) {
                lanes[0].resize(size_t(count));
                for (int i = 0; i < count; ++i) lanes[0][size_t(i)] = interleaved[i * channels + c];
                plans.push_back(analyse(lanes[0].data(), count, bits));
            }
        }

        BitWriter w;
        w.d.reserve(size_t(count) * size_t(channels) * 2);
        int sizeCode = blockSizeCode(count);
        int rateCode = sampleRateCode(rate);
        w.put(0xFFF8, 16);
        w.put(uint32_t(sizeCode), 4);
        w.put(uint32_t(rateCode), 4);
        w.put(uint32_t(assignment), 4);
        w.put(uint32_t(sampleSizeCode(bits)), 3);
        w.put(0, 1);
        if (frameNumber < 0x80) {
            w.put(frameNumber, 8);
        } else {
            int extra = frameNumber < 0x800 ? 1 : frameNumber < 0x10000 ? 2 : frameNumber < 0x200000 ? 3 : frameNumber < 0x4000000 ? 4 : 5;
            w.put((0xFF00u >> (extra + 1)) | (frameNumber >> (6 * extra)), 8);
            for (int i = extra - 1; i >= 0; --i) w.put(0x80 | ((frameNumber >> (6 * i)) & 0x3F), 8);
        }
        if (sizeCode == 6) w.put(uint32_t(count - 1), 8);
        if (sizeCode == 7) w.put(uint32_t(count - 1), 16);
        if (rateCode == 12) w.put(uint32_t(rate / 1000), 8);
        if (rateCode == 13) w.put(uint32_t(rate), 16);
        if (rateCode == 14) w.put(uint32_t(rate / 10), 16);
        uint8_t headCheck = 0;
        for (uint8_t byte : w.d) headCheck = tables().crc8[headCheck ^ byte];
        w.put(headCheck, 8);
        for (const Plan& plan : plans) writeSubframe(w, plan);
        w.align();
        uint16_t check = 0;
        for (uint8_t byte : w.d) check = uint16_t((check << 8) ^ tables().crc16[(check >> 8) ^ byte]);
        w.put(check, 16);

        int width = (bits + 7) / 8;
        packed.resize(size_t(count) * size_t(channels) * size_t(width));
        size_t at = 0;
        for (int i = 0; i < count * channels; ++i) for (int k = 0; k < width; ++k) packed[at++] = uint8_t(uint32_t(interleaved[i]) >> (8 * k));
        digest.add(packed.data(), packed.size());

        uint32_t size = uint32_t(w.d.size());
        smallest = smallest ? std::min(smallest, size) : size;
        largest = std::max(largest, size);
        total += uint64_t(count);
        ++frameNumber;
        sink(w.d.data(), w.d.size(), uint32_t(count));
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
    size_t step = size_t(kBlock) * size_t(f.channels);
    size_t at = 0;
    while (f.pending.size() - at >= step) {
        f.block(f.pending.data() + at, kBlock, sink);
        at += step;
    }
    f.pending.erase(f.pending.begin(), f.pending.begin() + long(at));
}

void FlacEncoder::finish(const FlacSink& sink) {
    Impl& f = *impl;
    if (!f.pending.empty()) f.block(f.pending.data(), int(f.pending.size() / size_t(f.channels)), sink);
    f.pending.clear();
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

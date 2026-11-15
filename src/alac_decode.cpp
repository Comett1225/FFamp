#include "audio_codec.h"
#include "bits.h"
#include <algorithm>

namespace absent {

namespace {

const int kOffsets[8][8] = {{0}, {0, 1}, {2, 0, 1}, {2, 0, 1, 3}, {2, 0, 1, 3, 4}, {2, 0, 1, 4, 5, 3}, {2, 0, 1, 4, 5, 6, 3}, {2, 6, 7, 0, 1, 4, 5, 3}};

int32_t signExtend(uint32_t value, int bits) { return bits >= 32 ? int32_t(value) : int32_t(value << (32 - bits)) >> (32 - bits); }

int signOnly(int v) { return v > 0 ? 1 : v < 0 ? -1 : 0; }

int log2Of(unsigned v) { return v ? 31 - __builtin_clz(v) : 0; }

uint32_t readLong(BitReader& r, int bits) {
    if (bits <= 0) return 0;
    if (bits > 24) {
        const uint32_t high = r.read(bits - 16);
        return (high << 16) | r.read(16);
    }
    return r.read(bits);
}

struct AlacAudio : PacketDecoder {
    uint32_t frameLength = 0;
    int sampleSize = 16;
    int historyMult = 40;
    int initialHistory = 10;
    int riceLimit = 14;
    std::vector<int32_t> error[2], output[8], extra[2];

    void reset() override {}

    uint32_t scalar(BitReader& r, int k, int bits) {
        uint32_t x = 0;
        while (x < 9 && r.bit()) ++x;
        if (x > 8) return readLong(r, bits);
        if (k != 1) {
            const uint32_t more = r.peek(k);
            x = (x << k) - x;
            if (more > 1) {
                x += more - 1;
                r.skip(k);
            } else {
                r.skip(k - 1);
            }
        }
        return x;
    }

    bool rice(BitReader& r, int32_t* out, int count, int bits, int mult) {
        uint32_t history = uint32_t(initialHistory);
        int modifier = 0;
        for (int i = 0; i < count; ++i) {
            if (r.left() <= 0) return false;
            int k = std::min(log2Of((history >> 9) + 3), riceLimit);
            uint32_t x = scalar(r, k, bits) + uint32_t(modifier);
            modifier = 0;
            out[i] = int32_t(x >> 1) ^ -int32_t(x & 1);
            history = x > 0xFFFF ? 0xFFFF : history + x * uint32_t(mult) - ((history * uint32_t(mult)) >> 9);
            if (history < 128 && i + 1 < count) {
                k = std::min(7 - log2Of(history) + int((history + 16) >> 6), riceLimit);
                int block = int(scalar(r, k, 16));
                if (block > 0) {
                    block = std::min(block, count - i - 1);
                    std::fill(out + i + 1, out + i + 1 + block, 0);
                    i += block;
                }
                if (block <= 0xFFFF) modifier = 1;
                history = 0;
            }
        }
        return true;
    }

    static void predict(const int32_t* in, int32_t* outSamples, int count, int bits, int16_t* coefficients, int order, int quant) {
        uint32_t* out = reinterpret_cast<uint32_t*>(outSamples);
        out[0] = uint32_t(in[0]);
        if (count <= 1) return;
        if (!order) {
            for (int i = 1; i < count; ++i) out[i] = uint32_t(in[i]);
            return;
        }
        if (order == 31) {
            for (int i = 1; i < count; ++i) out[i] = uint32_t(signExtend(out[i - 1] + uint32_t(in[i]), bits));
            return;
        }
        int i = 1;
        for (; i <= order && i < count; ++i) out[i] = uint32_t(signExtend(out[i - 1] + uint32_t(in[i]), bits));
        const uint32_t* pred = out;
        for (; i < count; ++i) {
            const int d = int(*pred++);
            int value = 0;
            for (int j = 0; j < order; ++j) value += int(pred[j] - uint32_t(d)) * coefficients[j];
            value = int((int64_t(value) + (int64_t(1) << (quant - 1))) >> quant);
            uint32_t residue = uint32_t(in[i]);
            value += d + int(residue);
            out[i] = uint32_t(signExtend(uint32_t(value), bits));
            const int sign = signOnly(int(residue));
            if (sign) {
                for (int j = 0; j < order && int(residue * uint32_t(sign)) > 0; ++j) {
                    int gap = d - int(pred[j]);
                    const int step = signOnly(gap) * sign;
                    coefficients[j] = int16_t(coefficients[j] - step);
                    gap *= step;
                    residue -= uint32_t(gap >> quant) * uint32_t(j + 1);
                }
            }
        }
    }

    bool element(BitReader& r, int first, int count, uint32_t& samples) {
        r.skip(4 + 12);
        const bool sized = r.read(1);
        const int extraBits = int(r.read(2)) << 3;
        const int bits = sampleSize - extraBits + count - 1;
        if (bits > 32 || bits < 1) return false;
        const bool compressed = !r.read(1);
        uint32_t length = sized ? readLong(r, 32) : frameLength;
        if (!length || length > frameLength) return false;
        if (!samples) samples = length;
        else if (length != samples) return false;
        for (int c = 0; c < count; ++c) {
            error[c].resize(length);
            extra[c].resize(length);
        }
        int shift = 0, weight = 0, usedExtra = extraBits;
        int32_t* outs[2] = {output[size_t(first)].data(), count > 1 ? output[size_t(first + 1)].data() : nullptr};
        if (compressed) {
            int16_t coefficients[2][32];
            int order[2], type[2], quant[2], mult[2];
            shift = int(r.read(8));
            weight = int(r.read(8));
            if (count == 2 && weight && shift > 31) return false;
            for (int c = 0; c < count; ++c) {
                type[c] = int(r.read(4));
                quant[c] = int(r.read(4));
                mult[c] = int(r.read(3));
                order[c] = int(r.read(5));
                if (uint32_t(order[c]) >= frameLength || !quant[c]) return false;
                for (int i = order[c] - 1; i >= 0; --i) coefficients[c][i] = int16_t(r.read(16));
            }
            if (extraBits) {
                if (r.left() < int64_t(length) * count * extraBits) return false;
                for (uint32_t i = 0; i < length; ++i)
                    for (int c = 0; c < count; ++c) extra[c][i] = int32_t(r.read(extraBits));
            }
            for (int c = 0; c < count; ++c) {
                if (!rice(r, error[c].data(), int(length), bits, mult[c] * historyMult / 4)) return false;
                if (type[c] == 15) predict(error[c].data(), error[c].data(), int(length), bits, nullptr, 31, 0);
                predict(error[c].data(), outs[c], int(length), bits, coefficients[c], order[c], quant[c]);
            }
        } else {
            if (r.left() < int64_t(length) * count * sampleSize) return false;
            for (uint32_t i = 0; i < length; ++i)
                for (int c = 0; c < count; ++c) outs[c][i] = signExtend(readLong(r, sampleSize), sampleSize);
            usedExtra = 0;
        }
        if (count == 2 && weight) {
            for (uint32_t i = 0; i < length; ++i) {
                uint32_t a = uint32_t(outs[0][i]), b = uint32_t(outs[1][i]);
                a -= uint32_t(int(b * uint32_t(weight)) >> shift);
                b += a;
                outs[0][i] = int32_t(b);
                outs[1][i] = int32_t(a);
            }
        }
        if (usedExtra) {
            for (int c = 0; c < count; ++c)
                for (uint32_t i = 0; i < length; ++i) outs[c][i] = int32_t((uint32_t(outs[c][i]) << usedExtra) | uint32_t(extra[c][i]));
        }
        return true;
    }

    void feed(const std::vector<uint8_t>& packet, std::vector<float>& out) override {
        BitReader r(packet.data(), packet.size());
        for (int c = 0; c < channels; ++c) output[c].assign(frameLength, 0);
        uint32_t samples = 0;
        int used = 0;
        while (r.left() >= 3) {
            const int kind = int(r.read(3));
            if (kind == 7) break;
            if (kind > 1 && kind != 3) return;
            const int count = kind == 1 ? 2 : 1;
            if (used + count > channels || kOffsets[channels - 1][used] + count > channels) return;
            if (!element(r, kOffsets[channels - 1][used], count, samples)) return;
            used += count;
        }
        if (!samples) return;
        const float scale = 1.0f / float(int64_t(1) << (sampleSize - 1));
        const size_t at = out.size();
        out.resize(at + size_t(samples) * size_t(channels));
        float* to = out.data() + at;
        for (uint32_t i = 0; i < samples; ++i)
            for (int c = 0; c < channels; ++c) *to++ = float(output[c][i]) * scale;
    }
};

}

std::unique_ptr<PacketDecoder> openAlacAudio(const Track& track, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    if (track.config.size() != 24) return fail("ALAC track without its configuration");
    const uint8_t* c = track.config.data();
    auto decoder = std::make_unique<AlacAudio>();
    decoder->frameLength = (uint32_t(c[0]) << 24) | (uint32_t(c[1]) << 16) | (uint32_t(c[2]) << 8) | c[3];
    decoder->sampleSize = c[5];
    decoder->historyMult = c[6];
    decoder->initialHistory = c[7];
    decoder->riceLimit = c[8];
    decoder->channels = c[9];
    decoder->rate = int((uint32_t(c[20]) << 24) | (uint32_t(c[21]) << 16) | (uint32_t(c[22]) << 8) | c[23]);
    if (decoder->frameLength < 1 || decoder->frameLength > (1u << 20) || decoder->channels < 1 || decoder->channels > 8 || decoder->rate < 1 ||
        (decoder->sampleSize != 16 && decoder->sampleSize != 20 && decoder->sampleSize != 24 && decoder->sampleSize != 32) || !decoder->riceLimit) {
        return fail("ALAC configuration not understood");
    }
    decoder->packetFrames = int(decoder->frameLength);
    decoder->longestPacket = int(decoder->frameLength);
    decoder->stateless = true;
    return decoder;
}

}

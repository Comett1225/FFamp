#include "audio_codec.h"
#include "bits.h"
#include <algorithm>

namespace absent {

namespace {

int64_t signedBits(BitReader& r, int bits) {
    if (bits <= 0) return 0;
    if (bits > 32) {
        const uint64_t high = r.read(bits - 32);
        const uint64_t value = (high << 32) | r.read(32);
        return int64_t(value << (64 - bits)) >> (64 - bits);
    }
    const uint32_t value = r.read(bits);
    return int64_t(int32_t(value << (32 - bits)) >> (32 - bits));
}

uint32_t unary(BitReader& r) {
    uint32_t zeros = 0;
    for (;;) {
        const uint64_t window = r.cache();
        if (window) {
            const int run = __builtin_clzll(window);
            if (run < 56) {
                zeros += uint32_t(run);
                r.skip(run + 1);
                return zeros;
            }
        }
        if (r.left() <= 0 || zeros > (1u << 24)) return zeros;
        zeros += 48;
        r.skip(48);
    }
}

bool utf8Number(BitReader& r) {
    const uint32_t first = r.read(8);
    int more = 0;
    if (first >= 0xC0) {
        while (more < 7 && (first & (0x80u >> (more + 1)))) ++more;
        if (more > 6) return false;
    } else if (first >= 0x80) {
        return false;
    }
    for (int i = 0; i < more; ++i) if ((r.read(8) & 0xC0) != 0x80) return false;
    return true;
}

struct FlacAudio : PacketDecoder {
    int infoBits = 16;
    int infoRate = 0;
    std::vector<int64_t> samples[8];

    void reset() override {}

    bool residual(BitReader& r, int blocksize, int order, int64_t* out) {
        const int method = int(r.read(2));
        if (method > 1) return false;
        const int partitionOrder = int(r.read(4));
        const int partitions = 1 << partitionOrder;
        if ((blocksize >> partitionOrder) < order || (blocksize & (partitions - 1))) return false;
        const int paramBits = method ? 5 : 4, escape = method ? 31 : 15;
        int at = order;
        for (int p = 0; p < partitions; ++p) {
            const int count = (blocksize >> partitionOrder) - (p == 0 ? order : 0);
            const int param = int(r.read(paramBits));
            if (param == escape) {
                const int raw = int(r.read(5));
                for (int i = 0; i < count; ++i) out[at++] = signedBits(r, raw);
            } else {
                for (int i = 0; i < count; ++i) {
                    const uint64_t folded = (uint64_t(unary(r)) << param) | (param ? r.read(param) : 0);
                    out[at++] = int64_t(folded >> 1) ^ -int64_t(folded & 1);
                }
            }
            if (r.left() < 0) return false;
        }
        return true;
    }

    bool subframe(BitReader& r, int blocksize, int bits, int64_t* out) {
        if (r.read(1)) return false;
        const int type = int(r.read(6));
        int wasted = 0;
        if (r.read(1)) wasted = int(unary(r)) + 1;
        if (wasted >= bits) return false;
        bits -= wasted;
        if (type == 0) {
            const int64_t value = signedBits(r, bits);
            std::fill(out, out + blocksize, value);
        } else if (type == 1) {
            for (int i = 0; i < blocksize; ++i) out[i] = signedBits(r, bits);
        } else if (type >= 8 && type <= 12) {
            const int order = type - 8;
            if (order > blocksize) return false;
            for (int i = 0; i < order; ++i) out[i] = signedBits(r, bits);
            if (!residual(r, blocksize, order, out)) return false;
            for (int i = order; i < blocksize; ++i) {
                switch (order) {
                    case 1: out[i] += out[i - 1]; break;
                    case 2: out[i] += 2 * out[i - 1] - out[i - 2]; break;
                    case 3: out[i] += 3 * out[i - 1] - 3 * out[i - 2] + out[i - 3]; break;
                    case 4: out[i] += 4 * out[i - 1] - 6 * out[i - 2] + 4 * out[i - 3] - out[i - 4]; break;
                    default: break;
                }
            }
        } else if (type >= 32) {
            const int order = (type & 31) + 1;
            if (order > blocksize) return false;
            for (int i = 0; i < order; ++i) out[i] = signedBits(r, bits);
            const int precision = int(r.read(4)) + 1;
            if (precision == 16) return false;
            const int shift = int(signedBits(r, 5));
            if (shift < 0) return false;
            int64_t coefficients[32];
            for (int i = 0; i < order; ++i) coefficients[i] = signedBits(r, precision);
            if (!residual(r, blocksize, order, out)) return false;
            for (int i = order; i < blocksize; ++i) {
                int64_t sum = 0;
                for (int j = 0; j < order; ++j) sum += coefficients[j] * out[i - j - 1];
                out[i] += sum >> shift;
            }
        } else {
            return false;
        }
        if (wasted) for (int i = 0; i < blocksize; ++i) out[i] = int64_t(uint64_t(out[i]) << wasted);
        return true;
    }

    bool frame(const std::vector<uint8_t>& packet, std::vector<float>& out) {
        BitReader r(packet.data(), packet.size());
        if (r.read(15) != 0x7FFC) return false;
        r.read(1);
        const int sizeCode = int(r.read(4)), rateCode = int(r.read(4)), assignment = int(r.read(4)), bitsCode = int(r.read(3));
        r.read(1);
        if (!utf8Number(r) || sizeCode == 0 || rateCode == 15 || assignment > 10 || bitsCode == 3) return false;
        int blocksize = 0;
        if (sizeCode == 1) blocksize = 192;
        else if (sizeCode <= 5) blocksize = 576 << (sizeCode - 2);
        else if (sizeCode == 6) blocksize = int(r.read(8)) + 1;
        else if (sizeCode == 7) blocksize = int(r.read(16)) + 1;
        else blocksize = 256 << (sizeCode - 8);
        if (rateCode == 12) r.read(8);
        else if (rateCode == 13 || rateCode == 14) r.read(16);
        r.read(8);
        static const int sizes[8] = {0, 8, 12, 0, 16, 20, 24, 32};
        const int bits = bitsCode ? sizes[bitsCode] : infoBits;
        const int count = assignment < 8 ? assignment + 1 : 2;
        if (count != channels || bits != infoBits) return false;
        for (int c = 0; c < count; ++c) {
            samples[c].resize(size_t(blocksize));
            const bool side = (assignment == 8 && c == 1) || (assignment == 9 && c == 0) || (assignment == 10 && c == 1);
            if (!subframe(r, blocksize, bits + (side ? 1 : 0), samples[c].data())) return false;
        }
        if (r.left() < 0) return false;
        int64_t* a = samples[0].data();
        int64_t* b = count > 1 ? samples[1].data() : nullptr;
        for (int i = 0; i < blocksize && assignment >= 8; ++i) {
            if (assignment == 8) b[i] = a[i] - b[i];
            else if (assignment == 9) a[i] += b[i];
            else {
                const int64_t mid = int64_t(uint64_t(a[i]) << 1) | (b[i] & 1), side = b[i];
                a[i] = (mid + side) >> 1;
                b[i] = (mid - side) >> 1;
            }
        }
        const float scale = 1.0f / float(int64_t(1) << (bits - 1));
        const size_t at = out.size();
        out.resize(at + size_t(blocksize) * size_t(count));
        float* to = out.data() + at;
        for (int i = 0; i < blocksize; ++i)
            for (int c = 0; c < count; ++c) *to++ = float(samples[c][size_t(i)]) * scale;
        return true;
    }

    void feed(const std::vector<uint8_t>& packet, std::vector<float>& out) override {
        if (packet.size() >= 4) frame(packet, out);
    }
};

}

std::unique_ptr<PacketDecoder> openFlacAudio(const Track& track, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    if (track.config.size() != 34) return fail("FLAC track without its stream information");
    const uint8_t* info = track.config.data();
    auto decoder = std::make_unique<FlacAudio>();
    decoder->rate = int((uint32_t(info[10]) << 12) | (uint32_t(info[11]) << 4) | (info[12] >> 4));
    decoder->channels = ((info[12] >> 1) & 7) + 1;
    decoder->infoBits = (((info[12] & 1) << 4) | (info[13] >> 4)) + 1;
    const int least = (info[0] << 8) | info[1], most = (info[2] << 8) | info[3];
    if (decoder->rate < 1 || decoder->infoBits < 4 || decoder->infoBits > 32) return fail("FLAC stream information not understood");
    decoder->packetFrames = least == most ? least : 0;
    decoder->longestPacket = std::max(16, most);
    decoder->stateless = true;
    return decoder;
}

}

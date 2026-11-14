#include "audio_codec.h"
#include "read_common.h"
#include <algorithm>
#include <bit>
#include <cstring>

namespace absent {

namespace {

const uint32_t kShift1[41] = {0x00000001, 0x00000002, 0x00000004, 0x00000008, 0x00000010, 0x00000020, 0x00000040, 0x00000080, 0x00000100, 0x00000200,
    0x00000400, 0x00000800, 0x00001000, 0x00002000, 0x00004000, 0x00008000, 0x00010000, 0x00020000, 0x00040000, 0x00080000, 0x00100000, 0x00200000,
    0x00400000, 0x00800000, 0x01000000, 0x02000000, 0x04000000, 0x08000000, 0x10000000, 0x20000000, 0x40000000, 0x80000000, 0x80000000, 0x80000000,
    0x80000000, 0x80000000, 0x80000000, 0x80000000, 0x80000000, 0x80000000, 0xFFFFFFFF};
const uint32_t* const kShift16 = kShift1 + 4;
const uint8_t kFilterShift[4] = {10, 9, 10, 12};

struct Filter {
    int32_t qm[8];
    int32_t dx[8];
    int32_t dl[8];
    int32_t shift;
    int32_t round;
    int32_t error;

    int32_t process(int32_t in) {
        uint32_t* q = reinterpret_cast<uint32_t*>(qm);
        if (error < 0) {
            for (int i = 0; i < 8; ++i) q[i] -= uint32_t(dx[i]);
        } else if (error > 0) {
            for (int i = 0; i < 8; ++i) q[i] += uint32_t(dx[i]);
        }
        uint32_t sum = uint32_t(round);
        for (int i = 0; i < 8; ++i) sum += uint32_t(dl[i]) * q[i];
        for (int i = 0; i < 4; ++i) {
            dx[i] = dx[i + 1];
            dl[i] = dl[i + 1];
        }
        dx[4] = (dl[4] >> 30) | 1;
        dx[5] = ((dl[5] >> 30) | 2) & ~1;
        dx[6] = ((dl[6] >> 30) | 2) & ~1;
        dx[7] = ((dl[7] >> 30) | 4) & ~3;
        error = in;
        in += int32_t(sum) >> shift;
        dl[4] = int32_t(-uint32_t(dl[5]));
        dl[5] = int32_t(-uint32_t(dl[6]));
        dl[6] = int32_t(uint32_t(in) - uint32_t(dl[7]));
        dl[7] = in;
        dl[5] = int32_t(uint32_t(dl[5]) + uint32_t(dl[6]));
        dl[4] = int32_t(uint32_t(dl[4]) + uint32_t(dl[5]));
        return in;
    }
};

struct Rice {
    uint32_t k0, k1, sum0, sum1;
};

struct LittleBits {
    const uint8_t* p;
    size_t bits;
    size_t at = 0;

    int64_t left() const { return int64_t(bits) - int64_t(at); }
    uint64_t peek() const {
        const size_t byte = at >> 3, total = bits >> 3;
        uint64_t word = 0;
        if (byte + 8 <= total) {
            memcpy(&word, p + byte, 8);
        } else {
            for (size_t i = 0; byte + i < total && i < 8; ++i) word |= uint64_t(p[byte + i]) << (8 * i);
        }
        return word >> (at & 7);
    }
    uint32_t read(uint32_t n) {
        const uint32_t v = n ? uint32_t(peek() & ((uint64_t(1) << n) - 1)) : 0;
        at += n;
        return v;
    }
    uint32_t unary(int64_t most) {
        uint32_t count = 0;
        while (int64_t(count) < most) {
            const int ones = std::countr_one(peek());
            const int run = int(std::min<int64_t>(std::min(ones, 56), most - int64_t(count)));
            count += uint32_t(run);
            at += size_t(run);
            if (run < 56 || int64_t(count) >= most) {
                if (int64_t(count) < most) ++at;
                break;
            }
        }
        return count;
    }
};

struct TtaDecoder : PacketDecoder {
    int bytes = 2;
    int frameLength = 0;
    int lastFrameLength = 0;
    std::vector<int32_t> buffer;

    void reset() override {}

    bool decode(const std::vector<uint8_t>& packet, std::vector<float>& out) {
        const int ch = channels;
        LittleBits gb{packet.data(), packet.size() * 8};
        Filter filter[16];
        Rice rice[16];
        int32_t predictor[16];
        for (int c = 0; c < ch; ++c) {
            memset(&filter[c], 0, sizeof(Filter));
            filter[c].shift = kFilterShift[bytes - 1];
            filter[c].round = int32_t(kShift1[filter[c].shift - 1]);
            rice[c] = {10, 10, kShift16[10], kShift16[10]};
            predictor[c] = 0;
        }
        int frames = frameLength;
        buffer.assign(size_t(frames) * size_t(ch), 0);
        int c = 0, i = 0;
        for (size_t at = 0; at < size_t(frames) * size_t(ch); ++at) {
            uint32_t unary = gb.unary(gb.left());
            uint32_t k;
            int depth;
            if (unary == 0) {
                depth = 0;
                k = rice[c].k0;
            } else {
                depth = 1;
                k = rice[c].k1;
                --unary;
            }
            if (gb.left() < int64_t(k)) return false;
            int32_t value;
            if (k) {
                if (k > 25 || unary > uint32_t(INT32_MAX) >> k) return false;
                value = int32_t((unary << k) + gb.read(k));
            } else {
                value = int32_t(unary);
            }
            Rice& r = rice[c];
            if (depth == 1) {
                r.sum1 += uint32_t(value) - (r.sum1 >> 4);
                if (r.k1 > 0 && r.sum1 < kShift16[r.k1]) --r.k1;
                else if (r.sum1 > kShift16[r.k1 + 1]) ++r.k1;
                value = int32_t(uint32_t(value) + kShift1[r.k0]);
            }
            r.sum0 += uint32_t(value) - (r.sum0 >> 4);
            if (r.k0 > 0 && r.sum0 < kShift16[r.k0]) --r.k0;
            else if (r.sum0 > kShift16[r.k0 + 1]) ++r.k0;
            value = 1 + ((value >> 1) ^ ((value & 1) - 1));
            value = filter[c].process(value);
            const int64_t p = predictor[c];
            if (bytes == 1) value += int32_t(((uint64_t(p) << 4) - uint64_t(p)) >> 4);
            else value += int32_t(((uint64_t(p) << 5) - uint64_t(p)) >> 5);
            predictor[c] = value;
            buffer[at] = value;
            if (c < ch - 1) {
                ++c;
                continue;
            }
            if (ch > 1) {
                int32_t* q = &buffer[at];
                int32_t* r2 = q - 1;
                *q += *r2 / 2;
                for (; r2 > q - ch; --r2) *r2 = int32_t(uint32_t(*(r2 + 1)) - uint32_t(*r2));
            }
            c = 0;
            ++i;
            if (i == lastFrameLength && gb.left() / 8 == 4) {
                frames = lastFrameLength;
                break;
            }
        }
        gb.at = (gb.at + 7) & ~size_t(7);
        if (gb.left() < 32) return false;
        const size_t base = out.size();
        out.resize(base + size_t(frames) * size_t(ch));
        for (size_t k = 0; k < size_t(frames) * size_t(ch); ++k) {
            const int32_t v = buffer[k];
            out[base + k] = bytes == 1 ? float(int(uint8_t(v + 0x80)) - 128) / 128.0f
                            : bytes == 2 ? float(int16_t(v)) / 32768.0f
                                         : float(int32_t(uint32_t(v) * 256u)) * (1.0f / 2147483648.0f);
        }
        return true;
    }

    void feed(const std::vector<uint8_t>& packet, std::vector<float>& out) override { decode(packet, out); }

    bool counts(Reader&, const Track& track, std::vector<int64_t>& out) override {
        out.assign(track.samples.size(), frameLength);
        if (!out.empty() && lastFrameLength) out.back() = lastFrameLength;
        return true;
    }
};

}

std::unique_ptr<PacketDecoder> openTtaAudio(const Track& track, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    const std::vector<uint8_t>& c = track.config;
    if (c.size() < 22 || memcmp(c.data(), "TTA1", 4) != 0) return fail("TTA stream without its header");
    const int format = int(le16(c.data() + 4));
    auto d = std::make_unique<TtaDecoder>();
    d->channels = int(le16(c.data() + 6));
    const int bits = int(le16(c.data() + 8));
    d->rate = int(le32(c.data() + 10));
    const uint32_t length = le32(c.data() + 14);
    d->bytes = (bits + 7) / 8;
    if (format == 2) return fail("encrypted TTA streams are not supported");
    if (format != 1 || d->channels < 1 || d->channels > 16 || d->rate < 1 || d->rate > 0x7FFFFF || d->bytes < 1 || d->bytes > 3) return fail("TTA layout not supported");
    d->frameLength = 256 * d->rate / 245;
    d->lastFrameLength = int(length % uint32_t(d->frameLength));
    d->packetFrames = d->frameLength;
    d->stateless = true;
    return d;
}

}

#include "audio_codec.h"
#include "read_common.h"
#include "wma.h"
#include <algorithm>
#include <cstring>

namespace absent {

namespace {

const int kMaxChannels = 8;
const int kMaxSubframes = 32;
const int kMaxOrder = 256;
const int kMaxBlock = 1 << 14;
const int kMaxFrameBytes = 32768;

int log2Of(unsigned v) { return v ? 31 - __builtin_clz(v) : 0; }
int ceilLog2(unsigned v) { return log2Of((v - 1u) << 1); }
int signOf(int x) { return (x > 0) - (x < 0); }

int32_t signedRead(BitReader& in, int n) {
    if (!n) return 0;
    const uint32_t v = in.read(n);
    return n == 32 ? int32_t(v) : int32_t(v << (32 - n)) >> (32 - n);
}

struct Lms {
    int order = 0, scaling = 0, coefsend = 0, bitsend = 0, recent = 0;
    int16_t coefs[kMaxOrder + 8] = {};
    int16_t prev16[(kMaxOrder * 2 + 8) * 2] = {};
    int32_t prev32[kMaxOrder * 2 + 8] = {};
    int16_t updates[kMaxOrder * 2 + 8] = {};
};

struct Channel {
    int subframes = 0;
    int lengths[kMaxSubframes + 1] = {};
    int current = 0;
    int decoded = 0;
    int transientCounter = 0;
};

struct State {
    bool arith = false, acFilter = false, interChannel = false, mclms = false, lpc = false;
    int acOrder = 0, acScaling = 0;
    int16_t acCoefs[16] = {};
    int acPrev[kMaxChannels][16] = {};
    int mclmsOrder = 0, mclmsScaling = 0;
    int16_t mclmsCoefs[kMaxChannels * kMaxChannels * 32] = {};
    int16_t mclmsCurrent[kMaxChannels * kMaxChannels] = {};
    int32_t mclmsPrev[kMaxChannels * 2 * 32] = {};
    int32_t mclmsUpdates[kMaxChannels * 2 * 32] = {};
    int mclmsRecent = 0;
    int movingScaling = 0, quantStep = 0;
    int lmsCount[kMaxChannels] = {};
    int coded[kMaxChannels] = {};
    int updateSpeed[kMaxChannels] = {};
    unsigned average[kMaxChannels] = {};
    int lpcCoefs[kMaxChannels][40] = {};
    int lpcOrder = 0, lpcScaling = 0, lpcBits = 0;
};

struct Carry {
    int64_t putCount = 0, savedBits = 0, reading = 0;
    int32_t frameOffset = 0, packetOffset = 0, sequence = 0;
    uint8_t packetLoss = 1, packetDone = 0;
    uint64_t nextPacketStart = 0;
};

struct WmaLossless : PacketDecoder {
    int blockAlign = 0, bitsPerSample = 16;
    unsigned decodeFlags = 0;
    bool lengthPrefix = false, dynamicRange = false, v3 = false;
    int frameLength = 0, log2FrameSize = 0, maxSubframes = 1, minSubframe = 0;
    std::vector<uint8_t> stored;
    int64_t putCount = 0, savedBits = 0;
    int frameOffset = 0, packetOffset = 0, sequence = 0;
    bool packetLoss = true, packetDone = false;
    size_t nextPacketStart = 0;
    BitReader gb, pgb;
    int64_t bufferBits = 0;
    bool parsedAll = false, seekable = false;
    int forSubframe = 0, indexes[kMaxChannels] = {};
    Channel ch[kMaxChannels];
    std::unique_ptr<State> st = std::make_unique<State>();
    std::unique_ptr<Lms[]> filters = std::make_unique<Lms[]>(size_t(kMaxChannels) * 9);
    std::vector<int32_t> residues[kMaxChannels];
    std::vector<int32_t> pcm[kMaxChannels];
    int written[kMaxChannels] = {};
    int samples = 0;
    std::vector<float>* sink = nullptr;
    bool quiet = false;
    int64_t made = 0;

    WmaLossless() = default;
    WmaLossless(const WmaLossless& o) : PacketDecoder(o) {
        blockAlign = o.blockAlign;
        bitsPerSample = o.bitsPerSample;
        decodeFlags = o.decodeFlags;
        lengthPrefix = o.lengthPrefix;
        dynamicRange = o.dynamicRange;
        v3 = o.v3;
        frameLength = o.frameLength;
        log2FrameSize = o.log2FrameSize;
        maxSubframes = o.maxSubframes;
        minSubframe = o.minSubframe;
        stored.assign(o.stored.size(), 0);
        for (int c = 0; c < kMaxChannels; ++c) {
            residues[c] = o.residues[c];
            pcm[c] = o.pcm[c];
        }
    }

    bool setup(const std::vector<uint8_t>& w, std::string* error) {
        auto fail = [&](const char* text) {
            if (error) *error = text;
            return false;
        };
        const int declared = int(le16(w.data() + 2));
        rate = int(le32(w.data() + 4));
        blockAlign = int(le16(w.data() + 12));
        size_t extra = w.size() >= 18 ? std::min<size_t>(le16(w.data() + 16), w.size() - 18) : 0;
        const uint8_t* e = w.data() + 18;
        if (le16(w.data()) == 0xFFFE && extra >= 22) {
            e += 22;
            extra -= 22;
        }
        if (blockAlign <= 0) return fail("Windows Media Audio track without a block size");
        if (extra < 18) return fail("Windows Media Audio 9 Lossless track without its codec data");
        if (rate < 1 || rate > 768000) return fail("Windows Media Audio layout not supported");
        decodeFlags = le16(e + 14);
        const uint32_t mask = le32(e + 2);
        bitsPerSample = int(le16(e));
        if (bitsPerSample != 16 && bitsPerSample != 24) return fail("Windows Media Audio 9 Lossless sample size not supported");
        channels = mask ? __builtin_popcount(mask) : declared;
        if (channels < 1 || channels > kMaxChannels) return fail("Windows Media Audio 9 Lossless layout not supported");
        log2FrameSize = log2Of(unsigned(blockAlign)) + 4;
        lengthPrefix = decodeFlags & 0x40;
        frameLength = 1 << wma::frameLengthBits(rate, 3, decodeFlags);
        const int log2Subframes = int((decodeFlags & 0x38) >> 3);
        maxSubframes = 1 << log2Subframes;
        minSubframe = frameLength / maxSubframes;
        dynamicRange = decodeFlags & 0x80;
        v3 = decodeFlags & 0x100;
        if (maxSubframes > kMaxSubframes || frameLength > kMaxBlock || minSubframe < 1) return fail("Windows Media Audio 9 Lossless layout not supported");
        stored.assign(size_t(kMaxFrameBytes) * size_t(channels) + 64, 0);
        for (int c = 0; c < channels; ++c) {
            residues[c].assign(size_t(kMaxBlock), 0);
            pcm[c].assign(size_t(frameLength), 0);
        }
        reset();
        return true;
    }

    void reset() override {
        *st = State();
        for (int i = 0; i < kMaxChannels * 9; ++i) filters[size_t(i)] = Lms();
        for (int c = 0; c < channels; ++c) ch[c] = Channel();
        restore(Carry());
    }

    Lms& lms(int c, int i) { return filters[size_t(c) * 9 + size_t(i)]; }

    void restore(const Carry& k) {
        putCount = k.putCount;
        savedBits = k.savedBits;
        frameOffset = k.frameOffset;
        packetOffset = k.packetOffset;
        sequence = k.sequence;
        packetLoss = k.packetLoss;
        packetDone = k.packetDone;
        nextPacketStart = size_t(k.nextPacketStart);
        gb = BitReader(stored.data(), stored.size(), savedBits);
        gb.skip(k.reading);
    }

    bool save(std::vector<uint8_t>& out) const override {
        Carry k;
        k.putCount = putCount;
        k.savedBits = savedBits;
        k.reading = gb.position();
        k.frameOffset = frameOffset;
        k.packetOffset = packetOffset;
        k.sequence = sequence;
        k.packetLoss = packetLoss;
        k.packetDone = packetDone;
        k.nextPacketStart = nextPacketStart;
        const size_t bytes = std::min(stored.size(), size_t((std::max(putCount, savedBits) + 7) >> 3));
        out.resize(sizeof k + sizeof(State));
        memcpy(out.data(), &k, sizeof k);
        memcpy(out.data() + sizeof k, st.get(), sizeof(State));
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < st->lmsCount[c]; ++i) {
                const uint8_t* from = reinterpret_cast<const uint8_t*>(&filters[size_t(c) * 9 + size_t(i)]);
                out.insert(out.end(), from, from + sizeof(Lms));
            }
        out.insert(out.end(), stored.begin(), stored.begin() + long(bytes));
        return true;
    }

    void load(const std::vector<uint8_t>& in) override {
        if (in.size() < sizeof(Carry) + sizeof(State)) return;
        Carry k;
        memcpy(&k, in.data(), sizeof k);
        memcpy(st.get(), in.data() + sizeof k, sizeof(State));
        size_t at = sizeof k + sizeof(State);
        for (int i = 0; i < kMaxChannels * 9; ++i) filters[size_t(i)] = Lms();
        for (int c = 0; c < channels; ++c) {
            st->lmsCount[c] = std::clamp(st->lmsCount[c], 0, 9);
            for (int i = 0; i < st->lmsCount[c] && in.size() - at >= sizeof(Lms); ++i, at += sizeof(Lms)) memcpy(&lms(c, i), in.data() + at, sizeof(Lms));
        }
        std::fill(stored.begin(), stored.end(), 0);
        memcpy(stored.data(), in.data() + at, std::min(in.size() - at, stored.size()));
        k.savedBits = std::clamp<int64_t>(k.savedBits, 0, int64_t(stored.size()) * 8);
        k.putCount = std::clamp<int64_t>(k.putCount, 0, int64_t(stored.size() - 64) * 8);
        restore(k);
    }

    int subframeLengthAt(int offset) {
        if (offset == frameLength - minSubframe) return minSubframe;
        const int ratio = int(gb.read(log2Of(unsigned(maxSubframes - 1)) + 1));
        const int length = minSubframe * (ratio + 1);
        if (length < minSubframe || length > frameLength) return -1;
        return length;
    }

    bool tiles() {
        int samplesOf[kMaxChannels] = {};
        bool contains[kMaxChannels] = {};
        int forCurrent = channels, least = 0;
        for (int c = 0; c < channels; ++c) ch[c].subframes = 0;
        const bool fixed = gb.bit() || maxSubframes == 1;
        do {
            bool used = false;
            for (int c = 0; c < channels; ++c) {
                if (samplesOf[c] == least) contains[c] = fixed || forCurrent == 1 || least == frameLength - minSubframe ? true : gb.bit();
                else contains[c] = false;
                used |= contains[c];
            }
            if (!used) return false;
            const int length = subframeLengthAt(least);
            if (length <= 0) return false;
            least += length;
            for (int c = 0; c < channels; ++c) {
                Channel& h = ch[c];
                if (contains[c]) {
                    if (h.subframes >= kMaxSubframes) return false;
                    h.lengths[h.subframes++] = length;
                    samplesOf[c] += length;
                    if (samplesOf[c] > frameLength) return false;
                } else if (samplesOf[c] <= least) {
                    if (samplesOf[c] < least) {
                        forCurrent = 0;
                        least = samplesOf[c];
                    }
                    ++forCurrent;
                }
            }
        } while (least < frameLength);
        return true;
    }

    bool cdlms() {
        State& s = *st;
        const bool send = gb.bit();
        for (int c = 0; c < channels; ++c) {
            s.lmsCount[c] = int(gb.read(3)) + 1;
            for (int i = 0; i < s.lmsCount[c]; ++i) {
                lms(c, i).order = (int(gb.read(7)) + 1) * 8;
                if (lms(c, i).order > kMaxOrder) {
                    lms(0, 0).order = 0;
                    return false;
                }
            }
            for (int i = 0; i < s.lmsCount[c]; ++i) lms(c, i).scaling = int(gb.read(4));
            if (send) {
                for (int i = 0; i < s.lmsCount[c]; ++i) {
                    Lms& l = lms(c, i);
                    l.coefsend = int(gb.read(ceilLog2(unsigned(l.order)))) + 1;
                    l.bitsend = int(gb.read(ceilLog2(unsigned(l.scaling + 1)))) + 2;
                    const int left = 32 - l.bitsend, right = 32 - l.scaling - 2;
                    for (int j = 0; j < l.coefsend; ++j) l.coefs[j] = int16_t((gb.read(l.bitsend) << left) >> right);
                }
            }
            for (int i = 0; i < s.lmsCount[c]; ++i) std::fill(lms(c, i).coefs + lms(c, i).order, lms(c, i).coefs + lms(c, i).order + 8, int16_t(0));
        }
        return true;
    }

    void channelResidues(int c, int size) {
        State& s = *st;
        int32_t* out = residues[c].data();
        if (gb.bit()) {
            gb.read(log2Of(unsigned(size)));
            ch[c].transientCounter = std::max(ch[c].transientCounter, frameLength / 2);
        }
        int i = 0;
        if (seekable) {
            const unsigned mean = gb.read(bitsPerSample);
            s.average[c] = mean << (s.movingScaling + 1);
            out[0] = signedRead(gb, s.interChannel ? bitsPerSample + 1 : bitsPerSample);
            ++i;
        }
        for (; i < size; ++i) {
            unsigned quo = 0, residue;
            while (gb.bit()) {
                ++quo;
                if (gb.left() <= 0) return;
            }
            if (quo >= 32) quo += gb.read(int(gb.read(5)) + 1);
            const unsigned mean = (s.average[c] + (1u << s.movingScaling)) >> (s.movingScaling + 1);
            if (mean <= 1) {
                residue = quo;
            } else {
                const int bits = ceilLog2(mean);
                const unsigned rem = gb.read(bits);
                residue = (quo << (bits & 31)) + rem;
            }
            s.average[c] = residue + s.average[c] - (s.average[c] >> s.movingScaling);
            out[i] = int32_t((residue >> 1) ^ (0u - (residue & 1)));
        }
    }

    void clearBuffers() {
        State& s = *st;
        memset(s.acCoefs, 0, sizeof s.acCoefs);
        memset(s.acPrev, 0, sizeof s.acPrev);
        memset(s.lpcCoefs, 0, sizeof s.lpcCoefs);
        memset(s.mclmsCoefs, 0, sizeof s.mclmsCoefs);
        memset(s.mclmsCurrent, 0, sizeof s.mclmsCurrent);
        memset(s.mclmsPrev, 0, sizeof s.mclmsPrev);
        memset(s.mclmsUpdates, 0, sizeof s.mclmsUpdates);
        for (int c = 0; c < channels; ++c) {
            for (int i = 0; i < s.lmsCount[c]; ++i) {
                Lms& l = lms(c, i);
                memset(l.coefs, 0, sizeof l.coefs);
                memset(l.prev16, 0, sizeof l.prev16);
                memset(l.prev32, 0, sizeof l.prev32);
                memset(l.updates, 0, sizeof l.updates);
            }
            s.average[c] = 0;
        }
    }

    void mclmsRevert(int size) {
        State& s = *st;
        const int order = s.mclmsOrder, n = channels, range = 1 << (bitsPerSample - 1);
        int pred[kMaxChannels] = {};
        for (int k = 0; k < size; ++k) {
            for (int c = 0; c < n; ++c) {
                pred[c] = 0;
                if (!s.coded[c]) continue;
                uint32_t sum = 0;
                for (int i = 0; i < order * n; ++i) sum += uint32_t(s.mclmsPrev[i + s.mclmsRecent]) * uint32_t(int32_t(s.mclmsCoefs[i + order * n * c]));
                for (int i = 0; i < c; ++i) sum += uint32_t(residues[i][size_t(k)]) * uint32_t(int32_t(s.mclmsCurrent[i + n * c]));
                sum += (1u << s.mclmsScaling) >> 1;
                pred[c] = int32_t(sum) >> s.mclmsScaling;
                residues[c][size_t(k)] = int32_t(uint32_t(residues[c][size_t(k)]) + uint32_t(pred[c]));
            }
            for (int c = 0; c < n; ++c) {
                const int error = int32_t(uint32_t(residues[c][size_t(k)]) - uint32_t(pred[c]));
                if (!error) continue;
                const int way = error > 0 ? 1 : -1;
                for (int i = 0; i < order * n; ++i) s.mclmsCoefs[i + c * order * n] = int16_t(s.mclmsCoefs[i + c * order * n] + way * s.mclmsUpdates[s.mclmsRecent + i]);
                for (int j = 0; j < c; ++j) s.mclmsCurrent[c * n + j] = int16_t(s.mclmsCurrent[c * n + j] + way * signOf(residues[j][size_t(k)]));
            }
            for (int c = n - 1; c >= 0; --c) {
                --s.mclmsRecent;
                s.mclmsPrev[s.mclmsRecent] = std::clamp(residues[c][size_t(k)], -range, range - 1);
                s.mclmsUpdates[s.mclmsRecent] = signOf(residues[c][size_t(k)]);
            }
            if (s.mclmsRecent == 0) {
                memcpy(&s.mclmsPrev[order * n], s.mclmsPrev, sizeof(int32_t) * size_t(order * n));
                memcpy(&s.mclmsUpdates[order * n], s.mclmsUpdates, sizeof(int32_t) * size_t(order * n));
                s.mclmsRecent = n * order;
            }
        }
    }

    void updateSpeed(int c, int speed) {
        State& s = *st;
        for (int i = s.lmsCount[c] - 1; i >= 0; --i) {
            Lms& l = lms(c, i);
            if (s.updateSpeed[c] == speed) continue;
            int16_t* u = v3 ? l.updates + l.recent : l.updates;
            for (int k = 0; k < l.order; ++k) u[k] = int16_t(speed == 16 ? u[k] * 2 : u[k] / 2);
        }
        s.updateSpeed[c] = speed;
    }

    template <typename T, size_t N>
    void lmsRevert(int c, int size, T (Lms::*storage)[N]) {
        State& s = *st;
        const int range = 1 << (bitsPerSample - 1);
        const int round = sizeof(T) == 2 ? 16 : 8;
        for (int il = s.lmsCount[c] - 1; il >= 0; --il) {
            Lms& l = lms(c, il);
            T* prev = l.*storage;
            const int order = l.order, span = (order + round - 1) & ~(round - 1);
            for (int k = 0; k < size; ++k) {
                const int residue = residues[c][size_t(k)];
                const int mul = signOf(residue);
                int16_t* v1 = l.coefs;
                const auto* v2 = prev + l.recent;
                const int16_t* v3 = l.updates + l.recent;
                uint32_t sum = 0;
                for (int j = 0; j < span; ++j) {
                    sum += uint32_t(int32_t(v1[j])) * uint32_t(int32_t(v2[j]));
                    v1[j] = int16_t(v1[j] + mul * v3[j]);
                }
                const uint32_t pred = ((1u << l.scaling) >> 1) + sum;
                const int input = int32_t(uint32_t(residue) + uint32_t(int32_t(pred) >> l.scaling));
                int recent = l.recent;
                if (recent) {
                    --recent;
                } else {
                    memcpy(prev + order, prev, sizeof(T) * size_t(order));
                    memcpy(l.updates + order, l.updates, sizeof(int16_t) * size_t(order));
                    recent = order - 1;
                }
                prev[recent] = T(std::clamp(input, -range, range - 1));
                l.updates[recent] = int16_t(signOf(input) * s.updateSpeed[c]);
                l.updates[recent + (order >> 4)] >>= 2;
                l.updates[recent + (order >> 3)] >>= 1;
                l.recent = recent;
                std::fill(l.updates + recent + order, l.updates + kMaxOrder * 2 + 8, int16_t(0));
                residues[c][size_t(k)] = input;
            }
        }
    }

    void acRevert(int size) {
        State& s = *st;
        const int order = s.acOrder, scaling = s.acScaling;
        for (int c = 0; c < channels; ++c) {
            int* prev = s.acPrev[c];
            int32_t* r = residues[c].data();
            for (int i = 0; i < size; ++i) {
                uint32_t pred = 0;
                for (int j = 0; j < order; ++j) {
                    if (i <= j) {
                        pred += uint32_t(int32_t(s.acCoefs[j])) * uint32_t(prev[j - i]);
                    } else {
                        pred += uint32_t(r[i - j - 1]) * uint32_t(int32_t(s.acCoefs[j]));
                    }
                }
                r[i] = int32_t(uint32_t(r[i]) + uint32_t(int32_t(pred) >> scaling));
            }
            for (int j = order - 1; j >= 0; --j) prev[j] = size <= j ? prev[j - size] : r[size - j - 1];
        }
    }

    int subframe() {
        State& s = *st;
        int offset = frameLength, length = frameLength;
        for (int c = 0; c < channels; ++c) {
            if (offset > ch[c].decoded) {
                offset = ch[c].decoded;
                length = ch[c].lengths[ch[c].current];
            }
        }
        forSubframe = 0;
        int total = frameLength * channels;
        for (int c = 0; c < channels; ++c) {
            Channel& h = ch[c];
            total -= h.decoded;
            if (offset == h.decoded && length == h.lengths[h.current]) {
                total -= h.lengths[h.current];
                h.decoded += h.lengths[h.current];
                indexes[forSubframe++] = c;
            }
        }
        if (!total) parsedAll = true;
        if (length < 1 || length > frameLength) return -1;
        seekable = gb.bit();
        if (seekable) {
            clearBuffers();
            s.arith = gb.bit();
            if (s.arith) return -1;
            s.acFilter = gb.bit();
            s.interChannel = gb.bit();
            s.mclms = gb.bit();
            if (s.acFilter) {
                s.acOrder = int(gb.read(4)) + 1;
                s.acScaling = int(gb.read(4));
                for (int i = 0; i < s.acOrder; ++i) s.acCoefs[i] = int16_t(gb.read(s.acScaling) + 1);
            }
            if (s.mclms) {
                s.mclmsOrder = (int(gb.read(4)) + 1) * 2;
                s.mclmsScaling = int(gb.read(4));
                if (gb.bit()) {
                    const int bits = int(gb.read(ceilLog2(unsigned(s.mclmsScaling + 1)))) + 2;
                    for (int i = 0; i < s.mclmsOrder * channels * channels; ++i) s.mclmsCoefs[i] = int16_t(gb.read(bits));
                    for (int i = 0; i < channels; ++i)
                        for (int c = 0; c < i; ++c) s.mclmsCurrent[i * channels + c] = int16_t(gb.read(bits));
                }
            }
            if (!cdlms()) return -1;
            s.movingScaling = int(gb.read(3));
            s.quantStep = int(gb.read(8)) + 1;
            s.mclmsRecent = s.mclmsOrder * channels;
            for (int c = 0; c < channels; ++c) {
                for (int i = 0; i < s.lmsCount[c]; ++i) lms(c, i).recent = lms(c, i).order;
                ch[c].transientCounter = frameLength;
            }
        }
        const bool raw = gb.bit();
        if (!raw && !lms(0, 0).order) return -1;
        for (int c = 0; c < channels; ++c) s.coded[c] = 1;
        if (!raw) {
            for (int c = 0; c < channels; ++c) s.coded[c] = gb.bit();
            s.lpc = false;
            if (v3) {
                s.lpc = gb.bit();
                if (s.lpc) {
                    s.lpcOrder = int(gb.read(5)) + 1;
                    s.lpcScaling = int(gb.read(4));
                    s.lpcBits = int(gb.read(3)) + 1;
                    for (int c = 0; c < channels; ++c)
                        for (int i = 0; i < s.lpcOrder; ++i) s.lpcCoefs[c][i] = signedRead(gb, s.lpcScaling + s.lpcBits);
                }
            }
        }
        if (gb.left() < 1) return -1;
        const int padding = gb.bit() ? int(gb.read(5)) : 0;
        if (raw) {
            const int bits = bitsPerSample - padding;
            if (bits <= 0) return -1;
            for (int c = 0; c < channels; ++c)
                for (int j = 0; j < length; ++j) residues[c][size_t(j)] = signedRead(gb, bits);
        } else {
            if (bitsPerSample < padding) return -1;
            for (int c = 0; c < channels; ++c) {
                if (s.coded[c]) {
                    channelResidues(c, length);
                    if (quiet) continue;
                    updateSpeed(c, seekable ? 16 : 8);
                    if (bitsPerSample > 16) lmsRevert(c, length, &Lms::prev32);
                    else lmsRevert(c, length, &Lms::prev16);
                } else if (!quiet) {
                    std::fill(residues[c].begin(), residues[c].begin() + length, 0);
                }
            }
            if (!quiet) {
                if (s.mclms) mclmsRevert(length);
                if (s.interChannel && channels == 2 && (s.coded[0] || s.coded[1])) {
                    for (int k = 0; k < length; ++k) {
                        residues[0][size_t(k)] = int32_t(uint32_t(residues[0][size_t(k)]) - uint32_t(residues[1][size_t(k)] >> 1));
                        residues[1][size_t(k)] = int32_t(uint32_t(residues[1][size_t(k)]) + uint32_t(residues[0][size_t(k)]));
                    }
                }
                if (s.acFilter) acRevert(length);
                if (s.quantStep != 1)
                    for (int c = 0; c < channels; ++c)
                        for (int k = 0; k < length; ++k) residues[c][size_t(k)] = int32_t(uint32_t(residues[c][size_t(k)]) * unsigned(s.quantStep));
            }
        }
        for (int i = 0; i < forSubframe; ++i) {
            const int c = indexes[i];
            int32_t* to = pcm[c].data();
            for (int j = 0; j < length && written[c] < frameLength; ++j) {
                const int32_t v = residues[c][size_t(j)];
                to[written[c]++] = bitsPerSample == 16 ? int32_t(int16_t(int32_t(int16_t(v)) * (1 << padding))) : int32_t(uint32_t(v) * (256u << padding));
            }
        }
        for (int i = 0; i < forSubframe; ++i) {
            Channel& h = ch[indexes[i]];
            if (h.current >= h.subframes) return -1;
            ++h.current;
        }
        return 0;
    }

    int frame() {
        samples = frameLength;
        for (int c = 0; c < channels; ++c) written[c] = 0;
        int length = 0;
        if (lengthPrefix) length = int(gb.read(log2FrameSize));
        if (!tiles()) {
            packetLoss = true;
            samples = 0;
            return -1;
        }
        if (dynamicRange) gb.skip(8);
        if (gb.bit()) {
            if (gb.bit()) gb.skip(log2Of(unsigned(frameLength * 2)));
            if (gb.bit()) {
                samples -= int(gb.read(log2Of(unsigned(frameLength * 2))));
                if (samples <= 0) return -1;
            }
        }
        parsedAll = false;
        for (int c = 0; c < channels; ++c) ch[c].decoded = ch[c].current = 0;
        while (!parsedAll) {
            const int decoded = ch[0].decoded;
            if (subframe() < 0) {
                packetLoss = true;
                if (samples) samples = decoded;
                return 0;
            }
        }
        if (lengthPrefix) {
            if (length != (gb.position() - frameOffset) + 2) {
                packetLoss = true;
                return 0;
            }
            gb.skip(length - (gb.position() - frameOffset) - 1);
        }
        return int(gb.bit());
    }

    void emit() {
        if (samples <= 0) return;
        made += samples;
        if (quiet) return;
        const size_t at = sink->size();
        sink->resize(at + size_t(samples) * size_t(channels));
        float* to = sink->data() + at;
        const float scale = bitsPerSample == 16 ? 1.0f / 32768 : 1.0f / 2147483648.0f;
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < samples; ++i) to[size_t(i) * size_t(channels) + size_t(c)] = float(pcm[c][size_t(i)]) * scale;
    }

    void write(uint32_t v, int n) {
        while (n > 0) {
            const size_t byte = size_t(putCount >> 3);
            const int free = 8 - int(putCount & 7);
            if (free == 8) stored[byte] = 0;
            const int take = std::min(free, n);
            stored[byte] |= uint8_t(((v >> (n - take)) & ((1u << take) - 1)) << (free - take));
            putCount += take;
            n -= take;
        }
    }

    void copy(const uint8_t* data, size_t size, int64_t at, int64_t n) {
        if (!(at & 7) && size_t((at + n + 7) >> 3) <= size) {
            const uint8_t* src = data + (at >> 3);
            uint8_t* dst = stored.data() + (putCount >> 3);
            const int64_t whole = n >> 3;
            const int r = int(putCount & 7);
            if (!r) {
                memcpy(dst, src, size_t(whole));
            } else {
                for (int64_t i = 0; i < whole; ++i) {
                    dst[i] = uint8_t((dst[i] & (0xFF00 >> r)) | (src[i] >> r));
                    dst[i + 1] = uint8_t(src[i] << (8 - r));
                }
            }
            putCount += whole * 8;
            if (n & 7) write(uint32_t(src[whole] >> (8 - (n & 7))), int(n & 7));
            return;
        }
        BitReader from(data, size);
        from.skip(at);
        for (; n >= 24; n -= 24) write(from.read(24), 24);
        if (n > 0) write(from.read(int(n)), int(n));
    }

    void saveBits(const uint8_t* data, size_t size, int64_t length, bool append) {
        if (!append) {
            frameOffset = int(pgb.position() & 7);
            savedBits = frameOffset;
            putCount = 0;
        }
        if (length <= 0 || (savedBits + length + 8) >> 3 > int64_t(stored.size()) - 64) {
            packetLoss = true;
            savedBits = 0;
            return;
        }
        savedBits += length;
        if (!append) {
            copy(data, size, pgb.position() & ~int64_t(7), savedBits);
        } else {
            const int align = int(std::min<int64_t>(8 - (pgb.position() & 7), length));
            write(pgb.read(align), align);
            length -= align;
            copy(data, size, pgb.position(), length);
        }
        pgb.skip(length);
        gb = BitReader(stored.data(), stored.size(), savedBits);
        gb.skip(frameOffset);
    }

    int64_t call(const uint8_t* data, size_t size) {
        samples = 0;
        size_t span;
        int64_t remaining;
        if (packetDone || packetLoss) {
            packetDone = false;
            span = std::min(size_t(blockAlign), size);
            nextPacketStart = size - span;
            bufferBits = int64_t(span) * 8;
            pgb = BitReader(data, span);
            const int number = int(pgb.read(4));
            pgb.skip(2);
            int64_t previous = pgb.read(log2FrameSize);
            if (!packetLoss && ((sequence + 1) & 15) != number) packetLoss = true;
            sequence = number;
            if (previous > 0) {
                remaining = bufferBits - pgb.position();
                if (previous >= remaining) {
                    previous = remaining;
                    packetDone = true;
                }
                saveBits(data, span, previous, true);
                if (previous < remaining && !packetLoss) frame();
            }
            if (packetLoss) {
                savedBits = 0;
                packetLoss = false;
                putCount = 0;
            }
        } else {
            if (size < nextPacketStart) {
                packetLoss = true;
                return -1;
            }
            span = size - nextPacketStart;
            bufferBits = int64_t(span) * 8;
            pgb = BitReader(data, span);
            pgb.skip(packetOffset);
            remaining = bufferBits - pgb.position();
            int64_t frameSize = 0;
            if (lengthPrefix && remaining > log2FrameSize && (frameSize = pgb.peek(log2FrameSize)) && frameSize <= remaining) {
                saveBits(data, span, frameSize, false);
                if (!packetLoss) packetDone = !frame();
            } else if (!lengthPrefix && savedBits > gb.position()) {
                packetDone = !frame();
            } else {
                packetDone = true;
            }
        }
        remaining = bufferBits - pgb.position();
        if (remaining < 0) packetLoss = true;
        if (packetDone && !packetLoss && remaining > 0) saveBits(data, span, remaining, false);
        emit();
        packetOffset = int(pgb.position() & 7);
        return packetLoss ? -1 : pgb.position() >> 3;
    }

    void run(const std::vector<uint8_t>& packet) {
        const uint8_t* data = packet.data();
        size_t size = packet.size();
        for (int turn = 0; size > 0 && turn < (1 << 16); ++turn) {
            const int64_t used = call(data, size);
            if (used < 0 || size_t(used) >= size) return;
            data += used;
            size -= size_t(used);
        }
    }

    void feed(const std::vector<uint8_t>& packet, std::vector<float>& to) override {
        sink = &to;
        run(packet);
        sink = nullptr;
    }

    void finish(std::vector<float>& to) override {
        sink = &to;
        for (int turn = 0; turn < (1 << 16) && savedBits > gb.position(); ++turn) {
            samples = 0;
            packetDone = false;
            if (!frame()) savedBits = 0;
            if (samples <= 0) break;
            if (!packetLoss) emit();
        }
        sink = nullptr;
    }

    bool counts(Reader& file, const Track& track, std::vector<int64_t>& frames) override {
        const size_t n = track.samples.size();
        frames.assign(n, 0);
        WmaLossless look(*this);
        look.reset();
        look.quiet = true;
        std::vector<uint8_t> packet;
        std::vector<float> none;
        for (size_t i = 0; i < n; ++i) {
            packet.resize(track.samples[i].size);
            look.made = 0;
            if (!packet.empty() && file.read(track, uint32_t(i), packet.data())) look.run(packet);
            if (i + 1 == n) look.finish(none);
            frames[i] = look.made;
        }
        return true;
    }
};

}

std::unique_ptr<PacketDecoder> openWmaLosslessAudio(const Track& track, std::string* error) {
    const std::vector<uint8_t>& w = track.config;
    if (track.configType != fourcc("wfex") || w.size() < 16) {
        if (error) *error = "Windows Media Audio track without its format header";
        return nullptr;
    }
    auto audio = std::make_unique<WmaLossless>();
    if (!audio->setup(w, error)) return nullptr;
    audio->prerollPackets = 3;
    audio->prerollFrames = 2 * audio->frameLength;
    audio->longestPacket = audio->frameLength;
    return audio;
}

}

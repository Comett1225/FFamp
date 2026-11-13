#include "audio_codec.h"
#include "read_common.h"
#include "mdct.h"
#include "wma.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace absent {

namespace {

using namespace wma;

const int kMaxSize = 2048;
const int kSizes = 5;
const int kHighMax = 16;
const int kReservoirMax = 32768;
const int kNoiseSize = 8192;

const Vlc& gainCodes() {
    static const Vlc codes = [] {
        Vlc v;
        v.buildInOrder(9, 37, kHighGainLengths, kHighGainSymbols, -18);
        return v;
    }();
    return codes;
}

const Vlc& exponentCodes() {
    static const Vlc codes = [] {
        Vlc v;
        v.build(8, 121, kScalefactorLengths, kScalefactorCodes);
        return v;
    }();
    return codes;
}

int floorLog2(unsigned v) { return v ? 31 - __builtin_clz(v) : 0; }

double tenTo(double x) { return exp2(3.32192809488736234787 * x); }

struct Carry {
    int32_t noiseIndex = 0;
    int32_t lastBitOffset = 0;
    int32_t lastLength = 0;
    int32_t resetBlockLengths = 1;
    int32_t blockBits = 0;
    int32_t nextBlockBits = 0;
    int32_t prevBlockBits = 0;
    uint8_t exponentsReady[2] = {};
};

struct WmaAudio : PacketDecoder {
    int version = 2;
    int blockAlign = 0;
    bool expVlc = false, reservoir = false, variable = false, noiseCoding = true;
    int byteOffsetBits = 0, frameBits = 0, frameLength = 0, blockSizes = 1;
    int exponentSizes[kSizes] = {};
    uint16_t exponentBands[kSizes][25] = {};
    int highBandStart[kSizes] = {};
    int coefsStart = 0;
    int coefsEnd[kSizes] = {};
    int highSizes[kSizes] = {};
    int highBands[kSizes][kHighMax] = {};
    float noiseMult = 0;
    float noiseTable[kNoiseSize] = {};
    const RunLevel* coefTables[2] = {};
    std::vector<InverseMdct> mdct;
    const float* windows[kSizes] = {};
    float lspCos[kMaxSize] = {}, lspPowE[256] = {}, lspPowM1[128] = {}, lspPowM2[128] = {};

    Carry c;
    int blockLength = 0, blockPos = 0;
    bool msStereo = false;
    bool coded[2] = {};
    int exponentsSize[2] = {};
    float exponents[2][kMaxSize] = {};
    float maxExponent[2] = {1.0f, 1.0f};
    float coefs1[2][kMaxSize] = {};
    float coefs[2][kMaxSize] = {};
    float output[kMaxSize * 2] = {};
    float frameOut[2][kMaxSize * 2] = {};
    uint8_t stored[kReservoirMax + 64] = {};
    int highCoded[2][kHighMax] = {};
    int highValues[2][kHighMax] = {};
    BitReader in;
    bool quiet = false;

    bool setup(int rate, int count, int64_t bitRate, unsigned flags) {
        if (rate < 1 || rate > 50000 || count < 1 || count > 2 || bitRate <= 0) return false;
        this->rate = rate;
        channels = count;
        frameBits = frameLengthBits(rate, version, 0);
        frameLength = 1 << frameBits;
        if (variable) {
            int nb = int((flags >> 3) & 3) + 1;
            if (bitRate / count >= 32000) nb += 2;
            blockSizes = std::min(nb, frameBits - 7) + 1;
        }
        float highFrequency = float(rate * 0.5);
        int normal = rate;
        if (version == 2) normal = rate >= 44100 ? 44100 : rate >= 22050 ? 22050 : rate >= 16000 ? 16000 : rate >= 11025 ? 11025 : rate >= 8000 ? 8000 : rate;
        const float bps = float(bitRate) / float(count * rate);
        const double frameBytes = bps * float(frameLength) / 8.0 + 0.5;
        if (!(frameBytes < 2147483647.0)) return false;
        byteOffsetBits = floorLog2(unsigned(int(frameBytes))) + 2;
        if (byteOffsetBits + 3 > 25) return false;
        float bps1 = bps;
        if (count == 2) bps1 = float(bps * 1.6);
        if (normal == 44100) {
            if (bps1 >= 0.61) noiseCoding = false;
            else highFrequency = float(highFrequency * 0.4);
        } else if (normal == 22050) {
            if (bps1 >= 1.16) noiseCoding = false;
            else highFrequency = float(highFrequency * (bps1 >= 0.72 ? 0.7 : 0.6));
        } else if (normal == 16000) {
            highFrequency = float(highFrequency * (bps > 0.5 ? 0.5 : 0.3));
        } else if (normal == 11025) {
            highFrequency = float(highFrequency * 0.7);
        } else if (normal == 8000) {
            if (bps <= 0.625) highFrequency = float(highFrequency * 0.5);
            else if (bps > 0.75) noiseCoding = false;
            else highFrequency = float(highFrequency * 0.65);
        } else {
            highFrequency = float(highFrequency * (bps >= 0.8 ? 0.75 : bps >= 0.6 ? 0.6 : 0.5));
        }
        coefsStart = version == 1 ? 3 : 0;
        for (int k = 0; k < blockSizes; ++k) {
            const int length = frameLength >> k;
            if (version == 1) {
                int last = 0, i = 0;
                for (; i < 25; ++i) {
                    int at = (length * 2 * kCriticalFrequencies[i] + (rate >> 1)) / rate;
                    at = std::min(at, length);
                    exponentBands[0][i] = uint16_t(at - last);
                    if (at >= length) {
                        ++i;
                        break;
                    }
                    last = at;
                }
                exponentSizes[0] = i;
            } else {
                const int row = frameBits - 7 - k;
                const uint8_t* table = nullptr;
                if (row < 3) table = rate >= 44100 ? kExponentBands[2][row] : rate >= 32000 ? kExponentBands[1][row] : rate >= 22050 ? kExponentBands[0][row] : nullptr;
                if (table) {
                    exponentSizes[k] = table[0];
                    for (int i = 0; i < table[0]; ++i) exponentBands[k][i] = table[i + 1];
                } else {
                    int j = 0, last = 0;
                    for (int i = 0; i < 25; ++i) {
                        int at = (length * 2 * kCriticalFrequencies[i] + (rate << 1)) / (4 * rate) * 4;
                        at = std::min(at, length);
                        if (at > last) exponentBands[k][j++] = uint16_t(at - last);
                        if (at >= length) break;
                        last = at;
                    }
                    exponentSizes[k] = j;
                }
            }
            coefsEnd[k] = (frameLength - frameLength * 9 / 100) >> k;
            highBandStart[k] = int(float(length * 2) * highFrequency / float(rate) + 0.5);
            int j = 0, at = 0;
            for (int i = 0; i < exponentSizes[k]; ++i) {
                int start = at;
                at += exponentBands[k][i];
                const int end = std::min(at, coefsEnd[k]);
                start = std::max(start, highBandStart[k]);
                if (end > start && j < kHighMax) highBands[k][j++] = end - start;
            }
            highSizes[k] = j;
        }
        for (int i = 0; i < blockSizes; ++i) {
            mdct.emplace_back(frameBits - i, double(float(1.0 / 32768.0)));
            windows[i] = sineWindow(frameBits - i);
        }
        if (noiseCoding) {
            noiseMult = expVlc ? 0.02f : 0.04f;
            uint32_t seed = 1;
            const float norm = float((1.0 / float(1LL << 31)) * std::sqrt(3.0) * noiseMult);
            for (int i = 0; i < kNoiseSize; ++i) {
                seed = seed * 314159 + 1;
                noiseTable[i] = float(int32_t(seed)) * norm;
            }
        }
        int table = 2;
        if (rate >= 32000) table = bps1 < 0.72 ? 0 : bps1 < 1.16 ? 1 : 2;
        coefTables[0] = &coefficientTable(table * 2);
        coefTables[1] = &coefficientTable(table * 2 + 1);
        if (!expVlc) {
            const float step = float(M_PI / frameLength);
            for (int i = 0; i < frameLength; ++i) lspCos[i] = float(2.0f * cos(double(step * float(i))));
            for (int i = 0; i < 256; ++i) lspPowE[i] = exp2f(float((i - 126) * -0.25));
            float b = 1.0f;
            for (int i = 127; i >= 0; --i) {
                float a = float(float(128 + i) * (0.5 / 128));
                a = float(1 / sqrt(sqrt(double(a))));
                lspPowM1[i] = 2 * a - b;
                lspPowM2[i] = b - a;
                b = a;
            }
        }
        reset();
        return true;
    }

    void reset() override {
        c = Carry();
        c.blockBits = c.nextBlockBits = c.prevBlockBits = frameBits;
        memset(frameOut, 0, sizeof frameOut);
        memset(exponents, 0, sizeof exponents);
        maxExponent[0] = maxExponent[1] = 1.0f;
        msStereo = false;
    }

    float powQuarter(float x) const {
        uint32_t u;
        memcpy(&u, &x, 4);
        const uint32_t e = (u >> 23) & 255, m = (u >> 16) & 127;
        const uint32_t tv = ((u << 7) & ((1u << 23) - 1)) | (127u << 23);
        float t;
        memcpy(&t, &tv, 4);
        return lspPowE[e] * (lspPowM1[m] + lspPowM2[m] * t);
    }

    void exponentsLsp(int ch) {
        float lsp[10];
        for (int i = 0; i < 10; ++i) lsp[i] = kLspCodebook[i][in.read(i == 0 || i >= 8 ? 3 : 4)];
        float top = 0;
        for (int i = 0; i < blockLength; ++i) {
            float p = 0.5f, q = 0.5f;
            const float w = lspCos[i];
            for (int j = 1; j < 10; j += 2) {
                q *= w - lsp[j - 1];
                p *= w - lsp[j];
            }
            p *= p * (2.0f - w);
            q *= q * (2.0f + w);
            const float v = powQuarter(p + q);
            if (v > top) top = v;
            exponents[ch][i] = v;
        }
        maxExponent[ch] = top;
    }

    bool exponentsVlc(int ch) {
        const uint16_t* bands = &exponentBands[0][0] + (frameBits - c.blockBits) * 25;
        const uint16_t* bandsEnd = &exponentBands[0][0] + kSizes * 25;
        float* q = exponents[ch];
        float* const end = q + blockLength;
        float* const limit = q + kMaxSize;
        auto put = [&](float v) {
            int n = bands < bandsEnd ? *bands : 0;
            ++bands;
            for (int k = n ? n : 4; k > 0; --k) {
                if (q < limit) *q = v;
                ++q;
            }
        };
        float top = 0;
        int last = 36;
        if (version == 1) {
            last = int(in.read(5)) + 10;
            top = kPowers[60 + last];
            put(top);
        }
        while (q < end) {
            last += exponentCodes().read(in) - 60;
            if (unsigned(last + 60) >= 156) return false;
            const float v = kPowers[60 + last];
            if (v > top) top = v;
            put(v);
        }
        maxExponent[ch] = top;
        return true;
    }

    void window(float* out) {
        const float* from = output;
        if (c.blockBits <= c.prevBlockBits) {
            const float* w = windows[frameBits - c.blockBits];
            for (int i = 0; i < blockLength; ++i) out[i] = from[i] * w[i] + out[i];
        } else {
            const int length = 1 << c.prevBlockBits, n = (blockLength - length) / 2;
            const float* w = windows[frameBits - c.prevBlockBits];
            for (int i = 0; i < length; ++i) out[n + i] = from[n + i] * w[i] + out[n + i];
            memcpy(out + n + length, from + n + length, size_t(n) * sizeof(float));
        }
        out += blockLength;
        from += blockLength;
        if (c.blockBits <= c.nextBlockBits) {
            const float* w = windows[frameBits - c.blockBits];
            for (int i = 0; i < blockLength; ++i) out[i] = from[i] * w[blockLength - 1 - i];
        } else {
            const int length = 1 << c.nextBlockBits, n = (blockLength - length) / 2;
            const float* w = windows[frameBits - c.nextBlockBits];
            memcpy(out, from, size_t(n) * sizeof(float));
            for (int i = 0; i < length; ++i) out[n + i] = from[n + i] * w[length - 1 - i];
            memset(out + n + length, 0, size_t(n) * sizeof(float));
        }
    }

    float noise() {
        const float v = noiseTable[c.noiseIndex];
        c.noiseIndex = (c.noiseIndex + 1) & (kNoiseSize - 1);
        return v;
    }

    void rebuild(int ch, int bsize, int gain, float norm, int count) {
        const float* e = exponents[ch];
        const int es = exponentsSize[ch];
        const float* c1 = coefs1[ch];
        float* out = coefs[ch];
        float mult = float(tenTo(gain * 0.05) / maxExponent[ch]);
        mult *= norm;
        if (!noiseCoding) {
            for (int i = 0; i < coefsStart; ++i) *out++ = 0.0f;
            for (int i = 0; i < count; ++i) *out++ = c1[i] * e[i << bsize >> es] * mult;
            for (int i = 0; i < blockLength - coefsEnd[bsize]; ++i) *out++ = 0.0f;
            return;
        }
        for (int i = 0; i < coefsStart; ++i) *out++ = noise() * e[i << bsize >> es] * mult;
        const int bands = highSizes[bsize];
        float power[kHighMax];
        int lastHigh = 0;
        int at = highBandStart[bsize] << bsize >> es;
        for (int j = 0; j < bands; ++j) {
            const int n = highBands[bsize][j];
            if (highCoded[ch][j]) {
                float sum = 0;
                for (int i = 0; i < n; ++i) {
                    const float v = e[at + (i << bsize >> es)];
                    sum += v * v;
                }
                power[j] = sum / float(n);
                lastHigh = j;
            }
            at += n << bsize >> es;
        }
        at = coefsStart << bsize >> es;
        for (int j = -1; j < bands; ++j) {
            const int n = j < 0 ? highBandStart[bsize] - coefsStart : highBands[bsize][j];
            if (j >= 0 && highCoded[ch][j]) {
                float mult1 = float(sqrt(double(power[j] / power[lastHigh])));
                mult1 = float(mult1 * tenTo(highValues[ch][j] * 0.05));
                mult1 = mult1 / (maxExponent[ch] * noiseMult);
                mult1 *= norm;
                for (int i = 0; i < n; ++i) *out++ = noise() * e[at + (i << bsize >> es)] * mult1;
            } else {
                for (int i = 0; i < n; ++i) {
                    const float v = noise();
                    *out++ = (*c1++ + v) * e[at + (i << bsize >> es)] * mult;
                }
            }
            at += n << bsize >> es;
        }
        const float mult1 = mult * e[at + ((-(1 << bsize)) >> es)];
        for (int i = 0; i < blockLength - coefsEnd[bsize]; ++i) *out++ = noise() * mult1;
    }

    int block() {
        if (variable) {
            const int n = floorLog2(unsigned(blockSizes - 1)) + 1;
            if (c.resetBlockLengths) {
                c.resetBlockLengths = 0;
                int v = int(in.read(n));
                if (v >= blockSizes) return -1;
                c.prevBlockBits = frameBits - v;
                v = int(in.read(n));
                if (v >= blockSizes) return -1;
                c.blockBits = frameBits - v;
            } else {
                c.prevBlockBits = c.blockBits;
                c.blockBits = c.nextBlockBits;
            }
            const int v = int(in.read(n));
            if (v >= blockSizes) return -1;
            c.nextBlockBits = frameBits - v;
        } else {
            c.nextBlockBits = c.prevBlockBits = c.blockBits = frameBits;
        }
        if (frameBits - c.blockBits >= blockSizes) return -1;
        blockLength = 1 << c.blockBits;
        if (blockPos + blockLength > frameLength) return -1;
        if (channels == 2) msStereo = in.bit();
        bool any = false;
        for (int ch = 0; ch < channels; ++ch) any |= (coded[ch] = in.bit());
        const int bsize = frameBits - c.blockBits;
        if (any) {
            int gain = 1;
            for (;;) {
                if (in.left() < 7) return -1;
                const int a = int(in.read(7));
                gain += a;
                if (a != 127) break;
            }
            const int escapeBits = gain < 15 ? 13 : gain < 32 ? 12 : gain < 40 ? 11 : gain < 45 ? 10 : 9;
            int count[2];
            count[0] = count[1] = coefsEnd[bsize] - coefsStart;
            if (noiseCoding) {
                for (int ch = 0; ch < channels; ++ch) {
                    if (!coded[ch]) continue;
                    for (int i = 0; i < highSizes[bsize]; ++i) {
                        highCoded[ch][i] = in.bit();
                        if (highCoded[ch][i]) count[ch] -= highBands[bsize][i];
                    }
                }
                for (int ch = 0; ch < channels; ++ch) {
                    if (!coded[ch]) continue;
                    int value = INT32_MIN;
                    for (int i = 0; i < highSizes[bsize]; ++i) {
                        if (!highCoded[ch][i]) continue;
                        value = value == INT32_MIN ? int(in.read(7)) - 19 : value + gainCodes().read(in);
                        highValues[ch][i] = value;
                    }
                }
            }
            if (c.blockBits == frameBits || in.bit()) {
                for (int ch = 0; ch < channels; ++ch) {
                    if (!coded[ch]) continue;
                    if (expVlc) {
                        if (!exponentsVlc(ch)) return -1;
                    } else {
                        exponentsLsp(ch);
                    }
                    exponentsSize[ch] = bsize;
                    c.exponentsReady[ch] = 1;
                }
            }
            for (int ch = 0; ch < channels; ++ch) if (coded[ch] && !c.exponentsReady[ch]) return -1;
            for (int ch = 0; ch < channels; ++ch) {
                if (coded[ch]) {
                    memset(coefs1[ch], 0, size_t(blockLength) * sizeof(float));
                    if (!runLevel(in, *coefTables[ch == 1 && msStereo], 0, coefs1[ch], 0, count[ch], blockLength, frameBits, escapeBits)) return -1;
                }
                if (version == 1 && channels >= 2) in.align();
            }
            const int n4 = blockLength / 2;
            float norm = float(1.0 / float(n4));
            if (version == 1) norm = float(norm * sqrt(double(n4)));
            if (quiet) {
                if (noiseCoding) {
                    int used = coefsStart + std::max(0, highBandStart[bsize] - coefsStart) + std::max(0, blockLength - coefsEnd[bsize]);
                    for (int j = 0; j < highSizes[bsize]; ++j) used += highBands[bsize][j];
                    for (int ch = 0; ch < channels; ++ch) if (coded[ch]) c.noiseIndex = (c.noiseIndex + used) & (kNoiseSize - 1);
                }
                blockPos += blockLength;
                return blockPos >= frameLength ? 1 : 0;
            }
            for (int ch = 0; ch < channels; ++ch) if (coded[ch]) rebuild(ch, bsize, gain, norm, count[ch]);
            if (msStereo && coded[1]) {
                if (!coded[0]) {
                    memset(coefs[0], 0, size_t(blockLength) * sizeof(float));
                    coded[0] = true;
                }
                for (int i = 0; i < blockLength; ++i) {
                    const float t = coefs[0][i] - coefs[1][i];
                    coefs[0][i] += coefs[1][i];
                    coefs[1][i] = t;
                }
            }
        }
        for (int ch = 0; ch < channels && !quiet; ++ch) {
            if (coded[ch]) mdct[size_t(bsize)].full(coefs[ch], output);
            else if (!(msStereo && ch == 1)) memset(output, 0, sizeof output);
            window(&frameOut[ch][frameLength / 2 + blockPos - blockLength / 2]);
        }
        blockPos += blockLength;
        return blockPos >= frameLength ? 1 : 0;
    }

    bool frame(std::vector<float>& out) {
        blockPos = 0;
        for (;;) {
            const int r = block();
            if (r < 0) return false;
            if (r) break;
        }
        if (quiet) return true;
        const size_t at = out.size();
        out.resize(at + size_t(frameLength) * size_t(channels));
        float* to = out.data() + at;
        for (int ch = 0; ch < channels; ++ch) {
            for (int i = 0; i < frameLength; ++i) to[size_t(i) * size_t(channels) + size_t(ch)] = frameOut[ch][i];
            memmove(frameOut[ch], frameOut[ch] + frameLength, size_t(frameLength) * sizeof(float));
        }
        return true;
    }

    void superframe(const uint8_t* data, std::vector<float>& out) {
        const int size = blockAlign;
        const size_t base = out.size();
        auto fail = [&] {
            out.resize(base);
            c.lastLength = 0;
        };
        in = BitReader(data, size_t(size));
        if (!reservoir) {
            if (!frame(out)) fail();
            return;
        }
        in.skip(4);
        const int stated = int(in.read(4));
        int frames = stated - (c.lastLength <= 0 ? 1 : 0);
        if (frames <= 0) {
            if (frames < 0 || in.left() <= 8) return;
            if (stated == 0) {
                if (c.lastLength + size - 1 > kReservoirMax) return fail();
                memcpy(stored + c.lastLength, data + 1, size_t(size - 1));
                c.lastLength += size - 1;
                memset(stored + c.lastLength, 0, 64);
                return;
            }
        }
        const int header = 4 + 4 + byteOffsetBits + 3;
        const int bitOffset = int(in.read(byteOffsetBits + 3));
        if (bitOffset > in.left()) return fail();
        if (c.lastLength > 0) {
            if (c.lastLength + (bitOffset + 7) / 8 > kReservoirMax) return fail();
            uint8_t* q = stored + c.lastLength;
            int length = bitOffset;
            for (; length > 7; length -= 8) *q++ = uint8_t(in.read(8));
            if (length > 0) *q++ = uint8_t(in.read(length) << (8 - length));
            memset(q, 0, 64);
            in = BitReader(stored, size_t(q - stored), int64_t(c.lastLength) * 8 + bitOffset);
            in.skip(c.lastBitOffset);
            if (!frame(out)) return fail();
            --frames;
        }
        int pos = bitOffset + header;
        if (pos >= kReservoirMax * 8 || pos > size * 8) {
            out.resize(base);
            return;
        }
        in = BitReader(data + pos / 8, size_t(size - pos / 8));
        in.skip(pos & 7);
        c.resetBlockLengths = 1;
        for (int i = 0; i < frames; ++i) if (!frame(out)) return fail();
        pos = int(in.position()) + ((bitOffset + header) & ~7);
        c.lastBitOffset = pos & 7;
        pos >>= 3;
        const int length = size - pos;
        if (length > kReservoirMax || length < 0) return fail();
        c.lastLength = length;
        memcpy(stored, data + pos, size_t(length));
    }

    void feed(const std::vector<uint8_t>& packet, std::vector<float>& out) override {
        for (size_t at = 0; packet.size() - at >= size_t(blockAlign); at += size_t(blockAlign)) superframe(packet.data() + at, out);
    }

    void skim(const std::vector<uint8_t>& packet) override {
        std::vector<float> none;
        quiet = true;
        feed(packet, none);
        quiet = false;
    }

    void finish(std::vector<float>& out) override {
        const size_t at = out.size();
        out.resize(at + size_t(frameLength) * size_t(channels));
        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < frameLength; ++i) out[at + size_t(i) * size_t(channels) + size_t(ch)] = frameOut[ch][i];
    }

    bool counts(Reader& file, const Track& track, std::vector<int64_t>& frames) override {
        const size_t n = track.samples.size();
        frames.assign(n, 0);
        std::vector<uint8_t> packet;
        bool first = true;
        for (size_t i = 0; i < n; ++i) {
            const uint32_t size = track.samples[i].size;
            const uint32_t units = size / uint32_t(blockAlign);
            if (!reservoir) {
                frames[i] = int64_t(units) * frameLength;
                continue;
            }
            packet.resize(size);
            if (!units || !file.read(track, uint32_t(i), packet.data())) continue;
            for (uint32_t u = 0; u < units; ++u) {
                const int stated = packet[size_t(u) * size_t(blockAlign)] & 15;
                if (first && !stated) continue;
                frames[i] += int64_t(first ? stated - 1 : stated) * frameLength;
                first = false;
            }
        }
        if (n) frames[n - 1] += frameLength;
        return true;
    }

    bool save(std::vector<uint8_t>& state) const override {
        if (!noiseCoding) return false;
        state.resize(sizeof c + size_t(std::max(0, c.lastLength)));
        memcpy(state.data(), &c, sizeof c);
        if (c.lastLength > 0) memcpy(state.data() + sizeof c, stored, size_t(c.lastLength));
        return true;
    }

    void load(const std::vector<uint8_t>& state) override {
        if (state.size() < sizeof c) return;
        memcpy(&c, state.data(), sizeof c);
        c.lastLength = std::clamp<int32_t>(c.lastLength, 0, int32_t(state.size() - sizeof c));
        memcpy(stored, state.data() + sizeof c, size_t(c.lastLength));
        memset(stored + c.lastLength, 0, 64);
    }
};

}

std::unique_ptr<PacketDecoder> openWmaAudio(const Track& track, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    const std::vector<uint8_t>& w = track.config;
    if (track.configType != fourcc("wfex") || w.size() < 16) return fail("Windows Media Audio track without its format header");
    auto audio = std::make_unique<WmaAudio>();
    audio->version = track.codec == fourcc("wma1") ? 1 : 2;
    const int count = int(le16(w.data() + 2));
    const int rate = int(le32(w.data() + 4));
    const int64_t bitRate = int64_t(le32(w.data() + 8)) * 8;
    audio->blockAlign = int(le16(w.data() + 12));
    size_t extra = w.size() >= 18 ? std::min<size_t>(le16(w.data() + 16), w.size() - 18) : 0;
    const uint8_t* e = w.data() + 18;
    if (le16(w.data()) == 0xFFFE && extra >= 22) {
        e += 22;
        extra -= 22;
    }
    unsigned flags = 0;
    if (audio->version == 1 && extra >= 4) flags = le16(e + 2);
    else if (audio->version == 2 && extra >= 6) flags = le16(e + 4);
    audio->expVlc = flags & 1;
    audio->reservoir = flags & 2;
    audio->variable = flags & 4;
    if (audio->version == 2 && extra >= 8 && le16(e + 4) == 0xd) audio->variable = false;
    if (!audio->blockAlign) return fail("Windows Media Audio track without a block size");
    if (!audio->setup(rate, count, bitRate, flags)) return fail("Windows Media Audio layout not supported");
    audio->prerollPackets = 3;
    audio->prerollFrames = 2 * audio->frameLength;
    audio->skip = 2 * audio->frameLength;
    audio->longestPacket = audio->frameLength;
    return audio;
}

}

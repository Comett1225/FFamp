#include "audio_codec.h"
#include "bits.h"
#include "dts_tables.h"
#include "mdct.h"
#include "readers.h"
#include "vlc.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <mutex>

namespace absent {

namespace {

using namespace dts;

const int kChannels = 7;
const int kSubbands = 32;
const int kSubframes = 16;
const int kSubbandSamples = 8;
const int kLfeHistory = 8;
const int kAbitsMax = 26;
const int kCodeBooks = 10;
const int kAdpcmCoeffs = 4;
const int kXxchChannelsMax = 2;
const int kCoreChannelsMax = 6;
const uint32_t kSyncCore = 0x7FFE8001u, kSyncCoreLe = 0xFE7F0180u, kSync14Be = 0x1FFFE800u, kSync14Le = 0xFF1F00E8u, kSyncXch = 0x5A5A5A5Au,
               kSyncXxch = 0x47004A03u, kSyncAux = 0x9A1105A0u, kSyncSubstream = 0x64582025u;
const int kExtXch = 0, kExtXxch = 6;
const int kCssXxch = 0x002, kCssXch = 0x008;

enum Speaker { C, L, R, Ls, Rs, Lfe1, Cs, Lsr, Rsr, Lss, Rss };
const int kMaskLs = 1 << Ls, kMaskRs = 1 << Rs, kMaskLfe1 = 1 << Lfe1, kMaskCs = 1 << Cs, kMaskLss = 1 << Lss, kMaskRss = 1 << Rss;
enum Header { kCore, kXch, kXxch };
enum AudioMode { kMono, kMonoDual, kStereo, kSumDiff, kTotal, k3F, k2F1R, k3F1R, k2F2R, k3F2R, kModes };

const int8_t kPrimaryToSpeaker[kModes][5] = {{C, -1, -1, -1, -1}, {L, R, -1, -1, -1}, {L, R, -1, -1, -1}, {L, R, -1, -1, -1}, {L, R, -1, -1, -1},
    {C, L, R, -1, -1}, {L, R, Cs, -1, -1}, {C, L, R, Cs, -1}, {L, R, Ls, Rs, -1}, {C, L, R, Ls, Rs}};
const uint8_t kModeMask[kModes] = {0x1, 0x6, 0x6, 0x6, 0x6, 0x7, 0x46, 0x47, 0x1E, 0x1F};
const uint8_t kBlockCodeBits[7] = {7, 10, 12, 13, 15, 17, 19};
const uint8_t kModeChannels[16] = {1, 2, 2, 2, 2, 3, 3, 4, 4, 5, 6, 6, 6, 7, 8, 8};
const uint8_t kQuantSelBits[kCodeBooks] = {1, 2, 2, 2, 2, 3, 3, 3, 3, 3};
const uint8_t kQuantGroupSize[kCodeBooks] = {1, 3, 3, 3, 3, 7, 7, 7, 7, 7};
const uint8_t kBitallocSizes[kCodeBooks] = {3, 5, 7, 9, 13, 17, 25, 33, 65, 129};
const int8_t kBitallocOffsets[kCodeBooks] = {-1, -2, -3, -4, -6, -8, -12, -16, -32, -64};
const uint8_t kBitallocMaxBits[kCodeBooks][7] = {{2}, {4, 3, 3}, {5, 5, 4}, {6, 5, 6}, {7, 6, 5}, {9, 8, 7, 9, 8, 8, 8}, {9, 9, 8, 9, 8, 9, 9},
    {9, 9, 9, 9, 9, 9, 9}, {9, 9, 9, 9, 9, 9, 9}, {9, 9, 9, 9, 9, 9, 9}};
const uint8_t kBitalloc12Bits[5] = {9, 7, 7, 9, 9};
const uint32_t kSampleRates[16] = {0, 8000, 16000, 32000, 0, 0, 11025, 22050, 44100, 0, 0, 12000, 24000, 48000, 96000, 192000};
const uint32_t kBitRates[32] = {32000, 56000, 64000, 96000, 112000, 128000, 192000, 224000, 256000, 320000, 384000, 448000, 512000, 576000, 640000, 768000,
    896000, 1024000, 1152000, 1280000, 1344000, 1408000, 1411200, 1472000, 1536000, 1920000, 2048000, 3072000, 3840000, 1, 2, 3};
const uint8_t kBitsPerSample[8] = {16, 16, 20, 20, 0, 24, 24, 0};
const uint8_t kDcaToWav[28] = {2, 0, 1, 9, 10, 3, 8, 4, 5, 9, 10, 6, 7, 12, 13, 14, 3, 6, 7, 11, 12, 14, 16, 15, 17, 8, 4, 5};

struct Vlcs {
    Vlc quant[kCodeBooks][7];
    Vlc bitAllocation[5];
    Vlc scale[5];
    Vlc transition[4];
};

const Vlcs& vlcs() {
    static const Vlcs* built = [] {
        auto* v = new Vlcs;
        const uint8_t (*source)[2] = kVlcSource;
        auto next = [&](Vlc& vlc, int bits, int count, int offset) {
            std::vector<uint8_t> lengths(static_cast<size_t>(count)), symbols(static_cast<size_t>(count));
            for (int i = 0; i < count; ++i) {
                symbols[size_t(i)] = source[i][0];
                lengths[size_t(i)] = source[i][1];
            }
            vlc.buildInOrder(bits, size_t(count), lengths.data(), symbols.data(), offset);
            source += count;
        };
        for (int i = 0; i < kCodeBooks; i++)
            for (int j = 0; j < kQuantGroupSize[i]; j++) next(v->quant[i][j], kBitallocMaxBits[i][j], kBitallocSizes[i], kBitallocOffsets[i]);
        for (int i = 0; i < 5; i++) next(v->bitAllocation[i], kBitalloc12Bits[i], 12, 1);
        for (int i = 0; i < 5; i++) next(v->scale[i], 9, 129, -64);
        for (int i = 0; i < 4; i++) next(v->transition[i], 3, 4, 0);
        return v;
    }();
    return *built;
}

int32_t norm(int64_t a, int bits) { return bits > 0 ? int32_t((a + (int64_t(1) << (bits - 1))) >> bits) : int32_t(a); }
int32_t mul(int32_t a, int32_t b, int bits) { return norm(int64_t(a) * b, bits); }
int32_t clip23(int32_t a) { return std::clamp(a, -(1 << 23), (1 << 23) - 1); }

uint16_t crcCcitt(const uint8_t* p, size_t n) {
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < n; ++i) {
        crc ^= uint16_t(p[i] << 8);
        for (int k = 0; k < 8; ++k) crc = uint16_t(crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    return crc;
}

uint32_t be32At(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

int convert(const uint8_t* src, size_t size, std::vector<uint8_t>& dst) {
    const uint32_t mark = be32At(src);
    if (mark == kSyncCore) {
        dst.assign(src, src + size);
        return int(size);
    }
    if (mark == kSyncCoreLe) {
        dst.assign(size + 1, 0);
        for (size_t i = 0; i < (size + 1) / 2; ++i) {
            dst[2 * i] = 2 * i + 1 < size ? src[2 * i + 1] : 0;
            dst[2 * i + 1] = src[2 * i];
        }
        dst.resize(size);
        return int(size);
    }
    if (mark == kSync14Be || mark == kSync14Le) {
        dst.assign(size + 8, 0);
        uint64_t acc = 0;
        int have = 0;
        size_t out = 0;
        for (size_t i = 0; i + 1 < size + 1; i += 2) {
            const uint16_t word = mark == kSync14Be ? uint16_t(src[i] << 8 | (i + 1 < size ? src[i + 1] : 0)) : uint16_t((i + 1 < size ? src[i + 1] : 0) << 8 | src[i]);
            acc = acc << 14 | (word & 0x3FFF);
            have += 14;
            while (have >= 8) {
                dst[out++] = uint8_t(acc >> (have - 8));
                have -= 8;
            }
        }
        if (have) dst[out++] = uint8_t(acc << (8 - have));
        dst.resize(out);
        return int(out);
    }
    return -1;
}

struct Synth {
    alignas(32) float hist1[1024];
    alignas(32) float hist2[64];
    int offset;
};

struct DtsAudio : PacketDecoder {
    BitReader gb, start;
    const uint8_t* buffer = nullptr;
    size_t bufferBytes = 0;
    int crcPresent = 0, npcmblocks = 0, frameSize = 0, audioMode = 0, sampleRate = 0, bitRate = 0, drcPresent = 0, tsPresent = 0, auxPresent = 0;
    int extAudioType = 0, extAudioPresent = 0, syncSsf = 0, lfePresent = 0, predictorHistory = 0, filterPerfect = 0, esFormat = 0, sumdiffFront = 0,
        sumdiffSurround = 0;
    int nsubframes = 0, nchannels = 0, chMask = 0;
    int8_t nsubbands[kChannels] = {}, vqStart[kChannels] = {}, jointIndex[kChannels] = {}, transitionSel[kChannels] = {}, scaleSel[kChannels] = {},
           bitAllocSel[kChannels] = {};
    int8_t quantSel[kChannels][kCodeBooks] = {};
    int32_t scaleAdj[kChannels][kCodeBooks] = {};
    int8_t nsubsubframes[kSubframes] = {};
    int8_t predictionMode[kChannels][64] = {};
    int16_t predictionVq[kChannels][64] = {};
    int8_t bitAllocation[kChannels][64] = {};
    int8_t transitionMode[kSubframes][kChannels][kSubbands] = {};
    int32_t scaleFactors[kChannels][kSubbands][2] = {};
    int8_t jointScaleSel[kChannels] = {};
    int32_t jointScales[kChannels][64] = {};
    int extMask = 0, xchPos = 0, xxchPos = 0;
    int xxchCrcPresent = 0, xxchMaskBits = 0, xxchCoreMask = 0, xxchSpeakerMask = 0, xxchDmixEmbedded = 0, xxchDmixScaleInv = 0;
    int xxchDmixMask[kXxchChannelsMax] = {};
    int xxchDmixCoeff[kXxchChannelsMax * kCoreChannelsMax] = {};
    std::vector<int32_t> subbandBuffer;
    int32_t* subband[kChannels][kSubbands] = {};
    int32_t* lfeSamples = nullptr;
    int allocatedBlocks = -1;
    Synth synth[kChannels] = {};
    InverseMdct imdct{5, 1.0};
    std::vector<uint8_t> converted;
    std::vector<float> scratch;
    int outputMask = 0;
    int remap[32] = {};
    int outputChannels = 0;

    void reset() override {
        if (allocatedBlocks >= 0) {
            eraseAdpcm();
            memset(lfeSamples, 0, kLfeHistory * sizeof(int32_t));
        }
        memset(synth, 0, sizeof synth);
    }

    void eraseAdpcm() {
        for (int ch = 0; ch < kChannels; ch++)
            for (int band = 0; band < kSubbands; band++) memset(subband[ch][band] - kAdpcmCoeffs, 0, kAdpcmCoeffs * sizeof(int32_t));
    }

    int sbits(int n) { return n ? int32_t(gb.read(n) << (32 - n)) >> (32 - n) : 0; }

    bool seek(int64_t p) {
        if (p < gb.position() || p > int64_t(bufferBytes) * 8) return false;
        gb.skip(p - gb.position());
        return true;
    }

    bool frameHeader() {
        if (gb.read(32) != kSyncCore) return false;
        const int normal = int(gb.read(1));
        (void)normal;
        if (int(gb.read(5)) + 1 != 32) return false;
        crcPresent = int(gb.read(1));
        npcmblocks = int(gb.read(7)) + 1;
        if (npcmblocks & (kSubbandSamples - 1)) return false;
        frameSize = int(gb.read(14)) + 1;
        if (frameSize < 96) return false;
        audioMode = int(gb.read(6));
        if (audioMode >= kModes) return false;
        const int srCode = int(gb.read(4));
        if (!kSampleRates[srCode]) return false;
        sampleRate = int(kSampleRates[srCode]);
        bitRate = int(kBitRates[gb.read(5)]);
        if (gb.read(1)) return false;
        drcPresent = int(gb.read(1));
        tsPresent = int(gb.read(1));
        auxPresent = int(gb.read(1));
        gb.skip(1);
        extAudioType = int(gb.read(3));
        extAudioPresent = int(gb.read(1));
        syncSsf = int(gb.read(1));
        lfePresent = int(gb.read(2));
        if (lfePresent == 3) return false;
        predictorHistory = int(gb.read(1));
        if (crcPresent) gb.skip(16);
        filterPerfect = int(gb.read(1));
        gb.skip(4 + 2);
        const int pcmr = int(gb.read(3));
        if (!kBitsPerSample[pcmr]) return false;
        esFormat = pcmr & 1;
        sumdiffFront = int(gb.read(1));
        sumdiffSurround = int(gb.read(1));
        gb.skip(4);
        return true;
    }

    bool codingHeader(Header header, int xchBase) {
        const int64_t headerPos = gb.position();
        int headerSize = 0;
        if (gb.left() < 0) return false;
        switch (header) {
            case kCore:
                nsubframes = int(gb.read(4)) + 1;
                nchannels = int(gb.read(3)) + 1;
                if (nchannels != kModeChannels[audioMode]) return false;
                chMask = kModeMask[audioMode];
                if (lfePresent) chMask |= kMaskLfe1;
                break;
            case kXch:
                nchannels = kModeChannels[audioMode] + 1;
                chMask |= kMaskCs;
                break;
            case kXxch: {
                headerSize = int(gb.read(7)) + 1;
                const int count = int(gb.read(3)) + 1;
                if (count > kXxchChannelsMax) return false;
                nchannels = kModeChannels[audioMode] + count;
                const uint32_t mask = gb.read(xxchMaskBits - Cs);
                xxchSpeakerMask = int(mask << Cs);
                if (std::popcount(unsigned(xxchSpeakerMask)) != count || (xxchCoreMask & xxchSpeakerMask)) return false;
                chMask = xxchCoreMask | xxchSpeakerMask;
                if (gb.read(1)) {
                    int* coeff = xxchDmixCoeff;
                    xxchDmixEmbedded = int(gb.read(1));
                    const unsigned index = gb.read(6) * 4 - (242 - 201) - 3;
                    if (index >= 201) return false;
                    xxchDmixScaleInv = int(kInvDmixTable[index]);
                    for (int ch = 0; ch < count; ch++) {
                        const int m = int(gb.read(xxchMaskBits));
                        if ((m & xxchCoreMask) != m) return false;
                        xxchDmixMask[ch] = m;
                    }
                    for (int ch = 0; ch < count; ch++) {
                        for (int n = 0; n < xxchMaskBits; n++) {
                            if (!(xxchDmixMask[ch] & (1 << n))) continue;
                            int code = int(gb.read(7));
                            const int sign = (code >> 6) - 1;
                            if ((code &= 63)) {
                                const unsigned i = unsigned(code * 4 - 3);
                                if (i >= 242) return false;
                                *coeff++ = (int(kDmixTable[i]) ^ sign) - sign;
                            } else {
                                *coeff++ = 0;
                            }
                        }
                    }
                } else {
                    xxchDmixEmbedded = 0;
                }
                break;
            }
        }
        for (int ch = xchBase; ch < nchannels; ch++) {
            nsubbands[ch] = int8_t(gb.read(5) + 2);
            if (nsubbands[ch] > kSubbands) return false;
        }
        for (int ch = xchBase; ch < nchannels; ch++) vqStart[ch] = int8_t(gb.read(5) + 1);
        for (int ch = xchBase; ch < nchannels; ch++) {
            int n = int(gb.read(3));
            if (n && header == kXxch) n += xchBase - 1;
            if (n > nchannels) return false;
            jointIndex[ch] = int8_t(n);
        }
        for (int ch = xchBase; ch < nchannels; ch++) transitionSel[ch] = int8_t(gb.read(2));
        for (int ch = xchBase; ch < nchannels; ch++) {
            scaleSel[ch] = int8_t(gb.read(3));
            if (scaleSel[ch] == 7) return false;
        }
        for (int ch = xchBase; ch < nchannels; ch++) {
            bitAllocSel[ch] = int8_t(gb.read(3));
            if (bitAllocSel[ch] == 7) return false;
        }
        for (int n = 0; n < kCodeBooks; n++)
            for (int ch = xchBase; ch < nchannels; ch++) quantSel[ch][n] = int8_t(gb.read(kQuantSelBits[n]));
        for (int n = 0; n < kCodeBooks; n++)
            for (int ch = xchBase; ch < nchannels; ch++)
                if (quantSel[ch][n] < kQuantGroupSize[n]) scaleAdj[ch][n] = int32_t(kScaleAdjust[gb.read(2)]);
        if (header == kXxch) return seek(headerPos + headerSize * 8);
        if (crcPresent) gb.skip(16);
        return true;
    }

    int scale(int& index, int sel) {
        const uint32_t* table = sel > 5 ? kScaleQuant7 : kScaleQuant6;
        const unsigned size = sel > 5 ? 128 : 64;
        if (sel < 5) index += vlcs().scale[sel].read(gb);
        else index = int(gb.read(sel + 1));
        if (unsigned(index) >= size) return -1;
        return int(table[index]);
    }

    int jointScale(int sel) {
        int index = sel < 5 ? vlcs().scale[sel].read(gb) : int(gb.read(sel + 1));
        index += 64;
        if (unsigned(index) >= 129) return -1;
        return int(kJointScales[index]);
    }

    bool subframeHeader(int sf, Header header, int xchBase) {
        if (gb.left() < 0) return false;
        if (header == kCore) {
            nsubsubframes[sf] = int8_t(gb.read(2) + 1);
            gb.skip(3);
        }
        for (int ch = xchBase; ch < nchannels; ch++)
            for (int band = 0; band < nsubbands[ch]; band++) predictionMode[ch][band] = int8_t(gb.read(1));
        for (int ch = xchBase; ch < nchannels; ch++)
            for (int band = 0; band < nsubbands[ch]; band++)
                if (predictionMode[ch][band]) predictionVq[ch][band] = int16_t(gb.read(12));
        for (int ch = xchBase; ch < nchannels; ch++) {
            const int sel = bitAllocSel[ch];
            for (int band = 0; band < vqStart[ch]; band++) {
                const int abits = sel < 5 ? vlcs().bitAllocation[sel].read(gb) : int(gb.read(sel - 1));
                if (abits > kAbitsMax) return false;
                bitAllocation[ch][band] = int8_t(abits);
            }
        }
        for (int ch = xchBase; ch < nchannels; ch++) {
            memset(transitionMode[sf][ch], 0, sizeof transitionMode[0][0]);
            if (nsubsubframes[sf] > 1) {
                const int sel = transitionSel[ch];
                for (int band = 0; band < vqStart[ch]; band++)
                    if (bitAllocation[ch][band]) transitionMode[sf][ch][band] = int8_t(vlcs().transition[sel].read(gb));
            }
        }
        for (int ch = xchBase; ch < nchannels; ch++) {
            const int sel = scaleSel[ch];
            int index = 0;
            for (int band = 0; band < vqStart[ch]; band++) {
                if (bitAllocation[ch][band]) {
                    int value = scale(index, sel);
                    if (value < 0) return false;
                    scaleFactors[ch][band][0] = value;
                    if (transitionMode[sf][ch][band]) {
                        if ((value = scale(index, sel)) < 0) return false;
                        scaleFactors[ch][band][1] = value;
                    }
                } else {
                    scaleFactors[ch][band][0] = 0;
                }
            }
            for (int band = vqStart[ch]; band < nsubbands[ch]; band++) {
                const int value = scale(index, sel);
                if (value < 0) return false;
                scaleFactors[ch][band][0] = value;
            }
        }
        for (int ch = xchBase; ch < nchannels; ch++) {
            if (jointIndex[ch]) {
                jointScaleSel[ch] = int8_t(gb.read(3));
                if (jointScaleSel[ch] == 7) return false;
            }
        }
        for (int ch = xchBase; ch < nchannels; ch++) {
            const int source = jointIndex[ch] - 1;
            if (source < 0) continue;
            for (int band = nsubbands[ch]; band < nsubbands[source]; band++) {
                const int value = jointScale(jointScaleSel[ch]);
                if (value < 0) return false;
                jointScales[ch][band] = value;
            }
        }
        if (drcPresent && header == kCore) gb.skip(8);
        if (crcPresent) gb.skip(16);
        return true;
    }

    static bool blockCodes(int code1, int code2, int levels, int32_t* audio) {
        const int offset = (levels - 1) / 2;
        int n = 0;
        for (; n < kSubbandSamples / 2; n++) {
            const int div = code1 / levels;
            audio[n] = code1 - div * levels - offset;
            code1 = div;
        }
        for (; n < kSubbandSamples; n++) {
            const int div = code2 / levels;
            audio[n] = code2 - div * levels - offset;
            code2 = div;
        }
        return !(code1 | code2);
    }

    int extract(int32_t* audio, int abits, int ch) {
        if (abits == 0) {
            memset(audio, 0, kSubbandSamples * sizeof *audio);
            return 0;
        }
        if (abits <= kCodeBooks) {
            const int sel = quantSel[ch][abits - 1];
            if (sel < kQuantGroupSize[abits - 1]) {
                const Vlc& v = vlcs().quant[abits - 1][sel];
                for (int i = 0; i < kSubbandSamples; i++) audio[i] = v.read(gb);
                return 1;
            }
            if (abits <= 7) {
                const int code1 = int(gb.read(kBlockCodeBits[abits - 1]));
                const int code2 = int(gb.read(kBlockCodeBits[abits - 1]));
                return blockCodes(code1, code2, int(kQuantLevels[abits]), audio) ? 0 : -1;
            }
        }
        for (int i = 0; i < kSubbandSamples; i++) audio[i] = sbits(abits - 3);
        return 0;
    }

    static void dequantize(int32_t* output, const int32_t* input, int32_t stepSize, int32_t scaleValue, int len) {
        int64_t stepScale = int64_t(stepSize) * scaleValue;
        int shift = 0;
        if (stepScale > (1 << 23)) {
            shift = (63 - std::countl_zero(uint64_t(stepScale >> 23))) + 1;
            stepScale >>= shift;
        }
        for (int n = 0; n < len; n++) output[n] = clip23(norm(input[n] * stepScale, 22 - shift));
    }

    bool subframeAudio(int sf, Header header, int xchBase, int& subPos, int& lfePos) {
        int32_t audio[16];
        const int nsamples = nsubsubframes[sf] * kSubbandSamples;
        if (subPos + nsamples > npcmblocks || gb.left() < 0) return false;
        for (int ch = xchBase; ch < nchannels; ch++) {
            int32_t vq[kSubbands];
            for (int band = vqStart[ch]; band < nsubbands[ch]; band++) vq[band] = int32_t(gb.read(10));
            for (int band = vqStart[ch]; band < nsubbands[ch]; band++) {
                const int8_t* coeff = kHighFreqVq[vq[band]];
                const int32_t s = scaleFactors[ch][band][0];
                for (int j = 0; j < nsamples; j++) subband[ch][band][j + subPos] = clip23((coeff[j] * s + (1 << 3)) >> 4);
            }
        }
        if (lfePresent && header == kCore) {
            const int count = 2 * lfePresent * nsubsubframes[sf];
            for (int n = 0; n < count; n++) audio[n] = sbits(8);
            const unsigned index = gb.read(8);
            if (index >= 128) return false;
            const int32_t s = mul(4697620, int32_t(kScaleQuant7[index]), 23);
            int ofs = lfePos;
            for (int n = 0; n < count; n++, ofs++) lfeSamples[ofs] = clip23((audio[n] * s) >> 4);
            lfePos = ofs;
        }
        int ofs = subPos;
        for (int ssf = 0; ssf < nsubsubframes[sf]; ssf++) {
            for (int ch = xchBase; ch < nchannels; ch++) {
                if (gb.left() < 0) return false;
                for (int band = 0; band < vqStart[ch]; band++) {
                    const int abits = bitAllocation[ch][band];
                    const int ret = extract(audio, abits, ch);
                    if (ret < 0) return false;
                    const int32_t step = int32_t(bitRate == 3 ? kLosslessQuant[abits] : kLossyQuant[abits]);
                    const int trans = transitionMode[sf][ch][band];
                    int32_t s = trans == 0 || ssf < trans ? scaleFactors[ch][band][0] : scaleFactors[ch][band][1];
                    if (ret > 0) s = clip23(int32_t((int64_t(scaleAdj[ch][abits - 1]) * s) >> 22));
                    dequantize(subband[ch][band] + ofs, audio, step, s, kSubbandSamples);
                }
            }
            if ((ssf == nsubsubframes[sf] - 1 || syncSsf) && gb.read(16) != 0xffff) return false;
            ofs += kSubbandSamples;
        }
        for (int ch = xchBase; ch < nchannels; ch++) {
            for (int band = 0; band < nsubbands[ch]; band++) {
                if (!predictionMode[ch][band]) continue;
                const int16_t* coeff = kAdpcmVb[predictionVq[ch][band]];
                int32_t* ptr = subband[ch][band] + subPos;
                for (int j = 0; j < nsamples; j++) {
                    const int32_t* input = ptr + j - kAdpcmCoeffs;
                    int64_t prediction = 0;
                    for (int i = 0; i < kAdpcmCoeffs; i++) prediction += int64_t(input[kAdpcmCoeffs - 1 - i]) * coeff[i];
                    ptr[j] = clip23(ptr[j] + clip23(norm(prediction, 13)));
                }
            }
        }
        for (int ch = xchBase; ch < nchannels; ch++) {
            const int source = jointIndex[ch] - 1;
            if (source < 0) continue;
            for (int band = nsubbands[ch]; band < nsubbands[source]; band++) {
                const int32_t s = jointScales[ch][band];
                for (int j = 0; j < nsamples; j++) subband[ch][band][j + subPos] = clip23(mul(subband[source][band][j + subPos], s, 17));
            }
        }
        subPos = ofs;
        return true;
    }

    void allocate() {
        if (allocatedBlocks != npcmblocks) {
            const int perChannel = kAdpcmCoeffs + npcmblocks;
            const int frameSamples = perChannel * kChannels * kSubbands;
            subbandBuffer.assign(size_t(frameSamples + kLfeHistory + npcmblocks / 2), 0);
            for (int ch = 0; ch < kChannels; ch++)
                for (int band = 0; band < kSubbands; band++) subband[ch][band] = subbandBuffer.data() + (ch * kSubbands + band) * perChannel + kAdpcmCoeffs;
            lfeSamples = subbandBuffer.data() + frameSamples;
            allocatedBlocks = npcmblocks;
        }
        if (!predictorHistory) eraseAdpcm();
    }

    bool frameData(Header header, int xchBase) {
        if (!codingHeader(header, xchBase)) return false;
        int subPos = 0, lfePos = kLfeHistory;
        for (int sf = 0; sf < nsubframes; sf++)
            if (!subframeHeader(sf, header, xchBase) || !subframeAudio(sf, header, xchBase, subPos, lfePos)) return false;
        for (int ch = xchBase; ch < nchannels; ch++) {
            int count = nsubbands[ch];
            if (jointIndex[ch]) count = std::max<int>(count, nsubbands[jointIndex[ch] - 1]);
            int band = 0;
            for (; band < count; band++) {
                int32_t* samples = subband[ch][band] - kAdpcmCoeffs;
                memcpy(samples, samples + npcmblocks, kAdpcmCoeffs * sizeof(int32_t));
            }
            for (; band < kSubbands; band++) memset(subband[ch][band] - kAdpcmCoeffs, 0, size_t(kAdpcmCoeffs + npcmblocks) * sizeof(int32_t));
        }
        return true;
    }

    bool xchFrame() {
        if (chMask & kMaskCs) return false;
        if (!frameData(kXch, nchannels)) return false;
        return seek(int64_t(frameSize) * 8);
    }

    bool xxchFrame() {
        const int64_t headerPos = gb.position();
        if (gb.read(32) != kSyncXxch) return false;
        const int headerSize = int(gb.read(6)) + 1;
        xxchCrcPresent = int(gb.read(1));
        xxchMaskBits = int(gb.read(5)) + 1;
        if (xxchMaskBits <= Cs) return false;
        if (gb.read(2) + 1 > 1) return false;
        const int xxchFrameSize = int(gb.read(14)) + 1;
        xxchCoreMask = int(gb.read(xxchMaskBits));
        int mask = chMask;
        if ((mask & kMaskLs) && (xxchCoreMask & kMaskLss)) mask = (mask & ~kMaskLs) | kMaskLss;
        if ((mask & kMaskRs) && (xxchCoreMask & kMaskRss)) mask = (mask & ~kMaskRs) | kMaskRss;
        if (mask != xxchCoreMask) return false;
        if (!seek(headerPos + headerSize * 8)) return false;
        if (!frameData(kXxch, nchannels)) return false;
        return seek(headerPos + headerSize * 8 + int64_t(xxchFrameSize) * 8);
    }

    void auxData() {
        static const uint8_t primaryChannels[8] = {1, 2, 2, 3, 3, 4, 4, 0};
        if (gb.left() < 0) return;
        gb.skip(6);
        gb.skip((-gb.position()) & 31);
        if (gb.read(32) != kSyncAux) return;
        if (gb.read(1)) gb.skip(47);
        if (gb.read(1)) {
            const unsigned type = gb.read(3);
            if (type >= 7) return;
            const int count = primaryChannels[type] * (kModeChannels[audioMode] + !!lfePresent);
            for (int i = 0; i < count; i++)
                if ((gb.read(9) & 0xff) >= 242) return;
        }
        gb.skip((-gb.position()) & 7);
        gb.skip(16);
    }

    void optionalInfo() {
        if (tsPresent) gb.skip(32);
        if (auxPresent) auxData();
        if (!extAudioPresent) return;
        int syncPos = std::min<int>(frameSize / 4, int(bufferBytes / 4)) - 1;
        const int lastPos = int(gb.position() / 32);
        uint32_t w1, w2 = 0;
        if (extAudioType == kExtXch) {
            for (; syncPos >= lastPos; syncPos--, w2 = w1) {
                w1 = be32At(buffer + syncPos * 4);
                if (w1 == kSyncXch) {
                    const int size = int(w2 >> 22) + 1;
                    const int dist = frameSize - syncPos * 4;
                    if (size >= 96 && (size == dist || size - 1 == dist) && ((w2 >> 15) & 0x7f) == 0x08) {
                        xchPos = syncPos * 32 + 49;
                        break;
                    }
                }
            }
        } else if (extAudioType == kExtXxch) {
            for (; syncPos >= lastPos; syncPos--, w2 = w1) {
                w1 = be32At(buffer + syncPos * 4);
                if (w1 == kSyncXxch) {
                    const int size = int(w2 >> 26) + 1;
                    const int dist = int(bufferBytes) - syncPos * 4;
                    if (size >= 11 && size <= dist && !crcCcitt(buffer + (syncPos + 1) * 4, size_t(size - 4))) {
                        xxchPos = syncPos * 32;
                        break;
                    }
                }
            }
        }
    }

    bool parse(const uint8_t* data, size_t size) {
        extMask = 0;
        xchPos = xxchPos = 0;
        buffer = data;
        bufferBytes = size;
        gb = BitReader(data, size);
        start = gb;
        if (!frameHeader()) return false;
        allocate();
        if (!frameData(kCore, 0)) return false;
        optionalInfo();
        if (frameSize > int(size)) frameSize = int(size);
        int ext = 0;
        bool ok = true;
        if (xxchPos) {
            gb = start;
            gb.skip(xxchPos);
            ok = xxchFrame();
            ext = kCssXxch;
        } else if (xchPos) {
            gb = start;
            gb.skip(xchPos);
            ok = xchFrame();
            ext = kCssXch;
        }
        if (!ok) {
            nchannels = kModeChannels[audioMode];
            chMask = kModeMask[audioMode];
            if (lfePresent) chMask |= kMaskLfe1;
        } else {
            extMask |= ext;
        }
        return true;
    }

    int speakerOf(int ch) {
        int pos = kModeChannels[audioMode];
        if (ch < pos) {
            const int spkr = kPrimaryToSpeaker[audioMode][ch];
            if (extMask & kCssXxch) {
                if (xxchCoreMask & (1 << spkr)) return spkr;
                if (spkr == Ls && (xxchCoreMask & kMaskLss)) return Lss;
                if (spkr == Rs && (xxchCoreMask & kMaskRss)) return Rss;
                return -1;
            }
            return spkr;
        }
        if ((extMask & kCssXch) && ch == pos) return Cs;
        if (extMask & kCssXxch)
            for (int spkr = Cs; spkr < xxchMaskBits; spkr++)
                if ((xxchSpeakerMask & (1 << spkr)) && pos++ == ch) return spkr;
        return -1;
    }

    void synthesize(int ch, float* pcm, const float* window) {
        Synth& s = synth[ch];
        alignas(32) float input[32];
        const float scaleOut = 1.0f / float(1 << 17);
        for (int j = 0; j < npcmblocks; j++) {
            for (int i = 0; i < 32; i++) input[i] = (i - 1) & 2 ? -float(subband[ch][i][j]) : float(subband[ch][i][j]);
            float* buf = s.hist1 + s.offset;
            imdct.half(input, buf);
            for (int i = 0; i < 16; i++) {
                float a = s.hist2[i], b = s.hist2[i + 16], c = 0, d = 0;
                int k = 0;
                for (; k < 512 - s.offset; k += 64) {
                    a += window[i + k] * (-buf[15 - i + k]);
                    b += window[i + k + 16] * (buf[i + k]);
                    c += window[i + k + 32] * (buf[16 + i + k]);
                    d += window[i + k + 48] * (buf[31 - i + k]);
                }
                for (; k < 512; k += 64) {
                    a += window[i + k] * (-buf[15 - i + k - 512]);
                    b += window[i + k + 16] * (buf[i + k - 512]);
                    c += window[i + k + 32] * (buf[16 + i + k - 512]);
                    d += window[i + k + 48] * (buf[31 - i + k - 512]);
                }
                pcm[i] = a * scaleOut;
                pcm[i + 16] = b * scaleOut;
                s.hist2[i] = c;
                s.hist2[i + 16] = d;
            }
            s.offset = (s.offset - 32) & 511;
            pcm += 32;
        }
    }

    void lfeFir(float* pcm, const int32_t* lfe, const float* coeff, int dec) {
        const int factor = 64 << dec, ncoeffs = 8 >> dec, count = npcmblocks >> (dec + 1);
        for (int i = 0; i < count; i++) {
            for (int j = 0; j < factor / 2; j++) {
                float a = 0, b = 0;
                for (int k = 0; k < ncoeffs; k++) {
                    a += coeff[j * ncoeffs + k] * float(lfe[-k]);
                    b += coeff[255 - j * ncoeffs - k] * float(lfe[-k]);
                }
                pcm[j] = a;
                pcm[factor / 2 + j] = b;
            }
            lfe++;
            pcm += factor;
        }
    }

    bool filter(std::vector<float>& out) {
        const int nsamples = npcmblocks * 32;
        if (chMask != outputMask) return false;
        float* speakers[32] = {};
        scratch.assign(size_t(nsamples) * size_t(std::popcount(unsigned(chMask))), 0.0f);
        float* ptr = scratch.data();
        for (int spkr = 0; spkr < 32; spkr++) {
            if (!(chMask & (1 << spkr))) continue;
            speakers[spkr] = ptr;
            ptr += nsamples;
        }
        const float* window = filterPerfect ? kFirPerfect : kFirNonperfect;
        for (int ch = 0; ch < nchannels; ch++) {
            const int spkr = speakerOf(ch);
            if (spkr < 0 || !speakers[spkr]) return false;
            synthesize(ch, speakers[spkr], window);
        }
        if (lfePresent) {
            const int dec = lfePresent == 1;
            lfeFir(speakers[Lfe1], lfeSamples + kLfeHistory, dec ? kLfeFir128 : kLfeFir64, dec);
            const int count = npcmblocks >> (dec + 1);
            for (int n = kLfeHistory - 1; n >= 0; n--) lfeSamples[n] = lfeSamples[count + n];
        }
        auto fmac = [&](float* dst, const float* src, float k) {
            for (int i = 0; i < nsamples; i++) dst[i] += src[i] * k;
        };
        if (esFormat && (extMask & kCssXch) && audioMode >= k2F2R) {
            fmac(speakers[Ls], speakers[Cs], -0.70710678118654752f);
            fmac(speakers[Rs], speakers[Cs], -0.70710678118654752f);
        }
        if ((extMask & kCssXxch) && xxchDmixEmbedded) {
            const float scaleInv = float(xxchDmixScaleInv) * (1.0f / float(1 << 16));
            const int* coeff = xxchDmixCoeff;
            const int base = kModeChannels[audioMode];
            for (int ch = base; ch < nchannels; ch++) {
                const int source = speakerOf(ch);
                if (source < 0) return false;
                for (int spkr = 0; spkr < xxchMaskBits; spkr++) {
                    if (!(xxchDmixMask[ch - base] & (1 << spkr))) continue;
                    const int k = *coeff++;
                    if (k && speakers[spkr]) fmac(speakers[spkr], speakers[source], float(k) * (-1.0f / float(1 << 15)));
                }
            }
            for (int spkr = 0; spkr < xxchMaskBits; spkr++)
                if ((xxchCoreMask & (1 << spkr)) && speakers[spkr])
                    for (int i = 0; i < nsamples; i++) speakers[spkr][i] *= scaleInv;
        }
        if (!(extMask & (kCssXxch | kCssXch))) {
            auto butterflies = [&](float* a, float* b) {
                for (int i = 0; i < nsamples; i++) {
                    const float t = a[i] - b[i];
                    a[i] += b[i];
                    b[i] = t;
                }
            };
            if ((sumdiffFront && audioMode > kMono) || audioMode == kSumDiff) butterflies(speakers[L], speakers[R]);
            if (sumdiffSurround && audioMode >= k2F2R) butterflies(speakers[Ls], speakers[Rs]);
        }
        const size_t at = out.size();
        out.resize(at + size_t(nsamples) * size_t(outputChannels));
        float* to = out.data() + at;
        for (int i = 0; i < nsamples; i++)
            for (int c = 0; c < outputChannels; c++) *to++ = speakers[remap[c]][i];
        return true;
    }

    void layoutFor(int mask) {
        int wavMask = 0, wavMap[18] = {};
        for (int dca = 0; dca < 28; dca++) {
            if (!(mask & (1 << dca))) continue;
            const int wav = kDcaToWav[dca];
            if (!(wavMask & (1 << wav))) {
                wavMap[wav] = dca;
                wavMask |= 1 << wav;
            }
        }
        outputChannels = 0;
        for (int wav = 0; wav < 18; wav++)
            if (wavMask & (1 << wav)) remap[outputChannels++] = wavMap[wav];
        outputMask = mask;
        layout = uint64_t(wavMask);
        channels = outputChannels;
    }

    const uint8_t* input(const std::vector<uint8_t>& packet, size_t& size) {
        size = packet.size();
        if (size < 16 || size > 0x104000) return nullptr;
        const uint32_t mark = be32At(packet.data());
        if (mark == kSyncCore) return packet.data();
        if (mark == kSyncSubstream) return nullptr;
        for (size_t i = 0; i + 16 <= size; i++) {
            if (be32At(packet.data() + i) == kSyncSubstream) return nullptr;
            const int made = convert(packet.data() + i, size - i, converted);
            if (made >= 0) {
                size = size_t(made);
                return converted.data();
            }
        }
        return nullptr;
    }

    void feed(const std::vector<uint8_t>& packet, std::vector<float>& out) override {
        size_t size = 0;
        const uint8_t* data = input(packet, size);
        if (!data || be32At(data) != kSyncCore) return;
        if (!parse(data, size)) return;
        filter(out);
    }
};

}

uint32_t dtsFrameSamples(const uint8_t* p, size_t have) {
    if (have < 10) return 0;
    std::vector<uint8_t> core;
    if (convert(p, have, core) < 10 || be32At(core.data()) != kSyncCore) return 0;
    return uint32_t(((core[4] & 1) << 6 | core[5] >> 2) + 1) * 32;
}

bool dtsFrame(const uint8_t* header, DolbyFrame& out) {
    if (be32At(header) != kSyncCore) return false;
    BitReader r(header, 16);
    r.skip(32 + 1);
    if (r.read(5) + 1 != 32) return false;
    r.skip(1);
    const uint32_t blocks = r.read(7) + 1;
    const uint32_t bytes = r.read(14) + 1;
    const uint32_t mode = r.read(6);
    const uint32_t rate = kSampleRates[r.read(4)];
    r.skip(5 + 1 + 4 + 3 + 1 + 1);
    const uint32_t lfe = r.read(2);
    if ((blocks & 7) || bytes < 96 || mode >= kModes || !rate || lfe == 3) return false;
    out.codec = fourcc("dts ");
    out.rate = int(rate);
    out.channels = kModeChannels[mode] + (lfe ? 1 : 0);
    out.bytes = bytes;
    out.samples = blocks * 32;
    out.dependent = false;
    return true;
}

std::unique_ptr<PacketDecoder> openDtsAudio(const Track&, const std::vector<uint8_t>& firstPacket, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    auto audio = std::make_unique<DtsAudio>();
    size_t size = 0;
    const uint8_t* data = audio->input(firstPacket, size);
    if (!data || be32At(data) != kSyncCore) return fail("DTS stream without a core (DTS Express and lossless-only streams are not supported)");
    if (!audio->parse(data, size)) return fail("DTS frame header not understood");
    audio->layoutFor(audio->chMask);
    audio->rate = audio->sampleRate;
    audio->packetFrames = audio->npcmblocks * 32;
    audio->prerollPackets = 2;
    audio->reset();
    if (audio->channels < 1) return fail("DTS layout not supported");
    return audio;
}

}

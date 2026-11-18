#include "absent/media.h"
#include "bytes.h"
#include "tools_common.h"
#include "writers.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include "portable.h"

namespace absent {

namespace {

struct Out {
    TrackSpec spec;
    std::vector<Sample> samples;
    std::vector<uint32_t> chunkSizes;
    std::vector<uint64_t> chunkOffsets;
    std::vector<uint8_t> opening;
    int64_t start = 0;
    int64_t shown = 0;
    int64_t mediaDuration = 0;
    uint32_t sourceScale = 0;
    uint32_t frameBytes = 0;
    uint64_t frames = 0;
    bool trimmed = false;
};

bool isPcm(const TrackSpec& spec) { return spec.codec == fourcc("lpcm"); }

bool isProRes(uint32_t codec) {
    return codec == fourcc("apch") || codec == fourcc("apcn") || codec == fourcc("apcs") || codec == fourcc("apco") || codec == fourcc("ap4h") || codec == fourcc("ap4x");
}

bool isDolby(uint32_t codec) { return codec == fourcc("ac-3") || codec == fourcc("ec-3"); }

bool isMpegAudio(uint32_t codec) { return codec == fourcc(".mp3") || codec == fourcc(".mp2"); }

uint32_t visualObjectType(uint32_t codec) { return codec == fourcc("jpeg") ? 0x6C : codec == fourcc("mp1v") ? 0x6A : codec == fourcc("mp2v") ? 0x61 : 0; }

bool needsConfig(uint32_t codec) {
    return codec == fourcc("avc1") || codec == fourcc("hvc1") || codec == fourcc("av01") || codec == fourcc("apv1") || codec == fourcc("mp4v") || codec == fourcc("mp4a") || codec == fourcc("Opus")
        || codec == fourcc("fLaC") || codec == fourcc("alac") || codec == fourcc("vorb");
}

bool storable(const TrackSpec& spec) {
    if (isPcm(spec)) return spec.bits == 8 || spec.bits == 16 || spec.bits == 24 || spec.bits == 32 || (spec.floating && spec.bits == 64);
    return needsConfig(spec.codec) || isProRes(spec.codec) || isDolby(spec.codec) || isMpegAudio(spec.codec) || spec.codec == fourcc("vp09") || spec.codec == fourcc("vp08") || visualObjectType(spec.codec);
}

bool setsInBand(const TrackSpec& spec) {
    const std::vector<uint8_t>& c = spec.config;
    if (spec.codec == fourcc("hvc1")) return c.size() < 23 || c[22] == 0;
    if (c.size() < 6 || (c[5] & 0x1F) == 0) return true;
    size_t at = 6;
    for (int i = c[5] & 0x1F; i > 0 && at + 2 <= c.size(); --i) at += 2 + size_t(c[at] << 8 | c[at + 1]);
    return at >= c.size() || c[at] == 0;
}

void wrap(Bytes& b, uint32_t tag, const std::vector<uint8_t>& body) {
    const uint32_t n = uint32_t(body.size());
    b.u8(tag);
    if (n < 0x80) b.u8(n);
    else for (int shift = 21; shift >= 0; shift -= 7) b.u8(((n >> shift) & 0x7F) | (shift ? 0x80 : 0));
    b.raw(body.data(), body.size());
}

std::vector<uint8_t> descriptor(uint32_t objectType, uint32_t streamType, const std::vector<uint8_t>& specific) {
    Bytes config;
    config.u8(objectType);
    config.u8(streamType);
    config.u24(0);
    config.u32(0);
    config.u32(0);
    if (!specific.empty()) wrap(config, 0x05, specific);
    Bytes stream;
    stream.u16(0);
    stream.u8(0);
    wrap(stream, 0x04, config.d);
    wrap(stream, 0x06, {0x02});
    Bytes b;
    b.u32(0);
    wrap(b, 0x03, stream.d);
    return b.d;
}

std::vector<uint8_t> elementaryStream(const TrackSpec& spec) {
    if (isMpegAudio(spec.codec)) return descriptor(spec.sampleRate > 24000 ? 0x6B : 0x69, 0x15, {});
    if (spec.codec == fourcc("vorb")) return descriptor(0xDD, 0x15, spec.config);
    if (spec.configType != fourcc("ASC ")) return spec.config;
    return descriptor(0x40, 0x15, spec.config);
}

std::vector<uint8_t> opusSpecific(const std::vector<uint8_t>& head) {
    Bytes b;
    if (head.size() < 19) return b.d;
    b.u8(0);
    b.u8(head[9]);
    b.u16(uint32_t(head[10]) | (uint32_t(head[11]) << 8));
    b.u32(uint32_t(head[12]) | (uint32_t(head[13]) << 8) | (uint32_t(head[14]) << 16) | (uint32_t(head[15]) << 24));
    b.u16(uint32_t(head[16]) | (uint32_t(head[17]) << 8));
    b.raw(head.data() + 18, head.size() - 18);
    return b.d;
}

std::vector<uint8_t> vpConfig(const Out& t) {
    const TrackSpec& spec = t.spec;
    if (!spec.config.empty()) return spec.config;
    int profile = 0, depth = 8, chroma = 1;
    bool full = spec.color.full;
    if (spec.codec == fourcc("vp09") && t.opening.size() >= 8) {
        uint64_t bits = 0;
        for (int i = 0; i < 8; ++i) bits = (bits << 8) | t.opening[size_t(i)];
        int at = 64;
        auto take = [&](int n) {
            at -= n;
            return int((bits >> at) & ((1u << n) - 1));
        };
        bool marked = take(2) == 2;
        int low = take(1);
        int version = low | (take(1) << 1);
        if (version == 3) take(1);
        bool existing = take(1);
        bool key = take(1) == 0;
        take(2);
        if (marked && !existing && key && take(24) == 0x498342) {
            profile = version;
            if (profile >= 2) depth = take(1) ? 12 : 10;
            bool odd = profile == 1 || profile == 3;
            if (take(3) != 7) {
                full = take(1);
                if (odd) {
                    int across = take(1), down = take(1);
                    chroma = across && down ? 1 : across ? 2 : 3;
                }
            } else {
                full = true;
                if (odd) chroma = 3;
            }
        }
    }
    static const int areas[] = {36864, 73728, 122880, 245760, 552960, 983040, 2228224, 8912896};
    static const int levels[] = {10, 11, 20, 21, 30, 31, 41, 51};
    int level = 62;
    for (int i = 7; i >= 0; --i) if (spec.width * spec.height <= areas[i]) level = levels[i];
    Bytes b;
    b.u32(1 << 24);
    b.u8(profile);
    b.u8(spec.codec == fourcc("vp09") ? level : 0);
    b.u8((depth << 4) | (chroma << 1) | (full ? 1 : 0));
    b.u8(spec.color.known ? spec.color.primaries : 2);
    b.u8(spec.color.known ? spec.color.transfer : 2);
    b.u8(spec.color.known ? spec.color.matrix : 2);
    b.u16(0);
    return b.d;
}

void child(Bytes& b, uint32_t type, const std::vector<uint8_t>& body) {
    size_t at = b.open(type);
    b.raw(body.data(), body.size());
    b.close(at);
}

void soundStart(Bytes& b, int version, int channels, int bits, int compression, uint32_t rate) {
    b.u16(version);
    b.u16(0);
    b.u32(0);
    b.u16(channels);
    b.u16(bits);
    b.u16(compression);
    b.u16(0);
    b.u32(rate <= 0xFFFF ? rate << 16 : 0);
}

void soundExtension(Bytes& b, uint32_t samplesPerPacket, uint32_t bytesPerSample, int channels) {
    b.u32(samplesPerPacket);
    b.u32(bytesPerSample);
    b.u32(bytesPerSample * uint32_t(channels));
    b.u32(2);
}

void soundEntry(Bytes& b, const Out& t, bool quickTime) {
    const TrackSpec& spec = t.spec;
    const uint32_t rate = uint32_t(spec.sampleRate);
    auto open = [&](uint32_t type) {
        size_t at = b.open(type);
        b.zero(6);
        b.u16(1);
        return at;
    };
    auto wave = [&](uint32_t format, const std::function<void()>& inside) {
        size_t at = b.open(fourcc("wave"));
        size_t frma = b.open(fourcc("frma"));
        b.u32(format);
        b.close(frma);
        inside();
        b.u32(8);
        b.u32(0);
        b.close(at);
    };
    if (isPcm(spec)) {
        const bool small = spec.channels <= 2;
        const bool bytes = !spec.floating && spec.bits == 8;
        if (!quickTime && !bytes) {
            size_t at = open(spec.floating ? fourcc("fpcm") : fourcc("ipcm"));
            soundStart(b, 0, spec.channels, spec.bits, 0, rate);
            size_t config = b.open(fourcc("pcmC"), 0, 0);
            b.u8(spec.bigEndian ? 0 : 1);
            b.u8(spec.bits);
            b.close(config);
            b.close(at);
        } else if (small && bytes && rate <= 0xFFFF) {
            size_t at = open(spec.bigEndian ? fourcc("twos") : fourcc("raw "));
            soundStart(b, 0, spec.channels, 8, 0, rate);
            b.close(at);
        } else if (small && !spec.floating && spec.bits == 16 && rate <= 0xFFFF) {
            size_t at = open(spec.bigEndian ? fourcc("twos") : fourcc("sowt"));
            soundStart(b, 0, spec.channels, 16, 0, rate);
            b.close(at);
        } else if (small && !spec.floating && spec.bits == 24 && rate <= 0xFFFF) {
            size_t at = open(fourcc("in24"));
            soundStart(b, 1, spec.channels, 16, 0, rate);
            soundExtension(b, 1, 3, spec.channels);
            wave(fourcc("in24"), [&] {
                size_t enda = b.open(fourcc("enda"));
                b.u16(spec.bigEndian ? 0 : 1);
                b.close(enda);
            });
            b.close(at);
        } else {
            size_t at = open(fourcc("lpcm"));
            b.u16(2);
            b.u16(0);
            b.u32(0);
            b.u16(3);
            b.u16(16);
            b.u16(0xFFFE);
            b.u16(0);
            b.u32(0x00010000);
            b.u32(72);
            double exact = spec.sampleRate;
            uint64_t pattern;
            memcpy(&pattern, &exact, 8);
            b.u64(pattern);
            b.u32(uint32_t(spec.channels));
            b.u32(0x7F000000);
            b.u32(uint32_t(spec.bits));
            b.u32((spec.floating ? 1u : bytes && !spec.bigEndian ? 0u : 4u) | (spec.bigEndian && !bytes ? 2u : 0u) | 8u);
            b.u32(t.frameBytes);
            b.u32(1);
            b.close(at);
        }
        return;
    }
    if (isDolby(spec.codec)) {
        const uint32_t box = spec.codec == fourcc("ac-3") ? fourcc("dac3") : fourcc("dec3");
        const std::vector<uint8_t> specific = dolbySpecific(spec.codec, t.opening.data(), t.opening.size());
        size_t at = open(spec.codec);
        if (quickTime) {
            soundStart(b, 1, spec.channels, 16, 0xFFFE, rate);
            soundExtension(b, 1536, 0, spec.channels);
            wave(spec.codec, [&] { child(b, box, specific); });
        } else {
            soundStart(b, 0, spec.channels, 16, 0, rate);
            child(b, box, specific);
        }
        b.close(at);
    } else if (quickTime && spec.codec == fourcc(".mp3")) {
        size_t at = open(spec.codec);
        soundStart(b, 1, spec.channels, 16, 0xFFFE, rate);
        soundExtension(b, rate >= 32000 ? 1152 : 576, 0, spec.channels);
        b.close(at);
    } else if (isMpegAudio(spec.codec) || spec.codec == fourcc("vorb")) {
        size_t at = open(fourcc("mp4a"));
        soundStart(b, 0, spec.channels, 16, 0, rate);
        child(b, fourcc("esds"), elementaryStream(spec));
        b.close(at);
    } else if (spec.codec == fourcc("mp4a")) {
        size_t at = open(spec.codec);
        if (quickTime) {
            soundStart(b, 1, spec.channels, 16, 0xFFFE, rate);
            soundExtension(b, 1024, 0, spec.channels);
            wave(spec.codec, [&] {
                b.u32(12);
                b.u32(spec.codec);
                b.u32(0);
                child(b, fourcc("esds"), elementaryStream(spec));
            });
        } else {
            soundStart(b, 0, spec.channels, 16, 0, rate);
            child(b, fourcc("esds"), elementaryStream(spec));
        }
        b.close(at);
    } else if (spec.codec == fourcc("alac")) {
        std::vector<uint8_t> cookie(4, 0);
        cookie.insert(cookie.end(), spec.config.end() - 24, spec.config.end());
        uint32_t frameLength = (uint32_t(cookie[4]) << 24) | (uint32_t(cookie[5]) << 16) | (uint32_t(cookie[6]) << 8) | cookie[7];
        size_t at = open(spec.codec);
        if (quickTime) {
            soundStart(b, 1, spec.channels, 16, 0xFFFE, rate);
            soundExtension(b, frameLength, 0, spec.channels);
            wave(spec.codec, [&] { child(b, fourcc("alac"), cookie); });
        } else {
            soundStart(b, 0, spec.channels, cookie[9], 0, rate);
            child(b, fourcc("alac"), cookie);
        }
        b.close(at);
    } else if (spec.codec == fourcc("Opus")) {
        size_t at = open(spec.codec);
        soundStart(b, 0, spec.channels, 16, 0, 48000);
        child(b, fourcc("dOps"), opusSpecific(spec.config));
        b.close(at);
    } else {
        uint32_t shown = rate;
        while (shown > 0xFFFF) shown /= 2;
        size_t at = open(spec.codec);
        soundStart(b, 0, spec.channels, spec.bits ? spec.bits : 16, 0, shown);
        size_t config = b.open(fourcc("dfLa"), 0, 0);
        b.u8(0x80);
        b.u24(uint32_t(spec.config.size()));
        b.raw(spec.config.data(), spec.config.size());
        b.close(config);
        b.close(at);
    }
}

const uint32_t kChunkLimit = 64;

void matrix(Bytes& b, int rotation) {
    static const int32_t turns[4][4] = {{1, 0, 0, 1}, {0, 1, -1, 0}, {-1, 0, 0, -1}, {0, -1, 1, 0}};
    const int32_t* m = turns[((rotation / 90) % 4 + 4) % 4];
    b.u32(uint32_t(m[0] * 0x10000));
    b.u32(uint32_t(m[1] * 0x10000));
    b.u32(0);
    b.u32(uint32_t(m[2] * 0x10000));
    b.u32(uint32_t(m[3] * 0x10000));
    b.u32(0);
    b.u32(0);
    b.u32(0);
    b.u32(0x40000000);
}

void prepare(Out& t) {
    for (;;) {
        int64_t widest = 0;
        for (size_t i = 0; i < t.samples.size(); ++i) {
            widest = std::max(widest, std::abs(t.samples[i].pts - t.samples[i].dts) * 2);
            if (i + 1 < t.samples.size()) widest = std::max(widest, t.samples[i + 1].dts - t.samples[i].dts);
        }
        if (widest <= 0xFFFFFFFFll || t.spec.timescale % 10 || isPcm(t.spec)) break;
        t.spec.timescale /= 10;
        for (Sample& s : t.samples) {
            s.pts = s.pts >= 0 ? s.pts / 10 : -((9 - s.pts) / 10);
            s.dts = s.dts >= 0 ? s.dts / 10 : -((9 - s.dts) / 10);
            s.duration = (s.duration + 9) / 10;
        }
        t.spec.skip /= 10;
        t.spec.length /= 10;
        t.spec.delay /= 10;
    }
    int64_t low = 0;
    for (const Sample& s : t.samples) low = std::min(low, s.pts - s.dts);
    int64_t base = t.samples.front().dts + low;
    for (Sample& s : t.samples) {
        s.dts += low - base;
        s.pts -= base;
    }
    for (size_t i = 0; i + 1 < t.samples.size(); ++i) {
        int64_t delta = t.samples[i + 1].dts - t.samples[i].dts;
        t.samples[i].duration = uint32_t(std::max<int64_t>(delta, 0));
    }
    int64_t first = t.samples.front().pts;
    int64_t end = 0;
    for (const Sample& s : t.samples) {
        first = std::min(first, s.pts);
        end = std::max(end, s.pts + int64_t(s.duration));
    }
    const bool empty = t.trimmed && t.spec.length <= 0;
    t.start = empty ? end : first + t.spec.skip;
    t.shown = t.spec.length > 0 ? t.spec.length : std::max<int64_t>(end - t.start, 0);
    Sample& tail = t.samples.back();
    if (t.spec.length > 0 && t.start + t.shown > tail.dts && t.start + t.shown < tail.dts + int64_t(tail.duration)) tail.duration = uint32_t(t.start + t.shown - tail.dts);
    if (t.spec.length > 0 && t.spec.kind == Kind::Video && t.start + t.shown > tail.dts + int64_t(tail.duration)) tail.duration = uint32_t(std::min<int64_t>(t.start + t.shown - tail.dts, 0xFFFFFFFFll));
    t.mediaDuration = tail.dts + tail.duration;
}

uint64_t scaled(int64_t value, uint32_t from, uint32_t to) {
    return from ? uint64_t((__int128(value) * to + from / 2) / from) : 0;
}

void sampleEntry(Bytes& b, const Out& t, bool quickTime) {
    const TrackSpec& spec = t.spec;
    if (spec.kind != Kind::Video) {
        soundEntry(b, t, quickTime);
        return;
    }
    const bool proRes = isProRes(spec.codec);
    const uint32_t visual = visualObjectType(spec.codec);
    const uint32_t named = spec.codec == fourcc("avc1") && setsInBand(spec) ? fourcc("avc3") : spec.codec == fourcc("hvc1") && setsInBand(spec) ? fourcc("hev1") : spec.codec;
    size_t at = b.open(visual && !quickTime ? fourcc("mp4v") : spec.codec == fourcc("mp1v") ? fourcc("m1v ") : spec.codec == fourcc("mp2v") ? fourcc("m2v1") : named);
    b.zero(6);
    b.u16(1);
    b.zero(4);
    b.u32(proRes ? fourcc("appl") : 0);
    b.zero(8);
    b.u16(spec.width);
    b.u16(spec.height);
    b.u32(0x00480000);
    b.u32(0x00480000);
    b.u32(0);
    b.u16(1);
    b.zero(32);
    b.u16(spec.codec == fourcc("ap4h") || spec.codec == fourcc("ap4x") ? 0x0020 : 0x0018);
    b.u16(0xFFFF);
    if (spec.codec == fourcc("vp09") || spec.codec == fourcc("vp08")) child(b, fourcc("vpcC"), vpConfig(t));
    else if (spec.codec == fourcc("mp4v")) child(b, fourcc("esds"), spec.config);
    else if (visual) {
        if (!quickTime) child(b, fourcc("esds"), descriptor(visual, 0x11, {}));
    } else if (!proRes) child(b, spec.codec == fourcc("avc1") ? fourcc("avcC") : spec.codec == fourcc("hvc1") ? fourcc("hvcC") : spec.codec == fourcc("apv1") ? fourcc("apvC") : fourcc("av1C"), spec.config);
    const bool headed = t.opening.size() > 20 && memcmp(t.opening.data() + 4, "icpf", 4) == 0;
    const int scan = headed ? (t.opening[20] >> 2) & 3 : 3;
    if (proRes && scan < 3) {
        size_t fields = b.open(fourcc("fiel"));
        b.u8(scan ? 2 : 1);
        b.u8(scan == 1 ? 9 : scan == 2 ? 14 : 0);
        b.close(fields);
    }
    if (spec.color.known && spec.color.primaries != 2 && spec.color.transfer != 2 && spec.color.matrix != 2) {
        size_t color = b.open(fourcc("colr"));
        b.u32(quickTime ? fourcc("nclc") : fourcc("nclx"));
        b.u16(spec.color.primaries);
        b.u16(spec.color.transfer);
        b.u16(spec.color.matrix);
        if (!quickTime) b.u8(spec.color.full ? 0x80 : 0);
        b.close(color);
    }
    b.close(at);
}

void tables(Bytes& b, const Out& t, uint64_t base, bool wide, bool quickTime) {
    const bool pcm = isPcm(t.spec);
    size_t stbl = b.open(fourcc("stbl"));
    size_t stsd = b.open(fourcc("stsd"), 0, 0);
    b.u32(1);
    sampleEntry(b, t, quickTime);
    b.close(stsd);

    size_t stts = b.open(fourcc("stts"), 0, 0);
    size_t count = b.d.size();
    b.u32(0);
    uint32_t runs = 0;
    if (pcm) {
        b.u32(uint32_t(t.frames));
        b.u32(1);
        ++runs;
    }
    for (size_t i = 0; !pcm && i < t.samples.size();) {
        size_t j = i;
        while (j < t.samples.size() && t.samples[j].duration == t.samples[i].duration) ++j;
        b.u32(uint32_t(j - i));
        b.u32(t.samples[i].duration);
        ++runs;
        i = j;
    }
    Bytes patch;
    patch.u32(runs);
    std::copy(patch.d.begin(), patch.d.end(), b.d.begin() + count);
    b.close(stts);

    bool allSync = pcm || std::all_of(t.samples.begin(), t.samples.end(), [](const Sample& s) { return s.sync; });
    if (!allSync) {
        size_t stss = b.open(fourcc("stss"), 0, 0);
        uint32_t syncs = uint32_t(std::count_if(t.samples.begin(), t.samples.end(), [](const Sample& s) { return s.sync; }));
        b.u32(syncs);
        for (size_t i = 0; i < t.samples.size(); ++i) if (t.samples[i].sync) b.u32(uint32_t(i + 1));
        b.close(stss);
    }

    bool shifted = !pcm && std::any_of(t.samples.begin(), t.samples.end(), [](const Sample& s) { return s.pts != s.dts; });
    if (shifted) {
        size_t ctts = b.open(fourcc("ctts"), 0, 0);
        size_t place = b.d.size();
        b.u32(0);
        uint32_t groups = 0;
        for (size_t i = 0; i < t.samples.size();) {
            int64_t offset = t.samples[i].pts - t.samples[i].dts;
            size_t j = i;
            while (j < t.samples.size() && t.samples[j].pts - t.samples[j].dts == offset) ++j;
            b.u32(uint32_t(j - i));
            b.u32(uint32_t(offset));
            ++groups;
            i = j;
        }
        Bytes fix;
        fix.u32(groups);
        std::copy(fix.d.begin(), fix.d.end(), b.d.begin() + place);
        b.close(ctts);
    }

    size_t stsc = b.open(fourcc("stsc"), 0, 0);
    size_t where = b.d.size();
    b.u32(0);
    uint32_t entries = 0;
    for (size_t i = 0; i < t.chunkSizes.size(); ++i) {
        if (i && t.chunkSizes[i] == t.chunkSizes[i - 1]) continue;
        b.u32(uint32_t(i + 1));
        b.u32(t.chunkSizes[i]);
        b.u32(1);
        ++entries;
    }
    Bytes amount;
    amount.u32(entries);
    std::copy(amount.d.begin(), amount.d.end(), b.d.begin() + where);
    b.close(stsc);

    size_t stsz = b.open(fourcc("stsz"), 0, 0);
    b.u32(pcm ? t.frameBytes : 0);
    b.u32(pcm ? uint32_t(t.frames) : uint32_t(t.samples.size()));
    for (size_t i = 0; !pcm && i < t.samples.size(); ++i) b.u32(t.samples[i].size);
    b.close(stsz);

    size_t offsets = b.open(wide ? fourcc("co64") : fourcc("stco"), 0, 0);
    b.u32(uint32_t(t.chunkOffsets.size()));
    for (uint64_t offset : t.chunkOffsets) {
        if (wide) b.u64(base + offset);
        else b.u32(uint32_t(base + offset));
    }
    b.close(offsets);

    const bool opus = t.spec.codec == fourcc("Opus");
    if (opus || (t.spec.codec == fourcc("mp4a") && t.spec.skip > 0)) {
        size_t sgpd = b.open(fourcc("sgpd"), 1, 0);
        b.u32(fourcc("roll"));
        b.u32(2);
        b.u32(1);
        b.u16(opus ? 0xFFFC : 0xFFFF);
        b.close(sgpd);
        size_t sbgp = b.open(fourcc("sbgp"), 0, 0);
        b.u32(fourcc("roll"));
        b.u32(1);
        b.u32(uint32_t(t.samples.size()));
        b.u32(1);
        b.close(sbgp);
    }
    b.close(stbl);
}

void handler(Bytes& b, uint32_t kind, uint32_t type, const char* name, bool quickTime) {
    size_t hdlr = b.open(fourcc("hdlr"), 0, 0);
    b.u32(quickTime ? kind : 0);
    b.u32(type);
    b.zero(12);
    if (quickTime) b.u8(uint32_t(strlen(name)));
    b.raw(name, strlen(name) + (quickTime ? 0 : 1));
    b.close(hdlr);
}

void userData(Bytes& b, const std::map<std::string, std::string>& tags) {
    static const std::pair<const char*, uint32_t> items[] = {
        {"title", 0xA96E616D}, {"artist", 0xA9415254}, {"album_artist", fourcc("aART")}, {"composer", 0xA9777274}, {"album", 0xA9616C62}, {"date", 0xA9646179},
        {"comment", 0xA9636D74}, {"genre", 0xA967656E}, {"copyright", fourcc("cprt")}, {"grouping", 0xA9677270}, {"lyrics", 0xA96C7972}, {"description", fourcc("desc")},
    };
    Bytes list;
    for (const auto& [name, type] : items) {
        auto found = tags.find(name);
        if (found == tags.end() || found->second.empty()) continue;
        size_t item = list.open(type);
        size_t data = list.open(fourcc("data"));
        list.u32(1);
        list.u32(0);
        list.raw(found->second.data(), found->second.size());
        list.close(data);
        list.close(item);
    }
    auto place = tags.find("location");
    const bool located = place != tags.end() && !place->second.empty() && place->second.size() < 0x10000;
    if (list.d.empty() && !located) return;
    size_t udta = b.open(fourcc("udta"));
    if (!list.d.empty()) {
        size_t meta = b.open(fourcc("meta"), 0, 0);
        size_t hdlr = b.open(fourcc("hdlr"), 0, 0);
        b.u32(0);
        b.u32(fourcc("mdir"));
        b.u32(fourcc("appl"));
        b.zero(9);
        b.close(hdlr);
        child(b, fourcc("ilst"), list.d);
        b.close(meta);
    }
    if (located) {
        size_t xyz = b.open(0xA978797A);
        b.u16(uint32_t(place->second.size()));
        b.u16(0x55C4);
        b.raw(place->second.data(), place->second.size());
        b.close(xyz);
    }
    b.close(udta);
}

uint64_t createdSince1904(const std::map<std::string, std::string>& tags) {
    auto found = tags.find("creation_time");
    if (found == tags.end()) return 0;
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    const int parts = sscanf(found->second.c_str(), "%d-%d-%d%*c%d:%d:%d", &year, &month, &day, &hour, &minute, &second);
    if (parts < 3 || month < 1 || day < 1) return 0;
    if (parts < 6 || hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60) hour = minute = second = 0;
    const std::chrono::year_month_day date{std::chrono::year(year), std::chrono::month(unsigned(month)), std::chrono::day(unsigned(day))};
    if (!date.ok() || year < 1904 || year > 9999) return 0;
    const int64_t days = std::chrono::sys_days(date).time_since_epoch().count();
    return uint64_t(days * 86400 + hour * 3600 + minute * 60 + second + 2082844800ll);
}

void stamps(Bytes& b, bool wide, uint64_t created) {
    for (int i = 0; i < 2; ++i) {
        if (wide) b.u64(created);
        else b.u32(uint32_t(created));
    }
}

void track(Bytes& b, const Out& t, uint32_t id, uint64_t base, bool wide, bool quickTime, uint32_t movieScale, uint64_t created) {
    const TrackSpec& spec = t.spec;
    bool video = spec.kind == Kind::Video;
    size_t trak = b.open(fourcc("trak"));

    const uint64_t shownLength = scaled(t.shown, spec.timescale, movieScale);
    const uint64_t trackLength = scaled(t.shown + spec.delay, spec.timescale, movieScale);
    const uint64_t waitLength = scaled(spec.delay, spec.timescale, movieScale);
    const bool longTrack = trackLength > 0xFFFFFFFFull || created > 0xFFFFFFFFull;
    size_t tkhd = b.open(fourcc("tkhd"), longTrack ? 1 : 0, 3);
    stamps(b, longTrack, created);
    b.u32(id);
    b.u32(0);
    if (longTrack) b.u64(trackLength);
    else b.u32(uint32_t(trackLength));
    b.zero(8);
    b.u16(0);
    b.u16(video ? 0 : 1);
    b.u16(video ? 0 : 0x0100);
    b.u16(0);
    matrix(b, video ? spec.rotation : 0);
    b.u32(uint32_t(spec.width) << 16);
    b.u32(uint32_t(spec.height) << 16);
    b.close(tkhd);

    if (t.start != 0 || spec.length > 0 || spec.delay > 0) {
        size_t edts = b.open(fourcc("edts"));
        const bool longEdit = shownLength >= 0xFFFFFFFFull || waitLength >= 0xFFFFFFFFull || t.start > 0x7FFFFFFF;
        size_t elst = b.open(fourcc("elst"), longEdit ? 1 : 0, 0);
        b.u32(spec.delay > 0 ? 2 : 1);
        auto edit = [&](uint64_t length, int64_t from) {
            if (longEdit) {
                b.u64(length);
                b.u64(uint64_t(from));
            } else {
                b.u32(uint32_t(length));
                b.u32(uint32_t(from));
            }
            b.u32(0x00010000);
        };
        if (spec.delay > 0) edit(waitLength, -1);
        edit(shownLength, t.start);
        b.close(elst);
        b.close(edts);
    }

    size_t mdia = b.open(fourcc("mdia"));
    const bool longMedia = t.mediaDuration > 0xFFFFFFFFll || created > 0xFFFFFFFFull;
    size_t mdhd = b.open(fourcc("mdhd"), longMedia ? 1 : 0, 0);
    stamps(b, longMedia, created);
    b.u32(spec.timescale);
    if (longMedia) b.u64(uint64_t(t.mediaDuration));
    else b.u32(uint32_t(t.mediaDuration));
    b.u16(0x55C4);
    b.u16(0);
    b.close(mdhd);

    handler(b, fourcc("mhlr"), video ? fourcc("vide") : fourcc("soun"), video ? "VideoHandler" : "SoundHandler", quickTime);

    size_t minf = b.open(fourcc("minf"));
    if (video) {
        size_t vmhd = b.open(fourcc("vmhd"), 0, 1);
        b.zero(8);
        b.close(vmhd);
    } else {
        size_t smhd = b.open(fourcc("smhd"), 0, 0);
        b.zero(4);
        b.close(smhd);
    }
    if (quickTime) handler(b, fourcc("dhlr"), fourcc("url "), "DataHandler", true);
    size_t dinf = b.open(fourcc("dinf"));
    size_t dref = b.open(fourcc("dref"), 0, 0);
    b.u32(1);
    size_t url = b.open(fourcc("url "), 0, 1);
    b.close(url);
    b.close(dref);
    b.close(dinf);
    tables(b, t, base, wide, quickTime);
    b.close(minf);
    b.close(mdia);
    b.close(trak);
}

}

struct Mp4Writer::Impl {
    std::string path;
    std::string temp;
    FILE* data = nullptr;
    uint64_t size = 0;
    std::vector<Out> tracks;
    int last = -1;
    bool failed = false;
    Mp4Brand brand = Mp4Brand::Mp4;
    Interleaver queue;
    std::map<std::string, std::string> tags;
    ~Impl() {
        if (data) {
            fclose(data);
            removeFile(temp);
        }
    }

    Bytes movie(uint64_t base, bool wide) const {
        Bytes b;
        size_t moov = b.open(fourcc("moov"));
        uint32_t movieScale = 0;
        for (const Out& t : tracks) {
            if (t.samples.empty()) continue;
            bool sound = t.spec.kind == Kind::Audio && (!movieScale || movieScale == t.spec.timescale);
            movieScale = sound ? t.spec.timescale : 1000;
            if (!sound) break;
        }
        uint64_t duration = 0;
        uint32_t used = 0;
        for (const Out& t : tracks) {
            if (t.samples.empty()) continue;
            duration = std::max(duration, scaled(t.shown + t.spec.delay, t.spec.timescale, movieScale));
            ++used;
        }
        const uint64_t created = createdSince1904(tags);
        const bool longMovie = duration > 0xFFFFFFFFull || created > 0xFFFFFFFFull;
        size_t mvhd = b.open(fourcc("mvhd"), longMovie ? 1 : 0, 0);
        stamps(b, longMovie, created);
        b.u32(movieScale);
        if (longMovie) b.u64(duration);
        else b.u32(uint32_t(duration));
        b.u32(0x00010000);
        b.u16(0x0100);
        b.zero(10);
        matrix(b, 0);
        b.zero(24);
        b.u32(used + 1);
        b.close(mvhd);
        uint32_t id = 0;
        for (const Out& t : tracks) if (!t.samples.empty()) track(b, t, ++id, base, wide, brand == Mp4Brand::QuickTime, movieScale, created);
        userData(b, tags);
        b.close(moov);
        return b;
    }

    bool put(const Queued& packet) {
        Out& t = tracks[size_t(packet.track)];
        const uint8_t* data = packet.data.data();
        const size_t size = packet.data.size();
        int64_t pts = packet.pts, dts = packet.dts;
        uint32_t duration = packet.duration;
        const bool pcm = isPcm(t.spec);
        if (size && fwrite(data, 1, size, this->data) != size) {
            failed = true;
            return false;
        }
        uint32_t frames = 1;
        if (pcm) {
            frames = uint32_t(size / t.frameBytes);
            pts = t.samples.empty() ? pts * int64_t(t.spec.timescale) / int64_t(t.sourceScale) : t.samples.back().pts + t.samples.back().duration;
            dts = pts;
            duration = frames;
            t.frames += frames;
        }
        if (t.samples.empty()) t.opening.assign(data, data + std::min<size_t>(size, isDolby(t.spec.codec) ? 65536 : 32));
        if (last != packet.track || t.chunkSizes.empty() || t.chunkSizes.back() >= (pcm ? std::max<uint32_t>(t.spec.timescale / 2, 1) : kChunkLimit)) {
            t.chunkSizes.push_back(0);
            t.chunkOffsets.push_back(this->size);
        }
        t.chunkSizes.back() += frames;
        Sample s;
        s.offset = this->size;
        s.size = uint32_t(size);
        s.pts = pts;
        s.dts = dts;
        s.duration = duration;
        s.sync = packet.sync;
        t.samples.push_back(s);
        this->size += size;
        last = packet.track;
        return true;
    }
};

Mp4Writer::Mp4Writer() : impl(new Impl) {}
Mp4Writer::~Mp4Writer() = default;

std::unique_ptr<Mp4Writer> Mp4Writer::open(const std::string& path, std::string* error) {
    return open(path, endsWith(path, ".mov") ? Mp4Brand::QuickTime : endsWith(path, ".m4a") ? Mp4Brand::M4a : Mp4Brand::Mp4, error);
}

std::unique_ptr<Mp4Writer> Mp4Writer::open(const std::string& path, Mp4Brand brand, std::string* error) {
    std::unique_ptr<Mp4Writer> writer(new Mp4Writer);
    writer->impl->brand = brand;
    writer->impl->path = path;
    writer->impl->temp = path + ".part";
    writer->impl->data = openFile(writer->impl->temp, "wb+");
    if (!writer->impl->data) {
        if (error) *error = "cannot create output file";
        return nullptr;
    }
    Impl* impl = writer->impl.get();
    impl->queue.sink = [impl](const Queued& packet) { return impl->put(packet); };
    return writer;
}

void Mp4Writer::tags(const std::map<std::string, std::string>& values) { impl->tags = values; }

int Mp4Writer::add(const TrackSpec& spec) {
    Out out;
    out.spec = spec;
    if (isPcm(spec)) {
        out.frameBytes = uint32_t(std::max(spec.channels, 1) * spec.bits / 8);
        out.sourceScale = spec.timescale;
        out.spec.timescale = uint32_t(spec.sampleRate);
        if (out.sourceScale && out.spec.timescale) {
            out.spec.skip = spec.skip * out.spec.timescale / out.sourceScale;
            out.spec.length = spec.length * out.spec.timescale / out.sourceScale;
            out.spec.delay = spec.delay * out.spec.timescale / out.sourceScale;
        }
    }
    impl->tracks.push_back(std::move(out));
    impl->queue.add();
    return int(impl->tracks.size()) - 1;
}

void Mp4Writer::trim(int track, int64_t skip, int64_t length) {
    if (track < 0 || track >= int(impl->tracks.size())) return;
    Out& t = impl->tracks[track];
    if (isPcm(t.spec) && t.sourceScale && t.spec.timescale) {
        skip = int64_t(__int128(skip) * t.spec.timescale / t.sourceScale);
        length = int64_t(__int128(length) * t.spec.timescale / t.sourceScale);
    }
    t.spec.skip = skip;
    t.spec.length = length;
    t.trimmed = true;
}

void Mp4Writer::configure(int track, uint32_t type, const std::vector<uint8_t>& config) {
    if (track < 0 || track >= int(impl->tracks.size())) return;
    impl->tracks[track].spec.configType = type;
    impl->tracks[track].spec.config = config;
}

bool Mp4Writer::write(int track, const uint8_t* data, size_t size, int64_t pts, int64_t dts, uint32_t duration, bool sync) {
    if (track < 0 || track >= int(impl->tracks.size()) || impl->failed || !impl->data || size > 0xFFFFFFFFu) return false;
    const Out& t = impl->tracks[track];
    const bool pcm = isPcm(t.spec);
    if (pcm && (!t.frameBytes || !t.sourceScale || !t.spec.timescale || size % t.frameBytes)) return false;
    const uint32_t scale = pcm ? t.sourceScale : t.spec.timescale;
    Queued packet;
    packet.track = track;
    if (size) packet.data.assign(data, data + size);
    packet.pts = pts;
    packet.dts = dts;
    packet.duration = duration;
    packet.sync = sync;
    packet.time = (scale ? double(dts) / scale : 0) + (t.spec.timescale ? double(t.spec.delay) / t.spec.timescale : 0);
    if (!impl->queue.push(std::move(packet))) impl->failed = true;
    return !impl->failed;
}

bool Mp4Writer::finish(std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return false;
    };
    if (impl->failed || !impl->data || !impl->queue.drain()) return fail("writing sample data failed");
    bool any = false;
    bool h264 = false;
    for (Out& t : impl->tracks) {
        if (t.samples.empty()) continue;
        if (!storable(t.spec)) return fail("a track cannot be stored in this kind of file");
        const size_t least = t.spec.codec == fourcc("alac") ? 24 : t.spec.codec == fourcc("Opus") ? 19 : t.spec.codec == fourcc("fLaC") ? 34 : 1;
        if (needsConfig(t.spec.codec) && t.spec.config.size() < least) return fail("a track has no codec configuration");
        if (t.spec.codec == fourcc("fLaC") && t.spec.config.size() != 34) return fail("a track has no codec configuration");
        if (impl->brand == Mp4Brand::M4a && t.spec.kind != Kind::Audio) return fail("an M4A file holds sound only");
        if (impl->brand == Mp4Brand::M4a && (isMpegAudio(t.spec.codec) || t.spec.codec == fourcc("vorb") || t.spec.codec == fourcc("ec-3"))) return fail("M4A files cannot hold this codec");
        if (impl->brand == Mp4Brand::QuickTime && (t.spec.codec == fourcc("Opus") || t.spec.codec == fourcc("fLaC") || t.spec.codec == fourcc("vp09") || t.spec.codec == fourcc("vp08") || t.spec.codec == fourcc("vorb") || t.spec.codec == fourcc(".mp2")))
            return fail("QuickTime files cannot hold this codec");
        if (isDolby(t.spec.codec) && dolbySpecific(t.spec.codec, t.opening.data(), t.opening.size()).empty()) return fail("a track has no codec configuration");
        prepare(t);
        any = true;
        h264 = h264 || t.spec.codec == fourcc("avc1");
    }
    if (!any) return fail("nothing was written");

    Bytes head;
    size_t ftyp = head.open(fourcc("ftyp"));
    if (impl->brand == Mp4Brand::QuickTime) {
        head.u32(fourcc("qt  "));
        head.u32(0x200);
        head.u32(fourcc("qt  "));
    } else if (impl->brand == Mp4Brand::M4a) {
        head.u32(fourcc("M4A "));
        head.u32(0);
        head.u32(fourcc("M4A "));
        head.u32(fourcc("mp42"));
        head.u32(fourcc("isom"));
    } else {
        head.u32(fourcc("isom"));
        head.u32(0x200);
        head.u32(fourcc("isom"));
        head.u32(fourcc("iso2"));
        if (h264) head.u32(fourcc("avc1"));
        head.u32(fourcc("mp41"));
    }
    head.close(ftyp);

    bool wide = impl->size > 0xF0000000ull;
    bool large = impl->size + 8 > 0xFFFFFFFFull;
    uint64_t dataHeader = large ? 16 : 8;
    Bytes probe = impl->movie(0, wide);
    if (!wide && head.d.size() + probe.d.size() + dataHeader + impl->size > 0xFFFFFFFFull) {
        wide = true;
        probe = impl->movie(0, wide);
    }
    uint64_t base = head.d.size() + probe.d.size() + dataHeader;
    Bytes moov = impl->movie(base, wide);
    if (moov.d.size() != probe.d.size()) return fail("internal layout error");

    Bytes mdat;
    if (large) {
        mdat.u32(1);
        mdat.u32(fourcc("mdat"));
        mdat.u64(impl->size + 16);
    } else {
        mdat.u32(uint32_t(impl->size + 8));
        mdat.u32(fourcc("mdat"));
    }
    if (!placeInFront(impl->data, impl->size, {&head, &moov, &mdat}, impl->temp, impl->path)) return fail("writing the output file failed");
    return true;
}

}

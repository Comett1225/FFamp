#include "absent/audio.h"
#include "audio_codec.h"
#include "readers.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace absent {

namespace {

struct PcmPackets : PacketDecoder {
    int bits = 0;
    bool floating = false;
    bool bigEndian = false;

    void reset() override {}

    void feed(const std::vector<uint8_t>& packet, std::vector<float>& out) override {
        const size_t width = size_t(bits / 8);
        const size_t frame = width * size_t(channels);
        size_t values = packet.size() / frame * size_t(channels);
        size_t at = out.size();
        out.resize(at + values);
        const uint8_t* p = packet.data();
        for (size_t i = 0; i < values; ++i, p += width) {
            uint64_t raw = 0;
            for (size_t k = 0; k < width; ++k) raw |= uint64_t(p[bigEndian ? width - 1 - k : k]) << (8 * k);
            float value;
            if (floating && bits == 32) {
                uint32_t word = uint32_t(raw);
                memcpy(&value, &word, 4);
            } else if (floating) {
                double wide;
                memcpy(&wide, &raw, 8);
                value = float(wide);
            } else if (bits == 8) {
                value = float(bigEndian ? int(int8_t(raw)) : int(raw) - 128) / 128.0f;
            } else if (bits == 16) {
                value = float(int16_t(raw)) / 32768.0f;
            } else if (bits == 24) {
                value = float(int32_t(uint32_t(raw) << 8) >> 8) / 8388608.0f;
            } else if (bits == 64) {
                value = float(int64_t(raw)) * (1.0f / 9223372036854775808.0f);
            } else {
                value = float(int32_t(raw)) * (1.0f / 2147483648.0f);
            }
            out[at + i] = value;
        }
    }
};

std::unique_ptr<PacketDecoder> openPcm(const Track& track, std::string* error) {
    bool known = track.floating ? (track.bits == 32 || track.bits == 64) : (track.bits == 8 || track.bits == 16 || track.bits == 24 || track.bits == 32 || track.bits == 64);
    if (!known) {
        if (error) *error = "PCM sample format not supported";
        return nullptr;
    }
    auto pcm = std::make_unique<PcmPackets>();
    pcm->rate = track.sampleRate;
    pcm->channels = track.channels;
    pcm->bits = track.bits;
    pcm->floating = track.floating;
    pcm->bigEndian = track.bigEndian;
    pcm->stateless = true;
    return pcm;
}

int64_t scaled(int64_t ticks, int64_t rate, int64_t timescale) {
    const long double limit = 4.0e18L;
    return int64_t(std::llroundl(std::clamp((long double)ticks * rate / timescale, -limit, limit)));
}

struct Block {
    int64_t index = -1;
    uint64_t used = 0;
    std::vector<float> pcm;
};

const int64_t kBlockFrames = 32768;
const int64_t kRunSeconds = 20;
const int64_t kHole = 131072;
const int64_t kSpareSeconds = 3600;
const int64_t kLongestPacket = 65536;
const size_t kMostBlockPackets = 1024;

}

struct AudioDecoder::Impl {
    Reader* file = nullptr;
    const Track* track = nullptr;
    std::unique_ptr<PacketDecoder> codec;
    std::vector<int64_t> starts;
    int64_t first = 0;
    int64_t last = 0;
    size_t blockPackets = 1;
    size_t runPackets = 1;
    Block cache[8];
    uint64_t clock = 0;
    int64_t run = -1;
    size_t next = 0;
    int64_t heldFrom = 0;
    bool ended = false;
    bool lagKnown = false;
    int64_t lag = 0;
    std::vector<float> held;
    std::vector<uint8_t> packet;
    int64_t hidden = INT64_MIN;
    std::vector<size_t> feeds;
    std::vector<std::vector<uint8_t>> marks;
    std::vector<char> marked;
    bool marking = false;

    size_t count() const { return starts.size() - 1; }

    void place() {
        const Track& t = *track;
        const size_t n = t.samples.size();
        const int64_t rate = codec->rate;
        const Sample& tail = t.samples.back();
        const int64_t endTicks = tail.pts > INT64_MAX - int64_t(tail.duration) ? INT64_MAX : tail.pts + int64_t(tail.duration);
        starts.resize(n + 1);
        std::vector<int64_t> counted;
        const bool known = codec->counts(*file, t, counted) && counted.size() == n;
        bool exact = rate % int64_t(t.timescale) == 0;
        if (known) {
            starts[0] = scaled(t.samples[0].pts, rate, t.timescale);
            for (size_t i = 0; i < n; ++i) starts[i + 1] = starts[i] + counted[i];
        } else if (exact || !codec->packetFrames) {
            int64_t grain = !exact && t.codec == fourcc("Opus") ? 120 : 1;
            for (size_t i = 0; i <= n; ++i) {
                int64_t at = scaled(i < n ? t.samples[i].pts : endTicks, rate, t.timescale);
                starts[i] = at >= 0 ? (at + grain / 2) / grain * grain : -((grain - grain / 2 - 1 - at) / grain * grain);
            }
        } else {
            int64_t base = scaled(t.samples[0].pts, rate, t.timescale);
            for (size_t i = 0; i <= n; ++i) starts[i] = base + int64_t(i) * codec->packetFrames;
            starts[n] = std::clamp(scaled(endTicks, rate, t.timescale), starts[n - 1], starts[n]);
        }
        for (size_t i = 1; i <= n; ++i) starts[i] = std::max(starts[i], starts[i - 1]);
        int64_t wantedFirst = std::clamp(scaled(t.mediaStart, rate, t.timescale), starts[0], starts[n]);
        int64_t wantedLast = t.mediaEnd < 0 ? starts[n] : std::clamp(scaled(t.mediaEnd, rate, t.timescale), wantedFirst, starts[n]);
        const int64_t frameBytes = std::max(1, track->channels * (track->bits / 8));
        const bool pcm = t.codec == fourcc("lpcm");
        int64_t closed = 0;
        int64_t spare = rate * kSpareSeconds;
        const int64_t longest = codec->longestPacket > 0 ? codec->longestPacket : codec->packetFrames > 0 ? codec->packetFrames : kLongestPacket;
        int64_t before = starts[0];
        first = last = 0;
        bool haveFirst = false, haveLast = false;
        for (size_t i = 0; i < n; ++i) {
            int64_t from = before;
            int64_t span = starts[i + 1] - from;
            int64_t real = known ? span : pcm ? int64_t(t.samples[i].size) / frameBytes : longest;
            int64_t extra = std::max<int64_t>(0, span - real);
            int64_t kept = extra > kHole || extra > spare ? std::min(span, real) : span;
            if (kept == span) spare -= extra;
            if (!haveFirst && wantedFirst < from + span) {
                first = from - closed + std::min(wantedFirst - from, kept);
                haveFirst = true;
            }
            if (!haveLast && wantedLast <= from + span) {
                last = from - closed + std::min(wantedLast - from, kept);
                haveLast = true;
            }
            closed += span - kept;
            before = starts[i + 1];
            starts[i + 1] = before - closed;
        }
        if (!haveFirst) first = starts[n];
        if (!haveLast) last = starts[n];
        last = std::max(last, first);
        int64_t average = std::max<int64_t>(1, (starts[n] - starts[0]) / int64_t(n));
        blockPackets = size_t(std::clamp<int64_t>((kBlockFrames + average - 1) / average, 1, int64_t(kMostBlockPackets)));
        int64_t blockFrames = int64_t(blockPackets) * average;
        runPackets = codec->stateless ? blockPackets : blockPackets * size_t(std::max<int64_t>(1, kRunSeconds * rate / blockFrames));
        hidden = wantedFirst > starts[0] ? INT64_MIN : starts[0] + codec->skip;
        feeds.resize((n + runPackets - 1) / runPackets);
        for (size_t g = 0; g < feeds.size(); ++g) feeds[g] = feedPoint(int64_t(g));
        std::vector<uint8_t> state;
        marking = codec->save(state);
        if (marking) {
            marks.assign(feeds.size(), {});
            marked.assign(feeds.size(), 0);
            marks[0] = std::move(state);
            marked[0] = 1;
        }
    }

    size_t feedPoint(int64_t group) const {
        size_t from = size_t(group) * runPackets;
        size_t feedFrom = from - std::min(from, size_t(codec->prerollPackets));
        while (feedFrom > 0 && starts[from] - starts[feedFrom] < codec->prerollFrames) --feedFrom;
        return feedFrom;
    }

    void remember(size_t at) {
        for (auto it = std::lower_bound(feeds.begin(), feeds.end(), at); it != feeds.end() && *it == at; ++it) {
            size_t g = size_t(it - feeds.begin());
            if (!marked[g]) marked[g] = codec->save(marks[g]) ? 1 : 0;
        }
    }

    void feedOne(size_t at, std::vector<float>* out) {
        packet.resize(track->samples[at].size);
        if (packet.empty() || !file->read(*track, uint32_t(at), packet.data())) return;
        if (out) codec->feed(packet, *out);
        else codec->skim(packet);
    }

    void restart(int64_t group) {
        const size_t feedFrom = feeds[size_t(group)];
        codec->reset();
        if (marking) {
            size_t k = size_t(group);
            while (!marked[k]) --k;
            codec->load(marks[k]);
            for (size_t i = feeds[k]; i < feedFrom; ++i) {
                remember(i);
                feedOne(i, nullptr);
            }
        }
        run = group;
        next = feedFrom;
        heldFrom = starts[feedFrom] - codec->lead;
        held.clear();
        ended = false;
        lagKnown = codec->packetFrames <= 0;
        lag = 0;
    }

    void forget(int64_t before) {
        const size_t channels = size_t(codec->channels);
        int64_t frames = std::min(before - heldFrom, int64_t(held.size() / channels));
        if (frames <= 0) return;
        held.erase(held.begin(), held.begin() + long(size_t(frames) * channels));
        heldFrom += frames;
    }

    const Block* block(int64_t index) {
        ++clock;
        Block* slot = &cache[0];
        for (Block& b : cache) {
            if (b.index == index) {
                b.used = clock;
                return &b;
            }
            if (b.used < slot->used) slot = &b;
        }
        const size_t n = count();
        const size_t channels = size_t(codec->channels);
        const size_t from = size_t(index) * blockPackets;
        const size_t to = std::min(n, from + blockPackets);
        const int64_t group = int64_t(from / runPackets);
        if (run != group || heldFrom > starts[from]) restart(group);
        slot->index = -1;
        while (!ended && heldFrom + int64_t(held.size() / channels) < starts[to]) {
            if (next >= n) {
                codec->finish(held);
                ended = true;
                break;
            }
            const int64_t wanted = starts[next] - lag;
            const int64_t have = heldFrom + int64_t(held.size() / channels);
            if (lagKnown && have < wanted) held.resize(held.size() + size_t(wanted - have) * channels, 0.0f);
            const size_t before = held.size();
            if (marking) remember(next);
            feedOne(next, &held);
            if (held.size() == before) {
                held.resize(before + size_t(starts[next + 1] - starts[next]) * channels, 0.0f);
            } else if (!lagKnown) {
                lag = std::clamp<int64_t>(starts[next] + codec->packetFrames - heldFrom - int64_t(held.size() / channels), 0, codec->packetFrames);
                lagKnown = true;
            }
            ++next;
            forget(starts[from]);
        }
        const int64_t copyFrom = std::max(starts[from], heldFrom);
        const int64_t copyTo = std::min(starts[to], heldFrom + int64_t(held.size() / channels));
        slot->pcm.assign(size_t(std::max<int64_t>(0, copyTo - starts[from])) * channels, 0.0f);
        if (copyTo > copyFrom) {
            memcpy(slot->pcm.data() + size_t(copyFrom - starts[from]) * channels, held.data() + size_t(copyFrom - heldFrom) * channels,
                   size_t(copyTo - copyFrom) * channels * sizeof(float));
        }
        if (starts[from] < hidden) {
            const size_t quiet = size_t(std::min<int64_t>(hidden - starts[from], int64_t(slot->pcm.size() / channels)));
            std::fill(slot->pcm.begin(), slot->pcm.begin() + long(quiet * channels), 0.0f);
        }
        forget(starts[to]);
        if (ended) run = -1;
        slot->index = index;
        slot->used = clock;
        return slot;
    }
};

AudioDecoder::AudioDecoder() : impl(new Impl) {}
AudioDecoder::~AudioDecoder() = default;

std::unique_ptr<AudioDecoder> AudioDecoder::open(Reader& file, const Track& track, std::string* error) {
    auto fail = [&](const char* text) {
        if (error && error->empty()) *error = text;
        return nullptr;
    };
    if (error) error->clear();
    if (track.kind != Kind::Audio || track.samples.empty() || !track.timescale) return fail("no audio in this track");
    if (track.samples.size() > (1u << 30)) return fail("too many audio packets");
    std::unique_ptr<AudioDecoder> decoder(new AudioDecoder);
    Impl& a = *decoder->impl;
    a.file = &file;
    a.track = &track;
    if (track.codec == fourcc("wma1") || track.codec == fourcc("wma2")) {
        a.codec = openWmaAudio(track, error);
    } else if (track.codec == fourcc("wmap")) {
        a.codec = openWmaProAudio(track, error);
    } else if (track.codec == fourcc("wmal")) {
        a.codec = openWmaLosslessAudio(track, error);
    } else if (track.codec == fourcc("wmas")) {
        a.codec = openWmaVoiceAudio(track, error);
    } else if (waveDecoded(track.codec)) {
        a.codec = openWaveAudio(track, error);
    } else if (track.codec == fourcc("TTA1")) {
        a.codec = openTtaAudio(track, error);
    } else if (track.codec == fourcc("lpcm")) {
        if (track.channels < 1 || track.channels > 64 || track.sampleRate < 1) return fail("PCM layout not supported");
        a.codec = openPcm(track, error);
    } else if (track.codec == fourcc("Opus")) {
#ifdef ABSENT_OPUS
        a.codec = openOpusAudio(track, error);
#else
        return fail("Opus needs libopus, which was not found when building");
#endif
    } else if (track.codec == fourcc("vorb")) {
#ifdef ABSENT_VORBIS
        a.codec = openVorbisAudio(track, error);
#else
        return fail("Vorbis needs libvorbis, which was not found when building");
#endif
    } else {
#ifndef __ANDROID__
        std::vector<uint8_t> packet(track.samples[0].size);
        if (!file.read(track, 0, packet.data())) return fail("cannot read the first audio packet");
        a.codec = openSystemAudio(track, packet, error);
#else
        return fail("no decoder for this audio codec on this system");
#endif
    }
    if (!a.codec) return fail("audio codec not supported");
    if (a.codec->rate < 1 || a.codec->rate > 768000 || a.codec->channels < 1 || a.codec->channels > 64) return fail("audio layout not supported");
    a.place();
    if (a.codec->refusal) {
        if (error) *error = a.codec->refusal;
        return nullptr;
    }
    return decoder;
}

int AudioDecoder::sampleRate() const { return impl->codec->rate; }
int AudioDecoder::channels() const { return impl->codec->channels; }
uint64_t AudioDecoder::layout() const { return impl->codec->layout; }
int64_t AudioDecoder::length() const { return impl->last - impl->first; }

bool AudioDecoder::read(int64_t start, size_t frames, float* out) {
    Impl& a = *impl;
    const size_t channels = size_t(a.codec->channels);
    std::fill(out, out + frames * channels, 0.0f);
    if (start >= a.last - a.first || start < -(int64_t(1) << 62)) return true;
    uint64_t skip = start < 0 ? std::min<uint64_t>(frames, uint64_t(-start)) : 0;
    int64_t at = a.first + start + int64_t(skip);
    int64_t end = at + int64_t(std::min<uint64_t>(frames - skip, uint64_t(a.last - at)));
    float* to = out + skip * channels;
    while (at < end) {
        size_t packet = size_t(std::upper_bound(a.starts.begin(), a.starts.end(), at) - a.starts.begin()) - 1;
        if (packet >= a.count()) break;
        int64_t index = int64_t(packet / a.blockPackets);
        const Block* block = a.block(index);
        int64_t blockStart = a.starts[size_t(index) * a.blockPackets];
        int64_t blockEnd = a.starts[std::min(a.count(), (size_t(index) + 1) * a.blockPackets)];
        int64_t take = std::min(end, blockStart + int64_t(block->pcm.size() / channels)) - at;
        if (take > 0) memcpy(to, block->pcm.data() + size_t(at - blockStart) * channels, size_t(take) * channels * sizeof(float));
        int64_t next = std::min(end, blockEnd);
        to += size_t(next - at) * channels;
        at = next;
    }
    return true;
}

}

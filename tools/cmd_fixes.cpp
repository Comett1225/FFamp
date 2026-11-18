#include "../src/aac_config.h"
#include "../src/audio_codec.h"
#include "../src/capture.h"
#include "../src/frames.h"
#include "../src/nal.h"
#include "../src/own_video.h"
#include "../src/resample.h"
#include "../src/tools_common.h"
#include "absent/android.h"
#include "absent/audio.h"
#include "absent/encoders.h"
#include "absent/probe.h"
#include "cli.h"
#ifdef __APPLE__
#include <CoreVideo/CoreVideo.h>
#endif
#include <cmath>
#include <cstring>
#include <functional>
#include <numbers>
#ifndef _WIN32
#include <sys/resource.h>
#endif

namespace {

using namespace cli;

std::string folder;
std::string fixtures;

std::string scratch(const std::string& name) { return folder + "/" + name; }

bool save(const std::string& path, const std::vector<uint8_t>& data) {
    FILE* file = openFile(path.c_str(), "wb");
    if (!file) return false;
    bool ok = fwrite(data.data(), 1, data.size(), file) == data.size();
    return fclose(file) == 0 && ok;
}

std::vector<uint8_t> load(const std::string& path) {
    std::vector<uint8_t> data;
    FILE* file = openFile(path.c_str(), "rb");
    if (!file) return data;
    uint8_t block[65536];
    while (size_t got = fread(block, 1, sizeof block, file)) data.insert(data.end(), block, block + got);
    fclose(file);
    return data;
}

struct CountingReader : Reader {
    std::vector<Track> list;
    int unreadable = -1;

    const char* format() const override { return "counting"; }
    const std::vector<Track>& tracks() const override { return list; }
    bool read(const Track& track, uint32_t index, uint8_t* out) override {
        if (int(index) == unreadable) return false;
        const int16_t value = int16_t(index + 1);
        for (size_t i = 0; i + 1 < track.samples[index].size; i += 2) memcpy(out + i, &value, 2);
        return true;
    }
};

struct Span {
    int64_t pts;
    uint32_t frames;
};

void fill(CountingReader& reader, int channels, const std::vector<Span>& spans) {
    Track track;
    track.id = 1;
    track.kind = Kind::Audio;
    track.codec = fourcc("lpcm");
    track.timescale = 48000;
    track.sampleRate = 48000;
    track.channels = channels;
    track.bits = 16;
    for (const Span& span : spans) {
        Sample s;
        s.size = span.frames * uint32_t(channels) * 2;
        s.pts = s.dts = span.pts;
        s.duration = span.frames;
        s.sync = true;
        track.samples.push_back(s);
    }
    reader.list.push_back(std::move(track));
}

long valueAt(AudioDecoder& decoder, int64_t frame) {
    std::vector<float> one(size_t(decoder.channels()), 0.0f);
    decoder.read(frame, 1, one.data());
    return std::lround(one[0] * 32768.0f);
}

bool audioGap() {
    CountingReader reader;
    std::vector<Span> spans;
    for (int i = 0; i < 40; ++i) spans.push_back({int64_t(i) * 1024 + (i > 10 ? 24000 : 0), 1024});
    fill(reader, 1, spans);
    auto decoder = AudioDecoder::open(reader, reader.list[0]);
    return decoder && valueAt(*decoder, 10 * 1024 + 5) == 11 && valueAt(*decoder, 11 * 1024 + 5) == 0 && valueAt(*decoder, 35264) == 12
        && valueAt(*decoder, 35264 + 5 * 1024) == 17 && valueAt(*decoder, 35264 + 28 * 1024 + 1023) == 40;
}

bool audioLost() {
    CountingReader reader;
    std::vector<Span> spans;
    for (int i = 0; i < 80; ++i) spans.push_back({int64_t(i) * 1024, 1024});
    fill(reader, 1, spans);
    reader.unreadable = 10;
    auto decoder = AudioDecoder::open(reader, reader.list[0]);
    if (!decoder) return false;
    for (int i = 0; i < 80; ++i) if (valueAt(*decoder, int64_t(i) * 1024 + 7) != (i == 10 ? 0 : i + 1)) return false;
    return true;
}

struct LossyReader : Reader {
    Reader* inner = nullptr;
    uint32_t lost = 0;

    const char* format() const override { return inner->format(); }
    const std::vector<Track>& tracks() const override { return inner->tracks(); }
    bool read(const Track& track, uint32_t index, uint8_t* out) override { return index != lost && inner->read(track, index, out); }
};

bool sameSound(AudioDecoder& clean, int64_t cleanFrom, AudioDecoder& other, int64_t otherFrom) {
    const size_t channels = size_t(clean.channels());
    std::vector<float> a(4096 * channels), b(4096 * channels);
    if (!clean.read(cleanFrom, 4096, a.data()) || !other.read(otherFrom, 4096, b.data())) return false;
    float loudest = 0, apart = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        loudest = std::max(loudest, std::fabs(a[i]));
        apart = std::max(apart, std::fabs(a[i] - b[i]));
    }
    return loudest > 0.05f && apart < 0.02f;
}

bool codedLoss() {
    for (const char* name : {"aac_stereo_44k.m4a", "he_aac_stereo_44k.m4a", "mp3_stereo_44k.mp4", "opus_stereo_48k.mp4", "flac_16_stereo_44k.mp4"}) {
        auto reader = Reader::open(fixtures + "/audio/" + name);
        const Track* track = reader ? reader->first(Kind::Audio) : nullptr;
        if (!track) return false;
        LossyReader lossy;
        lossy.inner = reader.get();
        lossy.lost = uint32_t(track->samples.size() / 4);
        auto clean = AudioDecoder::open(*reader, *track);
        auto holed = AudioDecoder::open(lossy, *track);
        if (!clean || !holed || clean->length() != holed->length()) return false;
        if (!sameSound(*clean, clean->length() * 6 / 10, *holed, clean->length() * 6 / 10)) return false;
        if (!sameSound(*clean, clean->length() - 5000, *holed, clean->length() - 5000)) return false;
    }
    return true;
}

bool codedGap() {
    for (const char* name : {"aac_stereo_44k.m4a", "he_aac_stereo_44k.m4a", "mp3_stereo_44k.mp4", "opus_stereo_48k.mp4"}) {
        auto reader = Reader::open(fixtures + "/audio/" + name);
        const Track* track = reader ? reader->first(Kind::Audio) : nullptr;
        if (!track) return false;
        auto clean = AudioDecoder::open(*reader, *track);
        if (!clean) return false;
        Track late = *track;
        const int64_t gapTicks = int64_t(late.timescale) / 10;
        const int64_t gapFrames = int64_t(clean->sampleRate()) / 10;
        for (size_t i = late.samples.size() / 4; i < late.samples.size(); ++i) {
            late.samples[i].pts += gapTicks;
            late.samples[i].dts += gapTicks;
        }
        if (late.mediaEnd >= 0) late.mediaEnd += gapTicks;
        auto apart = AudioDecoder::open(*reader, late);
        if (!apart || apart->length() != clean->length() + gapFrames) return false;
        if (!sameSound(*clean, clean->length() * 6 / 10, *apart, clean->length() * 6 / 10 + gapFrames)) return false;
    }
    return true;
}

Track retimed(const Track& from, int64_t timescale, size_t lateFrom, int64_t lateTicks) {
    Track t = from;
    auto move = [&](int64_t ticks) { return std::llround(double(ticks) * double(timescale) / double(from.timescale)); };
    t.timescale = uint32_t(timescale);
    t.mediaStart = move(from.mediaStart);
    if (from.mediaEnd >= 0) t.mediaEnd = move(from.mediaEnd) + lateTicks;
    for (size_t i = 0; i < t.samples.size(); ++i) {
        Sample& s = t.samples[i];
        s.pts = move(s.pts) + (i >= lateFrom ? lateTicks : 0);
        s.dts = move(s.dts) + (i >= lateFrom ? lateTicks : 0);
        s.duration = uint32_t(move(s.duration));
    }
    return t;
}

bool emptyFirstGap() {
    for (const char* name : {"aac_stereo_44k.m4a", "mp3_stereo_44k.mp4"}) {
        auto reader = Reader::open(fixtures + "/audio/" + name);
        const Track* track = reader ? reader->first(Kind::Audio) : nullptr;
        if (!track) return false;
        LossyReader lossy;
        lossy.inner = reader.get();
        lossy.lost = UINT32_MAX;
        Track late = *track;
        const int64_t gapTicks = int64_t(late.timescale) * 5;
        for (size_t i = 1; i < late.samples.size(); ++i) {
            late.samples[i].pts += gapTicks;
            late.samples[i].dts += gapTicks;
        }
        if (late.mediaEnd >= 0) late.mediaEnd += gapTicks;
        auto clean = AudioDecoder::open(lossy, *track);
        auto apart = AudioDecoder::open(lossy, late);
        if (!clean || !apart) return false;
        lossy.lost = 0;
        const int64_t gapFrames = int64_t(clean->sampleRate()) * 5;
        if (apart->length() != clean->length() + gapFrames) return false;
        std::vector<float> quiet(size_t(4096 * apart->channels()));
        if (!apart->read(gapFrames * 3 / 4, 4096, quiet.data())) return false;
        for (float v : quiet) if (v != 0) return false;
        if (!sameSound(*clean, clean->length() * 6 / 10, *apart, clean->length() * 6 / 10 + gapFrames)) return false;
    }
    return true;
}

bool codedGapTs() {
    for (const char* name : {"aac_stereo_44k.m4a", "mp3_stereo_44k.mp4"}) {
        auto reader = Reader::open(fixtures + "/audio/" + name);
        const Track* track = reader ? reader->first(Kind::Audio) : nullptr;
        if (!track) return false;
        const Track even = retimed(*track, 90000, 0, 0);
        auto clean = AudioDecoder::open(*reader, even);
        if (!clean) return false;
        const int64_t gapFrames = 9216;
        const Track late = retimed(*track, 90000, track->samples.size() / 4, std::llround(double(gapFrames) * 90000.0 / clean->sampleRate()));
        auto apart = AudioDecoder::open(*reader, late);
        if (!apart || apart->length() != clean->length() + gapFrames) return false;
        if (!sameSound(*clean, clean->length() / 10, *apart, clean->length() / 10)) return false;
        if (!sameSound(*clean, clean->length() * 6 / 10, *apart, clean->length() * 6 / 10 + gapFrames)) return false;
    }
    return true;
}

int64_t peakBytes() {
#ifdef _WIN32
    return 0;
#else
    rusage use{};
    getrusage(RUSAGE_SELF, &use);
#ifdef __APPLE__
    return int64_t(use.ru_maxrss);
#else
    return int64_t(use.ru_maxrss) * 1024;
#endif
#endif
}

bool longSilence() {
    CountingReader reader;
    std::vector<Span> spans;
    const int64_t gapFrames = int64_t(48000) * 3000;
    for (int i = 0; i < 40; ++i) spans.push_back({int64_t(i) * 1024 + (i > 10 ? gapFrames : 0), 1024});
    fill(reader, 1, spans);
    auto decoder = AudioDecoder::open(reader, reader.list[0]);
    if (!decoder || decoder->length() != 40 * 1024 + gapFrames) return false;
    const int64_t before = peakBytes();
    const bool right = valueAt(*decoder, 10 * 1024 + 5) == 11 && valueAt(*decoder, 11 * 1024 + 5) == 0 && valueAt(*decoder, gapFrames / 2) == 0
        && valueAt(*decoder, 11 * 1024 + gapFrames - 1) == 0 && valueAt(*decoder, 11 * 1024 + gapFrames + 5) == 12 && valueAt(*decoder, 39 * 1024 + gapFrames + 1023) == 40
        && valueAt(*decoder, 10 * 1024 + 7) == 11;
    return right && peakBytes() - before < (int64_t(64) << 20);
}

bool aacMarks() {
#ifdef _WIN32
    _putenv_s("FFAMP_NO_SYSTEM_AUDIO", "1");
#else
    setenv("FFAMP_NO_SYSTEM_AUDIO", "1", 1);
#endif
    auto reader = Reader::open(fixtures + "/audio/he_aac_v2_stereo_44k.m4a");
    const Track* track = reader ? reader->first(Kind::Audio) : nullptr;
    if (!track || track->samples.size() < 4) return false;
    Track looped = *track;
    looped.samples.clear();
    looped.mediaStart = 0;
    looped.mediaEnd = -1;
    const int64_t step = int64_t(track->samples[1].pts - track->samples[0].pts);
    for (int64_t i = 0; int64_t(looped.samples.size()) * step < int64_t(track->timescale) * 4500; ++i) {
        Sample s = track->samples[size_t(i) % track->samples.size()];
        s.pts = s.dts = i * step;
        s.duration = uint32_t(step);
        looped.samples.push_back(s);
    }
    auto decoder = AudioDecoder::open(*reader, looped);
    if (!decoder) return false;
    const int64_t before = peakBytes();
    const size_t channels = size_t(decoder->channels());
    std::vector<float> chunk(size_t(1) << 18), probe(64 * channels);
    std::vector<std::pair<int64_t, std::vector<float>>> seen;
    const int64_t frames = int64_t(chunk.size() / channels);
    for (int64_t at = 0, n = 0; at < decoder->length(); at += frames, ++n) {
        if (!decoder->read(at, size_t(frames), chunk.data())) return false;
        if (n % 151 == 75) seen.push_back({at + 1000, std::vector<float>(chunk.begin() + ptrdiff_t(1000 * channels), chunk.begin() + ptrdiff_t(1064 * channels))});
    }
    const bool small = peakBytes() - before < (int64_t(48) << 20);
    for (auto it = seen.rbegin(); it != seen.rend(); ++it) {
        if (!decoder->read(it->first, 64, probe.data()) || probe != it->second) return false;
    }
    return small && seen.size() > 4;
}

bool audioNegative() {
    CountingReader reader;
    fill(reader, 1, {{-300000, 1024}, {0, 1024}, {1024, 1024}, {2048, 1024}});
    auto decoder = AudioDecoder::open(reader, reader.list[0]);
    return decoder && decoder->length() == 3072 && valueAt(*decoder, 0) == 2 && valueAt(*decoder, 3071) == 4;
}

bool audioHeld() {
    CountingReader reader;
    std::vector<Span> spans;
    int64_t at = 0;
    for (int i = 0; i < 2000; ++i) {
        const uint32_t frames = i == 1 ? 300000 : 970;
        spans.push_back({at, frames});
        at += frames;
    }
    fill(reader, 64, spans);
    auto decoder = AudioDecoder::open(reader, reader.list[0]);
    return decoder && valueAt(*decoder, 303890) == 6;
}

bool mixCentre() {
    Resampler mixer(48000, 3, 48000, 2);
    std::vector<float> in(300);
    for (size_t i = 0; i < 100; ++i) in[i * 3 + 2] = 1.0f;
    mixer.push(in.data(), 100);
    mixer.finish();
    std::vector<float> out(200);
    return mixer.pull(out.data(), 100) == 100 && out[100] > 0.5f && out[101] > 0.5f;
}

bool s16Gain() {
    auto surround = PcmReader::open(fixtures + "/audio/aac_51_48k.m4a", 48000, 2);
    auto stereo = PcmReader::open(fixtures + "/audio/aac_stereo_48k.m4a", 48000, 2);
    return surround && stereo && std::fabs(surround->mixPeak() - (1 + std::sqrt(2.0))) < 0.001 && stereo->mixPeak() == 1;
}

std::unique_ptr<AudioFileWriter> levels(const std::string& path, AudioFormat format, int channels, size_t frames) {
    auto writer = AudioFileWriter::open(path, format, 48000, channels, 0);
    if (!writer) return nullptr;
    std::vector<float> pcm(frames * size_t(channels));
    for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = float(i % size_t(channels) + 1) / 16.0f;
    if (frames && !writer->write(pcm.data(), frames)) return nullptr;
    return writer->finish() ? std::move(writer) : nullptr;
}

bool wavMask() {
    for (int channels : {4, 7}) {
        const std::string path = scratch("mask.wav");
        if (!levels(path, AudioFormat::Wav16, channels, 16)) return false;
        auto reader = Reader::open(path);
        if (!reader || reader->tracks().empty() || reader->tracks()[0].channelMask != (channels == 4 ? 0x107u : 0x70Fu)) return false;
    }
    return true;
}

bool flacChannels() {
    for (int channels : {3, 4, 5, 6, 7, 8}) {
        const std::string path = scratch("levels.flac");
        if (!levels(path, AudioFormat::Flac, channels, 8192)) return false;
        auto reader = Reader::open(path);
        const Track* track = reader ? reader->first(Kind::Audio) : nullptr;
        auto decoder = track ? AudioDecoder::open(*reader, *track) : nullptr;
        if (!decoder || decoder->channels() != channels) return false;
        std::vector<float> frame(size_t(channels), 0.0f);
        decoder->read(5000, 1, frame.data());
        for (int c = 0; c < channels; ++c) if (std::fabs(frame[size_t(c)] - float(c + 1) / 16.0f) > 0.001f) return false;
    }
    return true;
}

bool channelOrder() {
#ifdef __APPLE__
    const size_t sixOne[7] = {0, 1, 2, 3, 4, 5, 6};
#else
    const size_t sixOne[7] = {0, 1, 2, 3, 6, 4, 5};
#endif
    const std::pair<const char*, bool> files[] = {{"alac_61.m4a", true}, {"aac_61.m4a", false}, {"aac_71.m4a", false}};
    for (const auto& [name, exact] : files) {
        auto reader = Reader::open(fixtures + "/fixes/" + name);
        const Track* track = reader ? reader->first(Kind::Audio) : nullptr;
        auto decoder = track ? AudioDecoder::open(*reader, *track) : nullptr;
        if (!decoder) return false;
        const size_t channels = size_t(decoder->channels());
        const size_t frames = size_t(decoder->length() / 2);
        std::vector<float> pcm(frames * channels);
        if (!frames || !decoder->read(decoder->length() / 4, frames, pcm.data())) return false;
        float before = 0;
        for (size_t s = 0; s < channels; ++s) {
            const size_t c = channels == 7 ? sixOne[s] : s;
            float peak = 0;
            for (size_t i = 0; i < frames; ++i) peak = std::max(peak, std::fabs(pcm[i * channels + c]));
            if (exact ? std::fabs(peak - 0.1f * float(s + 1)) > 0.01f : s != 3 && peak < before + 0.05f) return false;
            if (s != 3) before = peak;
        }
    }
    auto stereo = PcmReader::open(fixtures + "/fixes/alac_61.m4a", 8000, 2);
    if (!stereo) return false;
    const size_t frames = size_t(stereo->length() / 2);
    std::vector<float> mixed(frames * 2);
    if (!frames || !stereo->read(stereo->length() / 4, frames, mixed.data())) return false;
    float left = 0, right = 0;
    for (size_t i = 0; i < frames; ++i) {
        left = std::max(left, std::fabs(mixed[2 * i]));
        right = std::max(right, std::fabs(mixed[2 * i + 1]));
    }
    return std::fabs(left - 0.9864f) < 0.01f && std::fabs(right - 1.1571f) < 0.01f;
}

bool encoderNames() {
#if defined(__APPLE__)
    const std::vector<std::string> wanted = {"h264_videotoolbox", "hevc_videotoolbox", "aac_at"};
#elif defined(_WIN32)
    const std::vector<std::string> wanted = {"h264_mf", "hevc_mf", "aac_mf", "aac"};
#else
    const std::vector<std::string> wanted = {"libx264", "libx265", "aac"};
#endif
    const std::vector<EncoderInfo> list = encoders();
    for (const std::string& name : wanted)
        if (std::none_of(list.begin(), list.end(), [&](const EncoderInfo& e) { return e.name == name; })) return false;
    for (const EncoderInfo& e : list) {
        if (e.kind != Kind::Video) continue;
        EncodeSpec spec;
        spec.width = 320;
        spec.height = 240;
        spec.timescale = 30;
        spec.codec = e.codec;
        spec.hevc = e.codec == fourcc("hvc1");
        if (!encoderWorks(spec)) return false;
    }
    return true;
}

bool emptyFile(const std::string& name, AudioFormat format) {
    const std::string path = scratch(name);
    if (!levels(path, format, 2, 0)) return false;
    auto reader = Reader::open(path);
    const Track* track = reader ? reader->first(Kind::Audio) : nullptr;
    auto decoder = track ? AudioDecoder::open(*reader, *track) : nullptr;
    return decoder && decoder->length() == 0;
}

bool opusEmpty() { return emptyFile("empty.opus", AudioFormat::Opus); }

bool aacEmpty() { return emptyFile("empty.m4a", AudioFormat::Aac); }

bool shortFile(const std::string& name, AudioFormat format) {
    const std::string path = scratch(name);
    if (!levels(path, format, 2, 100)) return false;
    auto reader = Reader::open(path);
    const Track* track = reader ? reader->first(Kind::Audio) : nullptr;
    auto decoder = track ? AudioDecoder::open(*reader, *track) : nullptr;
    return decoder && decoder->length() == 100;
}

bool shortFiles() { return shortFile("short.opus", AudioFormat::Opus) && shortFile("short.m4a", AudioFormat::Aac); }

bool aacRate() {
    const uint8_t reservedOutputRate[4] = {0x2B, 0x96, 0x88, 0x00};
    const uint8_t plain[2] = {0x11, 0x90};
    AacConfig config;
    AacConfig usual;
    return !parseAacConfig(View{reservedOutputRate, 4}, config) && parseAacConfig(View{plain, 2}, usual) && usual.rate == 48000;
}

void text(std::vector<uint8_t>& out, const char* value) { out.insert(out.end(), value, value + strlen(value)); }

void little(std::vector<uint8_t>& out, uint64_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) out.push_back(uint8_t(value >> (8 * i)));
}

bool rf64Size() {
    std::vector<uint8_t> f;
    text(f, "RF64");
    little(f, 0xFFFFFFFFu, 4);
    text(f, "WAVE");
    text(f, "ds64");
    little(f, 28, 4);
    little(f, 0, 8);
    little(f, 8, 8);
    little(f, 4, 8);
    little(f, 0, 4);
    text(f, "fmt ");
    little(f, 16, 4);
    little(f, 1, 2);
    little(f, 1, 2);
    little(f, 8000, 4);
    little(f, 16000, 4);
    little(f, 2, 2);
    little(f, 16, 2);
    text(f, "data");
    little(f, 0xFFFFFFFFu, 4);
    for (int i = 0; i < 8; ++i) f.push_back(uint8_t(i + 1));
    text(f, "LIST");
    little(f, 18, 4);
    text(f, "INFOINAM");
    little(f, 6, 4);
    text(f, "Hello");
    f.push_back(0);
    const std::string path = scratch("ds64.wav");
    if (!save(path, f)) return false;
    auto reader = Reader::open(path);
    if (!reader || reader->tracks().empty()) return false;
    uint64_t bytes = 0;
    for (const Sample& s : reader->tracks()[0].samples) bytes += s.size;
    auto title = reader->tags().find("title");
    return bytes == 8 && title != reader->tags().end() && title->second == "Hello";
}

bool aifcFlip() {
    Bytes f;
    f.raw("FORM", 4);
    f.u32(4 + 8 + 24 + 8 + 12);
    f.raw("AIFC", 4);
    f.raw("COMM", 4);
    f.u32(24);
    f.u16(1);
    f.u32(1);
    f.u16(8);
    f.u16(0x400B);
    f.u64(0xFA00000000000000ull);
    f.raw("fl32", 4);
    f.u16(0);
    f.raw("SSND", 4);
    f.u32(12);
    f.u32(0);
    f.u32(0);
    f.u32(0x3F800000);
    const std::string path = scratch("flip.aifc");
    if (!save(path, f.d)) return false;
    auto reader = Reader::open(path);
    if (!reader || reader->tracks().empty() || reader->tracks()[0].samples.empty() || reader->tracks()[0].samples[0].size != 4) return false;
    uint8_t packet[4];
    return reader->read(reader->tracks()[0], 0, packet) && packet[0] == 0x3F && packet[1] == 0x80 && packet[2] == 0 && packet[3] == 0;
}

std::vector<uint8_t> fragmented(uint64_t decodeTime) {
    Bytes b;
    size_t ftyp = b.open(fourcc("ftyp"));
    b.u32(fourcc("isom"));
    b.u32(0);
    b.u32(fourcc("isom"));
    b.close(ftyp);
    size_t moov = b.open(fourcc("moov"));
    size_t mvhd = b.open(fourcc("mvhd"), 0, 0);
    b.zero(8);
    b.u32(1000);
    b.u32(0);
    b.u32(0x00010000);
    b.u16(0x0100);
    b.zero(10 + 36 + 24);
    b.u32(2);
    b.close(mvhd);
    size_t trak = b.open(fourcc("trak"));
    size_t tkhd = b.open(fourcc("tkhd"), 0, 3);
    b.zero(8);
    b.u32(1);
    b.zero(4 + 4 + 8 + 8 + 36 + 8);
    b.close(tkhd);
    size_t mdia = b.open(fourcc("mdia"));
    size_t mdhd = b.open(fourcc("mdhd"), 0, 0);
    b.zero(8);
    b.u32(48000);
    b.u32(0);
    b.u16(0x55C4);
    b.u16(0);
    b.close(mdhd);
    size_t hdlr = b.open(fourcc("hdlr"), 0, 0);
    b.u32(0);
    b.u32(fourcc("soun"));
    b.zero(13);
    b.close(hdlr);
    size_t minf = b.open(fourcc("minf"));
    size_t stbl = b.open(fourcc("stbl"));
    size_t stsd = b.open(fourcc("stsd"), 0, 0);
    b.u32(1);
    size_t entry = b.open(fourcc("Opus"));
    b.zero(6);
    b.u16(1);
    b.zero(8);
    b.u16(1);
    b.u16(16);
    b.zero(4);
    b.u32(48000u << 16);
    size_t dops = b.open(fourcc("dOps"));
    b.u8(0);
    b.u8(1);
    b.u16(0);
    b.u32(48000);
    b.u16(0);
    b.u8(0);
    b.close(dops);
    b.close(entry);
    b.close(stsd);
    for (uint32_t name : {fourcc("stts"), fourcc("stsc"), fourcc("stco")}) {
        size_t table = b.open(name, 0, 0);
        b.u32(0);
        b.close(table);
    }
    size_t stsz = b.open(fourcc("stsz"), 0, 0);
    b.u32(0);
    b.u32(0);
    b.close(stsz);
    b.close(stbl);
    b.close(minf);
    b.close(mdia);
    b.close(trak);
    size_t mvex = b.open(fourcc("mvex"));
    size_t trex = b.open(fourcc("trex"), 0, 0);
    b.u32(1);
    b.u32(1);
    b.zero(12);
    b.close(trex);
    b.close(mvex);
    b.close(moov);
    const size_t moofAt = b.d.size();
    size_t moof = b.open(fourcc("moof"));
    size_t mfhd = b.open(fourcc("mfhd"), 0, 0);
    b.u32(1);
    b.close(mfhd);
    size_t traf = b.open(fourcc("traf"));
    size_t tfhd = b.open(fourcc("tfhd"), 0, 0x020000);
    b.u32(1);
    b.close(tfhd);
    size_t tfdt = b.open(fourcc("tfdt"), 1, 0);
    b.u64(decodeTime);
    b.close(tfdt);
    size_t first = b.open(fourcc("trun"), 0, 0x301);
    b.u32(2);
    const size_t offsetAt = b.d.size();
    b.u32(0);
    for (int i = 0; i < 2; ++i) {
        b.u32(960);
        b.u32(4);
    }
    b.close(first);
    size_t second = b.open(fourcc("trun"), 0, 0x300);
    b.u32(2);
    for (int i = 0; i < 2; ++i) {
        b.u32(960);
        b.u32(4);
    }
    b.close(second);
    b.close(traf);
    b.close(moof);
    const uint32_t payload = uint32_t(b.d.size() + 8 - moofAt);
    for (int i = 0; i < 4; ++i) b.d[offsetAt + size_t(i)] = uint8_t(payload >> (24 - 8 * i));
    size_t mdat = b.open(fourcc("mdat"));
    b.raw("AAAABBBBCCCCDDDD", 16);
    b.close(mdat);
    return b.d;
}

bool trunOffset() {
    const std::string path = scratch("two_runs.mp4");
    if (!save(path, fragmented(0))) return false;
    auto reader = Reader::open(path);
    if (!reader || reader->tracks().empty() || reader->tracks()[0].samples.size() != 4) return false;
    std::string all(16, ' ');
    for (uint32_t i = 0; i < 4; ++i) if (!reader->read(reader->tracks()[0], i, reinterpret_cast<uint8_t*>(&all[i * 4]))) return false;
    return all == "AAAABBBBCCCCDDDD";
}

bool decodeTimeLimit() {
    const std::string path = scratch("late_fragment.mp4");
    if (!save(path, fragmented(0x7FFFFFFFFFFFFFFFull))) return false;
    auto reader = Reader::open(path);
    if (!reader || reader->tracks().empty() || reader->tracks()[0].samples.size() != 4) return false;
    for (const Sample& s : reader->tracks()[0].samples) if (s.dts < 0 || s.dts > INT64_MAX / 2 + 4000) return false;
    return true;
}

TrackSpec soundSpec(const char (&codec)[5], uint32_t timescale, int rate, int channels) {
    TrackSpec spec;
    spec.kind = Kind::Audio;
    spec.codec = fourcc(codec);
    spec.timescale = timescale;
    spec.sampleRate = rate;
    spec.channels = channels;
    spec.bits = 16;
    return spec;
}

bool flacRate() {
    const std::string path = scratch("rate.m4a");
    auto writer = Mp4Writer::open(path);
    if (!writer) return false;
    TrackSpec spec = soundSpec("fLaC", 96000, 96000, 2);
    spec.configType = fourcc("fLaC");
    spec.config = {0x10, 0x00, 0x10, 0x00, 0, 0, 0, 0, 0, 0, 0x17, 0x70, 0x02, 0xF0};
    spec.config.resize(34, 0);
    int track = writer->add(spec);
    const uint8_t packet[10] = {0xFF, 0xF8, 1, 2, 3, 4, 5, 6, 7, 8};
    if (!writer->write(track, packet, sizeof packet, 0, 0, 4096, true) || !writer->finish()) return false;
    auto reader = Reader::open(path);
    return reader && !reader->tracks().empty() && reader->tracks()[0].sampleRate == 96000 && reader->tracks()[0].channels == 2 && reader->tracks()[0].bits == 16;
}

void element(std::vector<uint8_t>& out, uint32_t id, const std::vector<uint8_t>& body) {
    bool started = false;
    for (int shift = 24; shift >= 0; shift -= 8) {
        uint8_t byte = uint8_t(id >> shift);
        if (byte || started) out.push_back(byte);
        started = started || byte;
    }
    out.push_back(0x01);
    for (int shift = 48; shift >= 0; shift -= 8) out.push_back(uint8_t(uint64_t(body.size()) >> shift));
    out.insert(out.end(), body.begin(), body.end());
}

std::vector<uint8_t> number(uint64_t value, int bytes) {
    std::vector<uint8_t> out;
    for (int i = bytes - 1; i >= 0; --i) out.push_back(uint8_t(value >> (8 * i)));
    return out;
}

std::vector<uint8_t> matroska(const char* codec, const std::vector<uint8_t>& secret, const std::vector<uint8_t>& block) {
    std::vector<uint8_t> file, head, entry, sound, tracks, cluster, segment;
    const char* kind = "matroska";
    element(head, 0x4282, std::vector<uint8_t>(kind, kind + strlen(kind)));
    element(file, 0x1A45DFA3, head);
    element(entry, 0xD7, number(1, 1));
    element(entry, 0x73C5, number(1, 1));
    element(entry, 0x83, number(2, 1));
    element(entry, 0x86, std::vector<uint8_t>(codec, codec + strlen(codec)));
    element(sound, 0xB5, number(0x473B8000, 4));
    element(sound, 0x9F, number(1, 1));
    element(sound, 0x6264, number(16, 1));
    element(entry, 0xE1, sound);
    if (!secret.empty()) element(entry, 0x63A2, secret);
    std::vector<uint8_t> list;
    element(list, 0xAE, entry);
    element(segment, 0x1654AE6B, list);
    element(cluster, 0xE7, number(0, 1));
    element(cluster, 0xEC, std::vector<uint8_t>(1200, 0));
    element(cluster, 0xA3, block);
    element(segment, 0x1F43B675, cluster);
    element(file, 0x18538067, segment);
    return file;
}

bool laceBounds() {
    std::vector<uint8_t> block = {0x81, 0, 0, 0x86, 65};
    const int64_t step = int64_t(1) << 54;
    int64_t before = 0;
    for (int i = 0; i < 65; ++i) {
        int64_t size = i == 64 ? 1000 : step * std::min(i + 1, 64 - i) - (int64_t(1) << 53) - (i == 31 ? 1000 : 0);
        uint64_t coded = i == 0 ? uint64_t(size) : uint64_t(size - before + (int64_t(1) << 55) - 1);
        block.push_back(0x01);
        for (int shift = 48; shift >= 0; shift -= 8) block.push_back(uint8_t(coded >> shift));
        before = size;
    }
    block.insert(block.end(), 2000, 0x55);
    std::vector<uint8_t> file = matroska("A_PCM/INT/LIT", {}, block);
    const uint64_t dataStart = file.size() - 2000;
    const std::string path = scratch("lace.mka");
    if (!save(path, file)) return false;
    auto reader = Reader::open(path);
    if (!reader) return true;
    for (const Track& t : reader->tracks()) for (const Sample& s : t.samples) if (s.offset < dataStart || s.offset + s.size > file.size()) return false;
    return true;
}

bool vorbisModes() {
    std::vector<uint8_t> block = {0x81, 0, 0, 0x80, 0x02, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    const std::string path = scratch("modes.mka");
    if (!save(path, matroska("A_VORBIS", {0x02, 0x01, 0x01, 0xAA, 0xBB, 0xCC}, block))) return false;
    auto reader = Reader::open(path);
    if (!reader) return true;
    for (const Track& t : reader->tracks()) if (t.codec == fourcc("vorb") && !t.samples.empty()) return false;
    return true;
}

struct BitWriter {
    std::vector<uint8_t> d;
    int used = 0;
    void bit(uint32_t value) {
        if (used % 8 == 0) d.push_back(0);
        d.back() |= uint8_t((value & 1) << (7 - used % 8));
        ++used;
    }
    void bits(uint32_t value, int count) { for (int i = count - 1; i >= 0; --i) bit(value >> i); }
    void ue(uint32_t value) {
        const uint64_t code = uint64_t(value) + 1;
        int length = 0;
        while ((code >> length) > 1) ++length;
        for (int i = 0; i < length; ++i) bit(0);
        for (int i = length; i >= 0; --i) bit(uint32_t(code >> i));
    }
};

bool spsLimits() {
    BitWriter w;
    w.bits(0x67, 8);
    w.bits(66, 8);
    w.bits(0, 8);
    w.bits(30, 8);
    w.ue(0);
    w.ue(0xFFFFFFFDu);
    w.ue(0);
    w.ue(0);
    w.ue(0);
    w.bit(0);
    w.ue(19);
    w.ue(14);
    w.bit(1);
    w.bit(1);
    w.bit(0);
    w.bit(0);
    w.bit(1);
    absent::VideoInfo info = absent::parseAvcSps(w.d.data(), w.d.size());
    return !info.valid || info.frameNumBits >= 4;
}

bool tagFlags() {
    std::vector<uint8_t> f = {'I', 'D', '3', 3, 0, 0, 0, 0, 0, 16, 'T', 'I', 'T', '2', 0, 0, 0, 6, 0, 0x40, 0, 'S', 'e', 'c', 'r', 't'};
    std::vector<uint8_t> sound = load(fixtures + "/readers/plain.mp3");
    if (sound.empty()) return false;
    f.insert(f.end(), sound.begin(), sound.end());
    const std::string path = scratch("flagged.mp3");
    if (!save(path, f)) return false;
    auto reader = Reader::open(path);
    return reader && !reader->tracks().empty() && !reader->tracks()[0].samples.empty() && !reader->tags().count("title");
}

bool pcmChecked() {
    const std::string path = scratch("checked.mp4");
    auto writer = Mp4Writer::open(path);
    if (!writer) return false;
    int track = writer->add(soundSpec("lpcm", 48000, 48000, 2));
    const uint8_t odd[5] = {'x', 'x', 'x', 'x', 'x'};
    if (writer->write(track, odd, 5, 0, 0, 0, true)) return false;
    if (!writer->write(track, reinterpret_cast<const uint8_t*>("ABCDEFGH"), 8, 0, 0, 2, true) || !writer->finish()) return false;
    auto reader = Reader::open(path);
    if (!reader || reader->tracks().empty()) return false;
    const Track& t = reader->tracks()[0];
    std::string all;
    for (uint32_t i = 0; i < t.samples.size(); ++i) {
        std::string piece(t.samples[i].size, ' ');
        if (!reader->read(t, i, reinterpret_cast<uint8_t*>(piece.data()))) return false;
        all += piece;
    }
    return all == "ABCDEFGH";
}

bool afterFinish() {
    for (const char* name : {"closed.mp4", "closed.mka"}) {
        auto writer = Writer::open(scratch(name));
        if (!writer) return false;
        int track = writer->add(soundSpec("lpcm", 48000, 48000, 1));
        const uint8_t frames[4] = {1, 2, 3, 4};
        if (!writer->write(track, frames, 4, 0, 0, 2, true) || !writer->finish()) return false;
        if (writer->write(track, frames, 4, 2, 2, 2, true)) return false;
    }
    return true;
}

bool pcmTrim() {
    const std::string path = scratch("trimmed.mp4");
    auto writer = Mp4Writer::open(path);
    if (!writer) return false;
    int track = writer->add(soundSpec("lpcm", 1000, 48000, 1));
    std::vector<uint8_t> second(96000, 0);
    if (!writer->write(track, second.data(), second.size(), 0, 0, 1000, true) || !writer->write(track, second.data(), second.size(), 1000, 1000, 1000, true)) return false;
    writer->trim(track, 100, 1000);
    if (!writer->finish()) return false;
    auto reader = Reader::open(path);
    return reader && !reader->tracks().empty() && reader->tracks()[0].mediaStart == 4800 && reader->tracks()[0].mediaEnd == 4800 + 48000;
}

bool longFile() {
    const std::string path = scratch("long.m4a");
    auto writer = Mp4Writer::open(path);
    if (!writer) return false;
    const int64_t length = (int64_t(1) << 32) + 90000;
    TrackSpec spec = soundSpec("mp4a", 90000, 48000, 2);
    spec.configType = fourcc("ASC ");
    spec.config = {0x11, 0x90};
    spec.length = length;
    int track = writer->add(spec);
    const uint8_t packet[4] = {1, 2, 3, 4};
    for (int i = 0; i < 3; ++i) if (!writer->write(track, packet, 4, int64_t(i) << 31, int64_t(i) << 31, i == 2 ? 90000 : 0x80000000u, true)) return false;
    if (!writer->finish()) return false;
    auto reader = Reader::open(path);
    if (!reader || reader->tracks().empty()) return false;
    const Track& t = reader->tracks()[0];
    return t.samples.size() == 3 && t.samples[2].pts == int64_t(1) << 32 && t.mediaStart == 0 && t.mediaEnd == length;
}

bool streamFlags() {
    const std::string path = scratch("flags.mka");
    auto writer = Writer::open(path);
    if (!writer) return false;
    TrackSpec spec = soundSpec("mp4a", 48000, 48000, 2);
    spec.configType = fourcc("esds");
    Bytes config;
    config.u32(0);
    config.u8(0x03);
    config.u8(27);
    config.u16(0);
    config.u8(0x80);
    config.u16(10);
    config.u8(0x04);
    config.u8(17);
    config.u8(0x40);
    config.u8(0x15);
    config.zero(11);
    config.u8(0x05);
    config.u8(2);
    config.u8(0x11);
    config.u8(0x90);
    config.u8(0x06);
    config.u8(1);
    config.u8(2);
    spec.config = config.d;
    int track = writer->add(spec);
    const uint8_t packet[4] = {1, 2, 3, 4};
    if (!writer->write(track, packet, 4, 0, 0, 1024, true) || !writer->finish()) return false;
    auto reader = Reader::open(path);
    return reader && !reader->tracks().empty() && reader->tracks()[0].config == std::vector<uint8_t>({0x11, 0x90});
}

bool lateTrim() {
    const std::string path = scratch("late.mka");
    auto writer = Writer::open(path);
    if (!writer) return false;
    int track = writer->add(soundSpec("lpcm", 48000, 48000, 1));
    std::vector<uint8_t> packet(2000, 0);
    for (int i = 0; i < 3; ++i) if (!writer->write(track, packet.data(), packet.size(), i * 1000, i * 1000, 1000, true)) return false;
    writer->trim(track, 0, 2500);
    if (!writer->finish()) return false;
    auto reader = Reader::open(path);
    return reader && !reader->tracks().empty() && reader->tracks()[0].samples.size() == 3 && reader->tracks()[0].mediaEnd == 2500;
}

bool emptySample() {
    auto source = Reader::open(fixtures + "/video/h264_270p.mp4");
    const Track* from = source ? source->first(Kind::Video) : nullptr;
    if (!from || from->samples.size() < 12) return false;
    const std::string path = scratch("hole.mp4");
    auto writer = Mp4Writer::open(path);
    if (!writer) return false;
    int track = writer->add(specOf(*from));
    int64_t latest = 0;
    for (const Sample& s : from->samples) latest = std::max(latest, s.pts + int64_t(s.duration));
    std::vector<uint8_t> data;
    for (uint32_t i = 0; i < from->samples.size(); ++i) {
        const Sample& s = from->samples[i];
        if (i == 3 && !writer->write(track, nullptr, 0, latest, s.dts, 0, false)) return false;
        data.resize(s.size);
        if (!source->read(*from, i, data.data()) || !writer->write(track, data.data(), s.size, s.pts, s.dts, s.duration, s.sync)) return false;
    }
    if (!writer->finish()) return false;
    auto reader = Reader::open(path);
    const Track* video = reader ? reader->first(Kind::Video) : nullptr;
    if (!video || video->samples.size() != from->samples.size() + 1 || video->samples[3].size != 0) return false;
    auto decoder = VideoDecoder::open(*reader, *video);
    if (!decoder) return false;
    for (uint32_t i = 0; i < 10; ++i) if (!decoder->frame(i)) return false;
    return true;
}

bool wideClock() {
    auto source = Reader::open(fixtures + "/video/h264_270p.mp4");
    const Track* from = source ? source->first(Kind::Video) : nullptr;
    if (!from) return false;
    Track copy = *from;
    copy.timescale = 0x80000000u;
    return VideoDecoder::open(*source, *from) && !VideoDecoder::open(*source, copy);
}

void chunk(std::vector<uint8_t>& out, const char* id, const std::vector<uint8_t>& body) {
    text(out, id);
    little(out, body.size(), 4);
    out.insert(out.end(), body.begin(), body.end());
    if (body.size() & 1) out.push_back(0);
}

std::vector<uint8_t> listed(const char* kind, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> inner, out;
    text(inner, kind);
    inner.insert(inner.end(), body.begin(), body.end());
    chunk(out, "LIST", inner);
    return out;
}

std::vector<uint8_t> repeatedSound(uint16_t tag, const std::vector<uint8_t>& payload, int repeats) {
    std::vector<uint8_t> header, format, stream, hdrl, movi, index, body, file;
    text(header, "auds");
    little(header, 0, 4);
    little(header, 0, 4);
    little(header, 0, 8);
    little(header, 1, 4);
    little(header, 8000, 4);
    header.resize(header.size() + 28, 0);
    little(format, tag, 2);
    little(format, 1, 2);
    little(format, 8000, 4);
    little(format, 8000, 4);
    little(format, 1, 2);
    little(format, 8, 2);
    chunk(stream, "strh", header);
    chunk(stream, "strf", format);
    chunk(hdrl, "avih", std::vector<uint8_t>(56, 0));
    std::vector<uint8_t> list = listed("strl", stream);
    hdrl.insert(hdrl.end(), list.begin(), list.end());
    chunk(movi, "00wb", payload);
    for (int i = 0; i < repeats; ++i) {
        text(index, "00wb");
        little(index, 0x10, 4);
        little(index, 4, 4);
        little(index, payload.size(), 4);
    }
    text(body, "AVI ");
    for (const std::vector<uint8_t>& part : {listed("hdrl", hdrl), listed("movi", movi)}) body.insert(body.end(), part.begin(), part.end());
    chunk(body, "idx1", index);
    chunk(file, "RIFF", body);
    return file;
}

bool aviRepeatedIndex() {
    auto mp3 = Reader::open(fixtures + "/readers/cbr.mp3");
    if (!mp3 || mp3->tracks().empty()) return false;
    const Track& t = mp3->tracks()[0];
    std::vector<uint8_t> frames;
    for (uint32_t i = 0; i < t.samples.size() && frames.size() < 8192; ++i) {
        std::vector<uint8_t> one(t.samples[i].size);
        if (!mp3->read(t, i, one.data())) return false;
        frames.insert(frames.end(), one.begin(), one.end());
    }
    for (const auto& [tag, payload] : {std::pair<uint16_t, std::vector<uint8_t>>{1, std::vector<uint8_t>(8192, 128)}, {0x55, frames}}) {
        const std::vector<uint8_t> file = repeatedSound(tag, payload, 2000);
        const std::string path = scratch("repeated.avi");
        if (!save(path, file)) return false;
        auto reader = Reader::open(path);
        if (!reader || reader->tracks().empty()) return false;
        uint64_t bytes = 0;
        for (const Sample& s : reader->tracks()[0].samples) bytes += s.size;
        if (!bytes || bytes > file.size()) return false;
    }
    return true;
}

bool trackSeconds() {
    const std::string source = fixtures + "/asf/h264_mp3.asf";
    std::vector<uint8_t> file = load(source);
    static const uint8_t properties[16] = {0xA1, 0xDC, 0xAB, 0x8C, 0x47, 0xA9, 0xCF, 0x11, 0x8E, 0xE4, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
    auto at = std::search(file.begin(), file.end(), properties, properties + 16);
    if (file.end() - at < 88) return false;
    for (int i = 0; i < 8; ++i) at[80 + i] = uint8_t(uint64_t(100000) >> (8 * i));
    const std::string path = scratch("late.asf");
    auto plain = Reader::open(source);
    if (!plain || !save(path, file)) return false;
    auto late = Reader::open(path);
    if (!late || late->tracks().size() != plain->tracks().size()) return false;
    for (size_t i = 0; i < plain->tracks().size(); ++i)
        if (late->tracks()[i].mediaStart >= 0 || std::fabs(late->tracks()[i].seconds() - plain->tracks()[i].seconds()) > 0.002) return false;
    return true;
}

bool firstSync() {
    std::vector<uint8_t> file = load(fixtures + "/probe/h264.mp4");
    static const char name[] = "stss";
    auto at = std::search(file.begin(), file.end(), name, name + 4);
    if (file.end() - at < 20 || at[11] != 2 || at[15] != 1 || at[19] != 31) return false;
    at[11] = 1;
    at[15] = 31;
    const std::string path = scratch("late_key.mp4");
    if (!save(path, file)) return false;
    auto reader = Reader::open(path);
    const Track* video = reader ? reader->first(Kind::Video) : nullptr;
    if (!video || video->syncs.size() != 1 || video->syncs[0] != 30) return false;
    auto decoder = VideoDecoder::open(*reader, *video);
    return decoder && decoder->frame(0) && decoder->frame(5) && decoder->frame(40);
}

#ifdef __APPLE__
bool scalerLabels() {
    CVPixelBufferRef source = nullptr;
    if (CVPixelBufferCreate(nullptr, 64, 64, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, nullptr, &source) != kCVReturnSuccess) return false;
    CVBufferSetAttachment(source, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_SMPTE_C, kCVAttachmentMode_ShouldPropagate);
    Frame frame;
    frame.native = std::shared_ptr<void>(source, [](void* p) { CFRelease(p); });
    frame.width = frame.height = 64;
    Scaler scaler(64, 64);
    void* out = scaler.fit(frame);
    CFTypeRef kept = CVBufferCopyAttachment(source, kCVImageBufferColorPrimariesKey, nullptr);
    const bool same = kept && CFEqual(kept, kCVImageBufferColorPrimaries_SMPTE_C);
    if (kept) CFRelease(kept);
    releaseBuffer(out);
    return out && out != source && same;
}
#endif

bool finishTwice() {
    CaptureSpec spec;
    spec.video = kTestDevice;
    spec.record = scratch("never.mp4");
    CaptureRecorder recorder(spec, false);
    return !recorder.finish(nullptr) && !recorder.finish(nullptr) && recorder.failed(nullptr);
}

bool manyTracks() {
    const std::string path = scratch("many.mka");
    auto writer = Writer::open(path);
    if (!writer) return false;
    std::vector<int> ids;
    for (int i = 0; i < 130; ++i) ids.push_back(writer->add(soundSpec("lpcm", 48000, 48000, 1)));
    const uint8_t frames[4] = {1, 2, 3, 4};
    for (int id : ids) if (!writer->write(id, frames, 4, 0, 0, 2, true)) return false;
    if (!writer->finish()) return false;
    auto reader = Reader::open(path);
    if (!reader || reader->tracks().size() != ids.size()) return false;
    for (const Track& t : reader->tracks()) if (t.samples.size() != 1) return false;
    return true;
}

bool longGap() {
    auto source = Reader::open(fixtures + "/video/h264_270p.mp4");
    const Track* from = source ? source->first(Kind::Video) : nullptr;
    if (!from || from->samples.size() < 3) return false;
    const std::string path = scratch("gap.mp4");
    auto writer = Mp4Writer::open(path);
    if (!writer) return false;
    TrackSpec spec = specOf(*from);
    spec.timescale = 1000000000;
    int track = writer->add(spec);
    const int64_t times[3] = {0, 5000000000, 5040000000};
    std::vector<uint8_t> data;
    for (uint32_t i = 0; i < 3; ++i) {
        data.resize(from->samples[i].size);
        if (!source->read(*from, i, data.data()) || !writer->write(track, data.data(), data.size(), times[i], times[i], 40000000, from->samples[i].sync)) return false;
    }
    if (!writer->finish()) return false;
    auto reader = Reader::open(path);
    const Track* video = reader ? reader->first(Kind::Video) : nullptr;
    if (!video || video->samples.size() != 3) return false;
    for (uint32_t i = 0; i < 3; ++i) if (std::fabs(double(video->samples[i].dts) / video->timescale - double(times[i]) / 1e9) > 1e-6) return false;
    return true;
}

bool wmaBlockAlign() {
    std::vector<uint8_t> format;
    little(format, 0x161, 2);
    little(format, 1, 2);
    little(format, 48000, 4);
    little(format, 4000, 4);
    little(format, 1, 2);
    little(format, 16, 2);
    little(format, 10, 2);
    little(format, 0, 4);
    little(format, 2, 2);
    little(format, 0, 4);
    std::vector<uint8_t> block = {0x81, 0, 0, 0x80};
    block.resize(4 + 8192, 0xFF);
    const std::string path = scratch("tiny_align.mka");
    if (!save(path, matroska("A_MS/ACM", format, block))) return false;
    auto reader = Reader::open(path);
    if (!reader || reader->tracks().empty() || reader->tracks()[0].codec != fourcc("wma2")) return false;
    auto decoder = AudioDecoder::open(*reader, reader->tracks()[0]);
    return decoder && decoder->length() > 0 && decoder->length() < 1000000;
}

struct Placed {
    uint64_t offset;
    double time;
    bool video;
};

double leadInFile(const std::string& path) {
    auto reader = Reader::open(path);
    if (!reader) return 1e9;
    std::vector<Placed> placed;
    for (const Track& t : reader->tracks())
        for (const Sample& s : t.samples) placed.push_back({s.offset, double(s.dts - t.mediaStart + t.delay) / t.timescale, t.kind == Kind::Video});
    std::sort(placed.begin(), placed.end(), [](const Placed& a, const Placed& b) { return a.offset < b.offset; });
    double video = -1e9, lead = 0;
    for (const Placed& p : placed) {
        if (p.video) video = std::max(video, p.time);
        else if (video > -1e9) lead = std::max(lead, p.time - video);
    }
    return lead;
}

bool interleave() {
    auto source = Reader::open(fixtures + "/tools/clip.mp4");
    const Track* video = source ? source->first(Kind::Video) : nullptr;
    const Track* audio = source ? source->first(Kind::Audio) : nullptr;
    if (!video || !audio) return false;
    for (const char* name : {"early_sound.mkv", "early_sound.mp4"}) {
        const std::string path = scratch(name);
        auto writer = Writer::open(path);
        if (!writer) return false;
        const int pictures = writer->add(specOf(*video)), sound = writer->add(specOf(*audio));
        std::vector<uint8_t> data;
        auto put = [&](const Track& t, int out, uint32_t i) {
            const Sample& s = t.samples[i];
            data.resize(s.size);
            return source->read(t, i, data.data()) && writer->write(out, data.data(), data.size(), s.pts, s.dts, s.duration, s.sync);
        };
        uint32_t next = 0;
        for (uint32_t i = 0; i < video->samples.size(); ++i) {
            const double ahead = double(video->samples[i].dts) / video->timescale + 2;
            while (next < audio->samples.size() && double(audio->samples[next].dts) / audio->timescale < ahead) if (!put(*audio, sound, next++)) return false;
            if (!put(*video, pictures, i)) return false;
        }
        while (next < audio->samples.size()) if (!put(*audio, sound, next++)) return false;
        if (!writer->finish() || leadInFile(path) > 0.5) return false;
    }
    return true;
}

bool colourUnknown() {
    for (const char* name : {"untagged.mp4", "untagged.mov", "untagged.mkv"}) {
        if (!rewrap(fixtures + "/probe/h264.mp4", scratch(name), nullptr)) return false;
        auto reader = Reader::open(scratch(name));
        const Track* video = reader ? reader->first(Kind::Video) : nullptr;
        if (!video || video->color.known) return false;
    }
    for (const char* name : {"tagged.mp4", "tagged.mkv"}) {
        if (!rewrap(fixtures + "/probe/colr.mp4", scratch(name), nullptr)) return false;
        auto reader = Reader::open(scratch(name));
        const Track* video = reader ? reader->first(Kind::Video) : nullptr;
        if (!video || !video->color.known || video->color.matrix != 1 || video->color.primaries != 1 || video->color.transfer != 1) return false;
    }
    auto source = Reader::open(fixtures + "/probe/h264.mp4");
    const Track* from = source ? source->first(Kind::Video) : nullptr;
    if (!from) return false;
    TrackSpec encoded;
    encoded.codec = from->codec;
    encoded.timescale = from->timescale;
    encoded.width = from->width;
    encoded.height = from->height;
    encoded.configType = from->configType;
    encoded.config = from->config;
    for (const char* name : {"labelled.mp4", "labelled.mkv"}) {
        auto writer = Writer::open(scratch(name));
        if (!writer) return false;
        const int track = writer->add(encoded);
        std::vector<uint8_t> data(from->samples[0].size);
        if (!source->read(*from, 0, data.data()) || !writer->write(track, data.data(), data.size(), 0, 0, from->samples[0].duration, true) || !writer->finish()) return false;
        auto reader = Reader::open(scratch(name));
        const Track* video = reader ? reader->first(Kind::Video) : nullptr;
        if (!video || !video->color.known || video->color.matrix != 1 || video->color.full) return false;
    }
    return true;
}

bool samePackets(Reader& a, const Track& x, Reader& b, const Track& y) {
    if (x.samples.size() != y.samples.size()) return false;
    std::vector<uint8_t> one, two;
    for (uint32_t i = 0; i < x.samples.size(); ++i) {
        one.resize(x.samples[i].size);
        two.resize(y.samples[i].size);
        if (one.size() != two.size() || !a.read(x, i, one.data()) || !b.read(y, i, two.data()) || one != two) return false;
    }
    return true;
}

bool samePayload(Reader& a, const Track& x, Reader& b, const Track& y) {
    std::vector<uint8_t> one, two, piece;
    for (uint32_t i = 0; i < x.samples.size(); ++i) {
        piece.resize(x.samples[i].size);
        if (!a.read(x, i, piece.data())) return false;
        one.insert(one.end(), piece.begin(), piece.end());
    }
    for (uint32_t i = 0; i < y.samples.size(); ++i) {
        piece.resize(y.samples[i].size);
        if (!b.read(y, i, piece.data())) return false;
        two.insert(two.end(), piece.begin(), piece.end());
    }
    return !one.empty() && one == two;
}

std::vector<uint8_t> boxBody(const std::string& path, const char (&type)[5]) {
    const std::vector<uint8_t> file = load(path);
    auto at = std::search(file.begin(), file.end(), type, type + 4);
    if (at - file.begin() < 4 || file.end() - at < 4) return {};
    const size_t size = (size_t(at[-4]) << 24) | (size_t(at[-3]) << 16) | (size_t(at[-2]) << 8) | at[-1];
    if (size < 8 || size_t(file.end() - at) < size - 4) return {};
    return {at + 4, at + (size - 4)};
}

bool copyCodecs() {
    const std::pair<const char*, std::vector<const char*>> cases[] = {
        {"formats/avi_mpeg4_mp3.avi", {"mp4", "mov", "mkv"}}, {"formats/avi_mjpeg_pcm.avi", {"mp4", "mov", "mkv"}}, {"probe/h264_mp3.ts", {"mp4", "mov", "mkv"}},
        {"probe/h264_ac3.ts", {"mp4", "mov", "mkv"}}, {"probe/h264_eac3.ts", {"mp4", "mov", "mkv"}}, {"readers/vp8_vorbis.webm", {"mp4", "mkv"}},
        {"formats/eac3_51.mp4", {"mp4", "mkv"}}, {"formats/ac3_stereo.mp4", {"mp4", "mkv"}},
    };
    int number = 0;
    for (const auto& [name, kinds] : cases) {
        auto source = Reader::open(fixtures + "/" + name);
        if (!source) return false;
        ++number;
        for (const char* kind : kinds) {
            const std::string path = scratch("copy_" + std::to_string(number) + "." + kind);
            if (!rewrap(fixtures + "/" + name, path, nullptr)) return false;
            auto copy = Reader::open(path);
            if (!copy) return false;
            for (Kind k : {Kind::Video, Kind::Audio}) {
                const Track* x = source->first(k);
                const Track* y = copy->first(k);
                if (!x) continue;
                if (!y || x->codec != y->codec || !(x->codec == fourcc("lpcm") ? samePayload(*source, *x, *copy, *y) : samePackets(*source, *x, *copy, *y))) return false;
            }
        }
    }
    for (const char* box : {"dec3", "dac3"}) {
        const std::string name = box[2] == 'c' ? "formats/eac3_51.mp4" : "formats/ac3_stereo.mp4";
        const std::string path = scratch(std::string("again_") + box + ".mp4");
        if (!rewrap(fixtures + "/" + name, path, nullptr)) return false;
        const auto theirs = box[2] == 'c' ? boxBody(fixtures + "/" + name, "dec3") : boxBody(fixtures + "/" + name, "dac3");
        const auto ours = box[2] == 'c' ? boxBody(path, "dec3") : boxBody(path, "dac3");
        if (theirs.empty() || theirs != ours) return false;
    }
    return !rewrap(fixtures + "/readers/vp8_vorbis.webm", scratch("no_vorbis.mov"), nullptr) && !extractAudio(fixtures + "/readers/vp8_vorbis.webm", scratch("no_vorbis.m4a"), SoundChoice::Copy, 0, nullptr);
}

uint64_t defaultDuration(const std::string& path) {
    const std::vector<uint8_t> file = load(path);
    static const uint8_t id[3] = {0x23, 0xE3, 0x83};
    auto at = std::search(file.begin(), file.end(), id, id + 3);
    if (file.end() - at < 4) return 0;
    const int bytes = at[3] & 0x7F;
    if (!(at[3] & 0x80) || bytes > 8 || file.end() - at < 4 + bytes) return 0;
    uint64_t value = 0;
    for (int i = 0; i < bytes; ++i) value = value << 8 | at[4 + i];
    return value;
}

bool mkvFrameRate() {
    const std::string path = scratch("rate.mkv");
    if (!rewrap(fixtures + "/tools/clip.mp4", path, nullptr) || defaultDuration(path) != 33333333) return false;
    const std::string again = scratch("rate_again.mkv");
    return trimCopy(path, again, 500000, 2500000, nullptr) && defaultDuration(again) == 33333333;
}

bool gifHold() {
    GifInfo info;
    const std::string colour = scratch("hold.mp4"), matte = scratch("hold_matte.mp4");
    if (!gifToVideo(fixtures + "/tools/hold.gif", colour, matte, &info, nullptr) || info.durationMs != 3400) return false;
    auto reader = Reader::open(colour);
    const Track* video = reader ? reader->first(Kind::Video) : nullptr;
    if (!video || video->samples.size() != 5 || std::fabs(video->seconds() - 3.4) > 0.011) return false;
    return std::fabs(double(video->samples.back().duration) / video->timescale - 3.0) < 0.011;
}

bool chromaLines() {
    const int size = 64;
    std::vector<uint8_t> pixels(size_t(size) * size * 4);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            uint8_t* p = &pixels[(size_t(y) * size + x) * 4];
            const bool red = x == 9 || x == 21 || x == 33, blue = x == 40, green = y == 51;
            p[0] = red ? 0 : blue ? 255 : green ? 0 : 128;
            p[1] = red || blue ? 0 : green ? 200 : 128;
            p[2] = red ? 255 : blue || green ? 0 : 128;
            p[3] = 255;
        }
    }
    std::vector<uint8_t> planes(size_t(size) * size * 3 / 2);
    bgraToI420(pixels.data(), size_t(size) * 4, size, size, planes.data());
    if (!save(scratch("chroma.bgra"), pixels) || !save(scratch("chroma.yuv"), planes)) return false;
    const uint8_t* u = planes.data() + size * size;
    const uint8_t* v = u + size * size / 4;
    auto near = [](int value, int want) { return std::abs(value - want) <= 2; };
    for (int column : {4, 10, 16}) if (!near(v[5 * 32 + column], 184)) return false;
    if (!near(u[5 * 32 + 20], 184) || !near(u[5 * 32 + 19], 128) || !near(u[5 * 32 + 21], 128)) return false;
    return u[25 * 32 + 2] < 110 && v[25 * 32 + 2] < 110 && near(u[26 * 32 + 2], 128);
}

bool aacMonoRate() {
    const int rate = 48000, seconds = 4;
    std::vector<int16_t> pcm(size_t(rate) * seconds);
    uint32_t noise = 1;
    for (size_t i = 0; i < pcm.size(); ++i) {
        noise = noise * 1664525u + 1013904223u;
        pcm[i] = int16_t(6000 * std::sin(i * 0.0571) + 3000 * std::sin(i * 0.3313) + int(noise >> 21) - 1024);
    }
    for (int bitrate : {192000, 256000, 320000}) {
        auto encoder = AacEncoder::open(rate, 1, bitrate);
        if (!encoder) return false;
        size_t bytes = 0;
        AudioSink sink = [&](const uint8_t*, size_t n) { bytes += n; };
        for (size_t at = 0; at < pcm.size(); at += 4096) if (!encoder->encode(pcm.data() + at, std::min<size_t>(4096, pcm.size() - at), sink)) return false;
        if (!encoder->finish(sink)) return false;
        if (double(bytes) * 8 / seconds < 120000) return false;
    }
    return true;
}

bool copyTags() {
    const std::string tagged = fixtures + "/probe/tags.m4a";
    auto original = Reader::open(tagged);
    if (!original) return false;
    for (const char* name : {"tags.m4a", "tags.mkv"}) {
        if (!rewrap(tagged, scratch(name), nullptr)) return false;
        auto reader = Reader::open(scratch(name));
        if (!reader) return false;
        for (const char* key : {"title", "artist", "album", "date"}) {
            auto found = reader->tags().find(key);
            auto wanted = original->tags().find(key);
            if (wanted == original->tags().end() || found == reader->tags().end() || found->second != wanted->second) return false;
        }
    }
    auto source = Reader::open(fixtures + "/probe/h264.mp4");
    const Track* from = source ? source->first(Kind::Video) : nullptr;
    if (!from) return false;
    const std::string phone = scratch("phone.mov");
    auto writer = Writer::open(phone);
    if (!writer) return false;
    writer->tags({{"title", "Holiday"}, {"location", "+52.3700+004.8900/"}});
    const int track = writer->add(specOf(*from));
    std::vector<uint8_t> data;
    for (uint32_t i = 0; i < from->samples.size(); ++i) {
        const Sample& s = from->samples[i];
        data.resize(s.size);
        if (!source->read(*from, i, data.data()) || !writer->write(track, data.data(), data.size(), s.pts, s.dts, s.duration, s.sync)) return false;
    }
    if (!writer->finish()) return false;
    for (const char* name : {"trip.mp4", "trip.mov", "trip.mkv"}) {
        if (!trimCopy(phone, scratch(name), 200000, 800000, nullptr)) return false;
        auto reader = Reader::open(scratch(name));
        if (!reader || reader->tags().count("title") == 0 || reader->tags().at("title") != "Holiday" || reader->tags().count("location") == 0
            || reader->tags().at("location") != "+52.3700+004.8900/")
            return false;
    }
    return true;
}

struct Shown {
    int64_t stamp = 0;
    std::vector<uint8_t> luma, alpha;
    int width = 0, height = 0;
};

PictureSink keeper(std::vector<Shown>& list) {
    return [&list](const absent::Picture& p) {
        Shown s;
        s.stamp = p.stamp;
        s.width = p.width;
        s.height = p.height;
        for (int y = 0; y < p.height; ++y) s.luma.insert(s.luma.end(), p.plane[0] + size_t(y) * p.stride[0], p.plane[0] + size_t(y) * p.stride[0] + p.width);
        for (int y = 0; p.alpha && y < p.height; ++y) s.alpha.insert(s.alpha.end(), p.alpha + size_t(y) * p.alphaStride, p.alpha + size_t(y) * p.alphaStride + p.width);
        list.push_back(std::move(s));
    };
}

std::vector<uint8_t> hapSection(int type, const std::vector<uint8_t>& body, bool longHeader) {
    std::vector<uint8_t> out;
    if (longHeader) out = {0, 0, 0, uint8_t(type), uint8_t(body.size()), uint8_t(body.size() >> 8), uint8_t(body.size() >> 16), uint8_t(body.size() >> 24)};
    else out = {uint8_t(body.size()), uint8_t(body.size() >> 8), uint8_t(body.size() >> 16), uint8_t(type)};
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

bool ownVideoShapes() {
    Track hap;
    hap.codec = fourcc("HapM");
    hap.width = hap.height = 8;
    std::vector<uint8_t> colour, veil;
    for (int block = 0; block < 4; ++block) {
        const uint8_t texture[16] = {255, 255, 0, 0, 0, 0, 0, 0, uint8_t(block * 60), 0x80, uint8_t(block * 60), 0x80, 0, 0, 0, 0};
        colour.insert(colour.end(), texture, texture + 16);
        const uint8_t alpha[8] = {uint8_t(40 + block * 50), uint8_t(40 + block * 50), 0, 0, 0, 0, 0, 0};
        veil.insert(veil.end(), alpha, alpha + 8);
    }
    std::vector<std::vector<Shown>> results(2);
    for (int longHeader = 0; longHeader < 2; ++longHeader) {
        std::vector<uint8_t> inner = hapSection(0xAF, colour, longHeader);
        const std::vector<uint8_t> second = hapSection(0xA1, veil, longHeader);
        inner.insert(inner.end(), second.begin(), second.end());
        const std::vector<uint8_t> frame = hapSection(0x0D, inner, longHeader);
        auto decoder = openOwnVideo(hap, nullptr);
        if (!decoder || !decoder->decode(frame.data(), frame.size(), 0, keeper(results[size_t(longHeader)]))) return false;
    }
    const std::vector<Shown>& a = results[0];
    const std::vector<Shown>& b = results[1];
    if (a.size() != 1 || b.size() != 1 || a[0].luma != b[0].luma || a[0].alpha != b[0].alpha || a[0].alpha.size() != 64) return false;
    if (a[0].alpha[0] != 40 || a[0].alpha[63] != 190) return false;
    for (uint32_t tag : {fourcc("ULY0"), fourcc("Hap1"), fourcc("rle "), fourcc("FFV1")}) {
        Track empty;
        empty.codec = tag;
        empty.width = 0;
        empty.height = 16;
        empty.bits = 24;
        if (openOwnVideo(empty, nullptr)) return false;
    }
    return true;
}

bool vc1Concealed() {
    auto reader = Reader::open(fixtures + "/vc1/main_bframes.avi");
    const Track* track = reader ? reader->first(Kind::Video) : nullptr;
    if (!track || track->samples.size() < 8) return false;
    std::vector<Shown> clean, cut;
    for (int pass = 0; pass < 2; ++pass) {
        auto decoder = openVc1(track->codec, track->config, track->width, track->height, nullptr);
        if (!decoder) return false;
        for (uint32_t index : {0u, 7u}) {
            std::vector<uint8_t> packet(track->samples[index].size);
            if (!reader->read(*track, index, packet.data())) return false;
            if (pass && index) packet.resize(packet.size() / 3);
            if (!decoder->decode(packet.data(), packet.size(), index, keeper(pass ? cut : clean))) return false;
        }
    }
    if (clean.size() != 2 || cut.size() != 2) return false;
    const auto band = [](const Shown& s, bool bottom) {
        const size_t rows = 16 * size_t(s.width);
        return std::vector<uint8_t>(bottom ? s.luma.end() - ptrdiff_t(rows) : s.luma.begin(), bottom ? s.luma.end() : s.luma.begin() + ptrdiff_t(rows));
    };
    std::vector<uint8_t> reduced = band(cut[0], true);
    for (uint8_t& value : reduced) value &= 0xFE;
    return band(cut[1], false) == band(clean[1], false) && band(cut[1], true) == reduced && band(clean[1], true) != band(clean[0], true);
}

bool vc1BeforeAnchor() {
    auto reader = Reader::open(fixtures + "/vc1/main_bframes.avi");
    const Track* track = reader ? reader->first(Kind::Video) : nullptr;
    auto decoder = track ? openVc1(track->codec, track->config, track->width, track->height, nullptr) : nullptr;
    if (!decoder) return false;
    std::vector<Shown> shown;
    std::vector<int64_t> sent;
    for (size_t i = 0; i < track->samples.size(); ++i) {
        std::vector<uint8_t> packet(track->samples[i].size);
        if (!reader->read(*track, uint32_t(i), packet.data())) return false;
        sent.push_back(track->samples[i].pts);
        if (!decoder->decode(packet.data(), packet.size(), track->samples[i].pts, keeper(shown))) return false;
    }
    decoder->drain(keeper(shown));
    if (shown.size() != sent.size()) return false;
    for (size_t i = 0; i < sent.size(); ++i) if (shown[i].stamp != sent[i]) return false;
    return true;
}

struct PacketList : Reader {
    std::vector<Track> list;
    std::vector<std::vector<uint8_t>> packets;

    const char* format() const override { return "packets"; }
    const std::vector<Track>& tracks() const override { return list; }
    bool read(const Track&, uint32_t index, uint8_t* out) override {
        if (index >= packets.size()) return false;
        memcpy(out, packets[index].data(), packets[index].size());
        return true;
    }
};

bool eac3Substreams() {
    auto reader = Reader::open(fixtures + "/probe/h264_eac3.ts");
    const Track* track = reader ? reader->first(Kind::Audio) : nullptr;
    if (!track || track->codec != fourcc("ec-3") || track->samples.size() < 8) return false;
    std::vector<std::vector<uint8_t>> plain, joined;
    for (uint32_t i = 0; i < track->samples.size(); ++i) {
        std::vector<uint8_t> packet(track->samples[i].size);
        if (packet.size() < 8 || !reader->read(*track, i, packet.data())) return false;
        std::vector<uint8_t> both = packet;
        both.insert(both.end(), packet.begin(), packet.end());
        both[packet.size() + 2] = uint8_t((both[packet.size() + 2] & ~0x38) | 0x08);
        plain.push_back(std::move(packet));
        joined.push_back(std::move(both));
    }
    std::vector<float> want, got;
    for (int pass = 0; pass < 2; ++pass) {
        const std::vector<std::vector<uint8_t>>& packets = pass ? joined : plain;
        auto decoder = openAc3Audio(*track, packets[0], nullptr);
        if (!decoder) return false;
        for (const std::vector<uint8_t>& packet : packets) decoder->feed(packet, pass ? got : want);
    }
    return !want.empty() && got == want;
}

bool mixWide() {
    auto gains = [](int from, int to, uint64_t mask, int lit) {
        ChannelMix mix(from, to, mask);
        std::vector<float> in(size_t(from), 0.0f), out(size_t(to), 0.0f);
        in[size_t(lit)] = 1.0f;
        mix.apply(in.data(), 1, out.data());
        return out;
    };
    const float half = float(std::sqrt(0.5));
    auto near = [](float a, float b) { return std::fabs(a - b) < 1e-5f; };
    const std::vector<float> side = gains(8, 6, 0x63F, 6);
    if (!near(side[4], half) || !near(side[0], 0) || !near(side[5], 0)) return false;
    const std::vector<float> back = gains(6, 4, 0x3F, 5);
    if (!near(back[3], half) || !near(back[1], 0)) return false;
    const std::vector<float> sideOnly = gains(6, 5, 0x60F, 5);
    if (!near(sideOnly[4], 1)) return false;
    const std::vector<float> centre = gains(7, 8, 0x13F, 6);
    if (!near(centre[4], half) || !near(centre[5], half)) return false;
    const std::vector<float> wide = gains(10, 2, 0, 9);
    if (!(wide[1] > 0.4f) || !near(wide[0], 0)) return false;
    if (!near(gains(10, 4, 0, 0)[0], 1 / std::sqrt(3.0f)) || !near(gains(10, 4, 0, 3)[3], half) || !near(gains(10, 4, 0, 7)[3], half)) return false;
    return gains(16, 1, 0, 15)[0] > 0.2f;
}

float loudness(const std::vector<float>& sound, size_t from, size_t to) {
    double sum = 0;
    for (size_t i = from; i < to && i < sound.size(); ++i) sum += double(sound[i]) * sound[i];
    return to > from ? float(std::sqrt(sum / double(to - from))) : 0.0f;
}

bool speechShapes() {
    const int rate = 48000;
    std::vector<float> late(size_t(rate) * 2, 0.0f);
    for (size_t i = late.size() / 2; i < late.size(); ++i) late[i] = 0.5f * float(std::sin(double(i) * 0.05));
    const std::vector<float> quick = shapeSpeech(late, rate, 2, 0.25);
    if (quick.size() != late.size() / 2 || !(loudness(quick, quick.size() * 3 / 4, quick.size()) > 0.1f)) return false;
    std::vector<float> tone(size_t(rate), 0.0f);
    for (size_t i = 0; i < tone.size(); ++i) tone[i] = 0.5f * float(std::sin(double(i) * 0.05));
    const std::vector<float> slow = shapeSpeech(tone, rate, 0.5, 4);
    if (slow.size() != tone.size() * 2 || !(loudness(slow, slow.size() * 3 / 4, slow.size()) > 0.1f)) return false;
    std::vector<float> blip(size_t(rate) * 60 / 1000, 0.0f);
    for (size_t i = blip.size() * 2 / 3; i < blip.size(); ++i) blip[i] = 0.5f * float(std::sin(double(i) * 0.05));
    const std::vector<float> short2 = stretchSound(blip, rate, 2);
    if (short2.size() != blip.size() / 2 || !(loudness(short2, short2.size() / 2, short2.size()) > 0.03f)) return false;
    auto writer = AudioFileWriter::open(scratch("voice_stereo.wav"), AudioFormat::Wav16, rate, 2, 0);
    if (!writer) return false;
    std::vector<float> both(tone.size() * 2);
    for (size_t i = 0; i < tone.size(); ++i) both[2 * i] = both[2 * i + 1] = 0.9f * float(std::sin(double(i) * 0.05));
    if (!writer->write(both.data(), tone.size()) || !writer->finish()) return false;
    if (!convertSpeech(scratch("voice_stereo.wav"), scratch("voice_mono.wav"), rate, 1, 1)) return false;
    auto voice = PcmReader::open(scratch("voice_mono.wav"), rate, 1);
    if (!voice || voice->length() <= 0) return false;
    std::vector<float> mono(size_t(voice->length()));
    if (!voice->read(0, mono.size(), mono.data())) return false;
    float peak = 0;
    for (float v : mono) peak = std::max(peak, std::fabs(v));
    return peak > 0.85f && peak < 0.95f;
}

bool quarterChroma() {
    const int size = 8;
    std::vector<uint8_t> luma(size * size, 128), cb(4, 128), cr = {220, 220, 36, 36};
    absent::Picture p;
    p.width = p.height = size;
    p.chromaWidth = p.chromaHeight = 2;
    p.plane[0] = luma.data();
    p.plane[1] = cb.data();
    p.plane[2] = cr.data();
    p.stride[0] = size;
    p.stride[1] = p.stride[2] = 2;
    std::vector<uint8_t> bgra(size * size * 4);
    if (!pictureToBGRA(p, bgra.data(), size * 4)) return false;
    auto red = [&](int y) { return int(bgra[size_t(y) * size * 4 + 2]); };
    std::vector<uint8_t> i420;
    pictureToI420(p, i420);
    const uint8_t* v = i420.data() + size * size + 16;
    return red(0) == red(1) && red(2) > red(5) + 60 && red(3) > red(4) && red(6) == red(7) && v[0] == 220 && v[4] == 220 && v[8] == 36 && v[12] == 36;
}

bool mkvRotation() {
    auto source = Reader::open(fixtures + "/probe/rot90.mp4");
    const Track* from = source ? source->first(Kind::Video) : nullptr;
    if (!from || !from->rotation) return false;
    const std::string turned = scratch("turned.mkv"), silent = scratch("turned_silent.mkv");
    if (!rewrap(fixtures + "/probe/rot90.mp4", turned, nullptr) || !stripAudio(fixtures + "/probe/rot90.mp4", silent, nullptr)) return false;
    for (const std::string& name : {turned, silent}) {
        auto reader = Reader::open(name);
        const Track* video = reader ? reader->first(Kind::Video) : nullptr;
        if (!video || video->rotation != from->rotation) return false;
    }
    return true;
}
#ifdef _WIN32
bool manyReaders() {
    std::vector<std::unique_ptr<Reader>> held;
    for (int i = 0; i < 1000; ++i) {
        auto reader = Reader::open(fixtures + (i % 2 ? "/audio/aac_stereo_44k.m4a" : "/readers/f32.wav"));
        if (!reader) return false;
        held.push_back(std::move(reader));
    }
    return save(scratch("after_readers.bin"), {1, 2, 3}) && load(scratch("after_readers.bin")).size() == 3;
}
#endif

std::vector<uint8_t> escapedNal(const std::string& bits, uint8_t head) {
    std::vector<uint8_t> raw = {head};
    for (size_t i = 0; i < bits.size(); i += 8) {
        uint8_t value = 0;
        for (size_t k = 0; k < 8; ++k) value = uint8_t(value << 1 | (i + k < bits.size() && bits[i + k] == '1'));
        raw.push_back(value);
    }
    std::vector<uint8_t> out;
    int zeros = 0;
    for (uint8_t b : raw) {
        if (zeros >= 2 && b <= 3) {
            out.push_back(3);
            zeros = 0;
        }
        out.push_back(b);
        zeros = b ? 0 : zeros + 1;
    }
    return out;
}

std::vector<uint8_t> hostileAvi(bool loopingIndex) {
    const std::string wideCount = std::string(30, '0') + "1" + std::string(29, '0') + "1";
    const std::vector<uint8_t> sps = escapedNal("01000010" "11000000" "00011110" "111" + wideCount + "010" "0" "00100" "00100" "1" "1" "0" "0" "1", 0x67);
    std::vector<uint8_t> header, format, stream, hdrl, movi, index, body, file;
    text(header, "vids");
    text(header, "H264");
    little(header, 0, 12);
    little(header, 1, 4);
    little(header, 25, 4);
    header.resize(56, 0);
    little(format, 40, 4);
    little(format, 64, 4);
    little(format, 64, 4);
    little(format, 1, 2);
    little(format, 24, 2);
    text(format, "H264");
    format.resize(40, 0);
    for (uint8_t b : {1, 0x42, 0xC0, 0x1E, 0xFF, 0xE1}) format.push_back(b);
    format.push_back(uint8_t(sps.size() >> 8));
    format.push_back(uint8_t(sps.size()));
    format.insert(format.end(), sps.begin(), sps.end());
    for (uint8_t b : {1, 0, 4, 0x68, 0xCE, 0x38, 0x80}) format.push_back(b);
    chunk(stream, "strh", header);
    chunk(stream, "strf", format);
    if (loopingIndex) {
        std::vector<uint8_t> super;
        little(super, 4, 2);
        little(super, 0, 2);
        little(super, 100, 4);
        text(super, "00dc");
        super.resize(24, 0);
        for (int i = 0; i < 100; ++i) {
            little(super, 0, 8);
            little(super, 24 + 1600 + 8, 4);
            little(super, 1, 4);
        }
        chunk(stream, "indx", super);
    }
    chunk(hdrl, "avih", std::vector<uint8_t>(56, 0));
    std::vector<uint8_t> list = listed("strl", stream);
    hdrl.insert(hdrl.end(), list.begin(), list.end());
    const std::vector<uint8_t> slice = {0, 0, 0, 6, 0x65, 0x88, 0x87, 0xFF, 0xFF, 0xFF};
    for (int i = 0; i < 20; ++i) {
        text(index, "00dc");
        little(index, 0x10, 4);
        little(index, 4 + movi.size(), 4);
        little(index, slice.size(), 4);
        chunk(movi, "00dc", slice);
    }
    text(body, "AVI ");
    for (const std::vector<uint8_t>& part : {listed("hdrl", hdrl), listed("movi", movi)}) body.insert(body.end(), part.begin(), part.end());
    chunk(body, "idx1", index);
    chunk(file, "RIFF", body);
    if (loopingIndex) {
        static const char name[] = "indx";
        const uint64_t at = uint64_t(std::search(file.begin(), file.end(), name, name + 4) - file.begin());
        for (int i = 0; i < 100; ++i) for (int k = 0; k < 8; ++k) file[at + 8 + 24 + size_t(i) * 16 + size_t(k)] = uint8_t(at >> (8 * k));
    }
    return file;
}

bool aviHostile() {
    for (bool looping : {false, true}) {
        const std::string path = scratch(looping ? "looping_index.avi" : "wide_sps.avi");
        if (!save(path, hostileAvi(looping))) return false;
        const auto start = std::chrono::steady_clock::now();
        auto reader = Reader::open(path);
        const Track* video = reader ? reader->first(Kind::Video) : nullptr;
        if (!video || video->samples.size() != 20 || std::chrono::steady_clock::now() - start > std::chrono::seconds(3)) return false;
    }
    return true;
}

void fullBox(Bytes& b, const char (&name)[5], const std::vector<uint32_t>& words) {
    const size_t at = b.open(fourcc(name), 0, 0);
    for (uint32_t w : words) b.u32(w);
    b.close(at);
}

void testTrak(Bytes& b, uint32_t id, const char (&handler)[5], uint32_t scale, const std::vector<uint32_t>& edits, const std::function<void(Bytes&)>& tables) {
    const size_t trak = b.open(fourcc("trak"));
    const size_t tkhd = b.open(fourcc("tkhd"), 0, 3);
    b.zero(8);
    b.u32(id);
    b.zero(4 + 4 + 8 + 8 + 36 + 8);
    b.close(tkhd);
    if (!edits.empty()) {
        const size_t edts = b.open(fourcc("edts"));
        std::vector<uint32_t> list = {uint32_t(edits.size() / 2)};
        for (size_t i = 0; i + 1 < edits.size(); i += 2) list.insert(list.end(), {edits[i], edits[i + 1], 0x00010000});
        fullBox(b, "elst", list);
        b.close(edts);
    }
    const size_t mdia = b.open(fourcc("mdia"));
    fullBox(b, "mdhd", {0, 0, scale, 0, 0x55C40000});
    const size_t hdlr = b.open(fourcc("hdlr"), 0, 0);
    b.u32(0);
    b.u32(fourcc(handler));
    b.zero(13);
    b.close(hdlr);
    const size_t minf = b.open(fourcc("minf"));
    const size_t stbl = b.open(fourcc("stbl"));
    tables(b);
    b.close(stbl);
    b.close(minf);
    b.close(mdia);
    b.close(trak);
}

Bytes testMovie(uint32_t scale, const std::function<void(Bytes&)>& traks) {
    Bytes b;
    const size_t ftyp = b.open(fourcc("ftyp"));
    b.u32(fourcc("qt  "));
    b.u32(0);
    b.close(ftyp);
    const size_t moov = b.open(fourcc("moov"));
    fullBox(b, "mvhd", {0, 0, scale, 0, 0x00010000, 0x01000000, 0, 0, 0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000, 0, 0, 0, 0, 0, 0, 2});
    traks(b);
    b.close(moov);
    return b;
}

bool mp4SampleBudget() {
    Bytes b = testMovie(1000, [](Bytes& m) {
        testTrak(m, 1, "meta", 1000, {}, [](Bytes& t) {
            std::vector<uint32_t> sizes = {0, 3000}, offsets = {3000};
            sizes.resize(2 + 3000, 60000);
            offsets.resize(1 + 3000, 0);
            fullBox(t, "stsz", sizes);
            fullBox(t, "stsc", {1, 1, 1, 1});
            fullBox(t, "stco", offsets);
            fullBox(t, "stts", {1, 3000, 1});
        });
        for (uint32_t id = 2; id <= 101; ++id) {
            testTrak(m, id, "meta", 1000, {}, [](Bytes& t) {
                fullBox(t, "stsz", {1, 0xFFFFFFFFu});
                fullBox(t, "stsc", {1, 1, 0xFFFFFFFFu, 1});
                fullBox(t, "stco", {1, 0});
                fullBox(t, "stts", {1, 0xFFFFFFFFu, 1});
            });
        }
        const size_t mvex = m.open(fourcc("mvex"));
        fullBox(m, "trex", {1, 1, 1, 0, 0});
        m.close(mvex);
    });
    const size_t moof = b.open(fourcc("moof"));
    const size_t traf = b.open(fourcc("traf"));
    fullBox(b, "tfhd", {1});
    fullBox(b, "trun", {40000});
    b.close(traf);
    b.close(moof);
    const size_t mdat = b.open(fourcc("mdat"));
    b.zero(65536);
    b.close(mdat);
    const std::string path = scratch("many_tracks_budget.mov");
    if (!save(path, b.d)) return false;
    auto reader = Reader::open(path);
    if (!reader || reader->tracks().empty()) return false;
    uint64_t samples = 0;
    for (const Track& t : reader->tracks()) samples += t.samples.size();
    return samples <= b.d.size() / 8 + 65536 && reader->tracks()[0].id == 1 && reader->tracks()[0].samples.size() < 100;
}

bool mp4Descriptions() {
    const auto samplesOf = [](const char* name, uint32_t secondIndex, uint8_t secondSetting, uint16_t secondReference) -> long {
        Bytes b = testMovie(30, [&](Bytes& m) {
            testTrak(m, 1, "vide", 30, {}, [&](Bytes& t) {
                const size_t stsd = t.open(fourcc("stsd"), 0, 0);
                t.u32(2);
                for (int k = 0; k < 2; ++k) {
                    const size_t entry = t.open(fourcc("zzzz"));
                    t.zero(6);
                    t.u16(k ? secondReference : 1);
                    t.u32(k ? secondSetting : 7);
                    t.close(entry);
                }
                t.close(stsd);
                fullBox(t, "stsz", {1, 10});
                fullBox(t, "stsc", {2, 1, 5, 1, 2, 5, secondIndex});
                fullBox(t, "stco", {2, 8, 13});
                fullBox(t, "stts", {1, 10, 1});
            });
        });
        const size_t mdat = b.open(fourcc("mdat"));
        b.zero(16);
        b.close(mdat);
        const std::string path = scratch(name);
        if (!save(path, b.d)) return -1;
        auto reader = Reader::open(path);
        return reader && reader->tracks().size() == 1 ? long(reader->tracks()[0].samples.size()) : -1;
    };
    return samplesOf("one_description.mov", 1, 9, 1) == 10 && samplesOf("same_descriptions.mov", 2, 7, 2) == 10 && samplesOf("other_description.mov", 2, 9, 1) == 0 &&
        samplesOf("missing_description.mov", 3, 9, 1) == 10;
}

bool mp4EditGaps() {
    const auto video = [](Bytes& m, const std::vector<uint32_t>& edits, bool firstKeyOnly) {
        testTrak(m, 1, "vide", 30, edits, [&](Bytes& t) {
            if (firstKeyOnly) fullBox(t, "stss", {1, 1});
            fullBox(t, "stsz", {1, 10});
            fullBox(t, "stsc", {1, 1, 10, 1});
            fullBox(t, "stco", {1, 8});
            fullBox(t, "stts", {1, 10, 1});
        });
    };
    std::vector<uint32_t> repeated;
    for (int i = 0; i < 10000; ++i) repeated.insert(repeated.end(), {1, 9, 1, 0});
    for (bool gap : {true, false}) {
        Bytes b = testMovie(30, [&](Bytes& m) { video(m, gap ? std::vector<uint32_t>{4, 0, 3, 0xFFFFFFFFu, 4, 6} : repeated, !gap); });
        const size_t mdat = b.open(fourcc("mdat"));
        b.zero(16);
        b.close(mdat);
        const std::string path = scratch(gap ? "edit_gap.mov" : "edit_repeats.mov");
        if (!save(path, b.d)) return false;
        auto reader = Reader::open(path);
        const Track* t = reader ? reader->first(Kind::Video) : nullptr;
        if (!t) return false;
        if (gap && (t->mediaEnd != 11 || t->order.size() != 8 || t->samples[t->order[4]].pts != 7)) return false;
        if (!gap && t->samples.size() > 4 * 10 + 65536 + 10) return false;
    }
    return true;
}

bool mp4FragmentTimes() {
    Bytes b = testMovie(30, [](Bytes& m) {
        testTrak(m, 1, "vide", 30, {}, [](Bytes& t) {
            fullBox(t, "stsz", {1, 3});
            fullBox(t, "stsc", {1, 1, 3, 1});
            fullBox(t, "stco", {1, 8});
            fullBox(t, "stts", {1, 3, 1});
        });
        testTrak(m, 2, "vide", 30, {}, [](Bytes& t) {
            fullBox(t, "stsz", {0, 0});
            fullBox(t, "stsc", {0});
            fullBox(t, "stco", {0});
            fullBox(t, "stts", {0});
        });
        const size_t mvex = m.open(fourcc("mvex"));
        fullBox(m, "trex", {1, 1, 1, 1, 0});
        fullBox(m, "trex", {2, 1, 1, 1, 0});
        m.close(mvex);
    });
    const size_t moof = b.open(fourcc("moof"));
    for (uint32_t id : {1u, 2u}) {
        const size_t traf = b.open(fourcc("traf"));
        const size_t tfhd = b.open(fourcc("tfhd"), 0, 0x020000);
        b.u32(id);
        b.close(tfhd);
        if (id == 2) fullBox(b, "tfdt", {3600 * 30});
        fullBox(b, "trun", {id == 1 ? 2u : 30u});
        b.close(traf);
    }
    b.close(moof);
    const size_t mdat = b.open(fourcc("mdat"));
    b.zero(64);
    b.close(mdat);
    const std::string path = scratch("fragment_times.mp4");
    if (!save(path, b.d)) return false;
    auto reader = Reader::open(path);
    if (!reader || reader->tracks().size() != 2) return false;
    const Track& joined = reader->tracks()[0];
    const Track& late = reader->tracks()[1];
    if (joined.samples.size() != 5 || joined.samples[3].dts != 3 || joined.samples[4].dts != 4) return false;
    return late.samples.size() == 30 && std::fabs(late.seconds() - 1.0) < 0.01 && std::fabs(late.rate() - 30.0) < 0.5;
}

bool movMp3Packets() {
    const uint32_t frames = 1152 * 10;
    Bytes b = testMovie(44100, [&](Bytes& m) {
        testTrak(m, 1, "soun", 44100, {}, [&](Bytes& t) {
            const size_t stsd = t.open(fourcc("stsd"), 0, 0);
            t.u32(1);
            const size_t entry = t.open(fourcc(".mp3"));
            t.zero(6);
            t.u16(1);
            t.u16(1);
            t.zero(6);
            t.u16(1);
            t.u16(16);
            t.u16(0xFFFE);
            t.u16(0);
            t.u32(44100u << 16);
            for (uint32_t v : {1152u, 417u, 417u, 2u}) t.u32(v);
            t.close(entry);
            t.close(stsd);
            fullBox(t, "stsz", {1, frames});
            fullBox(t, "stsc", {1, 1, frames, 1});
            fullBox(t, "stco", {1, 0});
            fullBox(t, "stts", {2, frames / 2, 1, frames / 2, 1});
        });
    });
    const size_t mdat = b.open(fourcc("mdat"));
    const uint32_t data = uint32_t(b.d.size());
    b.zero(4170);
    b.close(mdat);
    static const char name[] = "stco";
    const size_t at = size_t(std::search(b.d.begin(), b.d.end(), name, name + 4) - b.d.begin()) + 12;
    for (int k = 0; k < 4; ++k) b.d[at + size_t(k)] = uint8_t(data >> (24 - 8 * k));
    const std::string path = scratch("mp3_frames.mov");
    if (!save(path, b.d)) return false;
    auto reader = Reader::open(path);
    const Track* t = reader ? reader->first(Kind::Audio) : nullptr;
    if (!t || t->codec != fourcc(".mp3") || t->samples.size() != 10) return false;
    for (const Sample& s : t->samples) if (s.size != 417 || s.duration != 1152) return false;
    return t->samples[9].offset == data + 9 * 417;
}

bool androidVorbisConfig() {
    auto reader = Reader::open(fixtures + "/readers/vp8_vorbis.webm");
    const Track* sound = reader ? reader->first(Kind::Audio) : nullptr;
    if (!sound) return false;
    const auto config = android::codecConfig(*sound);
    return config.size() == 2 && config[0].size() == 30 && config[0][0] == 1 && config[1].size() > 7 && config[1][0] == 5 && memcmp(config[1].data() + 1, "vorbis", 6) == 0;
}

bool scaledMatrixZero() {
    std::vector<uint8_t> luma(64 * 64, 81), blue(32 * 32, 90), red(32 * 32, 240);
    absent::Picture p;
    p.plane[0] = luma.data();
    p.plane[1] = blue.data();
    p.plane[2] = red.data();
    p.stride[0] = 64;
    p.stride[1] = p.stride[2] = 32;
    p.width = p.height = 64;
    p.chromaWidth = p.chromaHeight = 32;
    p.color.known = true;
    p.color.matrix = 0;
    std::vector<uint8_t> same, scaled;
    if (!pictureToLayout(p, 64, 64, PixelLayout::Bgra, same) || !pictureToLayout(p, 32, 32, PixelLayout::Bgra, scaled)) return false;
    for (int c = 0; c < 3; ++c) if (std::abs(int(same[size_t(c)]) - int(scaled[size_t(c)])) > 2) return false;
    return true;
}

bool contains(const std::vector<uint8_t>& data, const std::vector<uint8_t>& part) { return std::search(data.begin(), data.end(), part.begin(), part.end()) != data.end(); }

bool writerSuffix() {
    if (Writer::open(scratch("song.mp3")) || Writer::open(scratch("song.WAV"))) return false;
    auto web = Writer::open(scratch("upper.WEBM"));
    if (!web || web->add(soundSpec("lpcm", 48000, 48000, 1)) != -1) return false;
    const uint8_t frames[4] = {1, 2, 3, 4};
    for (const char* name : {"upper.MKV", "upper.MOV"}) {
        auto writer = Writer::open(scratch(name));
        if (!writer) return false;
        const int track = writer->add(soundSpec("lpcm", 48000, 48000, 1));
        if (track < 0 || !writer->write(track, frames, 4, 0, 0, 2, true) || !writer->finish()) return false;
    }
    auto matroska = Reader::open(scratch("upper.MKV"));
    const std::vector<uint8_t> movie = load(scratch("upper.MOV"));
    return matroska && std::string(matroska->format()) == "mkv" && contains(movie, {'f', 't', 'y', 'p', 'q', 't', ' ', ' '});
}

bool pcmLabels() {
    struct Case {
        const char* name;
        int bits;
        bool floating;
        bool bigEndian;
        int rate;
        std::vector<uint8_t> frame;
        long expected;
    };
    const Case cases[] = {
        {"signed8.mp4", 8, false, true, 8000, {100}, 25600}, {"unsigned8.mp4", 8, false, false, 8000, {228}, 25600},
        {"signed8.mov", 8, false, true, 8000, {100}, 25600}, {"unsigned8.mov", 8, false, false, 8000, {228}, 25600},
        {"signed8.mkv", 8, false, true, 8000, {100}, 25600}, {"unsigned8.mkv", 8, false, false, 8000, {228}, 25600},
        {"float_be.mka", 32, true, true, 8000, {0x3F, 0, 0, 0}, 16384}, {"high16.mov", 16, false, false, 96000, {0, 0x40}, 16384},
        {"high24.mp4", 24, false, false, 96000, {0, 0, 0x40}, 16384}, {"high8.mov", 8, false, true, 96000, {64}, 16384},
    };
    for (const Case& c : cases) {
        const std::string path = scratch(c.name);
        auto writer = Writer::open(path);
        if (!writer) return false;
        TrackSpec spec = soundSpec("lpcm", uint32_t(c.rate), c.rate, 1);
        spec.bits = c.bits;
        spec.floating = c.floating;
        spec.bigEndian = c.bigEndian;
        const int track = writer->add(spec);
        std::vector<uint8_t> block;
        for (int i = 0; i < 800; ++i) block.insert(block.end(), c.frame.begin(), c.frame.end());
        if (!writer->write(track, block.data(), block.size(), 0, 0, 800, true) || !writer->finish()) return false;
        auto reader = Reader::open(path);
        const Track* sound = reader ? reader->first(Kind::Audio) : nullptr;
        if (!sound || sound->sampleRate != c.rate) return false;
        auto decoder = AudioDecoder::open(*reader, *sound);
        if (!decoder || std::labs(valueAt(*decoder, 100) - c.expected) > 1) return false;
    }
    return true;
}

bool creationTime() {
    const std::string stamp = "2024-05-06T07:08:09.000000Z";
    for (const char* name : {"made.mp4", "made.mov"}) {
        auto writer = Writer::open(scratch(name));
        if (!writer) return false;
        writer->tags({{"creation_time", stamp}});
        const int track = writer->add(soundSpec("lpcm", 48000, 48000, 1));
        const uint8_t frames[4] = {1, 2, 3, 4};
        if (!writer->write(track, frames, 4, 0, 0, 2, true) || !writer->finish()) return false;
        auto reader = Reader::open(scratch(name));
        if (!reader) return false;
        auto found = reader->tags().find("creation_time");
        if (found == reader->tags().end() || found->second != stamp) return false;
    }
    return true;
}

bool sampleEntryNames() {
    TrackSpec prores;
    prores.kind = Kind::Video;
    prores.codec = fourcc("apcn");
    prores.timescale = 25;
    prores.width = 64;
    prores.height = 64;
    TrackSpec avc = prores;
    avc.codec = fourcc("avc1");
    avc.configType = fourcc("avcC");
    avc.config = {1, 0x64, 0, 0x1F, 0xFF, 0xE0, 0};
    std::vector<uint8_t> frame(64, 0);
    memcpy(frame.data() + 4, "icpf", 4);
    const std::pair<int, std::vector<uint8_t>> scans[] = {{0, {1, 0}}, {1, {2, 9}}, {2, {2, 14}}};
    for (const auto& [scan, fields] : scans) {
        const std::string path = scratch("scan" + std::to_string(scan) + ".mov");
        auto writer = Writer::open(path);
        frame[20] = uint8_t(scan << 2);
        const int track = writer ? writer->add(prores) : -1;
        if (track < 0 || !writer->write(track, frame.data(), frame.size(), 0, 0, 1, true) || !writer->finish()) return false;
        if (!contains(load(path), {'f', 'i', 'e', 'l', fields[0], fields[1]})) return false;
    }
    const std::string path = scratch("inband.mp4");
    auto writer = Writer::open(path);
    const int track = writer ? writer->add(avc) : -1;
    const uint8_t nal[8] = {0, 0, 0, 4, 0x65, 0x88, 0x84, 0};
    if (track < 0 || !writer->write(track, nal, sizeof nal, 0, 0, 1, true) || !writer->finish()) return false;
    return contains(load(path), {'a', 'v', 'c', '3'});
}

bool mkvTrackCap() {
    std::vector<uint8_t> file, head, list, segment;
    const char* kind = "matroska";
    element(head, 0x4282, std::vector<uint8_t>(kind, kind + strlen(kind)));
    element(file, 0x1A45DFA3, head);
    for (uint32_t i = 1; i <= 3000; ++i) {
        std::vector<uint8_t> entry;
        element(entry, 0xD7, number(i, 2));
        element(entry, 0x83, number(2, 1));
        element(list, 0xAE, entry);
    }
    element(segment, 0x1654AE6B, list);
    element(file, 0x18538067, segment);
    const std::string path = scratch("tracks.mka");
    if (!save(path, file)) return false;
    auto reader = Reader::open(path);
    return !reader || reader->tracks().size() <= 1024;
}

bool lateOpus() {
    const std::string path = scratch("late.webm");
    auto writer = Writer::open(path);
    if (!writer) return false;
    TrackSpec spec = soundSpec("Opus", 48000, 48000, 1);
    spec.configType = fourcc("OpHd");
    spec.config = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 1, 0x38, 1, 0x80, 0xBB, 0, 0, 0, 0, 0};
    spec.skip = 312;
    spec.delay = int64_t(3600) * 48000;
    const int track = writer->add(spec);
    const uint8_t packet[3] = {0xF8, 0xFF, 0xFE};
    for (int i = 0; i < 10; ++i) if (!writer->write(track, packet, sizeof packet, i * 960, i * 960, 960, true)) return false;
    if (!writer->finish()) return false;
    auto reader = Reader::open(path);
    const Track* sound = reader ? reader->first(Kind::Audio) : nullptr;
    if (!sound || sound->samples.empty()) return false;
    const int64_t first = sound->samples.front().pts;
    return first > int64_t(3599) * 48000 && sound->delay == first && sound->mediaStart == first + 312;
}

bool framesFromZero() {
    auto source = Reader::open(fixtures + "/tools/clip.mp4");
    const Track* from = source ? source->first(Kind::Video) : nullptr;
    if (!from) return false;
    const std::string path = scratch("late_start.mkv");
    auto writer = Writer::open(path);
    TrackSpec spec = specOf(*from);
    spec.delay = int64_t(100) * from->timescale;
    const int track = writer ? writer->add(spec) : -1;
    if (track < 0) return false;
    std::vector<uint8_t> packet;
    for (uint32_t i = 0; i < from->samples.size(); ++i) {
        const Sample& s = from->samples[i];
        packet.resize(s.size);
        if (!source->read(*from, i, packet.data()) || !writer->write(track, packet.data(), packet.size(), s.pts, s.dts, s.duration, s.sync)) return false;
    }
    if (!writer->finish()) return false;
    FrameReaderSpec wanted;
    auto frames = FrameReader::open(path, wanted);
    return frames && std::fabs(frames->seconds(0)) < 0.001 && frames->indexAt(0.5) > 0;
}

bool leadingPictures() {
    auto source = Reader::open(fixtures + "/video/h264_270p.mp4");
    const Track* from = source ? source->first(Kind::Video) : nullptr;
    if (!from || from->syncs.size() < 2) return false;
    const uint32_t key = from->syncs[1], next = from->syncs.size() > 2 ? from->syncs[2] : uint32_t(from->samples.size());
    const std::string path = scratch("leading.mkv");
    auto writer = Writer::open(path);
    const int track = writer ? writer->add(specOf(*from)) : -1;
    if (track < 0) return false;
    const Sample& first = from->samples[key];
    const int64_t step = std::max<int64_t>(first.duration, 1);
    std::vector<uint8_t> packet(first.size);
    if (!source->read(*from, key, packet.data()) || !writer->write(track, packet.data(), packet.size(), first.pts, first.dts, first.duration, true)) return false;
    const uint8_t broken[8] = {0, 0, 0, 4, 0x41, 0x9A, 0x55, 0xAA};
    for (int k = 2; k >= 1; --k) if (!writer->write(track, broken, sizeof broken, first.pts - k * step, first.dts + (3 - k), uint32_t(step), false)) return false;
    for (uint32_t i = key + 1; i < next; ++i) {
        const Sample& s = from->samples[i];
        packet.resize(s.size);
        if (!source->read(*from, i, packet.data()) || !writer->write(track, packet.data(), packet.size(), s.pts, s.dts, s.duration, s.sync)) return false;
    }
    if (!writer->finish()) return false;
    auto reader = Reader::open(path);
    const Track* video = reader ? reader->first(Kind::Video) : nullptr;
    auto decoder = video ? VideoDecoder::open(*reader, *video) : nullptr;
    if (!decoder || decoder->frames() < 4) return false;
    const Frame shown = decoder->frame(0);
    return shown && shown.pts == decoder->pts(0) && decoder->frame(1) && decoder->frame(3);
}

uint8_t checksum8(const uint8_t* p, size_t n) {
    uint8_t crc = 0;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k) crc = uint8_t((crc << 1) ^ ((crc & 0x80) ? 0x07 : 0));
    }
    return crc;
}

bool flacSuccessors() {
    std::vector<uint8_t> file = {'f', 'L', 'a', 'C', 0x80, 0, 0, 34, 0x10, 0, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0x0A, 0xC4, 0x4E, 0xF0};
    file.resize(8 + 34, 0);
    for (uint32_t number = 0; file.size() < (2u << 20); number += 2) {
        uint8_t head[10] = {0xFF, 0xF8, 0x10, 0x70};
        size_t n = 4;
        if (number < 0x80) {
            head[n++] = uint8_t(number);
        } else if (number < 0x800) {
            head[n++] = uint8_t(0xC0 | (number >> 6));
            head[n++] = uint8_t(0x80 | (number & 0x3F));
        } else if (number < 0x10000) {
            head[n++] = uint8_t(0xE0 | (number >> 12));
            head[n++] = uint8_t(0x80 | ((number >> 6) & 0x3F));
            head[n++] = uint8_t(0x80 | (number & 0x3F));
        } else {
            head[n++] = uint8_t(0xF0 | (number >> 18));
            head[n++] = uint8_t(0x80 | ((number >> 12) & 0x3F));
            head[n++] = uint8_t(0x80 | ((number >> 6) & 0x3F));
            head[n++] = uint8_t(0x80 | (number & 0x3F));
        }
        head[n] = checksum8(head, n);
        file.insert(file.end(), head, head + n + 1);
    }
    const std::string path = scratch("headers.flac");
    if (!save(path, file)) return false;
    const auto start = std::chrono::steady_clock::now();
    auto reader = Reader::open(path);
    return std::chrono::steady_clock::now() - start < std::chrono::seconds(3) && (!reader || reader->tracks().size() == 1);
}

bool compressLead() {
    auto source = Reader::open(fixtures + "/tools/clip.mp4");
    const Track* video = source ? source->first(Kind::Video) : nullptr;
    const Track* audio = source ? source->first(Kind::Audio) : nullptr;
    if (!video || !audio) return false;
    const std::string path = scratch("sound_first.mkv");
    auto writer = Writer::open(path);
    if (!writer) return false;
    TrackSpec pictureSpec = specOf(*video);
    pictureSpec.delay = int64_t(video->timescale) * 2 / 5;
    const int pictures = writer->add(pictureSpec), sound = writer->add(specOf(*audio));
    std::vector<uint8_t> data;
    for (const auto& [t, out] : {std::pair{video, pictures}, std::pair{audio, sound}}) {
        for (uint32_t i = 0; i < t->samples.size(); ++i) {
            const Sample& s = t->samples[i];
            data.resize(s.size);
            if (!source->read(*t, i, data.data()) || !writer->write(out, data.data(), data.size(), s.pts, s.dts, s.duration, s.sync)) return false;
        }
    }
    if (!writer->finish()) return false;
    CompressJob job;
    job.input = path;
    job.output = scratch("sound_first.mp4");
    job.width = 320;
    if (!compress(job, nullptr)) return false;
    auto result = Reader::open(job.output);
    const Track* made = result ? result->first(Kind::Video) : nullptr;
    if (!made || made->order.size() < 2) return false;
    const double firstShown = double(made->samples[made->order[1]].pts - made->samples[made->order[0]].pts) / made->timescale;
    return firstShown > 0.4;
}

bool ivfSharedStamp() {
    std::vector<uint8_t> file = load(fixtures + "/video/vp8_270p.ivf");
    if (file.size() < 32) return false;
    std::vector<size_t> frames;
    for (size_t at = size_t(file[6] | (file[7] << 8)); at + 12 <= file.size(); at += 12 + size_t(file[at] | (file[at + 1] << 8) | (file[at + 2] << 16) | (uint32_t(file[at + 3]) << 24))) frames.push_back(at);
    if (frames.size() < 5) return false;
    memcpy(&file[frames[2] + 4], &file[frames[3] + 4], 8);
    const std::string path = scratch("shared_stamp.ivf");
    if (!save(path, file)) return false;
    auto reader = Reader::open(path);
    const Track* video = reader ? reader->first(Kind::Video) : nullptr;
    if (!video || video->samples.size() != frames.size() || video->order.size() != frames.size() - 1) return false;
    auto decoder = VideoDecoder::open(*reader, *video);
    return decoder && decoder->frames() == frames.size() - 1 && decoder->frame(2) && decoder->frame(3);
}

bool keptOnFailure() {
    const std::vector<uint8_t> kept = {'k', 'e', 'e', 'p'};
    const std::string video = scratch("kept.mp4");
    if (!save(video, kept)) return false;
    FrameWriterSpec spec;
    spec.width = 320;
    spec.height = 240;
    spec.layout = PixelLayout::I420;
    spec.audioFrom = scratch("no such sound.wav");
    if (FrameWriter::open(video, spec) || load(video) != kept) return false;
    std::vector<std::pair<std::string, AudioFormat>> sounds = {{"kept.wav", AudioFormat::Wav16}, {"kept.flac", AudioFormat::Flac}, {"kept.mp3", AudioFormat::Mp3}};
#ifdef ABSENT_OPUS
    sounds.push_back({"kept.opus", AudioFormat::Opus});
#endif
    const std::vector<float> silence(48000 * 2, 0.0f);
    for (const auto& [name, format] : sounds) {
        const std::string path = scratch(name);
        if (!save(path, kept)) return false;
        auto writer = AudioFileWriter::open(path, format, 48000, 2, 0);
        if (!writer) continue;
        if (!writer->write(silence.data(), 48000) || load(path) != kept) return false;
        writer.reset();
        if (load(path) != kept || fileBytes(path + ".part") != 0) return false;
        writer = AudioFileWriter::open(path, format, 48000, 2, 0);
        if (!writer || !writer->write(silence.data(), 48000) || !writer->finish() || load(path) == kept || fileBytes(path + ".part") != 0) return false;
    }
    return true;
}

bool sameInput() {
    const std::string path = scratch("same.m4a");
    if (!save(path, load(fixtures + "/audio/aac_stereo_44k.m4a"))) return false;
    const std::vector<uint8_t> before = load(path);
    std::string error;
    return !extractAudio(path, folder + "/./same.m4a", SoundChoice::Copy, 0, nullptr, &error) && error == "the output would replace the input" && load(path) == before;
}

bool layerOne() {
    std::vector<uint8_t> file;
    for (int frame = 0; frame < 100; ++frame) {
        std::vector<uint8_t> bits;
        auto put = [&](uint32_t value, int count) { for (int i = count - 1; i >= 0; --i) bits.push_back(uint8_t((value >> i) & 1)); };
        put(0xFFFF, 16);
        put(0x48, 8);
        put(0xC0, 8);
        for (int band = 0; band < 32; ++band) put(band == 1 || band == 3 ? 3 : 0, 4);
        put(8, 6);
        put(8, 6);
        for (int s = 0; s < 12; ++s) {
            for (int band : {1, 3}) {
                const double t = frame * 12 + s;
                put(uint32_t(std::clamp(int(7.5 + 7 * std::sin(2 * std::numbers::pi * t / (band == 1 ? 5 : 3))), 0, 14)), 4);
            }
        }
        std::vector<uint8_t> bytes(192, 0);
        for (size_t i = 0; i < bits.size(); ++i) bytes[i / 8] |= uint8_t(bits[i] << (7 - i % 8));
        file.insert(file.end(), bytes.begin(), bytes.end());
    }
    const std::string path = scratch("tone.mp1");
    if (!save(path, file)) return false;
    auto reader = Reader::open(path);
    const Track* sound = reader ? reader->first(Kind::Audio) : nullptr;
    auto decoder = sound && sound->codec == fourcc(".mp1") ? AudioDecoder::open(*reader, *sound) : nullptr;
    if (!decoder || decoder->length() != 38400) return false;
    std::vector<float> pcm(38400);
    if (!decoder->read(0, pcm.size(), pcm.data())) return false;
    float loudest = 0;
    for (float v : pcm) loudest = std::max(loudest, std::fabs(v));
    return loudest > 0.05f;
}

bool probeRateBudget() {
    const uint32_t count = 2400000;
    Bytes b = testMovie(90000, [&](Bytes& m) {
        testTrak(m, 1, "vide", 90000, {}, [&](Bytes& t) {
            fullBox(t, "stsz", {1, count});
            fullBox(t, "stsc", {1, 1, count, 1});
            fullBox(t, "stco", {1, 8});
            fullBox(t, "stts", {3, count - 2, 90000, 1, 90003, 1, 90000});
        });
    });
    const size_t mdat = b.open(fourcc("mdat"));
    b.zero(size_t(count) * 9);
    b.close(mdat);
    const std::string path = scratch("rate_budget.mov");
    if (!save(path, b.d)) return false;
    auto reader = Reader::open(path);
    const Track* video = reader ? reader->first(Kind::Video) : nullptr;
    if (!video || video->samples.size() != count) return false;
    const auto start = std::chrono::steady_clock::now();
    const std::string json = probeJson(path, false);
    return !json.empty() && std::chrono::steady_clock::now() - start < std::chrono::seconds(3);
}

const std::pair<const char*, std::function<bool()>> kChecks[] = {
    {"audio-gap", audioGap}, {"coded-gap-ts", codedGapTs}, {"empty-first-gap", emptyFirstGap}, {"aac-marks", aacMarks}, {"eac3-substreams", eac3Substreams}, {"long-silence", longSilence}, {"audio-lost", audioLost}, {"coded-loss", codedLoss}, {"coded-gap", codedGap}, {"audio-negative", audioNegative}, {"audio-held", audioHeld}, {"mix-centre", mixCentre}, {"s16-gain", s16Gain},
    {"wav-mask", wavMask}, {"flac-channels", flacChannels}, {"channel-order", channelOrder}, {"encoder-names", encoderNames}, {"opus-empty", opusEmpty}, {"aac-empty", aacEmpty}, {"short-files", shortFiles},
    {"aac-rate", aacRate}, {"rf64-size", rf64Size}, {"aifc-flip", aifcFlip}, {"trun-offset", trunOffset}, {"decode-time-limit", decodeTimeLimit},
    {"flac-rate", flacRate}, {"lace-bounds", laceBounds}, {"vorbis-modes", vorbisModes}, {"sps-limits", spsLimits}, {"tag-flags", tagFlags},
    {"pcm-checked", pcmChecked}, {"after-finish", afterFinish}, {"pcm-trim", pcmTrim}, {"long-file", longFile}, {"stream-flags", streamFlags},
    {"late-trim", lateTrim}, {"empty-sample", emptySample}, {"wide-clock", wideClock}, {"avi-repeated-index", aviRepeatedIndex}, {"track-seconds", trackSeconds},
    {"first-sync", firstSync}, 
#ifdef __APPLE__
    {"scaler-labels", scalerLabels},
#endif
#ifdef _WIN32
    {"many-readers", manyReaders},
#endif
    {"finish-twice", finishTwice}, {"many-tracks", manyTracks}, {"long-gap", longGap}, {"wma-block-align", wmaBlockAlign},
    {"interleave", interleave}, {"colour-unknown", colourUnknown}, {"copy-codecs", copyCodecs}, {"mkv-frame-rate", mkvFrameRate}, {"gif-hold", gifHold},
    {"chroma-lines", chromaLines}, {"aac-mono-rate", aacMonoRate}, {"copy-tags", copyTags}, {"mkv-rotation", mkvRotation},
    {"avi-hostile", aviHostile}, {"mp4-sample-budget", mp4SampleBudget}, {"mp4-descriptions", mp4Descriptions}, {"mp4-edit-gaps", mp4EditGaps}, {"mp4-fragment-times", mp4FragmentTimes},
    {"mov-mp3-packets", movMp3Packets}, {"android-vorbis-config", androidVorbisConfig}, {"scaled-matrix-zero", scaledMatrixZero},
    {"writer-suffix", writerSuffix}, {"pcm-labels", pcmLabels}, {"creation-time", creationTime}, {"sample-entry-names", sampleEntryNames},
    {"mkv-track-cap", mkvTrackCap}, {"late-opus", lateOpus}, {"frames-from-zero", framesFromZero}, {"leading-pictures", leadingPictures},
    {"flac-successors", flacSuccessors}, {"compress-lead", compressLead}, {"ivf-shared-stamp", ivfSharedStamp}, {"kept-on-failure", keptOnFailure},
    {"same-input", sameInput}, {"layer-one", layerOne}, {"probe-rate-budget", probeRateBudget},
    {"own-video-shapes", ownVideoShapes}, {"vc1-concealed", vc1Concealed}, {"vc1-before-anchor", vc1BeforeAnchor}, {"mix-wide", mixWide}, {"speech-shapes", speechShapes}, {"quarter-chroma", quarterChroma},
};

int fixcheck(const Args& args) {
    if (args.plain.size() < 3) {
        for (const auto& item : kChecks) printf("%s\n", item.first);
        return args.plain.empty() ? 0 : fail("usage: fixcheck <name> <scratch folder> <fixtures folder>");
    }
    folder = args.plain[1];
    fixtures = args.plain[2];
    for (const auto& item : kChecks) if (args.plain[0] == item.first) return item.second() ? 0 : 1;
    return fail("no such check");
}

Register fixcheckCommand("fixcheck", fixcheck);

}

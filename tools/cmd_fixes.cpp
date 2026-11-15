#include "../src/aac_config.h"
#include "../src/capture.h"
#include "../src/nal.h"
#include "../src/tools_common.h"
#include "absent/audio.h"
#include "cli.h"
#include <CoreVideo/CoreVideo.h>
#include <cmath>
#include <cstring>
#include <functional>

namespace {

using namespace cli;

std::string folder;
std::string fixtures;

std::string scratch(const std::string& name) { return folder + "/" + name; }

bool save(const std::string& path, const std::vector<uint8_t>& data) {
    FILE* file = fopen(path.c_str(), "wb");
    if (!file) return false;
    bool ok = fwrite(data.data(), 1, data.size(), file) == data.size();
    return fclose(file) == 0 && ok;
}

std::vector<uint8_t> load(const std::string& path) {
    std::vector<uint8_t> data;
    FILE* file = fopen(path.c_str(), "rb");
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
        for (size_t c = 0; c < channels; ++c) {
            float peak = 0;
            for (size_t i = 0; i < frames; ++i) peak = std::max(peak, std::fabs(pcm[i * channels + c]));
            if (exact ? std::fabs(peak - 0.1f * float(c + 1)) > 0.01f : c != 3 && peak < before + 0.05f) return false;
            if (c != 3) before = peak;
        }
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

const std::pair<const char*, std::function<bool()>> kChecks[] = {
    {"audio-gap", audioGap}, {"audio-lost", audioLost}, {"coded-loss", codedLoss}, {"coded-gap", codedGap}, {"audio-negative", audioNegative}, {"audio-held", audioHeld}, {"mix-centre", mixCentre},
    {"wav-mask", wavMask}, {"flac-channels", flacChannels}, {"channel-order", channelOrder}, {"opus-empty", opusEmpty}, {"aac-empty", aacEmpty}, {"short-files", shortFiles},
    {"aac-rate", aacRate}, {"rf64-size", rf64Size}, {"aifc-flip", aifcFlip}, {"trun-offset", trunOffset}, {"decode-time-limit", decodeTimeLimit},
    {"flac-rate", flacRate}, {"lace-bounds", laceBounds}, {"vorbis-modes", vorbisModes}, {"sps-limits", spsLimits}, {"tag-flags", tagFlags},
    {"pcm-checked", pcmChecked}, {"after-finish", afterFinish}, {"pcm-trim", pcmTrim}, {"long-file", longFile}, {"stream-flags", streamFlags},
    {"late-trim", lateTrim}, {"empty-sample", emptySample}, {"wide-clock", wideClock}, {"avi-repeated-index", aviRepeatedIndex}, {"track-seconds", trackSeconds},
    {"first-sync", firstSync}, {"scaler-labels", scalerLabels}, {"finish-twice", finishTwice}, {"many-tracks", manyTracks}, {"long-gap", longGap}, {"wma-block-align", wmaBlockAlign},
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

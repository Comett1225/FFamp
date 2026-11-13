#include "absent/audio.h"
#include "cli.h"
#include <algorithm>
#include <cmath>

namespace {

using namespace cli;

const size_t kChunk = 8192;

template <class Source>
bool spans(Source& source, int channels, const std::string& list) {
    std::vector<float> pcm;
    const char* at = list.c_str();
    while (*at) {
        char* end = nullptr;
        long long start = strtoll(at, &end, 10);
        if (*end != ':') return false;
        long long frames = strtoll(end + 1, &end, 10);
        if (frames < 0 || frames > (1 << 24)) return false;
        pcm.resize(size_t(frames) * size_t(channels));
        if (!source.read(start, size_t(frames), pcm.data())) return false;
        fwrite(pcm.data(), sizeof(float) * size_t(channels), size_t(frames), stdout);
        at = *end == ',' ? end + 1 : end;
        if (*end && *end != ',') return false;
    }
    return true;
}

void rawConfig(Track& track) {
    if (track.codec != fourcc("mp4a") || track.configType != fourcc("esds")) return;
    const std::vector<uint8_t>& c = track.config;
    for (size_t i = 4; i + 2 < c.size(); ++i) {
        if (c[i] != 0x05) continue;
        size_t at = i + 1;
        while (at < c.size() && (c[at] & 0x80)) ++at;
        if (at >= c.size() || at + 1 + c[at] > c.size() || c[at] < 2) continue;
        std::vector<uint8_t> specific(c.begin() + long(at) + 1, c.begin() + long(at) + 1 + c[at]);
        track.config = specific;
        track.configType = fourcc("ASC ");
        return;
    }
}

int adecode(const Args& args) {
    if (args.plain.empty()) return fail("usage: adecode <file> [--first frame] [--frames n] [--chunk n] [--list start:frames,...] [--raw-config]");
    std::string error;
    auto reader = Reader::open(args.plain[0], &error);
    if (!reader) return fail(error);
    const Track* found = reader->first(Kind::Audio);
    if (!found) return fail("no audio track");
    Track track = *found;
    if (args.has("raw-config")) rawConfig(track);
    auto decoder = AudioDecoder::open(*reader, track, &error);
    if (!decoder) return fail(error);
    fprintf(stderr, "%d Hz, %d channels, %lld frames\n", decoder->sampleRate(), decoder->channels(), (long long)decoder->length());
    if (args.has("list")) return spans(*decoder, decoder->channels(), args.get("list")) ? 0 : fail("bad --list, expected start:frames,start:frames");
    int64_t at = args.number("first", 0);
    int64_t end = args.has("frames") ? at + args.number("frames", 0) : decoder->length();
    size_t chunk = size_t(std::max(1L, args.number("chunk", long(kChunk))));
    std::vector<float> pcm(chunk * size_t(decoder->channels()));
    while (at < end) {
        size_t count = size_t(std::min<int64_t>(int64_t(chunk), end - at));
        if (!decoder->read(at, count, pcm.data())) return fail("decoding failed");
        fwrite(pcm.data(), sizeof(float) * size_t(decoder->channels()), count, stdout);
        at += int64_t(count);
    }
    return 0;
}

int pcm(const Args& args) {
    if (args.plain.empty()) return fail("usage: pcm <file> --rate R --channels C [--start sec | --first frame] [--frames n] [--format f32le|s16le] [--chunk n] [--list start:frames,...]");
    std::string error;
    int rate = int(args.number("rate", 48000));
    int channels = int(args.number("channels", 2));
    std::string format = args.get("format", "f32le");
    if (format != "f32le" && format != "s16le") return fail("format must be f32le or s16le");
    auto reader = PcmReader::open(args.plain[0], rate, channels, &error);
    if (!reader) return fail(error);
    fprintf(stderr, "%d Hz, %d channels, %lld frames\n", rate, channels, (long long)reader->length());
    if (args.has("list")) return format == "f32le" && spans(*reader, channels, args.get("list")) ? 0 : fail("bad --list, expected start:frames,start:frames with f32le");
    int64_t at = args.has("first") ? args.number("first", 0) : int64_t(std::llround(atof(args.get("start", "0").c_str()) * rate));
    int64_t end = args.has("frames") ? at + args.number("frames", 0) : reader->length();
    size_t chunk = size_t(std::max(1L, args.number("chunk", long(kChunk))));
    float gain = 1;
    if (format == "s16le") {
        std::unique_ptr<Reader> file = Reader::open(args.plain[0]);
        const Track* track = file ? file->first(Kind::Audio) : nullptr;
        auto decoder = track ? AudioDecoder::open(*file, *track) : nullptr;
        double peak = decoder ? mixPeak(decoder->channels(), channels) : 1;
        if (peak > 1) gain = float(1 / peak);
    }
    std::vector<float> pcm(chunk * size_t(channels));
    std::vector<int16_t> whole(chunk * size_t(channels));
    while (at < end) {
        size_t count = size_t(std::min<int64_t>(int64_t(chunk), end - at));
        if (!reader->read(at, count, pcm.data())) return fail("decoding failed");
        if (format == "f32le") {
            fwrite(pcm.data(), sizeof(float) * size_t(channels), count, stdout);
        } else {
            for (size_t i = 0; i < count * size_t(channels); ++i) whole[i] = int16_t(std::clamp(std::lrintf(pcm[i] * gain * 32768.0f), -32768L, 32767L));
            fwrite(whole.data(), sizeof(int16_t) * size_t(channels), count, stdout);
        }
        at += int64_t(count);
    }
    return 0;
}

int resample(const Args& args) {
    if (!args.has("from") || !args.has("to")) return fail("usage: resample --from R --to R [--channels C] [--to-channels C] [--chunk n] < f32le > f32le");
    int inChannels = int(args.number("channels", 1));
    int outChannels = int(args.number("to-channels", inChannels));
    size_t chunk = size_t(std::max(1L, args.number("chunk", long(kChunk))));
    Resampler resampler(int(args.number("from", 0)), inChannels, int(args.number("to", 0)), outChannels);
    std::vector<float> in(chunk * size_t(inChannels));
    std::vector<float> out(kChunk * size_t(outChannels));
    auto drain = [&] {
        while (size_t made = resampler.pull(out.data(), kChunk)) fwrite(out.data(), sizeof(float) * size_t(outChannels), made, stdout);
    };
    while (size_t got = fread(in.data(), sizeof(float) * size_t(inChannels), chunk, stdin)) {
        resampler.push(in.data(), got);
        drain();
    }
    resampler.finish();
    drain();
    return 0;
}

Register registerAdecode("adecode", adecode);
Register registerPcm("pcm", pcm);
Register registerResample("resample", resample);

}

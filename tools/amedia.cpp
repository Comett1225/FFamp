#include "absent/media.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

using namespace absent;

namespace {

double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Args {
    std::vector<std::string> plain;
    std::map<std::string, std::string> named;
    std::string get(const std::string& key, const std::string& fallback = "") const {
        auto found = named.find(key);
        return found == named.end() ? fallback : found->second;
    }
    long number(const std::string& key, long fallback) const {
        auto found = named.find(key);
        return found == named.end() ? fallback : atol(found->second.c_str());
    }
    bool has(const std::string& key) const { return named.count(key) > 0; }
};

Args parse(int argc, char** argv) {
    Args args;
    for (int i = 2; i < argc; ++i) {
        std::string token = argv[i];
        if (token.rfind("--", 0) == 0) {
            bool value = i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0;
            args.named[token.substr(2)] = value ? argv[++i] : "1";
        } else {
            args.plain.push_back(token);
        }
    }
    return args;
}

int fail(const std::string& text) {
    fprintf(stderr, "amedia: %s\n", text.c_str());
    return 1;
}

TrackSpec specOf(const Track& t) {
    TrackSpec spec;
    spec.kind = t.kind;
    spec.codec = t.codec;
    spec.timescale = t.timescale;
    spec.width = t.width;
    spec.height = t.height;
    spec.rotation = t.rotation;
    spec.sampleRate = t.sampleRate;
    spec.channels = t.channels;
    spec.configType = t.configType;
    spec.config = t.config;
    spec.color = t.color;
    int64_t first = t.samples.empty() ? 0 : t.samples.front().pts;
    for (const Sample& s : t.samples) first = std::min(first, s.pts);
    spec.skip = std::max<int64_t>(0, t.mediaStart - first);
    return spec;
}

struct Copier {
    Mp4Reader& reader;
    Mp4Writer& writer;
    const Track& track;
    int out;
    uint32_t next = 0;
    std::vector<uint8_t> buffer;

    double time() const { return next < track.samples.size() ? double(track.samples[next].dts) / track.timescale : 1e300; }
    bool step() {
        const Sample& s = track.samples[next];
        buffer.resize(s.size);
        if (!reader.read(track, next, buffer.data())) return false;
        ++next;
        return writer.write(out, buffer.data(), s.size, s.pts, s.dts, s.duration, s.sync);
    }
    bool until(double seconds) {
        while (time() <= seconds) if (!step()) return false;
        return true;
    }
};

int probe(const Args& args) {
    if (args.plain.empty()) return fail("probe needs a file");
    std::string error;
    auto reader = Mp4Reader::open(args.plain[0], &error);
    if (!reader) return fail(error);
    for (const Track& t : reader->tracks()) {
        if (t.kind == Kind::Video) {
            printf("track %u video %s %dx%d rotation %d frames %zu duration %.3f fps %.3f timescale %u keyframes %zu start %lld\n", t.id, fourccText(t.codec).c_str(),
                t.width, t.height, t.rotation, t.order.size(), t.seconds(), t.rate(), t.timescale, t.syncs.size(), (long long)t.mediaStart);
        } else if (t.kind == Kind::Audio) {
            printf("track %u audio %s %d Hz %d ch packets %zu duration %.3f timescale %u start %lld\n", t.id, fourccText(t.codec).c_str(), t.sampleRate, t.channels,
                t.samples.size(), t.seconds(), t.timescale, (long long)t.mediaStart);
        } else {
            printf("track %u other %s samples %zu\n", t.id, fourccText(t.codec).c_str(), t.samples.size());
        }
    }
    return 0;
}

int decode(const Args& args) {
    if (args.plain.empty()) return fail("decode needs a file");
    std::string error;
    auto reader = Mp4Reader::open(args.plain[0], &error);
    if (!reader) return fail(error);
    const Track* track = reader->first(Kind::Video);
    if (!track) return fail("no video track");
    auto decoder = VideoDecoder::open(*reader, *track, &error);
    if (!decoder) return fail(error);
    std::vector<uint8_t> picture;
    uint32_t count = decoder->frames();
    for (uint32_t n = 0; n < count; ++n) {
        Frame frame = decoder->frame(n);
        if (!frame || !copyI420(frame, picture)) return fail("frame " + std::to_string(n) + " did not decode");
        if (fwrite(picture.data(), 1, picture.size(), stdout) != picture.size()) return fail("output closed");
    }
    return 0;
}

int frames(const Args& args) {
    if (args.plain.empty() || !args.has("list")) return fail("frames needs a file and --list a,b,c");
    std::string error;
    auto reader = Mp4Reader::open(args.plain[0], &error);
    if (!reader) return fail(error);
    const Track* track = reader->first(Kind::Video);
    if (!track) return fail("no video track");
    auto decoder = VideoDecoder::open(*reader, *track, &error);
    if (!decoder) return fail(error);
    std::vector<uint8_t> picture;
    std::vector<double> times;
    std::string list = args.get("list");
    size_t at = 0;
    while (at < list.size()) {
        size_t comma = list.find(',', at);
        if (comma == std::string::npos) comma = list.size();
        uint32_t n = uint32_t(atol(list.substr(at, comma - at).c_str()));
        at = comma + 1;
        uint64_t before = decoder->fed();
        double start = now();
        Frame frame = decoder->frame(n);
        if (!frame || !copyI420(frame, picture)) return fail("frame " + std::to_string(n) + " did not decode");
        double spent = (now() - start) * 1000;
        times.push_back(spent);
        fprintf(stderr, "seek %u ms %.2f fed %llu\n", n, spent, (unsigned long long)(decoder->fed() - before));
        if (fwrite(picture.data(), 1, picture.size(), stdout) != picture.size()) return fail("output closed");
    }
    if (!times.empty()) {
        std::sort(times.begin(), times.end());
        double sum = 0;
        for (double t : times) sum += t;
        fprintf(stderr, "seeks %zu mean_ms %.2f median_ms %.2f max_ms %.2f\n", times.size(), sum / times.size(), times[times.size() / 2], times.back());
    }
    return 0;
}

int bench(const Args& args) {
    if (args.plain.empty()) return fail("bench needs a file");
    std::string error;
    auto reader = Mp4Reader::open(args.plain[0], &error);
    if (!reader) return fail(error);
    const Track* track = reader->first(Kind::Video);
    if (!track) return fail("no video track");
    auto decoder = VideoDecoder::open(*reader, *track, &error);
    if (!decoder) return fail(error);
    std::string mode = args.get("mode", "async");
    double start = now();
    uint64_t count = 0;
    if (mode == "copy") {
        std::vector<uint8_t> picture;
        for (uint32_t n = 0; n < decoder->frames(); ++n) {
            Frame frame = decoder->frame(n);
            if (!frame || !copyI420(frame, picture)) return fail("frame " + std::to_string(n) + " did not decode");
            ++count;
        }
    } else if (mode == "frames") {
        for (uint32_t n = 0; n < decoder->frames(); ++n) {
            if (!decoder->frame(n)) return fail("frame " + std::to_string(n) + " did not decode");
            ++count;
        }
    } else {
        count = decoder->run(mode == "async");
    }
    double spent = now() - start;
    printf("mode %s frames %llu seconds %.3f fps %.1f\n", mode.c_str(), (unsigned long long)count, spent, count / spent);
    return 0;
}

int encode(const Args& args) {
    if (args.plain.empty() || !args.has("video") || !args.has("size")) return fail("encode needs an output file, --video raw.yuv and --size WxH");
    int width = 0, height = 0;
    if (sscanf(args.get("size").c_str(), "%dx%d", &width, &height) != 2 || width <= 0 || height <= 0 || width > 16384 || height > 16384) return fail("bad --size");
    long num = 30, den = 1;
    std::string fps = args.get("fps", "30");
    if (sscanf(fps.c_str(), "%ld/%ld", &num, &den) < 1 || num <= 0 || den <= 0) return fail("bad --fps");
    FILE* video = fopen(args.get("video").c_str(), "rb");
    if (!video) return fail("cannot open the raw video");
    size_t frameSize = size_t(width) * height + 2 * size_t((width + 1) / 2) * ((height + 1) / 2);
    fseeko(video, 0, SEEK_END);
    long available = long(ftello(video) / off_t(frameSize));
    fseeko(video, 0, SEEK_SET);
    long total = std::min(available, args.number("frames", available));
    bool hevc = args.get("codec", "h264") == "hevc";

    std::string error;
    EncodeSpec spec;
    spec.width = width;
    spec.height = height;
    spec.timescale = uint32_t(num);
    spec.fps = double(num) / den;
    spec.bitrate = int(args.number("bitrate", 8000000));
    spec.keyInterval = int(args.number("gop", long(spec.fps + 0.5)));
    spec.hevc = hevc;
    spec.reorder = !args.has("no-reorder");
    auto encoder = VideoEncoder::open(spec, &error);
    if (!encoder) return fail(error);
    auto writer = Mp4Writer::open(args.plain[0], &error);
    if (!writer) return fail(error);

    TrackSpec picture;
    picture.kind = Kind::Video;
    picture.codec = hevc ? fourcc("hvc1") : fourcc("avc1");
    picture.timescale = uint32_t(num);
    picture.width = width;
    picture.height = height;
    int videoTrack = writer->add(picture);

    FILE* audio = nullptr;
    std::unique_ptr<AacEncoder> aac;
    int audioTrack = -1;
    int rate = int(args.number("rate", 48000)), channels = int(args.number("channels", 2));
    long soundFrames = 0;
    if (args.has("audio")) {
        audio = fopen(args.get("audio").c_str(), "rb");
        if (!audio) return fail("cannot open the raw audio");
        aac = AacEncoder::open(rate, channels, int(args.number("abitrate", 160000)), &error);
        if (!aac) return fail(error);
        fseeko(audio, 0, SEEK_END);
        soundFrames = long(ftello(audio) / off_t(channels * 2));
        fseeko(audio, 0, SEEK_SET);
        soundFrames = std::min(soundFrames, long(double(total) * den / num * rate + 0.5));
        TrackSpec sound;
        sound.kind = Kind::Audio;
        sound.codec = fourcc("mp4a");
        sound.timescale = uint32_t(rate);
        sound.sampleRate = rate;
        sound.channels = channels;
        sound.configType = fourcc("esds");
        sound.config = aac->config();
        sound.skip = aac->priming();
        sound.length = soundFrames;
        audioTrack = writer->add(sound);
    }

    int64_t soundPackets = 0;
    AudioSink sink = [&](const uint8_t* data, size_t size) {
        writer->write(audioTrack, data, size, soundPackets * 1024, soundPackets * 1024, 1024, true);
        ++soundPackets;
    };
    bool configured = false;
    auto drain = [&] {
        Packet packet;
        while (encoder->take(packet)) {
            if (!configured) {
                writer->configure(videoTrack, encoder->configType(), encoder->config());
                configured = true;
            }
            writer->write(videoTrack, packet.data.data(), packet.data.size(), packet.pts, packet.dts, packet.duration ? packet.duration : uint32_t(den), packet.sync);
        }
    };

    std::vector<uint8_t> raw(frameSize);
    std::vector<int16_t> pcm;
    long soundDone = 0;
    double start = now();
    for (long n = 0; n < total; ++n) {
        if (fread(raw.data(), 1, frameSize, video) != frameSize) return fail("raw video ended early");
        if (!encoder->encodeI420(raw.data(), n * den, den)) return fail("the encoder refused a frame");
        if (aac) {
            long want = std::min(soundFrames, long(double(n + 1) * den / num * rate)) - soundDone;
            if (want > 0) {
                pcm.resize(size_t(want) * channels);
                size_t got = fread(pcm.data(), size_t(channels) * 2, size_t(want), audio);
                if (!aac->encode(pcm.data(), got, sink)) return fail("the AAC encoder failed");
                soundDone += long(got);
            }
        }
        drain();
    }
    if (!encoder->finish()) return fail("the encoder failed");
    drain();
    if (aac && !aac->finish(sink)) return fail("the AAC encoder failed");
    if (!writer->finish(&error)) return fail(error);
    double spent = now() - start;
    fprintf(stderr, "encoded %ld frames in %.3f s (%.1f fps)\n", total, spent, total / spent);
    return 0;
}

int transcode(const Args& args) {
    if (args.plain.size() < 2) return fail("transcode needs an input and an output file");
    std::string error;
    auto reader = Mp4Reader::open(args.plain[0], &error);
    if (!reader) return fail(error);
    const Track* track = reader->first(Kind::Video);
    if (!track) return fail("no video track");
    auto decoder = VideoDecoder::open(*reader, *track, &error);
    if (!decoder) return fail(error);
    bool hevc = args.get("codec", "h264") == "hevc";
    EncodeSpec spec;
    spec.width = track->width;
    spec.height = track->height;
    spec.timescale = track->timescale;
    spec.fps = track->rate();
    spec.bitrate = int(args.number("bitrate", 8000000));
    spec.keyInterval = int(spec.fps + 0.5);
    spec.hevc = hevc;
    auto encoder = VideoEncoder::open(spec, &error);
    if (!encoder) return fail(error);
    auto writer = Mp4Writer::open(args.plain[1], &error);
    if (!writer) return fail(error);
    TrackSpec picture;
    picture.kind = Kind::Video;
    picture.codec = hevc ? fourcc("hvc1") : fourcc("avc1");
    picture.timescale = track->timescale;
    picture.width = track->width;
    picture.height = track->height;
    picture.rotation = track->rotation;
    int videoTrack = writer->add(picture);
    const Track* sound = reader->first(Kind::Audio);
    std::unique_ptr<Copier> copier;
    if (sound && !sound->config.empty()) copier.reset(new Copier{*reader, *writer, *sound, writer->add(specOf(*sound))});
    bool configured = false;
    auto drain = [&] {
        Packet packet;
        while (encoder->take(packet)) {
            if (!configured) {
                writer->configure(videoTrack, encoder->configType(), encoder->config());
                configured = true;
            }
            writer->write(videoTrack, packet.data.data(), packet.data.size(), packet.pts, packet.dts, packet.duration, packet.sync);
        }
    };
    double start = now();
    uint32_t count = decoder->frames();
    for (uint32_t n = 0; n < count; ++n) {
        Frame frame = decoder->frame(n);
        if (!frame) return fail("frame " + std::to_string(n) + " did not decode");
        const Sample& s = track->samples[track->order[n]];
        if (!encoder->encode(frame, s.pts - track->mediaStart, s.duration)) return fail("the encoder refused a frame");
        drain();
        if (copier && !copier->until(double(s.pts - track->mediaStart) / track->timescale)) return fail("copying audio failed");
    }
    if (!encoder->finish()) return fail("the encoder failed");
    drain();
    if (copier && !copier->until(1e299)) return fail("copying audio failed");
    if (!writer->finish(&error)) return fail(error);
    double spent = now() - start;
    fprintf(stderr, "transcoded %u frames in %.3f s (%.1f fps)\n", count, spent, count / spent);
    return 0;
}

int remux(const Args& args) {
    if (args.plain.size() < 2) return fail("remux needs an input and an output file");
    std::string error;
    auto reader = Mp4Reader::open(args.plain[0], &error);
    if (!reader) return fail(error);
    auto writer = Mp4Writer::open(args.plain[1], &error);
    if (!writer) return fail(error);
    std::vector<Copier> copiers;
    for (const Track& t : reader->tracks()) {
        if (t.kind == Kind::Other || t.config.empty() || t.samples.empty()) continue;
        if (t.kind == Kind::Audio && args.has("video-only")) continue;
        copiers.push_back(Copier{*reader, *writer, t, writer->add(specOf(t))});
    }
    if (copiers.empty()) return fail("nothing to copy");
    for (;;) {
        Copier* best = nullptr;
        for (Copier& c : copiers) if (c.next < c.track.samples.size() && (!best || c.time() < best->time())) best = &c;
        if (!best) break;
        if (!best->step()) return fail("copying failed");
    }
    if (!writer->finish(&error)) return fail(error);
    return 0;
}

}

int main(int argc, char** argv) {
    if (argc < 2) return fail("commands: probe, decode, frames, bench, encode, transcode, remux");
    std::string command = argv[1];
    Args args = parse(argc, argv);
    if (command == "probe") return probe(args);
    if (command == "decode") return decode(args);
    if (command == "frames") return frames(args);
    if (command == "bench") return bench(args);
    if (command == "encode") return encode(args);
    if (command == "transcode") return transcode(args);
    if (command == "remux") return remux(args);
    return fail("unknown command " + command);
}

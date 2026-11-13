#include "cli.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace cli {

namespace {

int probe(const Args& args) {
    if (args.plain.empty()) return fail("probe needs a file");
    std::string error;
    auto reader = Reader::open(args.plain[0], &error);
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
    auto reader = Reader::open(args.plain[0], &error);
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
    auto reader = Reader::open(args.plain[0], &error);
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
    auto reader = Reader::open(args.plain[0], &error);
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
    FILE* video = args.get("video") == "-" ? stdin : fopen(args.get("video").c_str(), "rb");
    if (!video) return fail("cannot open the raw video");
    bool redFirst = args.get("format", "i420") == "rgb0";
    bool bgra = redFirst || args.get("format", "i420") == "bgra";
    bool piped = args.get("video") == "-";
    size_t frameSize = bgra ? size_t(width) * height * 4 : size_t(width) * height + 2 * size_t((width + 1) / 2) * ((height + 1) / 2);
    long available = 0x7FFFFFFF;
    if (!piped) {
        fseeko(video, 0, SEEK_END);
        available = long(ftello(video) / off_t(frameSize));
        fseeko(video, 0, SEEK_SET);
    }
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
    spec.bgra = bgra;
    auto encoder = VideoEncoder::open(spec, &error);
    if (!encoder) return fail(error);
    auto writer = Writer::open(args.plain[0], &error);
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
    int step = 1024;
    bool opus = args.get("acodec", "aac") == "opus";
#ifdef ABSENT_OPUS
    std::unique_ptr<OpusEncoder> voice;
#endif
    if (args.has("audio")) {
        audio = fopen(args.get("audio").c_str(), "rb");
        if (!audio) return fail("cannot open the raw audio");
        if (opus) {
#ifdef ABSENT_OPUS
            if (rate != 48000) return fail("Opus needs 48000 Hz input");
            voice = OpusEncoder::open(channels, int(args.number("abitrate", 128000)), &error);
            if (!voice) return fail(error);
            step = 960;
#else
            return fail("built without Opus");
#endif
        } else {
            aac = AacEncoder::open(rate, channels, int(args.number("abitrate", 160000)), &error);
            if (!aac) return fail(error);
        }
        fseeko(audio, 0, SEEK_END);
        soundFrames = long(ftello(audio) / off_t(channels * 2));
        fseeko(audio, 0, SEEK_SET);
        if (!piped) soundFrames = std::min(soundFrames, long(double(total) * den / num * rate + 0.5));
        TrackSpec sound;
        sound.kind = Kind::Audio;
        sound.timescale = uint32_t(rate);
        sound.sampleRate = rate;
        sound.channels = channels;
        sound.length = soundFrames;
        if (aac) {
            sound.codec = fourcc("mp4a");
            sound.configType = fourcc("esds");
            sound.config = aac->config();
            sound.skip = aac->priming();
        }
#ifdef ABSENT_OPUS
        if (voice) {
            sound.codec = fourcc("Opus");
            sound.configType = fourcc("OpHd");
            sound.config = voice->config();
            sound.skip = voice->priming();
        }
#endif
        audioTrack = writer->add(sound);
    }
    auto sound = [&](const int16_t* pcm, size_t count, bool last, const AudioSink& sink) {
#ifdef ABSENT_OPUS
        if (voice) return last ? voice->finish(sink) : voice->encode(pcm, count, sink);
#endif
        return last ? aac->finish(sink) : aac->encode(pcm, count, sink);
    };

    int64_t soundPackets = 0;
    AudioSink sink = [&](const uint8_t* data, size_t size) {
        writer->write(audioTrack, data, size, soundPackets * step, soundPackets * step, uint32_t(step), true);
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
    long done = 0;
    for (long n = 0; n < total; ++n) {
        size_t got = 0;
        while (got < frameSize) {
            size_t more = fread(raw.data() + got, 1, frameSize - got, video);
            if (!more) break;
            got += more;
        }
        if (got != frameSize) {
            if (piped && got == 0) break;
            return fail("raw video ended early");
        }
        ++done;
        if (!(bgra ? encoder->encodeBGRA(raw.data(), n * den, den, redFirst) : encoder->encodeI420(raw.data(), n * den, den))) return fail("the encoder refused a frame");
        if (audio) {
            long want = std::min(soundFrames, long(double(n + 1) * den / num * rate)) - soundDone;
            if (want > 0) {
                pcm.resize(size_t(want) * channels);
                size_t got = fread(pcm.data(), size_t(channels) * 2, size_t(want), audio);
                if (!sound(pcm.data(), got, false, sink)) return fail("the audio encoder failed");
                soundDone += long(got);
            }
        }
        drain();
    }
    if (!encoder->finish()) return fail("the encoder failed");
    drain();
    total = done;
    while (audio && soundDone < soundFrames) {
        long want = std::min<long>(soundFrames - soundDone, rate);
        pcm.resize(size_t(want) * channels);
        size_t got = fread(pcm.data(), size_t(channels) * 2, size_t(want), audio);
        if (!got) break;
        if (!sound(pcm.data(), got, false, sink)) return fail("the audio encoder failed");
        soundDone += long(got);
    }
    if (audio && !sound(nullptr, 0, true, sink)) return fail("the audio encoder failed");
    if (!writer->finish(&error)) return fail(error);
    double spent = now() - start;
    fprintf(stderr, "encoded %ld frames in %.3f s (%.1f fps)\n", total, spent, total / spent);
    return 0;
}

int transcode(const Args& args) {
    if (args.plain.size() < 2) return fail("transcode needs an input and an output file");
    std::string error;
    auto reader = Reader::open(args.plain[0], &error);
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
    auto writer = Writer::open(args.plain[1], &error);
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
    auto reader = Reader::open(args.plain[0], &error);
    if (!reader) return fail(error);
    auto writer = Writer::open(args.plain[1], &error);
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

Register probeCommand("probe", probe);
Register decodeCommand("decode", decode);
Register framesCommand("frames", frames);
Register benchCommand("bench", bench);
Register encodeCommand("encode", encode);
Register transcodeCommand("transcode", transcode);
Register remuxCommand("remux", remux);

}

}

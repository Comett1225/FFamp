#include "cli.h"
#include "absent/tools.h"
#include <cmath>
#include <csignal>
#include <cstring>

namespace cli {

namespace {

volatile sig_atomic_t stopRequested = 0;

void requestStop(int) { stopRequested = 1; }

Progress watcher() {
    signal(SIGINT, requestStop);
    signal(SIGTERM, requestStop);
    auto last = std::make_shared<int>(-1);
    return [last](double fraction) {
        const int step = int(fraction * 100 + 1e-9);
        if (step != *last) {
            *last = step;
            printf("progress %.2f\n", step / 100.0);
            fflush(stdout);
        }
        return !stopRequested;
    };
}

int done(bool ok, const std::string& error) {
    if (ok) return 0;
    fail(error.empty() ? "failed" : error);
    return error == "cancelled" ? 130 : 1;
}

int64_t microseconds(const Args& args, const char* name) {
    if (args.has(std::string(name) + "-us")) return atoll(args.get(std::string(name) + "-us").c_str());
    return args.has(name) ? std::llround(atof(args.get(name).c_str()) * 1e6) : 0;
}

bool hasExtension(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    const size_t dot = path.find_last_of('.');
    return dot != std::string::npos && (slash == std::string::npos || dot > slash + 1);
}

int compress(const Args& args) {
    if (args.plain.size() < 2) {
        return fail("compress needs <in> <out> [--start s --end s] [--size WxH | --fit WxH] [--fps N or N/D] [--codec h264|hevc|av1|vp9|apv] [--bitrate bits] [--audio copy|aac|none] [--audio-bitrate bits]");
    }
    CompressJob job;
    job.input = args.plain[0];
    job.output = args.plain[1];
    job.startUs = microseconds(args, "start");
    job.endUs = microseconds(args, "end");
    const std::string size = args.has("fit") ? args.get("fit") : args.get("size");
    if (!size.empty() && (sscanf(size.c_str(), "%dx%d", &job.width, &job.height) != 2 || job.width < 0 || job.height < 0 || job.width > 16384 || job.height > 16384)) return fail("bad size");
    job.keepAspect = args.has("fit");
    if (args.has("fps")) {
        long numerator = 0, denominator = 1;
        if (sscanf(args.get("fps").c_str(), "%ld/%ld", &numerator, &denominator) < 1 || numerator <= 0 || denominator <= 0 || numerator > 1000000 || denominator > 1000000) return fail("bad --fps");
        job.fpsNumerator = int(numerator);
        job.fpsDenominator = int(denominator);
    }
    job.codec = videoCodec(args);
    if (!job.codec) return fail("--codec is h264, hevc, av1, vp9 or apv");
    job.bitrate = int(args.number("bitrate", 0));
    const std::string audio = args.get("audio", "copy");
    if (audio != "copy" && audio != "aac" && audio != "none") return fail("--audio is copy, aac or none");
    job.sound = audio == "copy" ? SoundChoice::Copy : audio == "aac" ? SoundChoice::Aac : SoundChoice::None;
    job.audioBitrate = int(args.number("audio-bitrate", 128000));
    std::string error;
    return done(absent::compress(job, watcher(), &error), error);
}

int trim(const Args& args) {
    if (args.plain.size() < 2) return fail("trim needs <in> <out> --start seconds --end seconds");
    std::string error;
    return done(trimCopy(args.plain[0], args.plain[1], microseconds(args, "start"), microseconds(args, "end"), watcher(), &error), error);
}

int extractAudio(const Args& args) {
    if (args.plain.size() < 2) return fail("extract-audio needs <in> <out> [--mp3 | --aac] [--bitrate bits]; without --mp3 or --aac the sound is copied");
    std::string error;
    std::string output = args.plain[1];
    const SoundChoice how = args.has("mp3") ? SoundChoice::Mp3 : args.has("aac") ? SoundChoice::Aac : SoundChoice::Copy;
    if (!hasExtension(output)) {
        const char* extension = how == SoundChoice::Mp3 ? "mp3" : "m4a";
        if (how == SoundChoice::Copy) {
            auto reader = Reader::open(args.plain[0], &error);
            if (!reader) return fail(error);
            const Track* track = reader->first(Kind::Audio);
            if (!track) return fail("no audio track");
            extension = audioCopyExtension(track->codec);
        }
        output += std::string(".") + extension;
        printf("output %s\n", output.c_str());
    }
    return done(absent::extractAudio(args.plain[0], output, how, int(args.number("bitrate", 128000)), watcher(), &error), error);
}

int reverse(const Args& args) {
    if (args.plain.size() < 2) return fail("reverse needs <in> <out.mp4> [--start s --end s] [--bitrate bits] [--audio-bitrate bits] [--memory megabytes]");
    ReverseJob job;
    job.input = args.plain[0];
    job.output = args.plain[1];
    job.startUs = microseconds(args, "start");
    job.endUs = microseconds(args, "end");
    job.bitrate = int(args.number("bitrate", 0));
    job.audioBitrate = int(args.number("audio-bitrate", 128000));
    if (args.has("memory")) job.memoryBytes = uint64_t(std::max(1l, args.number("memory", 1024))) << 20;
    ReverseResult result;
    std::string error;
    if (!absent::reverse(job, watcher(), &result, &error)) return done(false, error);
    printf("reversed %dx%d frames %u duration_us %lld spilled %u\n", result.width, result.height, result.frames, (long long)result.durationUs, result.spilledGroups);
    return 0;
}

void describe(const GifInfo& info) {
    printf("gif %dx%d video %dx%d frames %zu duration_ms %lld fps %d transparent %d\n", info.width, info.height, info.videoWidth, info.videoHeight, info.delaysMs.size(),
        (long long)info.durationMs, info.fps, info.transparent ? 1 : 0);
}

int gifInfo(const Args& args) {
    if (args.plain.empty()) return fail("gif-info needs <in.gif> [--frames out.bgra]");
    FILE* out = args.has("frames") ? openFile(args.get("frames").c_str(), "wb") : nullptr;
    if (args.has("frames") && !out) return fail("cannot create the frame file");
    GifInfo info;
    std::string error;
    bool wrote = true;
    const bool ok = gifRead(args.plain[0], info, [&](uint32_t, const uint8_t* pixels) {
        const size_t size = size_t(info.width) * size_t(info.height) * 4;
        if (out) wrote = wrote && fwrite(pixels, 1, size, out) == size;
        return true;
    }, &error);
    if (out) fclose(out);
    if (!ok) return fail(error);
    if (!wrote) return fail("cannot write the frame file");
    describe(info);
    std::string delays = "delays_ms";
    for (int delay : info.delaysMs) delays += " " + std::to_string(delay);
    printf("%s\n", delays.c_str());
    return 0;
}

int gifVideo(const Args& args) {
    if (args.plain.size() < 2) return fail("gif-video needs <in.gif> <colour.mp4> [<matte.mp4>]; the matte file is written only when the GIF has transparent pixels");
    GifInfo info;
    std::string error;
    if (!gifToVideo(args.plain[0], args.plain[1], args.plain.size() > 2 ? args.plain[2] : "", &info, watcher(), &error)) return done(false, error);
    describe(info);
    return 0;
}

int merge(const Args& args) {
    if (args.plain.size() < 3) return fail("merge needs <video file> <audio file> <out.mp4|out.webm|out.mkv>; an output name without an extension gets the one that fits the codecs");
    std::string error;
    std::string output = args.plain[2];
    if (!hasExtension(output)) {
        auto video = Reader::open(args.plain[0], &error);
        auto audio = video ? Reader::open(args.plain[1], &error) : nullptr;
        if (!audio) return fail(error);
        const Track* picture = video->first(Kind::Video);
        const Track* sound = audio->first(Kind::Audio);
        output += std::string(".") + mergeExtension(picture ? picture->codec : 0, sound ? sound->codec : 0);
        printf("output %s\n", output.c_str());
    }
    return done(absent::merge(args.plain[0], args.plain[1], output, watcher(), &error), error);
}

int stripAudio(const Args& args) {
    if (args.plain.size() < 2) return fail("strip-audio needs <in> <out>");
    std::string error;
    return done(absent::stripAudio(args.plain[0], args.plain[1], watcher(), &error), error);
}

int rewrap(const Args& args) {
    if (args.plain.size() < 2) return fail("rewrap needs <in> <out.mp4|out.mkv|out.webm>");
    std::string error;
    return done(absent::rewrap(args.plain[0], args.plain[1], watcher(), &error), error);
}

Register compressCommand("compress", compress);
Register trimCommand("trim", trim);
Register extractAudioCommand("extract-audio", extractAudio);
Register reverseCommand("reverse", reverse);
Register gifInfoCommand("gif-info", gifInfo);
Register gifVideoCommand("gif-video", gifVideo);
Register mergeCommand("merge", merge);
Register stripAudioCommand("strip-audio", stripAudio);
Register rewrapCommand("rewrap", rewrap);

}

}

#include "cli.h"
#include "absent/capture.h"
#include <cmath>
#include <thread>
#include <unistd.h>

namespace cli {

namespace {

bool sizeOf(const Args& args, int& width, int& height) {
    if (!args.has("size")) return true;
    return sscanf(args.get("size").c_str(), "%dx%d", &width, &height) == 2 && width > 0 && height > 0 && width <= 16384 && height <= 16384;
}

double real(const Args& args, const char* key, double fallback) {
    return args.has(key) ? atof(args.get(key).c_str()) : fallback;
}

int framesOut(const Args& args) {
    if (args.plain.empty()) {
        return fail("frames-out needs <file> [--size WxH] [--format rgba|rgb24|bgra|i420] [--start sec | --frame n] [--count n] [--length sec] [--fps f] [--no-rotate] [--realtime] [--loop] [--software] [--out file] [--info]");
    }
    FrameReaderSpec spec;
    if (!sizeOf(args, spec.width, spec.height)) return fail("bad --size");
    if (!pixelLayoutNamed(args.get("format", "rgba"), spec.layout)) return fail("unknown --format");
    spec.rotate = !args.has("no-rotate");
    spec.software = args.has("software");
    std::string error;
    auto reader = FrameReader::open(args.plain[0], spec, &error);
    if (!reader) return fail(error);
    if (args.has("info")) {
        printf("size %dx%d source %dx%d rotation %d frames %u rate %.6f length %.6f\n", reader->width(), reader->height(), reader->sourceWidth(), reader->sourceHeight(), reader->rotation(),
            reader->frames(), reader->rate(), reader->length());
        return 0;
    }
    const long count = args.number("count", -1);
    const double length = real(args, "length", -1);
    const double fps = real(args, "fps", 0);
    if (args.has("fps") && (!(fps > 0) || fps > 1000)) return fail("bad --fps");
    double start = real(args, "start", 0);
    if (args.has("frame")) {
        const long frame = args.number("frame", 0);
        if (frame < 0 || frame >= long(reader->frames())) return fail("no such frame");
        start = reader->seconds(uint32_t(frame));
    }
    std::vector<uint32_t> list;
    if (fps > 0) {
        list = reader->steady(start, fps, length, count);
    } else {
        for (uint32_t index = reader->indexAt(start); index < reader->frames(); ++index) {
            if (count >= 0 && long(list.size()) >= count) break;
            if (length >= 0 && reader->seconds(index) - start >= length - 5e-7) break;
            list.push_back(index);
        }
    }
    FILE* out = args.has("out") ? fopen(args.get("out").c_str(), "wb") : stdout;
    if (!out) return fail("cannot write " + args.get("out"));
    const bool paced = args.has("realtime"), loop = args.has("loop");
    std::vector<uint8_t> pixels;
    uint32_t held = ~0u;
    do {
        const double began = now();
        for (size_t n = 0; n < list.size(); ++n) {
            if (list[n] != held && !reader->read(list[n], pixels)) return fail("frame " + std::to_string(list[n]) + " did not decode");
            held = list[n];
            if (paced) {
                const double due = fps > 0 ? double(n) / fps : reader->seconds(list[n]) - reader->seconds(list[0]);
                const double wait = began + due - now();
                if (wait > 0) std::this_thread::sleep_for(std::chrono::duration<double>(wait));
            }
            if (fwrite(pixels.data(), 1, pixels.size(), out) != pixels.size()) return out == stdout ? 0 : fail("output closed");
            if (paced) fflush(out);
        }
        if (paced && loop && !list.empty()) {
            const double wait = began + (fps > 0 ? double(list.size()) / fps : reader->length() - reader->seconds(list[0])) - now();
            if (wait > 0) std::this_thread::sleep_for(std::chrono::duration<double>(wait));
        }
    } while (loop && !list.empty());
    if (out != stdout) fclose(out);
    return 0;
}

int framesIn(const Args& args) {
    if (args.plain.empty() || !args.has("size") || !args.has("fps")) {
        return fail("frames-in needs <out> --size WxH --fps F [--format rgba|rgb24|bgra|i420|gray] [--in file] [--codec h264|hevc] [--bitrate bits] [--start sec] [--limit sec] [--audio-from file] [--audio-start sec] [--audio-length sec] [--audio-bitrate bits]");
    }
    FrameWriterSpec spec;
    if (!sizeOf(args, spec.width, spec.height)) return fail("bad --size");
    if (!pixelLayoutNamed(args.get("format", "rgba"), spec.layout)) return fail("unknown --format");
    if (!frameRateFrom(args.get("fps"), spec.rateTop, spec.rateBottom)) return fail("bad --fps");
    const std::string codec = args.get("codec", "h264");
    if (codec != "h264" && codec != "hevc") return fail("--codec is h264 or hevc");
    spec.hevc = codec == "hevc";
    spec.bitrate = int(args.number("bitrate", 0));
    spec.start = real(args, "start", 0);
    spec.limit = real(args, "limit", -1);
    spec.audioFrom = args.get("audio-from");
    spec.audioStart = real(args, "audio-start", 0);
    spec.audioLength = real(args, "audio-length", -1);
    spec.audioBitrate = int(args.number("audio-bitrate", 160000));
    FILE* in = args.has("in") ? fopen(args.get("in").c_str(), "rb") : stdin;
    if (!in) return fail("cannot open " + args.get("in"));
    std::string error;
    auto writer = FrameWriter::open(args.plain[0], spec, &error);
    if (!writer) return fail(error);
    std::vector<uint8_t> pixels(writer->frameBytes());
    const double began = now();
    for (;;) {
        size_t got = 0;
        while (got < pixels.size()) {
            const size_t more = fread(pixels.data() + got, 1, pixels.size() - got, in);
            if (!more) break;
            got += more;
        }
        if (got != pixels.size()) {
            if (got) {
                remove(args.plain[0].c_str());
                return fail("the last frame is incomplete");
            }
            break;
        }
        if (!writer->write(pixels.data())) break;
    }
    if (in != stdin) fclose(in);
    if (!writer->finish(&error)) {
        remove(args.plain[0].c_str());
        return fail(error);
    }
    const double spent = now() - began;
    fprintf(stderr, "wrote %ld frames of %dx%d in %.3f s (%.1f fps)\n", writer->frames(), writer->width(), writer->height(), spent, writer->frames() / std::max(spent, 1e-9));
    return 0;
}

int speech(const Args& args) {
    if (args.plain.size() < 2) return fail("speech needs <in> <out.wav> [--rate R] [--speed s] [--pitch p]");
    std::string error;
    if (!convertSpeech(args.plain[0], args.plain[1], int(args.number("rate", 44100)), real(args, "speed", 1), real(args, "pitch", 1), &error)) {
        remove(args.plain[1].c_str());
        return fail(error);
    }
    return 0;
}

int devices(const Args&) {
    printf("access\tvideo\t%s\n", captureAccessName(captureAccess(true)));
    printf("access\taudio\t%s\n", captureAccessName(captureAccess(false)));
    for (const CaptureDevice& device : captureDevices()) printf("%s\t%s\t%s\n", device.video ? "video" : "audio", device.id.c_str(), device.name.c_str());
    return 0;
}

int capture(const Args& args) {
    if ((!args.has("video") && !args.has("audio")) || (args.plain.empty() && !args.has("preview"))) {
        return fail("capture needs [out.mp4] --video id|default|test [--audio id|default|test] [--seconds n] [--preview WxH|native] [--preview-fps f] [--bitrate bits] [--audio-bitrate bits] [--ask]");
    }
    CaptureSpec spec;
    spec.video = args.get("video");
    spec.audio = args.get("audio");
    if (!args.plain.empty()) spec.record = args.plain[0];
    spec.bitrate = int(args.number("bitrate", 10000000));
    spec.audioBitrate = int(args.number("audio-bitrate", 160000));
    spec.previewFps = real(args, "preview-fps", 15);
    spec.ask = args.has("ask");
    if (args.has("preview")) {
        if (args.get("preview") != "native" && args.get("preview") != "1" && (sscanf(args.get("preview").c_str(), "%dx%d", &spec.previewWidth, &spec.previewHeight) != 2 || spec.previewWidth <= 0 || spec.previewHeight <= 0)) {
            return fail("bad --preview");
        }
        bool told = false;
        spec.preview = [told](const uint8_t* bgra, int width, int height, double) mutable {
            if (!told) fprintf(stderr, "preview %dx%d\n", width, height);
            told = true;
            fwrite(bgra, 1, size_t(width) * size_t(height) * 4, stdout);
            fflush(stdout);
        };
    }
    std::string error;
    auto session = CaptureSession::open(spec, &error);
    if (!session) return fail(error);
    const double seconds = real(args, "seconds", 0);
    const double began = now();
    if (seconds > 0) {
        while (now() - began < seconds && !session->failed()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } else {
        for (int c = getchar(); c != EOF && c != 'q'; c = getchar()) {}
    }
    if (!session->finish(&error)) return fail(error);
    const CaptureTotals totals = session->totals();
    fprintf(stderr, "captured %ld pictures of %dx%d and %ld sound frames in %.3f s, %ld preview frames, %ld silent frames added, %ld sound frames dropped\n", totals.videoFrames, totals.width, totals.height,
        totals.audioFrames, totals.seconds, totals.previewFrames, totals.silenceAdded, totals.soundDropped);
    return 0;
}

Register devicesCommand("devices", devices);
Register captureCommand("capture", capture);
Register framesOutCommand("frames-out", framesOut);
Register speechCommand("speech", speech);
Register framesInCommand("frames-in", framesIn);

}

}

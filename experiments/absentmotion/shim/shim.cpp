#include "absent/audio.h"
#include "absent/probe.h"
#include "cli.h"
#include <cmath>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <unistd.h>

namespace cli {

std::map<std::string, Command>& commands() {
    static std::map<std::string, Command> all;
    return all;
}

}

namespace {

using namespace absent;
using Words = std::vector<std::string>;
using Found = std::map<std::string, std::string>;

const int kRefused = 64;
const char* kScale = "scale=out_color_matrix=bt709:out_range=tv";

std::string self;
std::string role;
Words given;
double startedAt = 0;

void note(const std::string& text) {
    const char* path = getenv("ABSENT_SHIM_LOG");
    if (!path || !*path) return;
    std::string line = "absent-media stand-in " + role + " pid " + std::to_string(getpid()) + " (" + self + ") " + text + "\n";
    int file = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (file < 0) return;
    (void)!write(file, line.data(), line.size());
    close(file);
}

std::string shown(const Words& words) {
    std::string text;
    for (const std::string& word : words) text += " [" + word + "]";
    return text;
}

int done(int code) {
    char spent[64];
    snprintf(spent, sizeof spent, "%.0f ms", (cli::now() - startedAt) * 1000);
    note("exit " + std::to_string(code) + " after " + spent);
    return code;
}

bool match(const Words& pattern, Found& found) {
    if (pattern.size() != given.size()) return false;
    Found got;
    for (size_t i = 0; i < pattern.size(); ++i) {
        const std::string& p = pattern[i];
        size_t open = p.find('{'), close = p.rfind('}');
        if (open == std::string::npos || close == std::string::npos) {
            if (p != given[i]) return false;
            continue;
        }
        std::string before = p.substr(0, open), after = p.substr(close + 1);
        const std::string& word = given[i];
        if (word.size() < before.size() + after.size() || word.compare(0, before.size(), before) != 0 ||
            word.compare(word.size() - after.size(), after.size(), after) != 0) return false;
        got[p.substr(open + 1, close - open - 1)] = word.substr(before.size(), word.size() - before.size() - after.size());
    }
    found = got;
    return true;
}

bool wholeNumber(const std::string& text, long& out) {
    char* end = nullptr;
    out = strtol(text.c_str(), &end, 10);
    return !text.empty() && !*end && out > 0;
}

bool seconds(const std::string& text, double& out) {
    char* end = nullptr;
    out = strtod(text.c_str(), &end);
    return !text.empty() && !*end && std::isfinite(out) && out >= 0;
}

double onFileClock(const Track& t, int64_t stamp) {
    return double(int64_t(uint64_t(stamp) - uint64_t(t.mediaStart) + uint64_t(t.delay))) / t.timescale;
}

double fileStart(const Reader& reader) {
    double begin = 1e300;
    for (const Track& t : reader.tracks()) {
        if (t.timescale && !t.samples.empty() && t.kind != Kind::Other) begin = std::min(begin, double(t.delay) / t.timescale);
    }
    return begin == 1e300 ? 0 : begin;
}

int describe(const std::string& path) {
    std::string error;
    std::string json = probeJson(path, false, &error);
    if (json.empty()) return cli::fail(path + ": " + error);
    fputs(json.c_str(), stdout);
    return 0;
}

int times(const std::string& path, bool decoded, const std::string& interval) {
    std::string error;
    auto reader = Reader::open(path, &error);
    if (!reader) return cli::fail(path + ": " + error);
    const Track* track = reader->first(Kind::Video);
    if (!track || track->order.empty()) return 0;
    bool firstOnly = interval == "%+#1";
    double from = 0;
    if (!firstOnly && (interval.empty() || interval.back() != '%' || !seconds(interval.substr(0, interval.size() - 1), from))) return -1;
    if (firstOnly && !decoded) return -1;
    from += fileStart(*reader);
    uint32_t key = track->syncs.empty() ? 0 : track->syncs[0];
    for (uint32_t index : track->syncs) {
        if (index < track->samples.size() && onFileClock(*track, track->samples[index].pts) <= from + 1e-9) key = index;
    }
    if (!decoded) {
        for (uint32_t n = key; n < track->samples.size(); ++n) printf("%.6f\n", onFileClock(*track, track->samples[n].pts));
        return 0;
    }
    auto decoder = VideoDecoder::open(*reader, *track, &error);
    if (!decoder) return cli::fail(path + ": " + error);
    uint32_t first = 0;
    if (!firstOnly) {
        while (first < track->order.size() && track->order[first] != key) ++first;
    }
    uint32_t end = firstOnly ? std::min<uint32_t>(1, decoder->frames()) : decoder->frames();
    for (uint32_t n = first; n < end; ++n) {
        Frame frame = decoder->frame(n);
        if (!frame) return cli::fail(path + ": frame " + std::to_string(n) + " cannot be decoded");
        printf("%.6f\n", onFileClock(*track, track->samples[track->order[n]].pts));
    }
    return 0;
}

int listEncoders() {
    puts("Encoders:");
    puts(" V....D h264_videotoolbox    VideoToolbox H.264 Encoder (codec h264)");
    puts(" A....D aac_at               aac (AudioToolbox) (codec aac)");
    return 0;
}

int sound(const Found& f) {
    long rate = 0, channels = 0;
    double before = 0, after = 0;
    if (!wholeNumber(f.at("rate"), rate) || !wholeNumber(f.at("channels"), channels)) return -1;
    if (f.count("before") && !seconds(f.at("before"), before)) return -1;
    if (f.count("after") && !seconds(f.at("after"), after)) return -1;
    std::string error;
    auto file = Reader::open(f.at("file"), &error);
    if (!file) return cli::fail(f.at("file") + ": " + error);
    const Track* track = file->first(Kind::Audio);
    if (!track) return cli::fail(f.at("file") + ": no audio track");
    double lead = double(track->delay) / track->timescale - fileStart(*file);
    auto reader = PcmReader::open(f.at("file"), int(rate), int(channels), &error);
    if (!reader) return cli::fail(f.at("file") + ": " + error);
    int64_t at = int64_t(std::llround(before * double(rate))) + int64_t(std::llround(after * double(rate))) - int64_t(std::llround(lead * double(rate)));
    const size_t chunk = 4096;
    std::vector<float> pcm(chunk * size_t(channels));
    while (at < reader->length()) {
        size_t count = size_t(std::min<int64_t>(int64_t(chunk), reader->length() - at));
        if (!reader->read(at, count, pcm.data())) return cli::fail("decoding failed");
        if (fwrite(pcm.data(), sizeof(float) * size_t(channels), count, stdout) != count) return 0;
        at += int64_t(count);
    }
    return 0;
}

bool pictureSettings(const Found& f, EncodeSpec& spec) {
    long fps = 0, bitrate = 0, gop = 0;
    if (f.at("codec") != "h264_videotoolbox") return false;
    if (sscanf(f.at("size").c_str(), "%dx%d", &spec.width, &spec.height) != 2 || spec.width < 2 || spec.height < 2) return false;
    if (!wholeNumber(f.at("fps"), fps) || !wholeNumber(f.at("bitrate"), bitrate) || !wholeNumber(f.at("gop"), gop)) return false;
    spec.timescale = uint32_t(fps);
    spec.fps = double(fps);
    spec.bitrate = int(bitrate);
    spec.keyInterval = int(gop);
    spec.codec = fourcc("avc1");
    spec.reorder = false;
    return true;
}

int tryEncoder(const Found& f) {
    EncodeSpec spec;
    if (!pictureSettings(f, spec)) return -1;
    std::string error;
    auto encoder = VideoEncoder::open(spec, &error);
    if (!encoder) return cli::fail(error);
    std::vector<uint8_t> black(size_t(spec.width) * spec.height + 2 * size_t((spec.width + 1) / 2) * ((spec.height + 1) / 2), 128);
    std::fill(black.begin(), black.begin() + long(spec.width) * spec.height, 16);
    for (int n = 0; n < 2; ++n) if (!encoder->encodeI420(black.data(), n, 1)) return cli::fail("the encoder refused a frame");
    if (!encoder->finish()) return cli::fail("the encoder failed");
    Packet packet;
    int packets = 0;
    while (encoder->take(packet)) ++packets;
    return packets == 2 ? 0 : cli::fail("the encoder gave no pictures");
}

int exportMovie(const Found& f) {
    EncodeSpec spec;
    long rate = 0, soundBitrate = 0;
    if (!pictureSettings(f, spec) || !wholeNumber(f.at("rate"), rate) || !wholeNumber(f.at("kbit"), soundBitrate)) return -1;
    if (f.at("outRate") != f.at("rate") || f.at("outChannels") != "2") return -1;
    const std::string& out = f.at("out");
    if (out.size() < 5 || out.compare(out.size() - 4, 4, ".mp4") != 0) return -1;
    cli::Args args;
    args.plain = {out};
    args.named = {{"video", "-"}, {"format", "rgb0"}, {"size", f.at("size")}, {"fps", f.at("fps")}, {"codec", "h264"}, {"bitrate", f.at("bitrate")}, {"gop", f.at("gop")},
        {"no-reorder", "1"}, {"audio", f.at("pcm")}, {"rate", f.at("rate")}, {"channels", "2"}, {"abitrate", std::to_string(soundBitrate * 1000)}};
    return cli::commands().at("encode")(args);
}

int asProbe() {
    Found f;
    if (match({"-v", "error", "-show_streams", "-show_format", "-of", "json", "{file}"}, f)) return describe(f["file"]);
    if (match({"-v", "error", "-show_entries", "stream=codec_type,codec_name,duration:stream_tags=DURATION:format=duration", "-of", "json", "{file}"}, f)) return describe(f["file"]);
    if (match({"-v", "error", "-select_streams", "v:0", "-read_intervals", "{from}", "-show_entries", "frame=pts_time", "-of", "csv=p=0", "{file}"}, f)) return times(f["file"], true, f["from"]);
    if (match({"-v", "error", "-select_streams", "v:0", "-read_intervals", "{from}", "-show_entries", "packet=pts_time", "-of", "csv=p=0", "{file}"}, f)) return times(f["file"], false, f["from"]);
    return -1;
}

int asEncoder() {
    Found f;
    if (match({"-hide_banner", "-encoders"}, f)) return listEncoders();
    const Words tail = {"-map", "0:a:0", "-vn", "-sn", "-dn", "-af", "aresample=async=1:first_pts=0", "-ar", "{rate}", "-ac", "{channels}", "-f", "f32le", "-"};
    for (int shape = 0; shape < 4; ++shape) {
        Words pattern = {"-nostdin", "-v", "error"};
        if (shape & 1) pattern.insert(pattern.end(), {"-ss", "{before}"});
        pattern.insert(pattern.end(), {"-i", "{file}"});
        if (shape & 2) pattern.insert(pattern.end(), {"-ss", "{after}"});
        pattern.insert(pattern.end(), tail.begin(), tail.end());
        if (match(pattern, f)) return sound(f);
    }
    const Words picture = {"-c:v", "{codec}", "-realtime", "0", "-profile:v", "high", "-b:v", "{bitrate}", "-g", "{gop}", "-pix_fmt", "yuv420p", "-colorspace", "bt709",
        "-color_primaries", "bt709", "-color_trc", "bt709", "-color_range", "tv"};
    Words trial = {"-v", "error", "-f", "lavfi", "-i", "color=c=black:s={size}:d=0.2", "-vf", kScale};
    trial.insert(trial.end(), picture.begin(), picture.end());
    trial.insert(trial.end(), {"-frames:v", "2", "-f", "null", "-"});
    if (match(trial, f)) {
        size_t split = f["size"].find(":r=");
        if (split == std::string::npos) return -1;
        f["fps"] = f["size"].substr(split + 3);
        f["size"].resize(split);
        return tryEncoder(f);
    }
    Words movie = {"-hide_banner", "-v", "error", "-f", "rawvideo", "-pix_fmt", "rgb0", "-s", "{size}", "-r", "{fps}", "-i", "-", "-f", "s16le", "-ar", "{rate}", "-ac", "2",
        "-i", "{pcm}", "-map", "0:v", "-vf", kScale};
    movie.insert(movie.end(), picture.begin(), picture.end());
    movie.insert(movie.end(), {"-map", "1:a", "-c:a", "aac_at", "-b:a", "{kbit}k", "-ar", "{outRate}", "-ac", "{outChannels}", "-f", "mp4", "-movflags", "+faststart", "-y", "{out}"});
    if (match(movie, f)) return exportMovie(f);
    return -1;
}

}

int main(int argc, char** argv) {
    startedAt = cli::now();
    signal(SIGPIPE, SIG_IGN);
    char path[4096];
    uint32_t size = sizeof path;
    self = _NSGetExecutablePath(path, &size) == 0 ? path : argv[0];
    role = self.substr(self.rfind('/') + 1);
    given.assign(argv + 1, argv + argc);
    note("called with" + shown(given));
    int code = role == "ffprobe" ? asProbe() : role == "ffmpeg" ? asEncoder() : -1;
    if (code < 0) {
        fprintf(stderr, "absent-media stand-in %s: this command line is not one it knows, nothing was done:%s\n", role.c_str(), shown(given).c_str());
        note("REFUSED, not a known command line");
        return done(kRefused);
    }
    fflush(stdout);
    return done(code);
}

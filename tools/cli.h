#pragma once
#include "absent/media.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace cli {

using namespace absent;

inline double now() {
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

inline Args parse(int argc, char** argv) {
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

inline int fail(const std::string& text) {
    fprintf(stderr, "amedia: %s\n", text.c_str());
    return 1;
}

inline TrackSpec specOf(const Track& t) {
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
    Reader& reader;
    Writer& writer;
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

inline bool pictureBytes(const Args& args, const Frame& frame, std::vector<uint8_t>& out) {
    return args.has("bgra") ? copyBGRA(frame, out) : copyYuv(frame, out);
}

inline uint32_t videoCodec(const Args& args) {
    const std::string name = args.get("codec", "h264");
    return name == "h264" ? fourcc("avc1") : name == "hevc" ? fourcc("hvc1") : name == "av1" ? fourcc("av01") : name == "vp9" ? fourcc("vp09") : 0;
}

std::unique_ptr<Writer> openOutput(const std::string& path, std::string* error);

using Command = int (*)(const Args&);

std::map<std::string, Command>& commands();

struct Register {
    Register(const char* name, Command command) { commands()[name] = command; }
};

}

#include "cli.h"
#include <cstdio>

namespace cli {

namespace {

int packets(const Args& args) {
    if (args.plain.empty()) return fail("packets needs a file");
    std::string error;
    auto reader = Reader::open(args.plain[0], &error);
    if (!reader) return fail(error);
    const long wanted = args.number("track", 0);
    const bool raw = args.has("raw");
    std::vector<uint8_t> data;
    if (!raw) printf("format %s\n", reader->format());
    for (const Track& t : reader->tracks()) {
        if (wanted ? long(t.id) != wanted : raw && t.samples.empty()) continue;
        if (!raw) {
            printf("track %u %s %s timescale %u start %lld end %lld config %s %zu packets %zu shown %zu keys %zu\n", t.id,
                t.kind == Kind::Video ? "video" : t.kind == Kind::Audio ? "audio" : "other", fourccText(t.codec).c_str(), t.timescale, (long long)t.mediaStart,
                (long long)t.mediaEnd, fourccText(t.configType).c_str(), t.config.size(), t.samples.size(), t.order.size(), t.syncs.size());
            if (t.kind == Kind::Video) printf("video %dx%d interlaced %d colour %d %d %d %d full %d\n", t.width, t.height, t.interlaced ? 1 : 0, t.color.known ? 1 : 0,
                t.color.primaries, t.color.transfer, t.color.matrix, t.color.full ? 1 : 0);
            if (t.kind == Kind::Audio) printf("audio %d Hz %d ch bits %d float %d big %d mask %llu\n", t.sampleRate, t.channels, t.bits, t.floating ? 1 : 0,
                t.bigEndian ? 1 : 0, (unsigned long long)t.channelMask);
        }
        for (uint32_t i = 0; i < t.samples.size(); ++i) {
            const Sample& s = t.samples[i];
            data.resize(s.size);
            if (!reader->read(t, i, data.data())) return fail("packet " + std::to_string(i) + " of track " + std::to_string(t.id) + " cannot be read");
            if (raw) {
                if (fwrite(data.data(), 1, data.size(), stdout) != data.size()) return fail("output closed");
                continue;
            }
            uint64_t hash = 1469598103934665603ull;
            for (uint8_t byte : data) hash = (hash ^ byte) * 1099511628211ull;
            printf("%lld %lld %u %u %c %016llx\n", (long long)s.pts, (long long)s.dts, s.duration, s.size, s.sync ? 'K' : '_', (unsigned long long)hash);
        }
        if (raw) break;
    }
    if (!raw && !reader->tags().empty()) {
        for (const auto& [key, value] : reader->tags()) printf("tag %s=%s\n", key.c_str(), value.c_str());
    }
    return 0;
}

Register packetsCommand("packets", packets);

}

}

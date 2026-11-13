#include "cli.h"
#include <cstdio>

namespace cli {

namespace {

int dump(const Args& args) {
    if (args.plain.empty()) return fail("dump needs a file");
    std::string error;
    auto reader = Reader::open(args.plain[0], &error);
    if (!reader) return fail(error);
    size_t index = size_t(args.number("track", 0));
    if (index >= reader->tracks().size()) return fail("no such track");
    const Track& track = reader->tracks()[index];
    std::vector<uint8_t> packet;
    for (uint32_t n = 0; n < track.samples.size(); ++n) {
        packet.resize(track.samples[n].size);
        if (!reader->read(track, n, packet.data())) return fail("packet " + std::to_string(n) + " cannot be read");
        if (fwrite(packet.data(), 1, packet.size(), stdout) != packet.size()) return fail("output closed");
    }
    return 0;
}

Register dumpCommand("dump", dump);

}

}

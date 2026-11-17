#include "cli.h"
#include "absent/android.h"

namespace cli {

namespace {

int softvideo(const Args& args) {
    if (args.plain.empty()) return fail("softvideo needs a file");
    std::string error;
    auto reader = Reader::open(args.plain[0], &error);
    if (!reader) return fail(error);
    const Track* track = reader->first(Kind::Video);
    if (!track) return fail("no video track");
    auto soft = android::SoftVideo::open(*reader, *track, &error);
    if (!soft) return fail(error);
    const size_t n = track->samples.size();
    std::vector<int64_t> later(n + 1, INT64_MAX);
    for (size_t i = n; i-- > 0;) later[i] = std::min(later[i + 1], track->samples[i].pts);
    std::vector<uint8_t> packet;
    std::vector<android::SoftPicture> out;
    size_t shown = 0, rgb = 0;
    int64_t last = INT64_MIN;
    for (size_t i = 0; i <= n; ++i) {
        if (i == n) {
            soft->drain(out);
        } else {
            packet.resize(track->samples[i].size);
            if (!reader->read(*track, uint32_t(i), packet.data())) return fail("cannot read sample " + std::to_string(i));
            soft->decode(packet.data(), packet.size(), track->samples[i].pts, out, later[i + 1]);
        }
        for (const android::SoftPicture& p : out) {
            if (p.stamp < last) return fail("picture at " + std::to_string(p.stamp) + " came after " + std::to_string(last));
            last = p.stamp;
            if (fwrite(p.i420.data(), 1, p.i420.size(), stdout) != p.i420.size()) return fail("output closed");
            ++shown;
            rgb += p.rgb;
        }
        out.clear();
    }
    fprintf(stderr, "%s pictures %zu, %zu of them from RGB\n", soft->name(), shown, rgb);
    return 0;
}

}

Register softvideoCommand("softvideo", softvideo);

}

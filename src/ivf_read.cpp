#include "absent/media.h"
#include "readers.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include "portable.h"

namespace absent {

namespace {

uint32_t little32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

struct IvfReader : Reader {
    FILE* file = nullptr;
    std::vector<Track> list;

    ~IvfReader() override {
        if (file) fclose(file);
    }
    const char* format() const override { return "ivf"; }
    const std::vector<Track>& tracks() const override { return list; }
    bool read(const Track& track, uint32_t index, uint8_t* out) override {
        if (index >= track.samples.size()) return false;
        const Sample& s = track.samples[index];
        return fseeko(file, int64_t(s.offset), SEEK_SET) == 0 && fread(out, 1, s.size, file) == s.size;
    }
};

bool keyFrame(uint32_t codec, const uint8_t* head, size_t n) {
    if (codec == fourcc("vp08")) return n >= 1 && !(head[0] & 1);
    if (codec == fourcc("vp09")) {
        if (n < 1 || (head[0] >> 6) != 2) return false;
        const int profile = ((head[0] >> 5) & 1) | (((head[0] >> 4) & 1) << 1);
        const int rest = profile == 3 ? head[0] & 7 : (head[0] & 15) >> 1;
        const int existing = (rest >> 2) & 1, type = (rest >> 1) & 1;
        return !existing && !type;
    }
    size_t at = 0;
    while (at + 2 <= n) {
        const uint8_t header = head[at];
        const int type = (header >> 3) & 15;
        if (type == 1) return true;
        if (type != 2 || !(header & 2) || (header & 4)) return false;
        at += 2 + head[at + 1];
    }
    return false;
}

}

std::unique_ptr<Reader> openIvf(const std::string& path, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    std::unique_ptr<IvfReader> reader(new IvfReader);
    reader->file = openRead(path);
    if (!reader->file) return fail("cannot open file");
    fseeko(reader->file, 0, SEEK_END);
    const uint64_t length = uint64_t(ftello(reader->file));
    fseeko(reader->file, 0, SEEK_SET);
    uint8_t head[32];
    if (fread(head, 1, 32, reader->file) != 32 || memcmp(head, "DKIF", 4) != 0) return fail("not an IVF file");
    const uint32_t headerSize = uint32_t(head[6]) | (uint32_t(head[7]) << 8);
    if (headerSize < 32 || headerSize > length) return fail("broken IVF header");
    Track track;
    track.id = 1;
    track.kind = Kind::Video;
    track.codec = memcmp(head + 8, "VP80", 4) == 0 ? fourcc("vp08") : memcmp(head + 8, "VP90", 4) == 0 ? fourcc("vp09") : memcmp(head + 8, "AV01", 4) == 0 ? fourcc("av01") : 0;
    if (!track.codec) return fail("IVF codec not supported");
    track.configType = track.codec == fourcc("av01") ? fourcc("av1C") : fourcc("vpcC");
    track.width = head[12] | (head[13] << 8);
    track.height = head[14] | (head[15] << 8);
    const uint32_t rate = little32(head + 16), scale = little32(head + 20);
    if (!rate || !scale || scale > 1000000) return fail("broken IVF header");
    track.timescale = rate;
    uint64_t at = headerSize;
    while (at + 12 <= length) {
        uint8_t frame[12 + 32];
        if (fseeko(reader->file, int64_t(at), SEEK_SET) != 0) break;
        const size_t got = fread(frame, 1, sizeof frame, reader->file);
        if (got < 12) break;
        uint32_t size = little32(frame);
        if (size > length - at - 12) break;
        Sample s;
        s.offset = at + 12;
        s.size = size;
        const uint64_t stamp = uint64_t(little32(frame + 4)) | (uint64_t(little32(frame + 8)) << 32);
        if (stamp > (1ull << 40)) return fail("broken IVF frame time");
        s.pts = s.dts = int64_t(stamp) * scale;
        s.duration = scale;
        const size_t seen = std::min<size_t>(got - 12, size);
        s.sync = keyFrame(track.codec, frame + 12, seen);
        if (track.codec == fourcc("av01") && seen >= 2 && frame[12] == 0x12 && frame[13] == 0) {
            s.offset += 2;
            s.size -= 2;
        }
        at += 12 + uint64_t(size);
        if (s.size == 0) continue;
        if (!track.samples.empty()) {
            Sample& before = track.samples.back();
            if (s.pts <= before.pts) return fail("IVF frames are out of order");
            before.duration = uint32_t(std::min<int64_t>(s.pts - before.pts, 0x7FFFFFFF));
            s.duration = before.duration;
        }
        if (s.sync) track.syncs.push_back(uint32_t(track.samples.size()));
        track.order.push_back(uint32_t(track.samples.size()));
        track.samples.push_back(s);
    }
    if (track.samples.empty()) return fail("IVF file has no frames");
    reader->list.push_back(std::move(track));
    return reader;
}

}

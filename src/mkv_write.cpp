#include "absent/media.h"
#include "aac_config.h"
#include "bytes.h"
#include "writers.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "portable.h"

namespace absent {

namespace {

const uint32_t kEbml = 0x1A45DFA3, kSegment = 0x18538067, kSeekHead = 0x114D9B74, kSeek = 0x4DBB, kSeekId = 0x53AB, kSeekPosition = 0x53AC;
const uint32_t kInfo = 0x1549A966, kTimestampScale = 0x2AD7B1, kDuration = 0x4489, kMuxingApp = 0x4D80, kWritingApp = 0x5741;
const uint32_t kTracks = 0x1654AE6B, kTrackEntry = 0xAE, kTrackNumber = 0xD7, kTrackUid = 0x73C5, kTrackType = 0x83, kFlagLacing = 0x9C;
const uint32_t kCodecId = 0x86, kCodecPrivate = 0x63A2, kCodecDelay = 0x56AA, kSeekPreRoll = 0x56BB;
const uint32_t kVideo = 0xE0, kPixelWidth = 0xB0, kPixelHeight = 0xBA, kAudio = 0xE1, kSamplingFrequency = 0xB5, kChannels = 0x9F, kBitDepth = 0x6264;
const uint32_t kColour = 0x55B0, kMatrix = 0x55B1, kRange = 0x55B9, kTransfer = 0x55BA, kPrimaries = 0x55BB;
const uint32_t kCluster = 0x1F43B675, kTimestamp = 0xE7, kSimpleBlock = 0xA3, kBlockGroup = 0xA0, kBlock = 0xA1, kDiscardPadding = 0x75A2;
const uint32_t kCues = 0x1C53BB6B, kCuePoint = 0xBB, kCueTime = 0xB3, kCueTrackPositions = 0xB7, kCueTrack = 0xF7, kCueClusterPosition = 0xF1;

void id(Bytes& b, uint32_t value) {
    if (value > 0xFFFFFF) b.u32(value);
    else if (value > 0xFFFF) b.u24(value);
    else if (value > 0xFF) b.u16(value);
    else b.u8(value);
}

void length(Bytes& b, uint64_t n) {
    if (n < 127) {
        b.u8(0x80 | uint32_t(n));
    } else if (n < 16383) {
        b.u16(0x4000 | uint32_t(n));
    } else {
        b.u8(0x01);
        b.u24(uint32_t(n >> 32));
        b.u32(uint32_t(n));
    }
}

size_t start(Bytes& b, uint32_t element) {
    id(b, element);
    size_t at = b.d.size();
    b.zero(8);
    return at;
}

void stop(Bytes& b, size_t at) {
    uint64_t n = b.d.size() - at - 8;
    b.d[at] = 0x01;
    for (int i = 1; i < 8; ++i) b.d[at + i] = uint8_t(n >> (8 * (7 - i)));
}

void number(Bytes& b, uint32_t element, uint64_t value) {
    id(b, element);
    int bytes = 1;
    while (bytes < 8 && (value >> (8 * bytes))) ++bytes;
    b.u8(0x80 | bytes);
    for (int i = bytes - 1; i >= 0; --i) b.u8(uint32_t(value >> (8 * i)));
}

void wide(Bytes& b, uint32_t element, uint64_t value) {
    id(b, element);
    b.u8(0x88);
    b.u64(value);
}

void real(Bytes& b, uint32_t element, double value) {
    uint64_t bits;
    memcpy(&bits, &value, 8);
    wide(b, element, bits);
}

void binary(Bytes& b, uint32_t element, const void* data, size_t size) {
    id(b, element);
    length(b, size);
    b.raw(data, size);
}

void text(Bytes& b, uint32_t element, const char* value) { binary(b, element, value, strlen(value)); }

struct Entry {
    const char* name = nullptr;
    std::vector<uint8_t> secret;
    bool needsSecret = false;
    bool web = false;
};

std::vector<uint8_t> audioConfig(const std::vector<uint8_t>& esds);

Entry describe(const TrackSpec& spec) {
    Entry e;
    e.secret = spec.config;
    auto is = [&](const char (&code)[5]) { return spec.codec == fourcc(code); };
    if (is("avc1")) e = {"V_MPEG4/ISO/AVC", spec.config, true, false};
    else if (is("hvc1")) e = {"V_MPEGH/ISO/HEVC", spec.config, true, false};
    else if (is("av01")) e = {"V_AV1", spec.config, true, true};
    else if (is("vp09")) e = {"V_VP9", {}, false, true};
    else if (is("vp08")) e = {"V_VP8", {}, false, true};
    else if (is("mp4a")) e = {"A_AAC", spec.configType == fourcc("esds") ? audioConfig(spec.config) : spec.config, true, false};
    else if (is("Opus")) e = {"A_OPUS", spec.config, true, true};
    else if (is("vorb")) e = {"A_VORBIS", spec.config, true, true};
    else if (is(".mp3")) e = {"A_MPEG/L3", {}, false, false};
    else if (is(".mp2")) e = {"A_MPEG/L2", {}, false, false};
    else if (is("ac-3")) e = {"A_AC3", {}, false, false};
    else if (is("mp4v")) e = {"V_MPEG4/ISO/ASP", spec.configType == fourcc("esds") ? audioConfig(spec.config) : spec.config, false, false};
    else if (is("mp1v")) e = {"V_MPEG1", {}, false, false};
    else if (is("mp2v")) e = {"V_MPEG2", {}, false, false};
    else if (is("jpeg")) e = {"V_MJPEG", {}, false, false};
    else if (is("alac")) e = {"A_ALAC", spec.config.size() >= 24 ? std::vector<uint8_t>(spec.config.end() - 24, spec.config.end()) : std::vector<uint8_t>(), true, false};
    else if (is("fLaC")) {
        e = {"A_FLAC", {'f', 'L', 'a', 'C', 0x80, 0, 0, uint8_t(spec.config.size())}, true, false};
        if (spec.config.size() == 34) e.secret.insert(e.secret.end(), spec.config.begin(), spec.config.end());
        else e.secret.clear();
    } else if (is("lpcm")) {
        if (spec.floating) e.name = spec.bigEndian ? nullptr : "A_PCM/FLOAT/IEEE";
        else e.name = spec.bigEndian && spec.bits > 8 ? "A_PCM/INT/BIG" : "A_PCM/INT/LIT";
        e.secret.clear();
    }
    return e;
}

std::vector<uint8_t> audioConfig(const std::vector<uint8_t>& esds) {
    ElementaryStream stream;
    if (!parseElementaryStream(View{esds.data(), esds.size()}, stream) || !stream.specific.p) return {};
    return {stream.specific.p, stream.specific.p + stream.specific.n};
}

struct Held {
    std::vector<uint8_t> data;
    int64_t time = 0;
    bool any = false;
};

struct Out {
    TrackSpec spec;
    Held held;
    int64_t decoded = 0;
    int64_t end = 0;
    bool trims = false;
};

struct Cue {
    int64_t time;
    int track;
    uint64_t offset;
};

}

struct MkvWriter::Impl {
    std::string path;
    std::string temp;
    FILE* data = nullptr;
    uint64_t size = 0;
    std::vector<Out> tracks;
    std::vector<Cue> cues;
    Bytes cluster;
    int64_t clusterTime = 0;
    bool clusterOpen = false;
    bool failed = false;
    bool webm = false;

    ~Impl() {
        if (data) {
            fclose(data);
            removeFile(temp);
        }
    }

    void flush() {
        if (!clusterOpen) return;
        Bytes head;
        id(head, kCluster);
        Bytes time;
        number(time, kTimestamp, uint64_t(clusterTime));
        uint64_t n = time.d.size() + cluster.d.size();
        head.u8(0x01);
        head.u24(uint32_t(n >> 32));
        head.u32(uint32_t(n));
        head.raw(time.d.data(), time.d.size());
        if (fwrite(head.d.data(), 1, head.d.size(), data) != head.d.size() || fwrite(cluster.d.data(), 1, cluster.d.size(), data) != cluster.d.size()) failed = true;
        size += head.d.size() + cluster.d.size();
        cluster.d.clear();
        clusterOpen = false;
    }

    void block(int track, const uint8_t* bytes, size_t count, int64_t time, bool key, int64_t discard) {
        const bool video = tracks[track].spec.kind == Kind::Video;
        const bool lead = video || std::none_of(tracks.begin(), tracks.end(), [](const Out& t) { return t.spec.kind == Kind::Video; });
        if (clusterOpen && ((video && key) || time - clusterTime > 30000 || time - clusterTime < -30000 || cluster.d.size() > (4u << 20) || (lead && !video && time - clusterTime >= 2000))) flush();
        if (!clusterOpen) {
            clusterOpen = true;
            clusterTime = std::max<int64_t>(time, 0);
            if (lead && key) cues.push_back({clusterTime, track, size});
        }
        Bytes body;
        length(body, uint64_t(track + 1));
        body.u16(uint32_t(int16_t(time - clusterTime)));
        body.u8(key && discard == 0 ? 0x80 : 0);
        if (discard) {
            size_t group = start(cluster, kBlockGroup);
            id(cluster, kBlock);
            length(cluster, body.d.size() + count);
            cluster.raw(body.d.data(), body.d.size());
            cluster.raw(bytes, count);
            id(cluster, kDiscardPadding);
            cluster.u8(0x88);
            cluster.u64(uint64_t(discard));
            stop(cluster, group);
        } else {
            id(cluster, kSimpleBlock);
            length(cluster, body.d.size() + count);
            cluster.raw(body.d.data(), body.d.size());
            cluster.raw(bytes, count);
        }
    }
};

MkvWriter::MkvWriter() : impl(new Impl) {}
MkvWriter::~MkvWriter() = default;

std::unique_ptr<MkvWriter> MkvWriter::open(const std::string& path, std::string* error) {
    std::unique_ptr<MkvWriter> writer(new MkvWriter);
    writer->impl->path = path;
    writer->impl->temp = path + ".part";
    writer->impl->webm = path.size() > 5 && path.compare(path.size() - 5, 5, ".webm") == 0;
    writer->impl->data = openFile(writer->impl->temp, "wb+");
    if (!writer->impl->data) {
        if (error) *error = "cannot create output file";
        return nullptr;
    }
    return writer;
}

int MkvWriter::add(const TrackSpec& spec) {
    Out out;
    out.spec = spec;
    out.trims = spec.kind == Kind::Audio;
    impl->tracks.push_back(std::move(out));
    return int(impl->tracks.size()) - 1;
}

void MkvWriter::trim(int track, int64_t skip, int64_t length) {
    if (track < 0 || track >= int(impl->tracks.size())) return;
    impl->tracks[track].spec.skip = skip;
    impl->tracks[track].spec.length = length;
}

void MkvWriter::configure(int track, uint32_t type, const std::vector<uint8_t>& config) {
    if (track < 0 || track >= int(impl->tracks.size())) return;
    impl->tracks[track].spec.configType = type;
    impl->tracks[track].spec.config = config;
}

bool MkvWriter::write(int track, const uint8_t* data, size_t size, int64_t pts, int64_t, uint32_t duration, bool sync) {
    if (track < 0 || track >= int(impl->tracks.size()) || impl->failed || !impl->data) return false;
    Out& t = impl->tracks[track];
    if (!t.spec.timescale) return false;
    pts += t.spec.delay;
    int64_t time = std::max<int64_t>(0, std::llround(double(pts) * 1000.0 / t.spec.timescale));
    t.decoded += duration;
    t.end = std::max<int64_t>(t.end, std::llround(double(pts + duration) * 1000.0 / t.spec.timescale));
    if (t.trims) {
        if (t.held.any) impl->block(track, t.held.data.data(), t.held.data.size(), t.held.time, true, 0);
        t.held.data.assign(data, data + size);
        t.held.time = time;
        t.held.any = true;
    } else {
        impl->block(track, data, size, time, t.spec.kind == Kind::Audio || sync, 0);
    }
    return !impl->failed;
}

bool MkvWriter::finish(std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return false;
    };
    Impl& m = *impl;
    if (m.failed || !m.data) return fail("writing sample data failed");
    double duration = 0;
    for (size_t i = 0; i < m.tracks.size(); ++i) {
        Out& t = m.tracks[i];
        double shown = double(t.end);
        if (t.trims && t.held.any) {
            int64_t extra = t.spec.length > 0 ? t.decoded - t.spec.skip - t.spec.length : 0;
            int64_t discard = extra > 0 ? std::llround(double(extra) * 1e9 / t.spec.timescale) : 0;
            m.block(int(i), t.held.data.data(), t.held.data.size(), t.held.time, true, discard);
            if (t.spec.length > 0) shown = double(t.spec.length + t.spec.delay) * 1000.0 / t.spec.timescale;
        }
        duration = std::max(duration, shown);
    }
    m.flush();
    if (m.failed) return fail("writing sample data failed");

    Bytes info;
    size_t at = start(info, kInfo);
    number(info, kTimestampScale, 1000000);
    text(info, kMuxingApp, "FFamp");
    text(info, kWritingApp, "FFamp");
    real(info, kDuration, duration);
    stop(info, at);

    Bytes tracks;
    at = start(tracks, kTracks);
    for (size_t i = 0; i < m.tracks.size(); ++i) {
        const TrackSpec& spec = m.tracks[i].spec;
        const Entry described = describe(spec);
        const std::vector<uint8_t>& secret = described.secret;
        const char* codec = described.name;
        if (!codec || (described.needsSecret && secret.empty())) return fail("a track cannot be stored in Matroska");
        if (m.webm && !described.web) return fail("WebM holds only VP8, VP9 or AV1 pictures and Opus or Vorbis sound");
        size_t entry = start(tracks, kTrackEntry);
        number(tracks, kTrackNumber, i + 1);
        number(tracks, kTrackUid, i + 1);
        number(tracks, kTrackType, spec.kind == Kind::Video ? 1 : 2);
        number(tracks, kFlagLacing, 0);
        text(tracks, kCodecId, codec);
        if (!secret.empty()) binary(tracks, kCodecPrivate, secret.data(), secret.size());
        if (spec.kind == Kind::Video) {
            size_t video = start(tracks, kVideo);
            number(tracks, kPixelWidth, uint64_t(spec.width));
            number(tracks, kPixelHeight, uint64_t(spec.height));
            size_t colour = start(tracks, kColour);
            number(tracks, kMatrix, uint64_t(spec.color.matrix));
            number(tracks, kRange, spec.color.full ? 2 : 1);
            number(tracks, kTransfer, uint64_t(spec.color.transfer));
            number(tracks, kPrimaries, uint64_t(spec.color.primaries));
            stop(tracks, colour);
            stop(tracks, video);
        } else {
            if (spec.skip > 0) number(tracks, kCodecDelay, uint64_t(std::llround(double(spec.skip) * 1e9 / spec.timescale)));
            if (spec.codec == fourcc("Opus")) number(tracks, kSeekPreRoll, 80000000);
            size_t audio = start(tracks, kAudio);
            real(tracks, kSamplingFrequency, spec.sampleRate);
            number(tracks, kChannels, uint64_t(spec.channels));
            int depth = spec.codec == fourcc("lpcm") || spec.codec == fourcc("fLaC") ? spec.bits : 0;
            if (depth > 0) number(tracks, kBitDepth, uint64_t(depth));
            stop(tracks, audio);
        }
        stop(tracks, entry);
    }
    stop(tracks, at);

    auto cueBytes = [&](uint64_t base) {
        Bytes cues;
        size_t all = start(cues, kCues);
        for (const Cue& cue : m.cues) {
            size_t point = start(cues, kCuePoint);
            number(cues, kCueTime, uint64_t(cue.time));
            size_t where = start(cues, kCueTrackPositions);
            number(cues, kCueTrack, uint64_t(cue.track + 1));
            wide(cues, kCueClusterPosition, base + cue.offset);
            stop(cues, where);
            stop(cues, point);
        }
        stop(cues, all);
        return cues;
    };
    const uint64_t seekSize = 4 + 1 + 3 * 21;
    Bytes probe = cueBytes(0);
    uint64_t infoAt = seekSize, tracksAt = infoAt + info.d.size(), cuesAt = tracksAt + tracks.d.size(), clustersAt = cuesAt + probe.d.size();
    Bytes cues = cueBytes(clustersAt);

    Bytes head;
    at = start(head, kEbml);
    number(head, 0x4286, 1);
    number(head, 0x42F7, 1);
    number(head, 0x42F2, 4);
    number(head, 0x42F3, 8);
    text(head, 0x4282, m.webm ? "webm" : "matroska");
    number(head, 0x4287, 4);
    number(head, 0x4285, 2);
    stop(head, at);
    id(head, kSegment);
    uint64_t total = clustersAt + m.size;
    head.u8(0x01);
    head.u24(uint32_t(total >> 32));
    head.u32(uint32_t(total));
    id(head, kSeekHead);
    length(head, 3 * 21);
    const std::pair<uint32_t, uint64_t> entries[3] = {{kInfo, infoAt}, {kTracks, tracksAt}, {kCues, cuesAt}};
    for (const auto& [element, position] : entries) {
        id(head, kSeek);
        length(head, 18);
        id(head, kSeekId);
        length(head, 4);
        head.u32(element);
        wide(head, kSeekPosition, position);
    }

    FILE* out = openFile(m.path, "wb");
    if (!out) return fail("cannot create output file");
    bool ok = true;
    for (const Bytes* part : {&head, &info, &tracks, &cues}) ok = ok && fwrite(part->d.data(), 1, part->d.size(), out) == part->d.size();
    std::vector<uint8_t> chunk(1 << 20);
    fflush(m.data);
    fseeko(m.data, 0, SEEK_SET);
    uint64_t left = m.size;
    while (ok && left) {
        size_t want = size_t(std::min<uint64_t>(left, chunk.size()));
        size_t got = fread(chunk.data(), 1, want, m.data);
        ok = got == want && fwrite(chunk.data(), 1, got, out) == got;
        left -= got;
    }
    ok = fclose(out) == 0 && ok;
    fclose(m.data);
    m.data = nullptr;
    removeFile(m.temp);
    if (!ok) {
        removeFile(m.path);
        return fail("writing the output file failed");
    }
    return true;
}

std::unique_ptr<Writer> Writer::open(const std::string& path, std::string* error) {
    auto ends = [&](const char* tail) {
        size_t n = strlen(tail);
        return path.size() > n && path.compare(path.size() - n, n, tail) == 0;
    };
    if (ends(".mkv") || ends(".webm") || ends(".mka")) return MkvWriter::open(path, error);
    if (ends(".ogg") || ends(".opus") || ends(".oga")) return openOggWriter(path, error);
    return Mp4Writer::open(path, error);
}

}

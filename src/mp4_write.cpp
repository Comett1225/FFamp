#include "absent/media.h"
#include "bytes.h"
#include <algorithm>
#include <cstdio>

namespace absent {

namespace {

struct Out {
    TrackSpec spec;
    std::vector<Sample> samples;
    std::vector<uint32_t> chunkSizes;
    std::vector<uint64_t> chunkOffsets;
    int64_t start = 0;
    int64_t shown = 0;
    int64_t mediaDuration = 0;
};

const uint32_t kMovieScale = 1000;
const uint32_t kChunkLimit = 64;

void matrix(Bytes& b, int rotation) {
    static const int32_t turns[4][4] = {{1, 0, 0, 1}, {0, 1, -1, 0}, {-1, 0, 0, -1}, {0, -1, 1, 0}};
    const int32_t* m = turns[((rotation / 90) % 4 + 4) % 4];
    b.u32(uint32_t(m[0] * 0x10000));
    b.u32(uint32_t(m[1] * 0x10000));
    b.u32(0);
    b.u32(uint32_t(m[2] * 0x10000));
    b.u32(uint32_t(m[3] * 0x10000));
    b.u32(0);
    b.u32(0);
    b.u32(0);
    b.u32(0x40000000);
}

void prepare(Out& t) {
    int64_t low = 0;
    for (const Sample& s : t.samples) low = std::min(low, s.pts - s.dts);
    int64_t base = t.samples.front().dts + low;
    for (Sample& s : t.samples) {
        s.dts += low - base;
        s.pts -= base;
    }
    for (size_t i = 0; i + 1 < t.samples.size(); ++i) {
        int64_t delta = t.samples[i + 1].dts - t.samples[i].dts;
        t.samples[i].duration = uint32_t(std::max<int64_t>(delta, 0));
    }
    int64_t first = t.samples.front().pts;
    int64_t end = 0;
    for (const Sample& s : t.samples) {
        first = std::min(first, s.pts);
        end = std::max(end, s.pts + int64_t(s.duration));
    }
    t.start = first + t.spec.skip;
    t.shown = t.spec.length > 0 ? t.spec.length : std::max<int64_t>(end - t.start, 0);
    Sample& tail = t.samples.back();
    if (t.spec.length > 0 && t.start + t.shown > tail.dts && t.start + t.shown < tail.dts + int64_t(tail.duration)) tail.duration = uint32_t(t.start + t.shown - tail.dts);
    t.mediaDuration = tail.dts + tail.duration;
}

uint64_t scaled(int64_t value, uint32_t from) {
    return from ? uint64_t((value * kMovieScale + from / 2) / from) : 0;
}

void sampleEntry(Bytes& b, const Out& t) {
    const TrackSpec& spec = t.spec;
    size_t at = b.open(spec.codec);
    b.zero(6);
    b.u16(1);
    if (spec.kind == Kind::Video) {
        b.zero(16);
        b.u16(spec.width);
        b.u16(spec.height);
        b.u32(0x00480000);
        b.u32(0x00480000);
        b.u32(0);
        b.u16(1);
        b.zero(32);
        b.u16(0x0018);
        b.u16(0xFFFF);
        size_t config = b.open(spec.configType);
        b.raw(spec.config.data(), spec.config.size());
        b.close(config);
        size_t color = b.open(fourcc("colr"));
        b.u32(fourcc("nclx"));
        b.u16(spec.color.primaries);
        b.u16(spec.color.transfer);
        b.u16(spec.color.matrix);
        b.u8(spec.color.full ? 0x80 : 0);
        b.close(color);
    } else {
        b.zero(8);
        b.u16(spec.channels);
        b.u16(16);
        b.u32(0);
        b.u32(uint32_t(spec.sampleRate) << 16);
        size_t config = b.open(spec.configType);
        b.raw(spec.config.data(), spec.config.size());
        b.close(config);
    }
    b.close(at);
}

void tables(Bytes& b, const Out& t, uint64_t base, bool wide) {
    size_t stbl = b.open(fourcc("stbl"));
    size_t stsd = b.open(fourcc("stsd"), 0, 0);
    b.u32(1);
    sampleEntry(b, t);
    b.close(stsd);

    size_t stts = b.open(fourcc("stts"), 0, 0);
    size_t count = b.d.size();
    b.u32(0);
    uint32_t runs = 0;
    for (size_t i = 0; i < t.samples.size();) {
        size_t j = i;
        while (j < t.samples.size() && t.samples[j].duration == t.samples[i].duration) ++j;
        b.u32(uint32_t(j - i));
        b.u32(t.samples[i].duration);
        ++runs;
        i = j;
    }
    Bytes patch;
    patch.u32(runs);
    std::copy(patch.d.begin(), patch.d.end(), b.d.begin() + count);
    b.close(stts);

    bool allSync = std::all_of(t.samples.begin(), t.samples.end(), [](const Sample& s) { return s.sync; });
    if (!allSync) {
        size_t stss = b.open(fourcc("stss"), 0, 0);
        uint32_t syncs = uint32_t(std::count_if(t.samples.begin(), t.samples.end(), [](const Sample& s) { return s.sync; }));
        b.u32(syncs);
        for (size_t i = 0; i < t.samples.size(); ++i) if (t.samples[i].sync) b.u32(uint32_t(i + 1));
        b.close(stss);
    }

    bool shifted = std::any_of(t.samples.begin(), t.samples.end(), [](const Sample& s) { return s.pts != s.dts; });
    if (shifted) {
        size_t ctts = b.open(fourcc("ctts"), 0, 0);
        size_t place = b.d.size();
        b.u32(0);
        uint32_t groups = 0;
        for (size_t i = 0; i < t.samples.size();) {
            int64_t offset = t.samples[i].pts - t.samples[i].dts;
            size_t j = i;
            while (j < t.samples.size() && t.samples[j].pts - t.samples[j].dts == offset) ++j;
            b.u32(uint32_t(j - i));
            b.u32(uint32_t(offset));
            ++groups;
            i = j;
        }
        Bytes fix;
        fix.u32(groups);
        std::copy(fix.d.begin(), fix.d.end(), b.d.begin() + place);
        b.close(ctts);
    }

    size_t stsc = b.open(fourcc("stsc"), 0, 0);
    size_t where = b.d.size();
    b.u32(0);
    uint32_t entries = 0;
    for (size_t i = 0; i < t.chunkSizes.size(); ++i) {
        if (i && t.chunkSizes[i] == t.chunkSizes[i - 1]) continue;
        b.u32(uint32_t(i + 1));
        b.u32(t.chunkSizes[i]);
        b.u32(1);
        ++entries;
    }
    Bytes amount;
    amount.u32(entries);
    std::copy(amount.d.begin(), amount.d.end(), b.d.begin() + where);
    b.close(stsc);

    size_t stsz = b.open(fourcc("stsz"), 0, 0);
    b.u32(0);
    b.u32(uint32_t(t.samples.size()));
    for (const Sample& s : t.samples) b.u32(s.size);
    b.close(stsz);

    size_t offsets = b.open(wide ? fourcc("co64") : fourcc("stco"), 0, 0);
    b.u32(uint32_t(t.chunkOffsets.size()));
    for (uint64_t offset : t.chunkOffsets) {
        if (wide) b.u64(base + offset);
        else b.u32(uint32_t(base + offset));
    }
    b.close(offsets);

    if (t.spec.kind == Kind::Audio && t.spec.skip > 0) {
        size_t sgpd = b.open(fourcc("sgpd"), 1, 0);
        b.u32(fourcc("roll"));
        b.u32(2);
        b.u32(1);
        b.u16(0xFFFF);
        b.close(sgpd);
        size_t sbgp = b.open(fourcc("sbgp"), 0, 0);
        b.u32(fourcc("roll"));
        b.u32(1);
        b.u32(uint32_t(t.samples.size()));
        b.u32(1);
        b.close(sbgp);
    }
    b.close(stbl);
}

void track(Bytes& b, const Out& t, uint32_t id, uint64_t base, bool wide) {
    const TrackSpec& spec = t.spec;
    bool video = spec.kind == Kind::Video;
    size_t trak = b.open(fourcc("trak"));

    size_t tkhd = b.open(fourcc("tkhd"), 0, 3);
    b.u32(0);
    b.u32(0);
    b.u32(id);
    b.u32(0);
    b.u32(uint32_t(scaled(t.shown, spec.timescale)));
    b.zero(8);
    b.u16(0);
    b.u16(video ? 0 : 1);
    b.u16(video ? 0 : 0x0100);
    b.u16(0);
    matrix(b, video ? spec.rotation : 0);
    b.u32(uint32_t(spec.width) << 16);
    b.u32(uint32_t(spec.height) << 16);
    b.close(tkhd);

    if (t.start != 0 || spec.length > 0) {
        size_t edts = b.open(fourcc("edts"));
        size_t elst = b.open(fourcc("elst"), 0, 0);
        b.u32(1);
        b.u32(uint32_t(scaled(t.shown, spec.timescale)));
        b.u32(uint32_t(t.start));
        b.u32(0x00010000);
        b.close(elst);
        b.close(edts);
    }

    size_t mdia = b.open(fourcc("mdia"));
    size_t mdhd = b.open(fourcc("mdhd"), 0, 0);
    b.u32(0);
    b.u32(0);
    b.u32(spec.timescale);
    b.u32(uint32_t(t.mediaDuration));
    b.u16(0x55C4);
    b.u16(0);
    b.close(mdhd);

    size_t hdlr = b.open(fourcc("hdlr"), 0, 0);
    b.u32(0);
    b.u32(video ? fourcc("vide") : fourcc("soun"));
    b.zero(12);
    const char* name = video ? "VideoHandler" : "SoundHandler";
    b.raw(name, 13);
    b.close(hdlr);

    size_t minf = b.open(fourcc("minf"));
    if (video) {
        size_t vmhd = b.open(fourcc("vmhd"), 0, 1);
        b.zero(8);
        b.close(vmhd);
    } else {
        size_t smhd = b.open(fourcc("smhd"), 0, 0);
        b.zero(4);
        b.close(smhd);
    }
    size_t dinf = b.open(fourcc("dinf"));
    size_t dref = b.open(fourcc("dref"), 0, 0);
    b.u32(1);
    size_t url = b.open(fourcc("url "), 0, 1);
    b.close(url);
    b.close(dref);
    b.close(dinf);
    tables(b, t, base, wide);
    b.close(minf);
    b.close(mdia);
    b.close(trak);
}

}

struct Mp4Writer::Impl {
    std::string path;
    std::string temp;
    FILE* data = nullptr;
    uint64_t size = 0;
    std::vector<Out> tracks;
    int last = -1;
    bool failed = false;
    ~Impl() {
        if (data) {
            fclose(data);
            remove(temp.c_str());
        }
    }

    Bytes movie(uint64_t base, bool wide) const {
        Bytes b;
        size_t moov = b.open(fourcc("moov"));
        uint64_t duration = 0;
        uint32_t used = 0;
        for (const Out& t : tracks) {
            if (t.samples.empty()) continue;
            duration = std::max(duration, scaled(t.shown, t.spec.timescale));
            ++used;
        }
        size_t mvhd = b.open(fourcc("mvhd"), 0, 0);
        b.u32(0);
        b.u32(0);
        b.u32(kMovieScale);
        b.u32(uint32_t(duration));
        b.u32(0x00010000);
        b.u16(0x0100);
        b.zero(10);
        matrix(b, 0);
        b.zero(24);
        b.u32(used + 1);
        b.close(mvhd);
        uint32_t id = 0;
        for (const Out& t : tracks) if (!t.samples.empty()) track(b, t, ++id, base, wide);
        b.close(moov);
        return b;
    }
};

Mp4Writer::Mp4Writer() : impl(new Impl) {}
Mp4Writer::~Mp4Writer() = default;

std::unique_ptr<Mp4Writer> Mp4Writer::open(const std::string& path, std::string* error) {
    std::unique_ptr<Mp4Writer> writer(new Mp4Writer);
    writer->impl->path = path;
    writer->impl->temp = path + ".part";
    writer->impl->data = fopen(writer->impl->temp.c_str(), "wb+");
    if (!writer->impl->data) {
        if (error) *error = "cannot create output file";
        return nullptr;
    }
    return writer;
}

int Mp4Writer::add(const TrackSpec& spec) {
    Out out;
    out.spec = spec;
    impl->tracks.push_back(std::move(out));
    return int(impl->tracks.size()) - 1;
}

void Mp4Writer::configure(int track, uint32_t type, const std::vector<uint8_t>& config) {
    if (track < 0 || track >= int(impl->tracks.size())) return;
    impl->tracks[track].spec.configType = type;
    impl->tracks[track].spec.config = config;
}

bool Mp4Writer::write(int track, const uint8_t* data, size_t size, int64_t pts, int64_t dts, uint32_t duration, bool sync) {
    if (track < 0 || track >= int(impl->tracks.size()) || impl->failed || size > 0xFFFFFFFFu) return false;
    if (fwrite(data, 1, size, impl->data) != size) {
        impl->failed = true;
        return false;
    }
    Out& t = impl->tracks[track];
    if (impl->last != track || t.chunkSizes.empty() || t.chunkSizes.back() >= kChunkLimit) {
        t.chunkSizes.push_back(0);
        t.chunkOffsets.push_back(impl->size);
    }
    ++t.chunkSizes.back();
    Sample s;
    s.offset = impl->size;
    s.size = uint32_t(size);
    s.pts = pts;
    s.dts = dts;
    s.duration = duration;
    s.sync = sync;
    t.samples.push_back(s);
    impl->size += size;
    impl->last = track;
    return true;
}

bool Mp4Writer::finish(std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return false;
    };
    if (impl->failed || !impl->data) return fail("writing sample data failed");
    bool any = false;
    bool h264 = false;
    for (Out& t : impl->tracks) {
        if (t.samples.empty()) continue;
        if (t.spec.config.empty()) return fail("a track has no codec configuration");
        prepare(t);
        any = true;
        h264 = h264 || t.spec.codec == fourcc("avc1");
    }
    if (!any) return fail("nothing was written");

    Bytes head;
    size_t ftyp = head.open(fourcc("ftyp"));
    head.u32(fourcc("isom"));
    head.u32(0x200);
    head.u32(fourcc("isom"));
    head.u32(fourcc("iso2"));
    if (h264) head.u32(fourcc("avc1"));
    head.u32(fourcc("mp41"));
    head.close(ftyp);

    bool wide = impl->size > 0xF0000000ull;
    bool large = impl->size + 8 > 0xFFFFFFFFull;
    uint64_t dataHeader = large ? 16 : 8;
    Bytes probe = impl->movie(0, wide);
    uint64_t base = head.d.size() + probe.d.size() + dataHeader;
    Bytes moov = impl->movie(base, wide);
    if (moov.d.size() != probe.d.size()) return fail("internal layout error");

    FILE* out = fopen(impl->path.c_str(), "wb");
    if (!out) return fail("cannot create output file");
    Bytes mdat;
    if (large) {
        mdat.u32(1);
        mdat.u32(fourcc("mdat"));
        mdat.u64(impl->size + 16);
    } else {
        mdat.u32(uint32_t(impl->size + 8));
        mdat.u32(fourcc("mdat"));
    }
    bool ok = fwrite(head.d.data(), 1, head.d.size(), out) == head.d.size()
        && fwrite(moov.d.data(), 1, moov.d.size(), out) == moov.d.size()
        && fwrite(mdat.d.data(), 1, mdat.d.size(), out) == mdat.d.size();
    std::vector<uint8_t> block(1 << 20);
    fflush(impl->data);
    fseeko(impl->data, 0, SEEK_SET);
    uint64_t left = impl->size;
    while (ok && left) {
        size_t want = size_t(std::min<uint64_t>(left, block.size()));
        size_t got = fread(block.data(), 1, want, impl->data);
        ok = got == want && fwrite(block.data(), 1, got, out) == got;
        left -= got;
    }
    ok = fclose(out) == 0 && ok;
    fclose(impl->data);
    impl->data = nullptr;
    remove(impl->temp.c_str());
    if (!ok) {
        remove(impl->path.c_str());
        return fail("writing the output file failed");
    }
    return true;
}

}

#include "absent/media.h"
#include "bytes.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <numeric>

namespace absent {

std::string fourccText(uint32_t value) {
    std::string text(4, ' ');
    for (int i = 0; i < 4; ++i) {
        char c = char(value >> (24 - 8 * i));
        text[i] = c >= 32 && c < 127 ? c : '.';
    }
    return text;
}

double Track::seconds() const {
    if (!timescale || order.empty()) return 0;
    int64_t end = 0;
    for (uint32_t i : order) end = std::max(end, samples[i].pts + int64_t(samples[i].duration));
    return double(end - mediaStart) / timescale;
}

double Track::rate() const {
    double s = seconds();
    return s > 0 ? order.size() / s : 0;
}

namespace {

template <class F>
void boxes(View v, F f) {
    while (v.left() >= 8) {
        uint64_t size = v.u32();
        uint32_t type = v.u32();
        uint64_t header = 8;
        if (size == 1) {
            size = v.u64();
            header = 16;
        } else if (size == 0) {
            size = v.left() + header;
        }
        if (!v.ok || size < header || size - header > v.left()) return;
        f(type, v.sub(size_t(size - header)));
    }
}

struct Run {
    uint32_t first = 0;
    uint32_t count = 0;
};

struct Defaults {
    uint32_t duration = 0;
    uint32_t size = 0;
    uint32_t flags = 0;
};

struct Pending {
    Track track;
    View stsd;
    std::vector<std::pair<uint32_t, uint32_t>> stts;
    std::vector<std::pair<uint32_t, int32_t>> ctts;
    std::vector<uint32_t> stss;
    std::vector<uint32_t> sizes;
    std::vector<Run> stsc;
    std::vector<uint64_t> chunks;
    uint32_t fixedSize = 0;
    uint32_t sampleCount = 0;
    bool hasStss = false;
    bool hasEdit = false;
    Defaults defaults;
    int64_t fragmentTime = 0;
};

void entry(uint32_t format, View v, Track& track) {
    track.codec = format;
    v.skip(8);
    if (track.kind == Kind::Video) {
        v.skip(16);
        track.width = v.u16();
        track.height = v.u16();
        v.skip(50);
        boxes(v, [&](uint32_t type, View body) {
            if (type == fourcc("avcC") || type == fourcc("hvcC")) {
                track.configType = type;
                track.config.assign(body.p, body.p + body.n);
            } else if (type == fourcc("colr")) {
                uint32_t kind = body.u32();
                if (kind == fourcc("nclx") || kind == fourcc("nclc")) {
                    track.color.primaries = body.u16();
                    track.color.transfer = body.u16();
                    track.color.matrix = body.u16();
                    track.color.full = kind == fourcc("nclx") && (body.u8() & 0x80);
                    track.color.known = body.ok;
                }
            }
        });
    } else if (track.kind == Kind::Audio) {
        uint16_t version = v.u16();
        v.skip(6);
        track.channels = v.u16();
        v.skip(6);
        track.sampleRate = int(v.u32() >> 16);
        if (version == 1) v.skip(16);
        else if (version == 2) v.skip(36);
        auto take = [&](uint32_t type, View body) {
            if (type == fourcc("esds")) {
                track.configType = type;
                track.config.assign(body.p, body.p + body.n);
            }
        };
        boxes(v, [&](uint32_t type, View body) {
            if (type == fourcc("wave")) boxes(body, take);
            else take(type, body);
        });
    }
}

void table(uint32_t type, View v, Pending& p) {
    if (type == fourcc("stsd")) {
        p.stsd = v;
        return;
    }
    v.skip(4);
    if (type == fourcc("stts")) {
        uint32_t count = v.u32();
        if (count > v.left() / 8) return;
        p.stts.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t n = v.u32();
            p.stts.push_back({n, v.u32()});
        }
    } else if (type == fourcc("ctts")) {
        uint32_t count = v.u32();
        if (count > v.left() / 8) return;
        p.ctts.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t n = v.u32();
            p.ctts.push_back({n, int32_t(v.u32())});
        }
    } else if (type == fourcc("stss")) {
        uint32_t count = v.u32();
        if (count > v.left() / 4) return;
        p.hasStss = true;
        p.stss.reserve(count);
        for (uint32_t i = 0; i < count; ++i) p.stss.push_back(v.u32());
    } else if (type == fourcc("stsc")) {
        uint32_t count = v.u32();
        if (count > v.left() / 12) return;
        p.stsc.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            Run run;
            run.first = v.u32();
            run.count = v.u32();
            v.skip(4);
            p.stsc.push_back(run);
        }
    } else if (type == fourcc("stsz")) {
        p.fixedSize = v.u32();
        p.sampleCount = v.u32();
        if (p.fixedSize) return;
        if (p.sampleCount > v.left() / 4) {
            p.sampleCount = 0;
            return;
        }
        p.sizes.reserve(p.sampleCount);
        for (uint32_t i = 0; i < p.sampleCount; ++i) p.sizes.push_back(v.u32());
    } else if (type == fourcc("stz2")) {
        v.skip(3);
        uint32_t field = v.u8();
        uint32_t count = v.u32();
        if (field != 4 && field != 8 && field != 16) return;
        if (uint64_t(count) * field > uint64_t(v.left()) * 8) return;
        p.sampleCount = count;
        p.sizes.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            if (field == 16) p.sizes.push_back(v.u16());
            else if (field == 8) p.sizes.push_back(v.u8());
            else if (i % 2 == 0) p.sizes.push_back(v.need(1) ? v.p[v.at] >> 4 : 0);
            else p.sizes.push_back(v.u8() & 15);
        }
    } else if (type == fourcc("stco")) {
        uint32_t count = v.u32();
        if (count > v.left() / 4) return;
        p.chunks.reserve(count);
        for (uint32_t i = 0; i < count; ++i) p.chunks.push_back(v.u32());
    } else if (type == fourcc("co64")) {
        uint32_t count = v.u32();
        if (count > v.left() / 8) return;
        p.chunks.reserve(count);
        for (uint32_t i = 0; i < count; ++i) p.chunks.push_back(v.u64());
    }
}

void header(View v, Track& track) {
    uint8_t version = v.u8();
    v.skip(3);
    v.skip(version ? 16 : 8);
    track.id = v.u32();
    v.skip(4);
    v.skip(version ? 8 : 4);
    v.skip(16);
    int32_t a = int32_t(v.u32());
    int32_t b = int32_t(v.u32());
    if (!v.ok || (a == 0 && b == 0)) return;
    double angle = std::atan2(double(b), double(a)) * 180.0 / 3.14159265358979323846;
    track.rotation = ((int(std::lround(angle / 90.0)) * 90) % 360 + 360) % 360;
}

void edits(View v, Pending& p) {
    uint8_t version = v.u8();
    v.skip(3);
    uint32_t count = v.u32();
    for (uint32_t i = 0; i < count && v.left() >= (version ? 20u : 12u); ++i) {
        int64_t time;
        if (version) {
            v.skip(8);
            time = int64_t(v.u64());
        } else {
            v.skip(4);
            time = int32_t(v.u32());
        }
        v.skip(4);
        if (time >= 0 && !p.hasEdit) {
            p.track.mediaStart = time;
            p.hasEdit = true;
        }
    }
}

void media(View v, Pending& p) {
    boxes(v, [&](uint32_t type, View body) {
        if (type == fourcc("mdhd")) {
            uint8_t version = body.u8();
            body.skip(3);
            body.skip(version ? 16 : 8);
            p.track.timescale = body.u32();
        } else if (type == fourcc("hdlr")) {
            body.skip(8);
            uint32_t handler = body.u32();
            p.track.kind = handler == fourcc("vide") ? Kind::Video : handler == fourcc("soun") ? Kind::Audio : Kind::Other;
        } else if (type == fourcc("minf")) {
            boxes(body, [&](uint32_t inner, View content) {
                if (inner == fourcc("stbl")) boxes(content, [&](uint32_t name, View data) { table(name, data, p); });
            });
        }
    });
}

void trak(View v, Pending& p) {
    boxes(v, [&](uint32_t type, View body) {
        if (type == fourcc("tkhd")) header(body, p.track);
        else if (type == fourcc("edts")) boxes(body, [&](uint32_t inner, View content) { if (inner == fourcc("elst")) edits(content, p); });
        else if (type == fourcc("mdia")) media(body, p);
    });
}

void place(Pending& p) {
    Track& track = p.track;
    if (p.stsd.n >= 8) {
        View v = p.stsd;
        v.skip(8);
        bool taken = false;
        boxes(v, [&](uint32_t format, View body) {
            if (!taken) entry(format, body, track);
            taken = true;
        });
    }
    uint32_t count = p.sampleCount;
    if (!p.fixedSize) count = std::min<uint32_t>(count, uint32_t(p.sizes.size()));
    uint64_t possible = 0;
    for (const Run& run : p.stsc) possible = std::max<uint64_t>(possible, run.count);
    possible *= p.chunks.size();
    if (count > possible) count = uint32_t(possible);
    track.samples.assign(count, Sample());
    uint32_t index = 0;
    for (size_t r = 0; r < p.stsc.size() && index < count; ++r) {
        uint64_t first = p.stsc[r].first;
        uint64_t last = r + 1 < p.stsc.size() ? p.stsc[r + 1].first : p.chunks.size() + 1;
        if (first < 1) break;
        for (uint64_t c = first; c < last && c <= p.chunks.size() && index < count; ++c) {
            uint64_t offset = p.chunks[c - 1];
            for (uint32_t k = 0; k < p.stsc[r].count && index < count; ++k) {
                Sample& s = track.samples[index];
                s.size = p.fixedSize ? p.fixedSize : p.sizes[index];
                s.offset = offset;
                offset += s.size;
                ++index;
            }
        }
    }
    track.samples.resize(index);
    count = index;
    int64_t dts = 0;
    uint32_t i = 0;
    for (auto [n, delta] : p.stts) {
        for (uint32_t k = 0; k < n && i < count; ++k) {
            track.samples[i].dts = dts;
            track.samples[i].pts = dts;
            track.samples[i].duration = delta;
            dts += delta;
            ++i;
        }
        if (i >= count) break;
    }
    track.samples.resize(i);
    count = i;
    i = 0;
    for (auto [n, offset] : p.ctts) {
        for (uint32_t k = 0; k < n && i < count; ++k) track.samples[i++].pts += offset;
        if (i >= count) break;
    }
    if (p.hasStss) {
        for (uint32_t n : p.stss) if (n >= 1 && n <= count) track.samples[n - 1].sync = true;
    } else {
        for (Sample& s : track.samples) s.sync = true;
    }
}

void fragment(View v, uint64_t start, std::map<uint32_t, Pending>& tracks) {
    uint64_t next = start;
    bool firstTraf = true;
    boxes(v, [&](uint32_t type, View traf) {
        if (type != fourcc("traf")) return;
        Pending* p = nullptr;
        Defaults d;
        uint64_t base = firstTraf ? start : next;
        firstTraf = false;
        boxes(traf, [&](uint32_t name, View body) {
            if (name == fourcc("tfhd")) {
                body.skip(1);
                uint32_t flags = body.u24();
                auto found = tracks.find(body.u32());
                if (found == tracks.end()) return;
                p = &found->second;
                d = p->defaults;
                if (flags & 0x1) base = body.u64();
                else if (flags & 0x20000) base = start;
                if (flags & 0x2) body.skip(4);
                if (flags & 0x8) d.duration = body.u32();
                if (flags & 0x10) d.size = body.u32();
                if (flags & 0x20) d.flags = body.u32();
            } else if (name == fourcc("tfdt") && p) {
                uint8_t version = body.u8();
                body.skip(3);
                p->fragmentTime = version ? int64_t(body.u64()) : int64_t(body.u32());
            } else if (name == fourcc("trun") && p) {
                uint8_t version = body.u8();
                (void)version;
                uint32_t flags = body.u24();
                uint32_t count = body.u32();
                uint64_t offset = base;
                if (flags & 0x1) offset = base + int64_t(int32_t(body.u32()));
                uint32_t firstFlags = d.flags;
                bool hasFirst = flags & 0x4;
                if (hasFirst) firstFlags = body.u32();
                uint32_t each = ((flags & 0x100) ? 4 : 0) + ((flags & 0x200) ? 4 : 0) + ((flags & 0x400) ? 4 : 0) + ((flags & 0x800) ? 4 : 0);
                if (each ? count > body.left() / each : count > (1u << 24)) return;
                for (uint32_t i = 0; i < count; ++i) {
                    Sample s;
                    s.duration = (flags & 0x100) ? body.u32() : d.duration;
                    s.size = (flags & 0x200) ? body.u32() : d.size;
                    uint32_t sampleFlags = (flags & 0x400) ? body.u32() : (i == 0 && hasFirst) ? firstFlags : d.flags;
                    int32_t shift = (flags & 0x800) ? int32_t(body.u32()) : 0;
                    s.offset = offset;
                    s.dts = p->fragmentTime;
                    s.pts = s.dts + shift;
                    s.sync = !(sampleFlags & 0x10000);
                    offset += s.size;
                    p->fragmentTime += s.duration;
                    p->track.samples.push_back(s);
                }
                next = offset;
            }
        });
    });
}

}

struct Mp4Reader::Impl {
    FILE* file = nullptr;
    std::vector<Track> tracks;
    ~Impl() { if (file) fclose(file); }
};

Mp4Reader::Mp4Reader() : impl(new Impl) {}
Mp4Reader::~Mp4Reader() = default;

const std::vector<Track>& Mp4Reader::tracks() const { return impl->tracks; }

const Track* Mp4Reader::first(Kind kind) const {
    for (const Track& t : impl->tracks) if (t.kind == kind && !t.samples.empty()) return &t;
    return nullptr;
}

bool Mp4Reader::read(const Track& track, uint32_t index, uint8_t* out) {
    if (index >= track.samples.size()) return false;
    const Sample& s = track.samples[index];
    if (fseeko(impl->file, off_t(s.offset), SEEK_SET) != 0) return false;
    return fread(out, 1, s.size, impl->file) == s.size;
}

std::unique_ptr<Mp4Reader> Mp4Reader::open(const std::string& path, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    std::unique_ptr<Mp4Reader> reader(new Mp4Reader);
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) return fail("cannot open file");
    reader->impl->file = file;
    fseeko(file, 0, SEEK_END);
    uint64_t total = uint64_t(ftello(file));
    std::map<uint32_t, Pending> pending;
    std::vector<uint32_t> ids;
    std::vector<uint8_t> movie;
    std::vector<uint8_t> piece;
    bool seen = false;
    uint64_t at = 0;
    while (at + 8 <= total) {
        uint8_t head[16];
        if (fseeko(file, off_t(at), SEEK_SET) != 0 || fread(head, 1, 8, file) != 8) break;
        View h{head, 16};
        uint64_t size = h.u32();
        uint32_t type = h.u32();
        uint64_t skip = 8;
        if (size == 1) {
            if (fread(head + 8, 1, 8, file) != 8) break;
            size = h.u64();
            skip = 16;
        } else if (size == 0) {
            size = total - at;
        }
        if (size < skip || size > total - at) break;
        uint64_t body = size - skip;
        if (type == fourcc("moov") && !seen) {
            if (body > (1ull << 30)) return fail("movie header too large");
            movie.resize(size_t(body));
            if (fread(movie.data(), 1, movie.size(), file) != movie.size()) return fail("movie header cut short");
            seen = true;
            boxes(View{movie.data(), movie.size()}, [&](uint32_t name, View content) {
                if (name == fourcc("trak")) {
                    Pending p;
                    trak(content, p);
                    place(p);
                    if (p.track.id && !pending.count(p.track.id)) {
                        ids.push_back(p.track.id);
                        pending.emplace(p.track.id, std::move(p));
                    }
                } else if (name == fourcc("mvex")) {
                    boxes(content, [&](uint32_t inner, View trex) {
                        if (inner != fourcc("trex")) return;
                        trex.skip(4);
                        auto found = pending.find(trex.u32());
                        if (found == pending.end()) return;
                        trex.skip(4);
                        found->second.defaults.duration = trex.u32();
                        found->second.defaults.size = trex.u32();
                        found->second.defaults.flags = trex.u32();
                    });
                }
            });
        } else if (type == fourcc("moof") && seen) {
            if (body > (1ull << 28)) return fail("fragment header too large");
            piece.resize(size_t(body));
            if (fread(piece.data(), 1, piece.size(), file) != piece.size()) break;
            fragment(View{piece.data(), piece.size()}, at, pending);
        }
        at += size;
    }
    if (!seen) return fail("no movie header: not an MP4 or MOV file");
    for (uint32_t id : ids) {
        Track track = std::move(pending.at(id).track);
        size_t keep = 0;
        while (keep < track.samples.size() && track.samples[keep].offset <= total && track.samples[keep].size <= total - track.samples[keep].offset) ++keep;
        track.samples.resize(keep);
        for (uint32_t i = 0; i < track.samples.size(); ++i) {
            if (track.samples[i].pts >= track.mediaStart) track.order.push_back(i);
            if (track.samples[i].sync) track.syncs.push_back(i);
        }
        std::stable_sort(track.order.begin(), track.order.end(), [&](uint32_t a, uint32_t b) { return track.samples[a].pts < track.samples[b].pts; });
        reader->impl->tracks.push_back(std::move(track));
    }
    if (reader->impl->tracks.empty()) return fail("no tracks");
    return reader;
}

}

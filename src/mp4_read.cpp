#include "absent/media.h"
#include "aac_config.h"
#include "bytes.h"
#include "nal.h"
#include "readers.h"
#include "read_common.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <map>
#include <numeric>
#include "portable.h"

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
    int64_t end = INT64_MIN;
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

struct Edit {
    uint64_t duration = 0;
    int64_t time = 0;
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
    std::vector<Edit> edits;
    Defaults defaults;
    int64_t fragmentTime = 0;
};

void pcm(Track& track, int bits, bool floating, bool bigEndian) {
    track.codec = fourcc("lpcm");
    track.bits = bits;
    track.floating = floating;
    track.bigEndian = bigEndian;
}

void audioConfig(uint32_t type, View body, Track& track) {
    if (type == fourcc("esds")) {
        ElementaryStream stream;
        parseElementaryStream(body, stream);
        int kind = stream.objectTypeIndication;
        if (kind == 0x69 || kind == 0x6B) {
            track.codec = fourcc(".mp3");
            return;
        }
        if (kind == 0xDD && stream.specific.p) {
            track.codec = fourcc("vorb");
            track.configType = fourcc("vorb");
            track.config.assign(stream.specific.p, stream.specific.p + stream.specific.n);
            return;
        }
        track.configType = type;
        track.config.assign(body.p, body.p + body.n);
        AacConfig aac;
        if (track.codec == fourcc("mp4a") && parseAacConfig(stream.specific, aac)) {
            track.channels = aac.outputChannels;
            track.sampleRate = aac.outputRate;
        }
    } else if (type == fourcc("dOps") && body.n >= 11) {
        Bytes head;
        head.raw("OpusHead", 8);
        head.u8(1);
        head.u8(body.p[1]);
        head.u8(body.p[3]);
        head.u8(body.p[2]);
        for (int i = 7; i >= 4; --i) head.u8(body.p[i]);
        head.u8(body.p[9]);
        head.u8(body.p[8]);
        head.raw(body.p + 10, body.n - 10);
        track.configType = fourcc("OpHd");
        track.config = std::move(head.d);
        track.channels = body.p[1];
        track.sampleRate = 48000;
    } else if (type == fourcc("dfLa") && body.n >= 42 && (body.p[4] & 0x7F) == 0) {
        track.configType = fourcc("fLaC");
        track.config.assign(body.p + 8, body.p + 42);
        FlacInfo info;
        if (flacInfo(body.p + 8, 34, info)) {
            track.sampleRate = info.sampleRate;
            track.channels = info.channels;
            track.bits = info.bits;
        }
    } else if (type == fourcc("alac") && track.codec == fourcc("alac") && body.n >= 28) {
        track.configType = type;
        track.config.assign(body.p + 4, body.p + 28);
        track.channels = body.p[13];
        track.bits = body.p[9];
        track.sampleRate = int(View{body.p + 24, 4}.u32());
    } else if ((type == fourcc("dac3") && body.n >= 2) || (type == fourcc("dec3") && body.n >= 4)) {
        static const int rates[4] = {48000, 44100, 32000, 0};
        static const int fronts[8] = {2, 1, 2, 3, 3, 4, 4, 5};
        BitReader r{body.p, body.n};
        const bool enhanced = type == fourcc("dec3");
        if (enhanced) r.skip(16);
        int rate = rates[r.bits(2)];
        r.skip(enhanced ? 10 : 8);
        int mode = int(r.bits(3));
        track.channels = fronts[mode] + int(r.bit());
        if (rate) track.sampleRate = rate;
    } else if ((type >> 16) == 0x6D73 && type == track.codec && body.n >= 16) {
        track.configType = fourcc("wfex");
        track.config.assign(body.p, body.p + body.n);
    } else if (type == fourcc("enda") && body.n >= 2 && track.codec == fourcc("lpcm")) {
        track.bigEndian = body.p[1] == 0;
    } else if (type == fourcc("pcmC") && body.n >= 6 && track.codec == fourcc("lpcm")) {
        track.bigEndian = !(body.p[4] & 1);
        track.bits = body.p[5];
    }
}

struct SoundUnit {
    uint32_t frames = 0;
    uint32_t bytes = 0;
};

void audioEntry(uint32_t format, View v, Track& track, SoundUnit& unit) {
    uint16_t version = v.u16();
    v.skip(6);
    track.channels = v.u16();
    int bits = v.u16();
    v.skip(4);
    track.sampleRate = int(v.u32() >> 16);
    uint32_t flags = 0;
    if (version == 1) {
        unit.frames = v.u32();
        v.skip(4);
        unit.bytes = v.u32();
        v.skip(4);
    } else if (version == 2) {
        v.skip(4);
        uint64_t raw = v.u64();
        double rate = 0;
        memcpy(&rate, &raw, sizeof rate);
        if (rate >= 1 && rate <= 768000) track.sampleRate = int(std::lround(rate));
        track.channels = int(v.u32() & 0xFFFF);
        v.skip(4);
        bits = int(v.u32() & 0xFFFF);
        flags = v.u32();
        unit.bytes = v.u32();
        unit.frames = v.u32();
    }
    if (format == fourcc("sowt") && bits == 16) pcm(track, 16, false, false);
    else if (format == fourcc("twos") && bits == 16) pcm(track, 16, false, true);
    else if ((format == fourcc("twos") || format == fourcc("sowt")) && bits == 8) pcm(track, 8, false, true);
    else if ((format == fourcc("raw ") || format == fourcc("NONE")) && bits == 8) pcm(track, 8, false, false);
    else if (format == fourcc("in24")) pcm(track, 24, false, true);
    else if (format == fourcc("in32")) pcm(track, 32, false, true);
    else if (format == fourcc("fl32")) pcm(track, 32, true, true);
    else if (format == fourcc("fl64")) pcm(track, 64, true, true);
    else if (format == fourcc("ipcm")) pcm(track, bits, false, true);
    else if (format == fourcc("fpcm")) pcm(track, bits, true, true);
    else if (format == fourcc("lpcm") && version == 2 && (bits == 8) != bool(flags & 5)) pcm(track, bits, flags & 1, flags & 2);
    else if (format == fourcc(".mp3") || format == 0x6D730055) track.codec = fourcc(".mp3");
    else if (format == fourcc("fLaC")) track.bits = bits;
    else if (format == fourcc("samr") || format == fourcc("sawb")) {
        track.channels = 1;
        track.sampleRate = format == fourcc("sawb") ? 16000 : 8000;
    }
    if (format == fourcc("alaw") || format == fourcc("ulaw")) unit = {1, uint32_t(track.channels)};
    else if (format == fourcc("ima4")) unit = {64, 34 * uint32_t(track.channels)};
    boxes(v, [&](uint32_t type, View body) {
        if (type == fourcc("wave")) boxes(body, [&](uint32_t inner, View content) { audioConfig(inner, content, track); });
        else audioConfig(type, body, track);
    });
}

void videoKind(Track& track) {
    uint32_t& c = track.codec;
    const uint32_t family = c >> 8;
    if (c == fourcc("m1v ") || c == fourcc("mpg1") || c == fourcc("mpeg")) c = fourcc("mp1v");
    else if (c == fourcc("m2v1") || c == fourcc("mpg2") || family == 0x686476 || family == 0x786476 || family == 0x786435 || (c >> 16) == 0x6D78) c = fourcc("mp2v");
    else if (c == fourcc("mjpg") || c == fourcc("MJPG") || c == fourcc("AVDJ") || c == fourcc("dmb1")) c = fourcc("jpeg");
    if (c == fourcc("s263") || c == fourcc("H263")) c = fourcc("h263");
    if (c != fourcc("mp4v") || track.configType != fourcc("esds")) return;
    ElementaryStream stream;
    parseElementaryStream(View{track.config.data(), track.config.size()}, stream);
    int kind = stream.objectTypeIndication;
    if (kind == 0x20) return;
    c = kind >= 0x60 && kind <= 0x65 ? fourcc("mp2v") : kind == 0x6A ? fourcc("mp1v") : kind == 0x6C ? fourcc("jpeg") : c;
    if (c != fourcc("mp4v")) {
        track.configType = 0;
        track.config.clear();
    }
}

void entry(uint32_t format, View v, Track& track, SoundUnit& unit) {
    track.codec = format;
    v.skip(8);
    if (track.kind == Kind::Video) {
        v.skip(16);
        track.width = v.u16();
        track.height = v.u16();
        v.skip(50);
        boxes(v, [&](uint32_t type, View body) {
            if (type == fourcc("avcC") || type == fourcc("hvcC") || type == fourcc("vpcC") || type == fourcc("av1C") || type == fourcc("apvC") || type == fourcc("esds")) {
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
        videoKind(track);
    } else if (track.kind == Kind::Audio) {
        audioEntry(format, v, track, unit);
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
    p.edits.clear();
    for (uint32_t i = 0; i < count && v.left() >= (version ? 20u : 12u); ++i) {
        Edit edit;
        if (version) {
            edit.duration = v.u64();
            edit.time = int64_t(v.u64());
        } else {
            edit.duration = v.u32();
            edit.time = int32_t(v.u32());
            if (edit.duration == 0xFFFFFFFFu) edit.duration = 0;
        }
        v.skip(4);
        p.edits.push_back(edit);
    }
}

int64_t movieToTrack(uint64_t ticks, uint32_t movieScale, uint32_t trackScale) {
    unsigned __int128 scaled = (static_cast<unsigned __int128>(ticks) * trackScale + movieScale / 2) / movieScale;
    return scaled > static_cast<unsigned __int128>(INT64_MAX / 2) ? INT64_MAX / 2 : int64_t(scaled);
}

void applyEdits(Pending& p, uint32_t movieScale) {
    Track& track = p.track;
    uint64_t wait = 0;
    size_t at = 0;
    while (at < p.edits.size() && p.edits[at].time == -1) wait += p.edits[at++].duration;
    while (at < p.edits.size() && p.edits[at].time < 0) ++at;
    if (at == p.edits.size()) return;
    track.mediaStart = p.edits[at].time;
    if (!movieScale || !track.timescale) return;
    track.delay = movieToTrack(wait, movieScale, track.timescale);
    int64_t end = track.mediaStart;
    for (; at < p.edits.size() && p.edits[at].duration > 0 && p.edits[at].time >= 0 && std::llabs(p.edits[at].time - end) <= 1; ++at) {
        int64_t length = movieToTrack(p.edits[at].duration, movieScale, track.timescale);
        if (length > INT64_MAX / 2 - end) return;
        end += length;
    }
    if (end > track.mediaStart) track.mediaEnd = end;
}

std::string languageName(uint32_t code) {
    if (code == 0) return "eng";
    if (code < 0x400 || code == 0x7FFF) return "";
    std::string text(3, ' ');
    for (int i = 0; i < 3; ++i) text[i] = char(0x60 + ((code >> (10 - 5 * i)) & 31));
    for (char c : text) if (c < 'a' || c > 'z') return "";
    return text;
}

uint32_t movieScaleOf(View moov) {
    uint32_t scale = 0;
    boxes(moov, [&](uint32_t type, View body) {
        if (type != fourcc("mvhd") || scale) return;
        uint8_t version = body.u8();
        body.skip(3);
        body.skip(version ? 16 : 8);
        scale = body.u32();
    });
    return scale;
}

std::string clockText(uint64_t since1904) {
    const uint64_t toUnix = 2082844800ull;
    time_t seconds = time_t(since1904 >= toUnix ? since1904 - toUnix : since1904);
    struct tm parts;
    if (!utcParts(seconds, parts)) return "";
    char text[64];
    snprintf(text, sizeof text, "%04d-%02d-%02dT%02d:%02d:%02d.000000Z", parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min, parts.tm_sec);
    return text;
}

std::string textOf(View v) {
    std::string text(reinterpret_cast<const char*>(v.p + v.at), v.left());
    while (!text.empty() && text.back() == 0) text.pop_back();
    return text;
}

std::string tagName(uint32_t type) {
    static const std::pair<uint32_t, const char*> names[] = {
        {0xA96E616D, "title"}, {0xA9415254, "artist"}, {0xA9616C62, "album"}, {0xA9646179, "date"}, {0xA9746F6F, "encoder"},
        {0xA9636D74, "comment"}, {0xA967656E, "genre"}, {0xA9777274, "composer"}, {0xA9677270, "grouping"}, {0xA96C7972, "lyrics"},
        {0xA9737766, "encoder"}, {0xA9617574, "artist"}, {0xA9696E66, "comment"}, {0xA9637079, "copyright"},
        {fourcc("aART"), "album_artist"}, {fourcc("desc"), "description"}, {fourcc("cprt"), "copyright"}, {0xA978797A, "location"},
    };
    for (auto& name : names) if (name.first == type) return name.second;
    return "";
}

bool dataText(View item, std::string& text) {
    bool found = false;
    boxes(item, [&](uint32_t type, View body) {
        if (type != fourcc("data") || found) return;
        uint32_t kind = body.u32() & 0xFFFFFF;
        body.skip(4);
        if (!body.ok || kind != 1) return;
        text = textOf(body);
        found = true;
    });
    return found;
}

void itemList(View meta, std::map<std::string, std::string>& tags) {
    if (meta.left() >= 8 && View{meta.p + 4, 4}.u32() != fourcc("hdlr")) meta.skip(4);
    std::vector<std::string> keys;
    boxes(meta, [&](uint32_t type, View body) {
        if (type == fourcc("keys")) {
            body.skip(4);
            uint32_t count = body.u32();
            for (uint32_t i = 0; i < count && body.left() >= 8; ++i) {
                uint32_t size = body.u32();
                body.skip(4);
                if (size < 8) return;
                keys.push_back(textOf(body.sub(size - 8)));
            }
        } else if (type == fourcc("ilst")) {
            boxes(body, [&](uint32_t item, View content) {
                std::string name = item >= 1 && item <= keys.size() ? keys[item - 1] : tagName(item);
                std::string text;
                if (!name.empty() && dataText(content, text) && !tags.count(name)) tags[name] = text;
            });
        }
    });
}

void movieTags(View moov, std::map<std::string, std::string>& tags) {
    boxes(moov, [&](uint32_t type, View body) {
        if (type == fourcc("mvhd")) {
            uint8_t version = body.u8();
            body.skip(3);
            uint64_t created = version ? body.u64() : body.u32();
            if (body.ok && created) tags["creation_time"] = clockText(created);
        } else if (type == fourcc("meta")) {
            itemList(body, tags);
        } else if (type == fourcc("udta")) {
            boxes(body, [&](uint32_t inner, View content) {
                if (inner == fourcc("meta")) {
                    itemList(content, tags);
                    return;
                }
                std::string name = tagName(inner);
                if (name.empty() || tags.count(name)) return;
                std::string text;
                if (content.left() >= 8 && View{content.p + 4, 4}.u32() == fourcc("data")) {
                    if (dataText(content, text)) tags[name] = text;
                    return;
                }
                uint32_t length = content.u16();
                content.skip(2);
                if (!content.ok || length > content.left()) return;
                tags[name] = textOf(content.sub(length));
            });
        }
    });
}

void media(View v, Pending& p) {
    boxes(v, [&](uint32_t type, View body) {
        if (type == fourcc("mdhd")) {
            uint8_t version = body.u8();
            body.skip(3);
            body.skip(version ? 16 : 8);
            p.track.timescale = body.u32();
            body.skip(version ? 8 : 4);
            p.track.language = languageName(body.u16());
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

void placePcm(Pending& p, uint64_t fileSize, uint32_t unit, uint32_t frames, uint64_t group) {
    Track& track = p.track;
    uint32_t delta = p.stts[0].second;
    uint64_t left = std::min(p.sampleCount, p.stts[0].first);
    if (unit < 1 || unit > 65536 || delta > 65536 || frames < 1 || frames > 65536) return;
    left = std::min(left, fileSize / unit * frames);
    int64_t dts = 0;
    for (size_t r = 0; r < p.stsc.size() && left; ++r) {
        uint64_t first = p.stsc[r].first;
        uint64_t last = r + 1 < p.stsc.size() ? p.stsc[r + 1].first : p.chunks.size() + 1;
        if (first < 1) break;
        for (uint64_t c = first; c < last && c <= p.chunks.size() && left; ++c) {
            uint64_t offset = p.chunks[c - 1];
            uint64_t taken = std::min<uint64_t>(p.stsc[r].count, left);
            left -= taken;
            uint64_t inChunk = taken / frames;
            while (inChunk) {
                uint64_t take = std::min(inChunk, group);
                Sample s;
                s.offset = offset;
                s.size = uint32_t(take * unit);
                s.dts = s.pts = dts;
                s.duration = uint32_t(take * frames * delta);
                s.sync = true;
                track.samples.push_back(s);
                offset += s.size;
                dts += s.duration;
                inChunk -= take;
            }
        }
    }
}

void gapless(View movie, std::vector<Track*>& audio, const std::map<uint32_t, bool>& edited) {
    static const char name[] = "iTunSMPB";
    const uint8_t* end = movie.p + movie.n;
    const uint8_t* found = std::search(movie.p, end, name, name + 8);
    if (found == end) return;
    View v{found + 8, size_t(end - found - 8)};
    uint32_t size = v.u32();
    if (v.u32() != fourcc("data") || size < 16 || size - 8 > v.left()) return;
    v.skip(8);
    std::string text(reinterpret_cast<const char*>(v.p + v.at), size - 16);
    unsigned zero = 0;
    unsigned priming = 0;
    unsigned padding = 0;
    unsigned long long count = 0;
    if (sscanf(text.c_str(), " %x %x %x %llx", &zero, &priming, &padding, &count) != 4 || !count) return;
    for (Track* track : audio) {
        if (edited.at(track->id) || track->samples.empty() || track->mediaStart != 0) continue;
        const Sample& last = track->samples.back();
        int64_t total = last.pts + int64_t(last.duration);
        if (int64_t(priming) >= total || count > uint64_t(total - priming)) return;
        track->mediaStart = priming;
        track->mediaEnd = int64_t(priming + count);
        std::erase_if(track->order, [&](uint32_t i) { return track->samples[i].pts < track->mediaStart || track->samples[i].pts >= track->mediaEnd; });
        return;
    }
}

void place(Pending& p, uint64_t fileSize) {
    Track& track = p.track;
    SoundUnit unit;
    if (p.stsd.n >= 8) {
        View v = p.stsd;
        v.skip(8);
        bool taken = false;
        boxes(v, [&](uint32_t format, View body) {
            if (!taken) entry(format, body, track, unit);
            taken = true;
        });
    }
    if (track.kind == Kind::Audio && (track.codec >> 16) == 0x6D73 && track.config.empty() && unit.bytes) {
        const uint32_t tag = track.codec & 0xFFFF, ch = uint32_t(track.channels), rate = uint32_t(track.sampleRate);
        const uint8_t form[18] = {uint8_t(tag), uint8_t(tag >> 8), uint8_t(ch), uint8_t(ch >> 8), uint8_t(rate), uint8_t(rate >> 8), uint8_t(rate >> 16),
            uint8_t(rate >> 24), 0, 0, 0, 0, uint8_t(unit.bytes), uint8_t(unit.bytes >> 8), 4, 0, 0, 0};
        track.configType = fourcc("wfex");
        track.config.assign(form, form + 18);
    }
    const bool framed = p.fixedSize == 1 && unit.frames && unit.bytes && waveDecoded(track.codec);
    if (track.kind == Kind::Audio && p.fixedSize && p.stts.size() == 1 && track.channels > 0 && (framed || (track.codec == fourcc("lpcm") && track.bits >= 8))) {
        if (framed) placePcm(p, fileSize, unit.bytes, unit.frames, unit.frames >= 160 ? 1 : std::max<uint32_t>(1, 1024 / unit.frames));
        else placePcm(p, fileSize, p.fixedSize == 1 ? uint32_t(track.channels) * uint32_t(track.bits / 8) : p.fixedSize, 1, 4096);
        return;
    }
    uint32_t count = p.sampleCount;
    if (!p.fixedSize) count = std::min<uint32_t>(count, uint32_t(p.sizes.size()));
    else count = uint32_t(std::min<uint64_t>(count, fileSize / p.fixedSize));
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

void fragment(View v, uint64_t start, std::map<uint32_t, Pending>& tracks, uint64_t& samplesLeft) {
    uint64_t next = start;
    bool firstTraf = true;
    boxes(v, [&](uint32_t type, View traf) {
        if (type != fourcc("traf")) return;
        Pending* p = nullptr;
        Defaults d;
        bool firstRun = true;
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
                p->fragmentTime = version ? int64_t(std::min<uint64_t>(body.u64(), uint64_t(INT64_MAX / 2))) : int64_t(body.u32());
            } else if (name == fourcc("trun") && p) {
                uint8_t version = body.u8();
                (void)version;
                uint32_t flags = body.u24();
                uint32_t count = body.u32();
                uint64_t offset = (flags & 0x1) ? base + int64_t(int32_t(body.u32())) : firstRun ? base : next;
                firstRun = false;
                uint32_t firstFlags = d.flags;
                bool hasFirst = flags & 0x4;
                if (hasFirst) firstFlags = body.u32();
                uint32_t each = ((flags & 0x100) ? 4 : 0) + ((flags & 0x200) ? 4 : 0) + ((flags & 0x400) ? 4 : 0) + ((flags & 0x800) ? 4 : 0);
                if (each ? count > body.left() / each : count > (1u << 24)) return;
                if (count > samplesLeft) return;
                samplesLeft -= count;
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
    std::map<std::string, std::string> tags;
    bool quicktime = false;
    ~Impl() { if (file) fclose(file); }
};

Mp4Reader::Mp4Reader() : impl(new Impl) {}
Mp4Reader::~Mp4Reader() = default;

const std::vector<Track>& Mp4Reader::tracks() const { return impl->tracks; }

const char* Mp4Reader::format() const { return impl->quicktime ? "mov" : "mp4"; }

const std::map<std::string, std::string>& Mp4Reader::tags() const { return impl->tags; }

bool Mp4Reader::read(const Track& track, uint32_t index, uint8_t* out) {
    if (index >= track.samples.size()) return false;
    const Sample& s = track.samples[index];
    if (fseeko(impl->file, int64_t(s.offset), SEEK_SET) != 0) return false;
    return fread(out, 1, s.size, impl->file) == s.size;
}

std::unique_ptr<Mp4Reader> Mp4Reader::open(const std::string& path, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    std::unique_ptr<Mp4Reader> reader(new Mp4Reader);
    FILE* file = openRead(path);
    if (!file) return fail("cannot open file");
    reader->impl->file = file;
    fseeko(file, 0, SEEK_END);
    uint64_t total = uint64_t(ftello(file));
    std::map<uint32_t, Pending> pending;
    std::map<uint32_t, bool> edited;
    std::vector<uint32_t> ids;
    std::vector<uint8_t> movie;
    std::vector<uint8_t> piece;
    bool seen = false;
    uint64_t at = 0;
    uint64_t samplesLeft = total;
    while (at + 8 <= total) {
        uint8_t head[16];
        if (fseeko(file, int64_t(at), SEEK_SET) != 0 || fread(head, 1, 8, file) != 8) break;
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
        if (type == fourcc("ftyp") && at == 0 && body >= 4) {
            uint8_t brand[4];
            if (fread(brand, 1, 4, file) == 4) reader->impl->quicktime = View{brand, 4}.u32() == fourcc("qt  ");
        }
        if (type == fourcc("moov") && !seen) {
            if (body > (1ull << 30)) return fail("movie header too large");
            movie.resize(size_t(body));
            if (fread(movie.data(), 1, movie.size(), file) != movie.size()) return fail("movie header cut short");
            seen = true;
            uint32_t movieScale = movieScaleOf(View{movie.data(), movie.size()});
            movieTags(View{movie.data(), movie.size()}, reader->impl->tags);
            boxes(View{movie.data(), movie.size()}, [&](uint32_t name, View content) {
                if (name == fourcc("mvhd")) {
                    uint8_t version = content.u8();
                    content.skip(version ? 19 : 11);
                    movieScale = content.u32();
                } else if (name == fourcc("trak")) {
                    Pending p;
                    trak(content, p);
                    place(p, total);
                    applyEdits(p, movieScale);
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
            fragment(View{piece.data(), piece.size()}, at, pending, samplesLeft);
        }
        at += size;
    }
    if (!seen) return fail("no movie header: not an MP4 or MOV file");
    for (uint32_t id : ids) {
        edited[id] = std::any_of(pending.at(id).edits.begin(), pending.at(id).edits.end(), [](const Edit& e) { return e.time >= 0; });
        Track track = std::move(pending.at(id).track);
        size_t keep = 0;
        while (keep < track.samples.size() && track.samples[keep].offset <= total && track.samples[keep].size <= total - track.samples[keep].offset) ++keep;
        track.samples.resize(keep);
        if (waveDecoded(track.codec) && track.mediaEnd >= 0) {
            for (const Sample& s : track.samples) {
                const int64_t end = s.pts + std::max<int64_t>(s.duration, waveFrames(track, s.size));
                if (s.pts < track.mediaEnd && end > track.mediaEnd) track.mediaEnd = end;
            }
        }
        for (uint32_t i = 0; i < track.samples.size(); ++i) {
            if (track.samples[i].pts >= track.mediaStart && (track.mediaEnd < 0 || track.samples[i].pts < track.mediaEnd)) track.order.push_back(i);
            if (track.samples[i].sync) track.syncs.push_back(i);
        }
        std::stable_sort(track.order.begin(), track.order.end(), [&](uint32_t a, uint32_t b) { return track.samples[a].pts < track.samples[b].pts; });
        reader->impl->tracks.push_back(std::move(track));
    }
    if (reader->impl->tracks.empty()) return fail("no tracks");
    std::vector<Track*> audio;
    for (Track& track : reader->impl->tracks) if (track.kind == Kind::Audio) audio.push_back(&track);
    gapless(View{movie.data(), movie.size()}, audio, edited);
    return reader;
}

}

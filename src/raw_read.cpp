#include "read_common.h"
#include "nal.h"
#include "readers.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace absent {

bool dolbyFrame(const uint8_t* h, DolbyFrame& f) {
    static const int rates[3] = {48000, 44100, 32000};
    static const int kilobits[19] = {32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 576, 640};
    static const int fronts[8] = {2, 1, 2, 3, 3, 4, 4, 5};
    if (h[0] != 0x0B || h[1] != 0x77) return false;
    int version = h[5] >> 3;
    BitReader r{h, 8};
    r.skip(16);
    f.dependent = false;
    if (version <= 10) {
        r.skip(16);
        int rateCode = int(r.bits(2));
        int sizeCode = int(r.bits(6));
        if (rateCode == 3 || sizeCode >= 38) return false;
        int rate = kilobits[sizeCode >> 1];
        f.rate = rates[rateCode];
        uint32_t words = rateCode == 0 ? uint32_t(rate * 2) : rateCode == 2 ? uint32_t(rate * 3) : uint32_t(rate * 320 / 147 + (sizeCode & 1));
        f.bytes = words * 2;
        r.skip(8);
        f.mode = int(r.bits(3));
        if ((f.mode & 1) && f.mode != 1) r.skip(2);
        if (f.mode & 4) r.skip(2);
        if (f.mode == 2) r.skip(2);
        f.lfe = r.bit();
        f.channels = fronts[f.mode] + int(f.lfe);
        f.samples = 1536;
        f.codec = fourcc("ac-3");
        return true;
    }
    if (version > 16) return false;
    static const int blocks[4] = {1, 2, 3, 6};
    int kind = int(r.bits(2));
    r.skip(3);
    f.bytes = (r.bits(11) + 1) * 2;
    int rateCode = int(r.bits(2));
    int count = 6;
    if (rateCode == 3) {
        int half = int(r.bits(2));
        if (half == 3) return false;
        f.rate = rates[half] / 2;
    } else {
        count = blocks[r.bits(2)];
        f.rate = rates[rateCode];
    }
    f.mode = int(r.bits(3));
    f.lfe = r.bit();
    f.channels = fronts[f.mode] + int(f.lfe);
    f.samples = uint32_t(256 * count);
    f.codec = fourcc("ec-3");
    f.dependent = kind == 1;
    return f.bytes >= 8;
}

uint32_t amrFrameBytes(bool wide, uint8_t first) {
    static const uint8_t narrowSizes[16] = {12, 13, 15, 17, 19, 20, 26, 31, 5, 0, 0, 0, 0, 0, 0, 0};
    static const uint8_t wideSizes[16] = {17, 23, 32, 36, 40, 46, 50, 58, 60, 5, 0, 0, 0, 0, 0, 0};
    return 1u + (wide ? wideSizes : narrowSizes)[(first >> 3) & 15];
}

namespace {

const uint64_t kSearch = 1 << 20;
const int kConfirm = 3;

struct Adts {
    int profile = 0;
    int rateIndex = 0;
    int channelCode = 0;
    uint32_t header = 0;
    uint32_t size = 0;
    uint32_t blocks = 1;
};

bool adts(const uint8_t* p, Adts& out) {
    if (p[0] != 0xFF || (p[1] & 0xF6) != 0xF0) return false;
    out.header = p[1] & 1 ? 7 : 9;
    out.profile = p[2] >> 6;
    out.rateIndex = (p[2] >> 2) & 15;
    out.channelCode = ((p[2] & 1) << 2) | (p[3] >> 6);
    out.size = (uint32_t(p[3] & 3) << 11) | (uint32_t(p[4]) << 3) | (p[5] >> 5);
    out.blocks = (p[6] & 3) + 1u;
    return out.rateIndex < 13 && out.size > out.header;
}

struct Scanner {
    Source& source;
    uint64_t end;
    bool aac = false;
    Adts firstAdts;
    MpegAudio firstMpeg;

    uint32_t frameAt(uint64_t at, bool first) {
        if (at > end || end - at < 7) return 0;
        const uint8_t* p = source.view(at, 7);
        if (!p) return 0;
        if (aac) {
            Adts a;
            if (!adts(p, a)) return 0;
            if (first) firstAdts = a;
            else if (a.rateIndex != firstAdts.rateIndex || a.profile != firstAdts.profile || a.channelCode != firstAdts.channelCode) return 0;
            return a.size;
        }
        MpegAudio m;
        if (!mpegAudio(p, m)) return 0;
        if (first) firstMpeg = m;
        else if (m.layer != firstMpeg.layer) return 0;
        return uint32_t(m.size);
    }

    bool chain(uint64_t at, bool first) {
        for (int i = 0; i < kConfirm; ++i) {
            uint32_t size = frameAt(at, first && i == 0);
            if (!size) return i > 0 && end - at < 7;
            if (size > end - at) return i > 0;
            at += size;
            if (at == end) return true;
        }
        return true;
    }

    uint64_t next(uint64_t from, uint64_t limit, bool first) {
        uint64_t stop = std::min(end, from + limit);
        while (from < stop) {
            static const uint8_t mark = 0xFF;
            from = source.find(from, stop, &mark, 1);
            if (from >= stop) break;
            if (first) {
                const uint8_t* p = source.view(from, std::min<uint64_t>(2, end - from));
                aac = p && end - from >= 2 && (p[1] & 0xF6) == 0xF0;
            }
            if (chain(from, first)) return from;
            ++from;
        }
        return end;
    }
};

void lameInfo(const uint8_t* frame, size_t size, const MpegAudio& m, bool& infoFrame, int& delay, int& padding) {
    size_t side = m.version == 1 ? (m.channels == 1 ? 17 : 32) : (m.channels == 1 ? 9 : 17);
    for (size_t at : {4 + side, 6 + side}) {
        if (size < at + 8 || (memcmp(frame + at, "Xing", 4) != 0 && memcmp(frame + at, "Info", 4) != 0)) continue;
        infoFrame = true;
        uint32_t flags = be32(frame + at + 4);
        size_t extra = at + 8 + (flags & 1 ? 4 : 0) + (flags & 2 ? 4 : 0) + (flags & 4 ? 100 : 0) + (flags & 8 ? 4 : 0);
        if (size < extra + 24 || frame[extra] < 0x20 || frame[extra] > 0x7E) return;
        delay = (frame[extra + 21] << 4) | (frame[extra + 22] >> 4);
        padding = ((frame[extra + 22] & 15) << 8) | frame[extra + 23];
        return;
    }
    if (size >= 40 && memcmp(frame + 36, "VBRI", 4) == 0) infoFrame = true;
}

const char* packets(FileReader& reader, uint64_t start) {
    Source& source = reader.source;
    uint64_t end = source.size();
    if (end >= start + 128) {
        const uint8_t* tail = source.view(end - 128, 3);
        if (tail && memcmp(tail, "TAG", 3) == 0) end -= 128;
    }
    Scanner scan{source, end, false, {}, {}};
    uint64_t at = scan.next(start, kSearch, true);
    if (at >= end) return "no MP3 or AAC frames found";
    Track track;
    track.id = 1;
    track.kind = Kind::Audio;
    int delay = -1, padding = 0;
    if (scan.aac) {
        static const int rates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
        const Adts& a = scan.firstAdts;
        reader.name = "aac";
        track.codec = fourcc("mp4a");
        track.configType = fourcc("ASC ");
        track.config = {uint8_t(((a.profile + 1) << 3) | (a.rateIndex >> 1)), uint8_t(((a.rateIndex & 1) << 7) | (a.channelCode << 3))};
        track.sampleRate = rates[a.rateIndex];
        track.channels = a.channelCode == 7 ? 8 : a.channelCode;
    } else {
        const MpegAudio& m = scan.firstMpeg;
        reader.name = m.layer == 3 ? "mp3" : m.layer == 2 ? "mp2" : "mp1";
        track.codec = m.layer == 3 ? fourcc(".mp3") : m.layer == 2 ? fourcc(".mp2") : fourcc(".mp1");
        track.sampleRate = m.sampleRate;
        track.channels = m.channels;
        const uint8_t* frame = source.view(at, size_t(m.size));
        bool infoFrame = false;
        if (frame) lameInfo(frame, size_t(m.size), m, infoFrame, delay, padding);
        if (infoFrame) at += uint64_t(m.size);
    }
    track.timescale = uint32_t(track.sampleRate);
    int64_t time = 0;
    long double exact = 0;
    while (at < end) {
        uint32_t size = scan.frameAt(at, false);
        if (!size || size > end - at) {
            at = scan.next(at + 1, end - at, false);
            continue;
        }
        Sample s;
        const uint8_t* p = source.view(at, 7);
        if (!p) break;
        if (scan.aac) {
            Adts a;
            adts(p, a);
            s.offset = at + a.header;
            s.size = size - a.header;
            s.duration = 1024 * a.blocks;
        } else {
            MpegAudio m;
            mpegAudio(p, m);
            s.offset = at;
            s.size = size;
            exact += (long double)m.samples * track.sampleRate / m.sampleRate;
            s.duration = uint32_t(std::max<int64_t>(1, std::llroundl(exact) - time));
        }
        s.pts = s.dts = time;
        s.sync = true;
        time += s.duration;
        track.samples.push_back(s);
        at += size;
    }
    if (track.samples.empty()) return "no MP3 or AAC frames found";
    if (delay >= 0) {
        track.mediaStart = std::min<int64_t>(time, delay + 529);
        track.mediaEnd = std::max<int64_t>(track.mediaStart, time - std::max(0, padding - 529));
    }
    indexTrack(track);
    reader.list.push_back(std::move(track));
    if (!scan.aac) id3v1(source, reader.found);
    return nullptr;
}

const char* dolby(FileReader& reader, uint64_t at) {
    Source& source = reader.source;
    const uint64_t end = source.size();
    Track track;
    track.id = 1;
    track.kind = Kind::Audio;
    int64_t time = 0;
    uint64_t lost = 0;
    while (end - at >= 8 && lost < kSearch) {
        const uint8_t* p = source.view(at, 8);
        DolbyFrame f;
        if (!p || !dolbyFrame(p, f) || f.bytes > end - at || (!track.samples.empty() && (f.rate != track.sampleRate || f.codec != track.codec))) {
            ++at;
            ++lost;
            continue;
        }
        lost = 0;
        if (f.dependent && !track.samples.empty() && track.samples.back().offset + track.samples.back().size == at && track.samples.back().size < (1u << 20)) {
            track.samples.back().size += f.bytes;
            at += f.bytes;
            continue;
        }
        if (track.samples.empty()) {
            if (f.dependent) {
                at += f.bytes;
                continue;
            }
            track.codec = f.codec;
            track.sampleRate = f.rate;
            track.timescale = uint32_t(f.rate);
            track.channels = f.channels;
        }
        Sample s;
        s.offset = at;
        s.size = f.bytes;
        s.pts = s.dts = time;
        s.duration = f.samples;
        s.sync = true;
        time += f.samples;
        track.samples.push_back(s);
        at += f.bytes;
    }
    if (track.samples.empty()) return "no AC-3 frames found";
    reader.name = track.codec == fourcc("ac-3") ? "ac3" : "eac3";
    indexTrack(track);
    reader.list.push_back(std::move(track));
    return nullptr;
}

const char* amr(FileReader& reader, bool wide) {
    Source& source = reader.source;
    const uint64_t end = source.size();
    reader.name = "amr";
    Track track;
    track.id = 1;
    track.kind = Kind::Audio;
    track.codec = wide ? fourcc("sawb") : fourcc("samr");
    track.sampleRate = wide ? 16000 : 8000;
    track.timescale = uint32_t(track.sampleRate);
    track.channels = 1;
    const uint32_t step = wide ? 320 : 160;
    uint64_t at = wide ? 9 : 6;
    while (at < end) {
        const uint8_t* p = source.view(at, 1);
        if (!p) break;
        uint32_t size = amrFrameBytes(wide, p[0]);
        if (size > end - at) break;
        Sample s;
        s.offset = at;
        s.size = size;
        s.pts = s.dts = int64_t(track.samples.size()) * step;
        s.duration = step;
        s.sync = true;
        track.samples.push_back(s);
        at += size;
    }
    if (track.samples.empty()) return "no sound frames in this AMR file";
    indexTrack(track);
    reader.list.push_back(std::move(track));
    return nullptr;
}

struct Candidate {
    uint64_t at;
    uint64_t number;
    uint32_t block;
    bool variable;
};

}

int64_t flacFrames(Source& source, uint64_t at, uint64_t limit, const FlacInfo& info, Track& track) {
    std::vector<Candidate> found;
    uint64_t from = at;
    while (limit - from >= 6) {
        size_t want = size_t(std::min<uint64_t>(limit - from, 1 << 16));
        const uint8_t* w = source.view(from, want);
        if (!w) break;
        size_t usable = want == limit - from ? want : want - 16;
        for (size_t i = 0; i < usable && i + 6 <= want; ++i) {
            if (w[i] != 0xFF || (w[i + 1] & 0xFE) != 0xF8) continue;
            FlacFrame f;
            if (!flacFrame(w + i, want - i, f)) continue;
            if ((f.sampleRate && f.sampleRate != info.sampleRate) || (f.bits && f.bits != info.bits) || f.channels != info.channels) continue;
            if (!found.empty() && f.variable != found.front().variable) continue;
            found.push_back({from + i, f.number, f.blockSize, f.variable});
        }
        from += usable;
    }
    if (found.empty()) return -1;
    const uint64_t fixedBlock = found.front().block;
    for (Candidate& c : found) if (!c.variable) c.number *= fixedBlock;
    const uint64_t reach = info.maxBlock ? uint64_t(info.maxBlock) * info.channels * 5 + 64 : 4u << 20;
    int64_t end = 0;
    size_t current = 0;
    while (current < found.size()) {
        const Candidate& c = found[current];
        size_t next = found.size();
        for (size_t j = current + 1; j < found.size() && j - current <= 256 && found[j].at - c.at <= reach; ++j) {
            if (found[j].number == c.number + c.block) {
                next = j;
                break;
            }
        }
        for (size_t j = current + 1; next == found.size() && j < found.size(); ++j) {
            if (found[j].number >= c.number + c.block) next = j;
        }
        uint64_t stop = next < found.size() ? found[next].at : limit;
        if (stop - c.at <= 0x7FFFFFFF) {
            Sample s;
            s.offset = c.at;
            s.size = uint32_t(stop - c.at);
            s.pts = s.dts = int64_t(c.number);
            s.duration = c.block;
            s.sync = true;
            end = std::max(end, s.pts + int64_t(s.duration));
            track.samples.push_back(s);
        }
        current = next;
    }
    return end;
}

namespace {

const char* flac(FileReader& reader, uint64_t start) {
    Source& source = reader.source;
    const uint64_t total = source.size();
    reader.name = "flac";
    Track track;
    track.id = 1;
    track.kind = Kind::Audio;
    track.codec = fourcc("fLaC");
    track.configType = fourcc("fLaC");
    uint64_t at = start + 4;
    for (;;) {
        const uint8_t* h = source.view(at, 4);
        if (!h) break;
        uint8_t kind = h[0] & 0x7F;
        bool last = h[0] & 0x80;
        uint32_t size = be24(h + 1);
        if (size > total - at - 4) break;
        std::vector<uint8_t> body;
        if (kind == 0 && size >= 34 && track.config.empty() && source.load(at + 4, 34, body, 34)) track.config = body;
        else if (kind == 4 && source.load(at + 4, size, body, 16u << 20)) vorbisComments(body.data(), body.size(), reader.found);
        at += 4 + uint64_t(size);
        if (last) break;
    }
    FlacInfo info;
    if (!flacInfo(track.config.data(), track.config.size(), info)) return "no stream description in this FLAC file";
    track.channelMask = takeChannelMask(reader.found);
    track.sampleRate = info.sampleRate;
    track.timescale = uint32_t(info.sampleRate);
    track.channels = info.channels;
    track.bits = info.bits;

    const int64_t end = flacFrames(source, at, total, info, track);
    if (end < 0) return "no sound frames in this FLAC file";
    if (info.total > 0 && int64_t(info.total) < end) track.mediaEnd = int64_t(info.total);
    indexTrack(track);
    reader.list.push_back(std::move(track));
    return nullptr;
}

}

std::unique_ptr<Reader> openRawAudio(const std::string& path, std::string* error) {
    auto reader = std::make_unique<FileReader>();
    Source& source = reader->source;
    const char* problem = nullptr;
    if (!source.open(path)) {
        problem = "cannot open file";
    } else {
        uint64_t at = 0;
        while (uint64_t skip = id3v2(source, at, reader->found)) {
            if (skip > source.size() - at) break;
            at += skip;
        }
        const uint8_t* head = source.view(at, std::min<uint64_t>(12, source.size() - at));
        const size_t have = size_t(std::min<uint64_t>(12, source.size() - at));
        auto starts = [&](const char* text) { return head && have >= strlen(text) && memcmp(head, text, strlen(text)) == 0; };
        if (starts("fLaC")) problem = flac(*reader, at);
        else if (starts("#!AMR-WB\n")) problem = amr(*reader, true);
        else if (starts("#!AMR\n")) problem = amr(*reader, false);
        else if (starts("#!AMR")) problem = "multichannel AMR files are not supported";
        else if (have >= 2 && head[0] == 0x0B && head[1] == 0x77) problem = dolby(*reader, at);
        else problem = packets(*reader, at);
    }
    if (problem) {
        if (error) *error = problem;
        return nullptr;
    }
    return reader;
}

}

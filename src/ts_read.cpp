#include "bytes.h"
#include "nal.h"
#include "readers.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <deque>

namespace absent {

namespace {

const int64_t kClockMask = (int64_t(1) << 33) - 1;
const uint64_t kLargestUnit = 64u << 20;
const uint64_t kNowhere = ~uint64_t(0);
const size_t kPacket = 188;

enum class Flavor { Avc, Hevc, Adts, MpegAudio, Dolby, Guess };

struct Place {
    uint64_t file = kNowhere;
    uint8_t skip = 0;
};

struct Where {
    uint32_t raw = 0;
    uint8_t skip = 0;
};

struct Geometry {
    size_t size = kPacket;
    size_t lead = 0;
};

uint32_t sectionCrc(const uint8_t* p, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        crc ^= uint32_t(p[i]) << 24;
        for (int k = 0; k < 8; ++k) crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
    }
    return crc;
}

int64_t stampAt(const uint8_t* b) {
    return (int64_t((b[0] >> 1) & 7) << 30) | (int64_t(b[1]) << 22) | (int64_t(b[2] >> 1) << 15) | (int64_t(b[3]) << 7) | int64_t(b[4] >> 1);
}

int64_t unwrap(int64_t raw, int64_t near) {
    int64_t step = (raw - near) & kClockMask;
    if (step > kClockMask / 2) step -= kClockMask + 1;
    return near + step;
}

struct Pes {
    enum class State { Idle, Header, Payload };
    State state = State::Idle;
    uint8_t head[9 + 255];
    size_t have = 0;
    bool fresh = false;
    bool timed = false;
    int64_t pts = 0;
    int64_t dts = 0;

    std::pair<const uint8_t*, size_t> feed(const uint8_t* p, size_t n, bool starts) {
        fresh = false;
        if (starts) {
            state = State::Header;
            have = 0;
        }
        if (state == State::Idle) return {p, 0};
        if (state == State::Payload) return {p, n};
        while (n) {
            size_t want = have < 9 ? 9 : 9 + size_t(head[8]);
            size_t take = std::min(n, want - have);
            memcpy(head + have, p, take);
            have += take;
            p += take;
            n -= take;
            if (have < 9) break;
            if (have == 9) {
                uint8_t id = head[3];
                bool plain = id == 0xBC || id == 0xBE || id == 0xBF || id == 0xF0 || id == 0xF1 || id == 0xF2 || id == 0xF8 || id == 0xFF;
                if (head[0] != 0 || head[1] != 0 || head[2] != 1 || plain || (head[6] & 0xC0) != 0x80) {
                    state = State::Idle;
                    return {p, 0};
                }
            }
            if (have == 9 + size_t(head[8])) {
                int flags = head[7] >> 6;
                timed = false;
                if ((flags & 2) && head[8] >= 5) {
                    pts = stampAt(head + 9);
                    dts = flags == 3 && head[8] >= 10 ? stampAt(head + 14) : pts;
                    timed = true;
                }
                state = State::Payload;
                fresh = true;
                return {p, n};
            }
        }
        return {p, 0};
    }
};

struct Payload {
    const uint8_t* p = nullptr;
    size_t n = 0;
    uint16_t pid = 0;
    int counter = 0;
    bool starts = false;
    bool scrambled = false;
    bool jump = false;
    bool hollow = false;
};

bool payloadOf(const uint8_t* packet, Payload& out) {
    if (packet[1] & 0x80) return false;
    out.pid = uint16_t(((packet[1] & 0x1F) << 8) | packet[2]);
    out.starts = packet[1] & 0x40;
    out.scrambled = packet[3] >> 6;
    out.counter = packet[3] & 15;
    out.jump = false;
    int control = (packet[3] >> 4) & 3;
    size_t start = 4;
    if (control & 2) {
        size_t length = packet[4];
        if (length > 183) return false;
        if (length > 0) out.jump = packet[5] & 0x80;
        start = 5 + length;
    }
    out.hollow = (control & 1) && start == kPacket;
    if (!(control & 1) || start >= kPacket) return false;
    out.p = packet + start;
    out.n = kPacket - start;
    return true;
}

struct Walker {
    FILE* file = nullptr;
    Geometry shape;
    std::vector<uint8_t> buffer;
    uint64_t base = 0;
    size_t fill = 0;
    size_t at = 0;
    bool synced = true;

    void start(uint64_t offset, size_t room) {
        buffer.resize(room);
        base = offset;
        fill = at = 0;
        synced = true;
        fseeko(file, off_t(offset), SEEK_SET);
    }
    bool ensure(size_t count) {
        if (fill - at >= count) return true;
        memmove(buffer.data(), buffer.data() + at, fill - at);
        base += at;
        fill -= at;
        at = 0;
        fill += fread(buffer.data() + fill, 1, buffer.size() - fill, file);
        return fill - at >= count;
    }
    bool next(const uint8_t*& packet, uint64_t& offset) {
        for (;;) {
            if (!ensure(shape.size)) return false;
            bool good = buffer[at + shape.lead] == 0x47;
            if (good && !synced && ensure(2 * shape.size)) good = buffer[at + shape.size + shape.lead] == 0x47;
            if (!good) {
                synced = false;
                ++at;
                continue;
            }
            synced = true;
            packet = buffer.data() + at + shape.lead;
            offset = base + at;
            at += shape.size;
            return true;
        }
    }
};

struct AudioFrame {
    uint32_t bytes = 0;
    uint32_t strip = 0;
    uint32_t samples = 0;
    int rate = 0;
    int channels = 0;
    uint32_t codec = 0;
    bool joins = false;
    uint8_t config[2] = {0, 0};
};

bool adtsHeader(const uint8_t* h, AudioFrame& f) {
    static const int rates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
    if (h[0] != 0xFF || (h[1] & 0xF6) != 0xF0) return false;
    int object = (h[2] >> 6) + 1;
    int index = (h[2] >> 2) & 15;
    int layout = ((h[2] & 1) << 2) | (h[3] >> 6);
    if (index > 12) return false;
    f.bytes = (uint32_t(h[3] & 3) << 11) | (uint32_t(h[4]) << 3) | (h[5] >> 5);
    f.strip = (h[1] & 1) ? 7 : 9;
    if (f.bytes <= f.strip) return false;
    f.samples = 1024 * ((h[6] & 3) + 1);
    f.rate = rates[index];
    f.channels = layout == 7 ? 8 : layout;
    f.codec = fourcc("mp4a");
    f.config[0] = uint8_t((object << 3) | (index >> 1));
    f.config[1] = uint8_t((index << 7) | (layout << 3));
    return true;
}

bool mpegAudioHeader(const uint8_t* h, AudioFrame& f) {
    static const int rates[3] = {44100, 48000, 32000};
    static const int kilobits[5][15] = {
        {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},
        {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320},
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
    };
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return false;
    int version = (h[1] >> 3) & 3;
    int layer = 4 - ((h[1] >> 1) & 3);
    int rateIndex = (h[2] >> 2) & 3;
    int bitIndex = h[2] >> 4;
    int padding = (h[2] >> 1) & 1;
    if (version == 1 || layer == 4 || rateIndex == 3 || bitIndex == 0 || bitIndex == 15) return false;
    bool first = version == 3;
    int row = first ? layer - 1 : layer == 1 ? 3 : 4;
    int bits = kilobits[row][bitIndex] * 1000;
    f.rate = rates[rateIndex] >> (first ? 0 : version == 2 ? 1 : 2);
    if (layer == 1) {
        f.bytes = uint32_t((12 * bits / f.rate + padding) * 4);
        f.samples = 384;
    } else if (layer == 2 || first) {
        f.bytes = uint32_t(144 * bits / f.rate + padding);
        f.samples = 1152;
    } else {
        f.bytes = uint32_t(72 * bits / f.rate + padding);
        f.samples = 576;
    }
    f.strip = 0;
    f.channels = (h[3] >> 6) == 3 ? 1 : 2;
    f.codec = layer == 3 ? fourcc(".mp3") : layer == 2 ? fourcc(".mp2") : fourcc(".mp1");
    return f.bytes >= 4;
}

bool dolbyHeader(const uint8_t* h, AudioFrame& f) {
    DolbyFrame d;
    if (!dolbyFrame(h, d)) return false;
    f.strip = 0;
    f.rate = d.rate;
    f.bytes = d.bytes;
    f.channels = d.channels;
    f.samples = d.samples;
    f.codec = d.codec;
    f.joins = d.dependent;
    return true;
}

struct Chunk {
    uint64_t at = 0;
    uint32_t size = 0;
    uint64_t file = 0;
    uint8_t skip = 0;
};

struct Stamp {
    uint64_t at = 0;
    int64_t pts = 0;
    int64_t dts = 0;
};

struct Unit {
    bool open = false;
    uint64_t start = 0;
    Place place;
    uint64_t size = 0;
    bool picture = false;
    bool key = false;
    bool stamped = false;
    int64_t pts = 0;
    int64_t dts = 0;
    bool field = false;
    bool bottom = false;
    uint32_t frameNumber = 0;
    int fields = 0;
};

struct Stream {
    uint16_t pid = 0;
    Flavor flavor = Flavor::Avc;
    Track track;
    std::vector<Where> where;
    int counter = -1;
    Pes pes;
    uint64_t position = 0;
    std::array<Chunk, 16> recent;
    size_t recentAt = 0;
    std::deque<Stamp> stamps;
    bool clocked = false;
    int64_t clock = 0;

    int zeros = 0;
    bool inside = false;
    uint64_t nalStart = 0;
    uint64_t nalBody = 0;
    Place nalPlace;
    std::vector<uint8_t> head;
    size_t limit = 0;
    Unit current;
    Unit lead;
    NalList vps, sps, pps;
    VideoInfo info;
    bool started = false;
    bool needKey = false;

    uint8_t header[8];
    size_t have = 0;
    uint64_t remaining = 0;
    uint64_t frameStart = 0;
    uint64_t frameEnd = 0;
    Place framePlace;
    AudioFrame frame;
    bool anchored = false;
    int64_t anchor = 0;
    uint64_t sinceAnchor = 0;

    bool video() const { return flavor == Flavor::Avc || flavor == Flavor::Hevc; }

    Place locate(uint64_t position) const {
        for (const Chunk& c : recent) {
            if (c.size && position >= c.at && position < c.at + c.size) return {c.file, uint8_t(c.skip + (position - c.at))};
        }
        return {};
    }

    bool stampFor(uint64_t position, Stamp& out) {
        bool found = false;
        while (!stamps.empty() && stamps.front().at <= position) {
            out = stamps.front();
            stamps.pop_front();
            found = true;
        }
        return found;
    }

    void lose() {
        if (current.picture && lead.open) emit(current, lead.start);
        pes.state = Pes::State::Idle;
        stamps.clear();
        inside = false;
        zeros = 0;
        current = Unit();
        lead = Unit();
        needKey = true;
        have = 0;
        remaining = 0;
        anchored = false;
    }

    void begin(Unit& unit) {
        unit.open = true;
        unit.start = nalStart;
        unit.place = nalPlace;
        Stamp stamp;
        if (stampFor(nalStart, stamp)) {
            unit.stamped = true;
            unit.pts = stamp.pts;
            unit.dts = stamp.dts;
        }
    }

    void emit(Unit& unit, uint64_t end) {
        Unit u = unit;
        unit = Unit();
        if (!u.open || !u.picture || !u.size || end <= u.start) return;
        uint64_t raw = end - u.start;
        if (raw > kLargestUnit || u.size > kLargestUnit || u.place.file == kNowhere) return;
        if (!started) {
            if (!u.key) return;
            std::vector<uint8_t> config = flavor == Flavor::Avc ? buildAvcConfig(sps, pps) : buildHevcConfig(vps, sps, pps);
            if (config.empty() || !info.valid) return;
            started = true;
            needKey = false;
            track.codec = flavor == Flavor::Avc ? fourcc("avc1") : fourcc("hvc1");
            track.configType = flavor == Flavor::Avc ? fourcc("avcC") : fourcc("hvcC");
            track.config = std::move(config);
            track.width = info.width;
            track.height = info.height;
            track.interlaced = info.interlaced;
            track.color.known = info.rangeKnown || info.colorKnown;
            track.color.primaries = info.primaries;
            track.color.transfer = info.transfer;
            track.color.matrix = info.matrix;
            track.color.full = info.full;
        }
        if (needKey && !u.key) return;
        needKey = false;
        Sample s;
        s.offset = u.place.file;
        s.size = uint32_t(u.size);
        s.sync = u.key;
        if (u.stamped) {
            s.pts = u.pts;
            s.dts = u.dts;
        } else if (!track.samples.empty()) {
            size_t n = track.samples.size();
            int64_t step = n > 1 ? track.samples[n - 1].dts - track.samples[n - 2].dts : 3000;
            s.dts = track.samples[n - 1].dts + std::max<int64_t>(step, 1);
            s.pts = s.dts;
        } else {
            return;
        }
        track.samples.push_back(s);
        where.push_back({uint32_t(raw), u.place.skip});
    }

    void keep(NalList& list) {
        if (started || list.size() >= 32 || head.size() >= 65536) return;
        if (std::find(list.begin(), list.end(), head) == list.end()) list.push_back(head);
    }

    void finishNal(uint64_t end) {
        if (end <= nalBody || head.empty()) return;
        uint64_t size = end - nalBody;
        if (head.size() > size) head.resize(size_t(size));
        bool picture, starter, key, first, dropped;
        bool field = false, bottom = false;
        uint32_t frameNumber = 0;
        if (flavor == Flavor::Avc) {
            int type = head[0] & 31;
            picture = type >= 1 && type <= 5;
            dropped = type == 9 || type == 12;
            starter = (type >= 6 && type <= 9) || (type >= 14 && type <= 18);
            key = type == 5;
            first = false;
            if (type == 7) {
                keep(sps);
                if (!info.valid) info = parseAvcSps(head.data(), head.size());
            } else if (type == 8) {
                keep(pps);
            } else if (picture) {
                std::vector<uint8_t> body = unescape(head.data() + 1, head.size() - 1);
                BitReader r{body.data(), body.size()};
                first = r.ue() == 0;
                r.ue();
                r.ue();
                if (info.separatePlanes) r.skip(2);
                frameNumber = r.bits(info.frameNumBits);
                if (info.valid && info.interlaced && r.bit()) {
                    field = true;
                    bottom = r.bit();
                }
                if (!r.ok) field = false;
            }
        } else {
            if (size < 2) return;
            int type = (head[0] >> 1) & 63;
            picture = type < 32;
            dropped = type == 35 || type == 38;
            starter = (type >= 32 && type <= 35) || type == 39 || (type >= 41 && type <= 44) || (type >= 48 && type <= 55);
            key = type >= 16 && type <= 21;
            first = picture && head.size() > 2 && (head[2] & 0x80);
            if (type == 32) keep(vps);
            if (type == 33) {
                keep(sps);
                if (!info.valid) info = parseHevcSps(head.data(), head.size());
            }
            if (type == 34) keep(pps);
        }
        uint64_t added = dropped ? 0 : 4 + size;
        if (picture) {
            bool boundary = current.picture && (first || lead.open);
            if (boundary && first && field && current.field && current.fields == 1 && current.bottom != bottom && current.frameNumber == frameNumber) {
                current.size += lead.size;
                current.fields = 2;
                lead = Unit();
                boundary = false;
            }
            if (boundary) {
                emit(current, lead.open ? lead.start : nalStart);
                if (lead.open) {
                    current = lead;
                    lead = Unit();
                }
            }
            if (!current.open) begin(current);
            if (!current.picture) {
                current.field = field;
                current.bottom = bottom;
                current.frameNumber = frameNumber;
                current.fields = 1;
            }
            current.size += added;
            current.picture = true;
            current.key = current.key || key;
        } else {
            Unit& target = (starter && current.picture) || lead.open ? lead : current;
            if (!target.open) begin(target);
            target.size += added;
        }
    }

    void video(const uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            uint8_t b = p[i];
            if (zeros >= 2 && b == 1) {
                if (inside) finishNal(position + i - zeros);
                inside = true;
                nalStart = position + i - 2;
                nalPlace = locate(nalStart);
                nalBody = position + i + 1;
                head.clear();
                limit = 0;
                zeros = 0;
                continue;
            }
            if (inside) {
                if (head.empty()) {
                    int type = flavor == Flavor::Avc ? b & 31 : (b >> 1) & 63;
                    bool whole = flavor == Flavor::Avc ? type == 7 || type == 8 : type >= 32 && type <= 34;
                    limit = whole ? 65536 : 32;
                }
                if (head.size() < limit) {
                    head.push_back(b);
                } else if (b != 0 && zeros == 0) {
                    const void* zero = memchr(p + i, 0, n - i);
                    if (!zero) return;
                    i = size_t(static_cast<const uint8_t*>(zero) - p);
                    zeros = 1;
                    continue;
                }
            }
            zeros = b == 0 ? zeros + 1 : 0;
        }
    }

    void finishFrame() {
        if (track.samples.empty()) {
            track.codec = frame.codec;
            track.sampleRate = frame.rate;
            track.channels = frame.channels;
            if (flavor == Flavor::Adts) {
                track.configType = fourcc("ASC ");
                track.config.assign(frame.config, frame.config + 2);
            }
        }
        if (frame.joins && !track.samples.empty() && frameEnd == frameStart && where.back().raw < (1u << 20)) {
            track.samples.back().size += frame.bytes;
            where.back().raw += frame.bytes;
            frameEnd = frameStart + frame.bytes;
            return;
        }
        Stamp stamp;
        int64_t pts;
        if (stampFor(frameStart, stamp)) {
            pts = stamp.pts;
            anchored = true;
            anchor = pts;
            sinceAnchor = 0;
        } else if (anchored) {
            pts = anchor + int64_t((sinceAnchor * 90000 + uint64_t(frame.rate) / 2) / uint64_t(frame.rate));
        } else {
            return;
        }
        sinceAnchor += frame.samples;
        Sample s;
        s.offset = framePlace.file;
        s.size = frame.bytes - frame.strip;
        s.pts = s.dts = pts;
        s.duration = uint32_t(uint64_t(frame.samples) * 90000 / uint64_t(frame.rate));
        s.sync = true;
        track.samples.push_back(s);
        where.push_back({frame.bytes, framePlace.skip});
        frameEnd = frameStart + frame.bytes;
    }

    void audio(const uint8_t* p, size_t n) {
        size_t need = flavor == Flavor::Adts ? 7 : flavor == Flavor::MpegAudio ? 4 : 8;
        size_t i = 0;
        while (i < n) {
            if (remaining) {
                size_t take = size_t(std::min<uint64_t>(remaining, n - i));
                remaining -= take;
                i += take;
                if (!remaining) finishFrame();
                continue;
            }
            if (have == 0) frameStart = position + i;
            header[have++] = p[i++];
            if (have < need) continue;
            AudioFrame parsed;
            bool good = flavor == Flavor::Adts ? adtsHeader(header, parsed) : flavor == Flavor::MpegAudio ? mpegAudioHeader(header, parsed) : dolbyHeader(header, parsed);
            if (good && parsed.bytes >= need && parsed.rate > 0) {
                frame = parsed;
                framePlace = locate(frameStart);
                remaining = frame.bytes - need;
                have = 0;
                if (framePlace.file == kNowhere) remaining = 0;
                else if (!remaining) finishFrame();
            } else {
                memmove(header, header + 1, need - 1);
                have = need - 1;
                ++frameStart;
            }
        }
    }

    void feed(const uint8_t* p, size_t n, uint64_t file, uint8_t skip, bool fresh) {
        if (flavor == Flavor::Guess) {
            if (!fresh || n < 2) return;
            if (p[0] == 0x0B && p[1] == 0x77) flavor = Flavor::Dolby;
            else if (p[0] == 0xFF && (p[1] & 0xF6) == 0xF0) flavor = Flavor::Adts;
            else if (p[0] == 0xFF && (p[1] & 0xE0) == 0xE0) flavor = Flavor::MpegAudio;
            else return;
        }
        recent[recentAt] = {position, uint32_t(n), file, skip};
        recentAt = (recentAt + 1) % recent.size();
        if (video()) video(p, n);
        else audio(p, n);
        position += n;
    }

    void finish() {
        if (!video()) return;
        uint64_t end = position - uint64_t(zeros);
        if (inside) finishNal(end);
        emit(current, lead.open ? lead.start : end);
    }
};

struct Section {
    std::vector<uint8_t> data;

    bool feed(const Payload& part) {
        if (part.starts) {
            size_t pointer = part.p[0];
            if (1 + pointer >= part.n) return false;
            data.assign(part.p + 1 + pointer, part.p + part.n);
        } else {
            if (data.empty() || data.size() > 2048) return false;
            data.insert(data.end(), part.p, part.p + part.n);
        }
        if (data.size() < 3) return false;
        size_t need = 3 + (size_t(data[1] & 0x0F) << 8 | data[2]);
        if (need > 1024 || need < 12) {
            data.clear();
            return false;
        }
        if (data.size() < need) return false;
        data.resize(need);
        return sectionCrc(data.data(), need) == 0;
    }
};

class TsReader : public Reader {
public:
    ~TsReader() override { if (file) fclose(file); }
    const char* format() const override { return "mpegts"; }
    const std::vector<Track>& tracks() const override { return list; }

    bool read(const Track& track, uint32_t index, uint8_t* out) override {
        size_t slot = 0;
        while (slot < list.size() && list[slot].id != track.id) ++slot;
        if (slot == list.size() || index >= list[slot].samples.size() || index >= places[slot].size()) return false;
        const Sample& s = list[slot].samples[index];
        const Where& w = places[slot][index];
        scratch.resize(w.raw);
        if (!collect(s.offset, w.skip, uint16_t(track.id), scratch.data(), w.raw)) return false;
        Flavor flavor = flavors[slot];
        if (flavor != Flavor::Avc && flavor != Flavor::Hevc) {
            size_t strip = 0;
            if (flavor == Flavor::Adts) strip = w.raw >= 2 && (scratch[1] & 1) ? 7 : 9;
            if (w.raw < strip || w.raw - strip != s.size) return false;
            memcpy(out, scratch.data() + strip, s.size);
            return true;
        }
        size_t total = 0;
        bool fits = true;
        annexNals(scratch.data(), scratch.size(), [&](const uint8_t* nal, size_t size) {
            int type = flavor == Flavor::Avc ? nal[0] & 31 : (nal[0] >> 1) & 63;
            bool dropped = flavor == Flavor::Avc ? type == 9 || type == 12 : type == 35 || type == 38;
            if (dropped || !fits || (flavor == Flavor::Hevc && size < 2)) return;
            if (size + 4 > s.size - total) {
                fits = false;
                return;
            }
            out[total] = uint8_t(size >> 24);
            out[total + 1] = uint8_t(size >> 16);
            out[total + 2] = uint8_t(size >> 8);
            out[total + 3] = uint8_t(size);
            memcpy(out + total + 4, nal, size);
            total += size + 4;
        });
        return fits && total == s.size;
    }

    bool open(const std::string& path, std::string* error) {
        auto fail = [&](const char* text) {
            if (error) *error = text;
            return false;
        };
        file = fopen(path.c_str(), "rb");
        if (!file) return fail("cannot open file");
        uint8_t head[1024];
        size_t got = fread(head, 1, sizeof head, file);
        static const Geometry shapes[3] = {{188, 0}, {192, 4}, {204, 0}};
        bool known = false;
        for (const Geometry& candidate : shapes) {
            size_t matches = 0;
            for (size_t at = candidate.lead; at < got && head[at] == 0x47; at += candidate.size) ++matches;
            if (matches >= 2 && (matches >= 5 || candidate.lead + matches * candidate.size >= got)) {
                shape = candidate;
                known = true;
                break;
            }
        }
        if (!known) return fail("not an MPEG transport stream");
        index();
        if (list.empty()) return fail("no supported streams in the transport stream");
        reading.file = file;
        reading.shape = shape;
        return true;
    }

private:
    FILE* file = nullptr;
    Geometry shape;
    std::vector<Track> list;
    std::vector<std::vector<Where>> places;
    std::vector<Flavor> flavors;
    std::vector<uint8_t> scratch;
    Walker reading;

    bool collect(uint64_t offset, uint8_t skip, uint16_t pid, uint8_t* out, size_t raw) {
        reading.start(offset, shape.size * 256);
        Pes pes;
        pes.state = Pes::State::Payload;
        int counter = -1;
        size_t got = 0;
        bool first = true;
        const uint8_t* packet;
        uint64_t place;
        while (got < raw) {
            if (!reading.next(packet, place)) return false;
            Payload part;
            if (!payloadOf(packet, part)) {
                if (part.hollow && part.pid == pid) counter = part.counter;
                continue;
            }
            if (part.pid != pid) continue;
            if (part.scrambled) return false;
            if (counter >= 0 && part.counter == counter) continue;
            counter = part.counter;
            std::pair<const uint8_t*, size_t> data;
            if (first) {
                if (place != offset || skip >= part.n) return false;
                data = {part.p + skip, part.n - skip};
                first = false;
            } else {
                data = pes.feed(part.p, part.n, part.starts);
            }
            size_t take = std::min(data.second, raw - got);
            memcpy(out + got, data.first, take);
            got += take;
        }
        return true;
    }

    void index() {
        Walker walk;
        walk.file = file;
        walk.shape = shape;
        walk.start(0, shape.size * 4096);
        std::vector<uint16_t> tables;
        std::vector<Section> sections;
        Section programs;
        std::vector<std::unique_ptr<Stream>> streams;
        bool clocked = false;
        bool early = false;
        int64_t clock = 0;
        const uint8_t* packet;
        uint64_t place;
        while (walk.next(packet, place)) {
            Payload part;
            if (!payloadOf(packet, part)) {
                if (part.hollow) for (auto& s : streams) if (s->pid == part.pid && part.counter == ((s->counter + 1) & 15)) s->counter = part.counter;
                continue;
            }
            if (part.pid == 0x1FFF) continue;
            if (part.pid == 0) {
                if (!programs.feed(part) || programs.data[0] != 0) continue;
                const std::vector<uint8_t>& d = programs.data;
                for (size_t at = 8; at + 4 <= d.size() - 4; at += 4) {
                    uint16_t program = uint16_t(d[at] << 8 | d[at + 1]);
                    uint16_t pid = uint16_t((d[at + 2] & 0x1F) << 8 | d[at + 3]);
                    if (program && pid && tables.size() < 64 && std::find(tables.begin(), tables.end(), pid) == tables.end()) {
                        tables.push_back(pid);
                        sections.emplace_back();
                    }
                }
                continue;
            }
            auto table = std::find(tables.begin(), tables.end(), part.pid);
            if (table != tables.end()) {
                Section& section = sections[size_t(table - tables.begin())];
                if (section.feed(part) && section.data[0] == 2) describe(section.data, streams);
                continue;
            }
            Stream* stream = nullptr;
            for (auto& candidate : streams) if (candidate->pid == part.pid) stream = candidate.get();
            if (!stream) continue;
            Stream& s = *stream;
            if (part.scrambled) {
                s.lose();
                s.counter = part.counter;
                continue;
            }
            if (s.counter >= 0) {
                if (part.counter == s.counter) continue;
                if (part.counter != ((s.counter + 1) & 15) && !part.jump) s.lose();
            }
            s.counter = part.counter;
            auto data = s.pes.feed(part.p, part.n, part.starts);
            if (s.pes.fresh && s.pes.timed) {
                int64_t near = s.clocked ? s.clock : clocked ? clock : s.pes.dts;
                int64_t dts = unwrap(s.pes.dts, near);
                int64_t pts = unwrap(s.pes.pts, dts);
                if (!clocked) early = dts > kClockMask - 60 * 90000;
                s.clocked = clocked = true;
                s.clock = clock = dts;
                if (s.stamps.size() >= 64) s.stamps.pop_front();
                s.stamps.push_back({s.position, pts, dts});
            }
            if (data.second) s.feed(data.first, data.second, place, uint8_t(data.first - part.p), s.pes.fresh);
        }
        for (auto& stream : streams) {
            stream->finish();
            settle(*stream, early ? kClockMask + 1 : 0);
        }
    }

    void describe(const std::vector<uint8_t>& d, std::vector<std::unique_ptr<Stream>>& streams) {
        size_t end = d.size() - 4;
        if (end < 12) return;
        size_t at = 12 + (size_t(d[10] & 0x0F) << 8 | d[11]);
        while (at + 5 <= end) {
            uint8_t type = d[at];
            uint16_t pid = uint16_t((d[at + 1] & 0x1F) << 8 | d[at + 2]);
            size_t extra = size_t(d[at + 3] & 0x0F) << 8 | d[at + 4];
            at += 5;
            if (extra > end - at) return;
            std::string language;
            bool dolby = false, hevc = false;
            for (size_t k = at; k + 2 <= at + extra;) {
                uint8_t tag = d[k];
                size_t length = d[k + 1];
                k += 2;
                if (length > at + extra - k) break;
                if (tag == 0x0A && length >= 3) language.assign(d.begin() + long(k), d.begin() + long(k) + 3);
                if (tag == 0x6A || tag == 0x7A) dolby = true;
                if (tag == 0x05 && length >= 4) {
                    std::string name(d.begin() + long(k), d.begin() + long(k) + 4);
                    if (name == "AC-3" || name == "EAC3") dolby = true;
                    if (name == "HEVC") hevc = true;
                }
                k += length;
            }
            at += extra;
            bool known = true;
            Flavor flavor = Flavor::Avc;
            if (type == 0x1B) flavor = Flavor::Avc;
            else if (type == 0x24 || (type == 0x06 && hevc)) flavor = Flavor::Hevc;
            else if (type == 0x0F) flavor = Flavor::Adts;
            else if (type == 0x03 || type == 0x04) flavor = Flavor::MpegAudio;
            else if (type == 0x81 || type == 0x87 || type == 0x84 || (type == 0x06 && dolby)) flavor = Flavor::Dolby;
            else if (type == 0x06) flavor = Flavor::Guess;
            else known = false;
            bool present = false;
            for (auto& s : streams) if (s->pid == pid) present = true;
            if (!known || present || streams.size() >= 64) continue;
            auto stream = std::make_unique<Stream>();
            stream->pid = pid;
            stream->flavor = flavor;
            stream->track.id = pid;
            stream->track.kind = stream->video() ? Kind::Video : Kind::Audio;
            stream->track.timescale = 90000;
            for (char& c : language) c = c >= 'a' && c <= 'z' ? c : c >= 'A' && c <= 'Z' ? char(c + 32) : '?';
            if (language.find('?') == std::string::npos) stream->track.language = language;
            streams.push_back(std::move(stream));
        }
    }

    void settle(Stream& s, int64_t turned) {
        Track& track = s.track;
        std::vector<Sample>& samples = track.samples;
        if (samples.empty() || samples.size() > 0xFFFFFFF0u) return;
        if (s.video()) {
            int64_t usual = 3000;
            if (s.info.ticks && s.info.timeScale) {
                int64_t exact = int64_t(uint64_t(s.info.ticks) * 90000 * (s.flavor == Flavor::Avc ? 2 : 1) / s.info.timeScale);
                if (exact > 0 && exact < 900000) usual = exact;
            }
            for (size_t i = 0; i < samples.size(); ++i) {
                int64_t step = i + 1 < samples.size() ? samples[i + 1].dts - samples[i].dts : 0;
                if (step <= 0 || step > 900000) step = usual;
                else usual = step;
                samples[i].duration = uint32_t(step);
            }
        }
        int64_t base = samples[0].dts;
        for (Sample& sample : samples) {
            sample.dts -= base;
            sample.pts -= base;
        }
        track.mediaStart = samples[0].pts;
        track.delay = track.mediaStart + base - turned;
        for (uint32_t i = 0; i < samples.size(); ++i) {
            if (samples[i].pts >= track.mediaStart) track.order.push_back(i);
            if (samples[i].sync) track.syncs.push_back(i);
        }
        std::stable_sort(track.order.begin(), track.order.end(), [&](uint32_t a, uint32_t b) { return samples[a].pts < samples[b].pts; });
        list.push_back(std::move(track));
        places.push_back(std::move(s.where));
        flavors.push_back(s.flavor);
    }
};

}

std::unique_ptr<Reader> openTransportStream(const std::string& path, std::string* error) {
    auto reader = std::make_unique<TsReader>();
    if (!reader->open(path, error)) return nullptr;
    return reader;
}

}

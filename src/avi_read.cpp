#include "nal.h"
#include "read_common.h"
#include "readers.h"
#include <algorithm>
#include <cstring>
#include <numeric>

namespace absent {

void riffInfo(Source& source, uint64_t at, uint64_t size, Tags& tags);

std::vector<uint8_t> mpeg4Descriptor(const uint8_t* headers, size_t size) {
    size = std::min<size_t>(size, 1 << 16);
    Bytes b;
    auto open = [&](uint32_t tag, size_t length) {
        b.u8(tag);
        b.u8(0x80 | ((length >> 21) & 0x7F));
        b.u8(0x80 | ((length >> 14) & 0x7F));
        b.u8(0x80 | ((length >> 7) & 0x7F));
        b.u8(length & 0x7F);
    };
    b.u32(0);
    open(0x03, 3 + 5 + 13 + 5 + size + 6);
    b.u16(0);
    b.u8(0);
    open(0x04, 13 + 5 + size);
    b.u8(0x20);
    b.u8(0x11);
    b.u24(0);
    b.u32(0);
    b.u32(0);
    open(0x05, size);
    b.raw(headers, size);
    open(0x06, 1);
    b.u8(0x02);
    return b.d;
}

uint32_t videoFourcc(uint32_t handler, int height) {
    char name[5] = {char(handler >> 24), char(handler >> 16), char(handler >> 8), char(handler), 0};
    for (char& c : name) if (c >= 'a' && c <= 'z') c = char(c - 32);
    auto any = [&](std::initializer_list<const char*> list) {
        for (const char* item : list) if (memcmp(name, item, 4) == 0) return true;
        return false;
    };
    if (any({"XVID", "DIVX", "DX50", "FMP4", "MP4V", "M4S2", "MP4S", "3IV2", "RMP4", "SEDG", "FVFW", "DIV5", "DIV6"})) return fourcc("mp4v");
    if (any({"H264", "X264", "AVC1", "VSSH", "DAVC"})) return fourcc("avc1");
    if (any({"MJPG", "JPEG", "IJPG", "AVRN", "DMB1"})) return fourcc("jpeg");
    if (any({"DVSD", "DV25", "DVSL", "DVC ", "DVCP", "DVIS", "PDVC"})) return height == 480 ? fourcc("dvc ") : fourcc("dvcp");
    if (any({"DV50", "DV5N", "DV5P"})) return height == 480 ? fourcc("dv5n") : fourcc("dv5p");
    if (any({"MPG1", "MPEG", "MP1V"}) || handler == 0x01000010) return fourcc("mp1v");
    if (any({"MPG2", "MP2V", "MMES", "M2V1"}) || handler == 0x02000010) return fourcc("mp2v");
    if (any({"H263", "S263"})) return fourcc("h263");
    return 0;
}

namespace {

const uint32_t kPacketFrames = 4096;
const uint64_t kLargestChunk = 256u << 20;
const size_t kMostChunks = 1u << 26;
const size_t kHead = 256;

struct Chunk {
    uint64_t at = 0;
    uint32_t size = 0;
};

struct Stream {
    Track track;
    bool video = false;
    bool audio = false;
    uint32_t handler = 0;
    uint32_t scale = 0;
    uint32_t rate = 0;
    uint32_t start = 0;
    uint32_t tag = 0;
    uint64_t indexAt = 0;
    uint64_t indexSize = 0;
    std::vector<uint8_t> extra;
    std::vector<Chunk> chunks;
    std::vector<uint64_t> ends;
    std::vector<uint32_t> raw;
};

struct Shown {
    uint64_t at = 0;
    uint32_t size = 0;
    uint32_t raw = 0;
    int64_t count = -1;
    int type = 0;
    bool empty = false;
};

struct OrderFields {
    bool usable = false;
    bool separate = false;
    bool framesOnly = true;
    int numberBits = 4;
    int countBits = 4;
};

OrderFields orderFields(const std::vector<uint8_t>& set) {
    OrderFields f;
    std::vector<uint8_t> plain = unescape(set.data(), set.size());
    if (plain.size() < 5) return f;
    BitReader r{plain.data() + 1, plain.size() - 1};
    int profile = int(r.bits(8));
    r.skip(16);
    r.ue();
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 || profile == 86 || profile == 118 || profile == 128) {
        uint32_t chroma = r.ue();
        if (chroma == 3) f.separate = r.bit();
        r.ue();
        r.ue();
        r.skip(1);
        if (r.bit()) {
            for (int i = 0; i < (chroma == 3 ? 12 : 8); ++i) {
                if (!r.bit()) continue;
                int last = 8, next = 8;
                for (int j = 0; j < (i < 6 ? 16 : 64) && r.ok; ++j) {
                    if (next) next = (last + r.se() + 256) % 256;
                    last = next ? next : last;
                }
            }
        }
    }
    f.numberBits = int(r.ue()) + 4;
    uint32_t kind = r.ue();
    if (kind == 0) f.countBits = int(r.ue()) + 4;
    else if (kind == 1) return f;
    r.ue();
    r.skip(1);
    r.ue();
    r.ue();
    f.framesOnly = r.bit();
    f.usable = r.ok && kind == 0 && f.numberBits <= 16 && f.countBits <= 16;
    return f;
}

bool packedMark(const uint8_t* p, size_t n) {
    static const char mark[] = "DivX";
    const uint8_t* end = p + n;
    const uint8_t* last = std::search(p, end, mark, mark + 4);
    if (last == end) return false;
    while (last < end && *last >= 0x20 && *last < 0x7F) ++last;
    return last[-1] == 'p';
}

int vopType(const uint8_t* p, size_t n, size_t from, size_t* found) {
    for (size_t i = from; i + 4 < n; ++i) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1 && p[i + 3] == 0xB6) {
            *found = i;
            return p[i + 4] >> 6;
        }
    }
    return -1;
}

int pictureType(const uint8_t* p, size_t n) {
    for (size_t i = 0; i + 5 < n; ++i) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1 && p[i + 3] == 0) {
            int type = (p[i + 5] >> 3) & 7;
            return type == 1 ? 0 : type == 3 ? 2 : 1;
        }
    }
    return -1;
}

class AviReader : public FileReader {
public:
    std::vector<Stream> streams;
    std::vector<std::pair<uint64_t, uint64_t>> movies;
    uint64_t oldIndexAt = 0;
    uint64_t oldIndexSize = 0;
    std::vector<uint8_t> scratch;

    const Stream* streamOf(const Track& track) const {
        for (const Stream& s : streams) if (s.track.id == track.id) return &s;
        return nullptr;
    }

    bool fetch(const Stream& s, uint64_t from, uint8_t* out, size_t count) {
        size_t slot = size_t(std::upper_bound(s.ends.begin(), s.ends.end(), from) - s.ends.begin());
        while (count) {
            if (slot >= s.chunks.size()) return false;
            const uint64_t begins = slot ? s.ends[slot - 1] : 0;
            const uint64_t inside = from - begins;
            const size_t take = size_t(std::min<uint64_t>(count, s.chunks[slot].size - inside));
            const uint8_t* p = source.view(s.chunks[slot].at + inside, take);
            if (!p) return false;
            memcpy(out, p, take);
            out += take;
            from += take;
            count -= take;
            ++slot;
        }
        return true;
    }

    bool read(const Track& track, uint32_t index, uint8_t* out) override {
        const Stream* s = streamOf(track);
        if (!s || index >= track.samples.size()) return false;
        const Sample& sample = track.samples[index];
        if (s->audio) return fetch(*s, sample.offset, out, sample.size);
        if (s->raw.empty()) return source.read(sample.offset, out, sample.size);
        if (index >= s->raw.size() || !source.load(sample.offset, s->raw[index], scratch, kLargestChunk)) return false;
        size_t total = 0;
        bool fits = true;
        annexNals(scratch.data(), scratch.size(), [&](const uint8_t* nal, size_t size) {
            int type = nal[0] & 31;
            if (type == 9 || type == 12 || !fits) return;
            if (size + 4 > sample.size - total) {
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
        return fits && total == sample.size;
    }

    void streamHeader(uint64_t at, uint64_t end) {
        Stream s;
        uint32_t kind = 0;
        while (end - at >= 8) {
            const uint8_t* h = source.view(at, 8);
            if (!h) break;
            uint32_t id = be32(h);
            uint64_t size = le32(h + 4);
            uint64_t body = at + 8;
            if (size > end - body) size = end - body;
            if (id == fourcc("strh") && size >= 36) {
                const uint8_t* p = source.view(body, 36);
                if (!p) break;
                kind = be32(p);
                s.handler = be32(p + 4);
                s.scale = le32(p + 20);
                s.rate = le32(p + 24);
                s.start = le32(p + 28);
            } else if (id == fourcc("strf") && kind == fourcc("vids") && size >= 40) {
                std::vector<uint8_t> format;
                if (!source.load(body, size, format, 1 << 20)) break;
                s.video = true;
                s.track.kind = Kind::Video;
                s.track.width = int(le32(format.data() + 4));
                int32_t height = int32_t(le32(format.data() + 8));
                s.track.height = height < 0 ? -height : height;
                uint32_t compression = be32(format.data() + 16);
                if (compression) s.handler = compression;
                s.extra.assign(format.begin() + 40, format.end());
            } else if (id == fourcc("strf") && kind == fourcc("auds") && size >= 16) {
                std::vector<uint8_t> format;
                if (!source.load(body, size, format, 1 << 20)) break;
                s.audio = true;
                s.track.kind = Kind::Audio;
                s.tag = le16(format.data());
                s.track.channels = int(le16(format.data() + 2));
                s.track.sampleRate = int(le32(format.data() + 4));
                s.track.bits = int(le16(format.data() + 14));
                if (s.tag == 0xFFFE && format.size() >= 40) {
                    s.track.channelMask = le32(format.data() + 20);
                    s.tag = le16(format.data() + 24);
                }
            } else if (id == fourcc("indx")) {
                s.indexAt = body;
                s.indexSize = size;
            }
            at = body + size + (size & 1);
        }
        s.track.id = uint32_t(streams.size() + 1);
        streams.push_back(std::move(s));
    }

    void header(uint64_t at, uint64_t end) {
        while (end - at >= 12) {
            const uint8_t* h = source.view(at, 12);
            if (!h) break;
            uint64_t size = le32(h + 4);
            uint64_t body = at + 8;
            if (size > end - body) size = end - body;
            if (be32(h) == fourcc("LIST") && be32(h + 8) == fourcc("strl") && size >= 4 && streams.size() < 100) streamHeader(body + 4, body + size);
            at = body + size + (size & 1);
        }
    }

    void outline() {
        const uint64_t total = source.size();
        uint64_t at = 0;
        while (total - at >= 12) {
            const uint8_t* h = source.view(at, 12);
            if (!h || be32(h) != fourcc("RIFF")) break;
            uint64_t size = le32(h + 4);
            uint64_t end = size < 4 || size > total - at - 8 ? total : at + 8 + size;
            uint64_t inner = at + 12;
            while (end - inner >= 8) {
                const uint8_t* c = source.view(inner, std::min<uint64_t>(12, end - inner));
                if (!c) break;
                uint32_t id = be32(c);
                uint64_t length = le32(c + 4);
                uint64_t body = inner + 8;
                bool open = length > end - body;
                if (open) length = end - body;
                if (id == fourcc("LIST") && length >= 4) {
                    uint32_t kind = be32(c + 8);
                    if (kind == fourcc("hdrl") && streams.empty()) header(body + 4, body + length);
                    else if (kind == fourcc("movi")) movies.push_back({body, open || length < 8 ? end : body + length});
                    else if (kind == fourcc("INFO")) riffInfo(source, body + 4, length - 4, found);
                } else if (id == fourcc("idx1") && !oldIndexSize) {
                    oldIndexAt = body;
                    oldIndexSize = length;
                }
                inner = body + length + (length & 1);
            }
            at = end + (end & 1 && end < total ? 1 : 0);
        }
    }

    Stream* numbered(const uint8_t* id) {
        auto digit = [](uint8_t c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
        int high = digit(id[0]), low = digit(id[1]);
        if (high < 0 || low < 0) return nullptr;
        size_t n = size_t(high * 16 + low);
        if (n >= streams.size()) return nullptr;
        Stream& s = streams[n];
        bool sound = id[2] == 'w' && id[3] == 'b';
        bool picture = id[2] == 'd' && (id[3] == 'c' || id[3] == 'b');
        return (s.audio && sound) || (s.video && picture) ? &s : nullptr;
    }

    void keep(Stream& s, uint64_t at, uint64_t size) {
        if (size > kLargestChunk || at > source.size() || size > source.size() - at || s.chunks.size() >= kMostChunks) return;
        s.chunks.push_back({at, uint32_t(size)});
    }

    void standardIndex(Stream& s, uint64_t at, uint64_t size) {
        if (size < 24) return;
        const uint8_t* h = source.view(at, 24);
        if (!h || h[3] != 1) return;
        uint64_t stride = uint64_t(le16(h)) * 4;
        uint64_t count = le32(h + 4);
        uint64_t base = le64(h + 12);
        if (stride < 8 || count > (size - 24) / stride) return;
        for (uint64_t i = 0; i < count; ++i) {
            const uint8_t* e = source.view(at + 24 + i * stride, 8);
            if (!e) return;
            keep(s, base + le32(e), le32(e + 4) & 0x7FFFFFFF);
        }
    }

    bool openIndexes() {
        bool any = false;
        for (Stream& s : streams) {
            if (s.indexSize < 24) continue;
            const uint8_t* h = source.view(s.indexAt, 24);
            if (!h) continue;
            if (h[3] == 1) {
                standardIndex(s, s.indexAt, s.indexSize);
            } else if (h[3] == 0 && le16(h) == 4) {
                uint64_t count = std::min<uint64_t>(le32(h + 4), (s.indexSize - 24) / 16);
                for (uint64_t i = 0; i < count; ++i) {
                    const uint8_t* e = source.view(s.indexAt + 24 + i * 16, 16);
                    if (!e) break;
                    uint64_t at = le64(e);
                    uint64_t size = le32(e + 8);
                    if (size < 8 || at > source.size() || size > source.size() - at) continue;
                    standardIndex(s, at + 8, size - 8);
                }
            }
            any = any || !s.chunks.empty();
        }
        return any;
    }

    bool oldIndex() {
        if (oldIndexSize < 16 || movies.empty()) return false;
        const uint64_t count = oldIndexSize / 16;
        uint64_t base = 0;
        bool based = false;
        for (uint64_t i = 0; i < count; ++i) {
            const uint8_t* e = source.view(oldIndexAt + i * 16, 16);
            if (!e) break;
            Stream* s = numbered(e);
            if (!s) continue;
            uint64_t offset = le32(e + 8);
            if (!based) {
                uint8_t id[4];
                memcpy(id, e, 4);
                const uint64_t relative = movies[0].first + offset;
                const uint8_t* there = source.view(relative, 4);
                base = there && memcmp(there, id, 4) == 0 ? movies[0].first : 0;
                const uint8_t* direct = source.view(base + offset, 4);
                if (!direct || memcmp(direct, id, 4) != 0) return false;
                based = true;
                e = source.view(oldIndexAt + i * 16, 16);
                if (!e) break;
            }
            keep(*s, base + offset + 8, le32(e + 12));
        }
        return based;
    }

    void walk() {
        for (auto& movie : movies) {
            uint64_t at = movie.first + 4;
            const uint64_t end = movie.second;
            while (at < end && end - at >= 8) {
                const uint8_t* h = source.view(at, 8);
                if (!h) return;
                if (be32(h) == fourcc("LIST")) {
                    at += 12;
                    continue;
                }
                uint64_t size = le32(h + 4);
                if (size > end - at - 8) size = end - at - 8;
                uint8_t id[4];
                memcpy(id, h, 4);
                if (Stream* s = numbered(id)) keep(*s, at + 8, size);
                at += 8 + size + (size & 1);
            }
        }
    }

    void sound(Stream& s) {
        Track& t = s.track;
        s.ends.reserve(s.chunks.size());
        uint64_t total = 0;
        for (const Chunk& c : s.chunks) s.ends.push_back(total += c.size);
        const bool pcm = s.tag == 1 || s.tag == 3;
        if (pcm) {
            const uint64_t frameBytes = uint64_t(t.channels) * uint64_t((t.bits + 7) / 8);
            const bool sized = s.tag == 3 ? t.bits == 32 || t.bits == 64 : t.bits == 8 || t.bits == 16 || t.bits == 24 || t.bits == 32;
            if (!sized || t.channels < 1 || t.channels > 64 || t.sampleRate < 1) return;
            t.codec = fourcc("lpcm");
            t.floating = s.tag == 3;
            t.timescale = uint32_t(t.sampleRate);
            const uint64_t frames = total / frameBytes;
            for (uint64_t done = 0; done < frames; done += kPacketFrames) {
                Sample sample;
                uint64_t count = std::min<uint64_t>(kPacketFrames, frames - done);
                sample.offset = done * frameBytes;
                sample.size = uint32_t(count * frameBytes);
                sample.pts = sample.dts = int64_t(done);
                sample.duration = uint32_t(count);
                sample.sync = true;
                t.samples.push_back(sample);
            }
            return;
        }
        const bool mpeg = s.tag == 0x55 || s.tag == 0x50;
        if (!mpeg && s.tag != 0x2000) return;
        t.bits = 0;
        uint64_t at = 0;
        int64_t time = 0;
        uint64_t lost = 0;
        uint8_t head[8];
        const size_t need = mpeg ? 4 : 8;
        while (total - at >= need && lost < (1u << 20)) {
            if (!fetch(s, at, head, need)) break;
            uint32_t size = 0, samples = 0, codec = 0;
            int rate = 0, channels = 0;
            MpegAudio m;
            DolbyFrame d;
            if (mpeg && mpegAudio(head, m)) {
                size = uint32_t(m.size);
                samples = uint32_t(m.samples);
                rate = m.sampleRate;
                channels = m.channels;
                codec = m.layer == 3 ? fourcc(".mp3") : m.layer == 2 ? fourcc(".mp2") : fourcc(".mp1");
            } else if (!mpeg && dolbyFrame(head, d) && !d.dependent) {
                size = d.bytes;
                samples = d.samples;
                rate = d.rate;
                channels = d.channels;
                codec = d.codec;
            }
            if (size < need || size > total - at || rate < 1 || (!t.samples.empty() && (codec != t.codec || rate != t.sampleRate))) {
                ++at;
                ++lost;
                continue;
            }
            lost = 0;
            if (t.samples.empty()) {
                t.codec = codec;
                t.sampleRate = rate;
                t.channels = channels;
                t.timescale = uint32_t(rate);
            }
            Sample sample;
            sample.offset = at;
            sample.size = size;
            sample.pts = sample.dts = time;
            sample.duration = samples;
            sample.sync = true;
            time += samples;
            t.samples.push_back(sample);
            at += size;
        }
    }

    void picture(Stream& s) {
        Track& t = s.track;
        t.codec = videoFourcc(s.handler, t.height);
        if (!t.codec) t.codec = s.handler;
        if (!s.scale || !s.rate || t.width <= 0 || t.height <= 0 || t.width > 32768 || t.height > 32768) return;
        const uint32_t shared = std::gcd(s.scale, s.rate);
        const uint32_t step = s.scale / shared;
        t.timescale = s.rate / shared;
        const bool part2 = t.codec == fourcc("mp4v");
        const bool mpeg = t.codec == fourcc("mp1v") || t.codec == fourcc("mp2v");
        const bool avc = t.codec == fourcc("avc1");
        std::vector<Shown> frames;
        frames.reserve(s.chunks.size());
        NalList sps, pps;
        OrderFields fields;
        int64_t count = -1;
        auto parameterSets = [&](const uint8_t* p, size_t n, size_t& packed, bool& key) {
            count = -1;
            annexNals(p, n, [&](const uint8_t* nal, size_t size) {
                int type = nal[0] & 31;
                if (type == 9 || type == 12) return;
                packed += size + 4;
                if (type == 5) key = true;
                if (type == 7 && sps.empty()) fields = orderFields(std::vector<uint8_t>(nal, nal + size));
                if ((type == 1 || type == 5) && count < 0 && fields.usable) {
                    std::vector<uint8_t> plain = unescape(nal, std::min<size_t>(size, 48));
                    BitReader r{plain.data() + 1, plain.size() - 1};
                    r.ue();
                    r.ue();
                    r.ue();
                    if (fields.separate) r.skip(2);
                    r.skip(size_t(fields.numberBits));
                    if (!fields.framesOnly && r.bit()) r.skip(1);
                    if (type == 5) r.ue();
                    uint32_t low = r.bits(fields.countBits);
                    if (r.ok) count = low;
                }
                NalList* list = type == 7 ? &sps : type == 8 ? &pps : nullptr;
                if (!list || list->size() >= 32 || size > 65535) return;
                std::vector<uint8_t> one(nal, nal + size);
                if (std::find(list->begin(), list->end(), one) == list->end()) list->push_back(std::move(one));
            });
        };
        if (avc) {
            size_t packed = 0;
            bool key = false;
            parameterSets(s.extra.data(), s.extra.size(), packed, key);
        }
        std::vector<uint8_t> headers;
        bool packedStream = part2 && packedMark(s.extra.data(), s.extra.size());
        bool checked = false;
        int placeholders = 0;
        std::vector<uint8_t> bytes;
        for (const Chunk& c : s.chunks) {
            Shown f;
            f.at = c.at;
            f.size = f.raw = c.size;
            f.empty = c.size == 0;
            if (f.empty && placeholders > 0) {
                --placeholders;
                continue;
            }
            if (avc && !f.empty) {
                if (!source.load(c.at, c.size, bytes, kLargestChunk)) continue;
                size_t packed = 0;
                bool key = false;
                parameterSets(bytes.data(), bytes.size(), packed, key);
                f.size = uint32_t(std::min<size_t>(packed, 0x7FFFFFFF));
                f.type = key ? 0 : 1;
                f.count = count;
                f.empty = packed == 0;
            } else if ((part2 || mpeg) && !f.empty) {
                const size_t want = packedStream ? c.size : std::min<size_t>(c.size, kHead);
                const uint8_t* p = source.view(c.at, want);
                if (!p) continue;
                if (mpeg) {
                    f.type = pictureType(p, want);
                } else {
                    size_t first = 0;
                    f.type = vopType(p, want, 0, &first);
                    if (headers.empty() && f.type >= 0 && first > 0) headers.assign(p, p + first);
                    if (!checked && !packedStream) {
                        packedStream = packedMark(p, want);
                        if (packedStream && want < c.size) {
                            p = source.view(c.at, c.size);
                            if (!p) continue;
                        }
                    }
                    checked = true;
                    if (packedStream && f.type >= 0) {
                        if (placeholders > 0 && c.size - first <= 8) {
                            --placeholders;
                            continue;
                        }
                        size_t second = 0;
                        int next = vopType(p, c.size, first + 4, &second);
                        if (next >= 0) {
                            Shown head = f;
                            head.size = uint32_t(second);
                            frames.push_back(head);
                            f.at = c.at + second;
                            f.size = uint32_t(c.size - second);
                            f.type = next;
                            ++placeholders;
                        }
                    }
                }
                if (f.type < 0) f.type = 1;
            }
            frames.push_back(f);
        }
        if (avc) {
            t.configType = fourcc("avcC");
            t.config = buildAvcConfig(sps, pps);
            if (t.config.empty()) return;
        } else if (part2) {
            size_t first = 0;
            if (vopType(s.extra.data(), s.extra.size(), 0, &first) >= 0) s.extra.resize(first);
            bool described = false;
            for (size_t i = 0; i + 3 < s.extra.size(); ++i) described = described || (s.extra[i] == 0 && s.extra[i + 1] == 0 && s.extra[i + 2] == 1 && (s.extra[i + 3] & 0xF0) == 0x20);
            const std::vector<uint8_t>& chosen = described ? s.extra : headers;
            t.configType = fourcc("esds");
            t.config = mpeg4Descriptor(chosen.data(), chosen.size());
        }
        const bool typed = avc || part2 || mpeg;
        bool reordered = false;
        if (part2 || mpeg) for (const Shown& f : frames) reordered = reordered || (!f.empty && f.type == 2);
        std::vector<int64_t> slots(frames.size(), 0);
        int64_t slot = 0;
        size_t held = frames.size();
        for (size_t i = 0; i < frames.size(); ++i) {
            if (reordered && !frames[i].empty && frames[i].type != 2) {
                if (held < frames.size()) slots[held] = slot++;
                held = i;
            } else {
                slots[i] = slot++;
            }
        }
        if (held < frames.size()) slots[held] = slot++;
        if (avc && fields.usable) {
            const int64_t span = int64_t(1) << fields.countBits;
            int64_t high = 0, before = 0;
            std::vector<size_t> group;
            auto close = [&](size_t end) {
                const size_t begin = group.empty() ? end : group.front();
                std::stable_sort(group.begin(), group.end(), [&](size_t a, size_t b) { return frames[a].count < frames[b].count; });
                for (size_t k = 0; k < group.size(); ++k) slots[group[k]] = int64_t(begin + k);
                group.clear();
            };
            for (size_t i = 0; i < frames.size(); ++i) {
                Shown& f = frames[i];
                if (f.empty || f.count < 0 || f.type == 0) {
                    close(i);
                    high = before = 0;
                    if (f.empty || f.count < 0) continue;
                }
                if (f.count < before && before - f.count >= span / 2) high += span;
                else if (f.count > before && f.count - before > span / 2) high -= span;
                before = f.count;
                f.count += high;
                group.push_back(i);
            }
            close(frames.size());
        }
        int64_t late = 0;
        for (size_t i = 0; i < frames.size(); ++i) late = std::max(late, int64_t(i) - slots[i]);
        std::vector<uint32_t> order;
        for (size_t i = 0; i < frames.size(); ++i) {
            const Shown& f = frames[i];
            if (f.empty) continue;
            Sample sample;
            sample.offset = f.at;
            sample.size = f.size;
            sample.pts = slots[i] * step;
            sample.dts = (int64_t(i) - late) * step;
            sample.duration = step;
            sample.sync = !typed || f.type == 0;
            order.push_back(uint32_t(t.samples.size()));
            t.samples.push_back(sample);
            if (avc) s.raw.push_back(f.raw);
        }
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return t.samples[a].pts < t.samples[b].pts; });
        for (size_t i = 0; i + 1 < order.size(); ++i) {
            int64_t gap = t.samples[order[i + 1]].pts - t.samples[order[i]].pts;
            t.samples[order[i]].duration = uint32_t(std::clamp<int64_t>(gap, 1, int64_t(step) * 100000));
        }
        if (!t.samples.empty()) t.mediaStart = t.samples[order[0]].pts;
        t.delay = int64_t(s.start) * step;
    }
};

}

std::unique_ptr<Reader> openAvi(const std::string& path, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    auto reader = std::make_unique<AviReader>();
    if (!reader->source.open(path)) return fail("cannot open file");
    reader->name = "avi";
    reader->outline();
    if (reader->streams.empty()) return fail("no stream descriptions in this AVI file");
    if (reader->movies.empty()) return fail("no picture or sound data in this AVI file");
    if (!reader->openIndexes() && !reader->oldIndex()) {
        for (Stream& s : reader->streams) s.chunks.clear();
        reader->walk();
    }
    for (Stream& s : reader->streams) {
        if (s.video) reader->picture(s);
        else if (s.audio) reader->sound(s);
        if (s.track.samples.empty() || !s.track.timescale) continue;
        if (s.audio && s.rate) s.track.delay = int64_t(uint64_t(s.start) * s.scale * s.track.timescale / s.rate);
        indexTrack(s.track);
        reader->list.push_back(s.track);
    }
    if (reader->list.empty()) return fail("no supported picture or sound streams in this AVI file");
    return reader;
}


}

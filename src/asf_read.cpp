#include "nal.h"
#include "read_common.h"
#include "readers.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <ctime>
#include <numeric>

namespace absent {

namespace {

using Guid = std::array<uint8_t, 16>;

constexpr Guid guid(uint32_t a, uint32_t b, uint32_t c, uint64_t d) {
    return {uint8_t(a), uint8_t(a >> 8), uint8_t(a >> 16), uint8_t(a >> 24), uint8_t(b), uint8_t(b >> 8), uint8_t(c), uint8_t(c >> 8),
            uint8_t(d >> 56), uint8_t(d >> 48), uint8_t(d >> 40), uint8_t(d >> 32), uint8_t(d >> 24), uint8_t(d >> 16), uint8_t(d >> 8), uint8_t(d)};
}

constexpr Guid kHeader = guid(0x75B22630, 0x668E, 0x11CF, 0xA6D900AA0062CE6Cull);
constexpr Guid kData = guid(0x75B22636, 0x668E, 0x11CF, 0xA6D900AA0062CE6Cull);
constexpr Guid kSimpleIndex = guid(0x33000890, 0xE5B1, 0x11CF, 0x89F400A0C90349CBull);
constexpr Guid kIndex = guid(0xD6E229D3, 0x35DA, 0x11D1, 0x903400A0C90349BEull);
constexpr Guid kFileProperties = guid(0x8CABDCA1, 0xA947, 0x11CF, 0x8EE400C00C205365ull);
constexpr Guid kStreamProperties = guid(0xB7DC0791, 0xA9B7, 0x11CF, 0x8EE600C00C205365ull);
constexpr Guid kHeaderExtension = guid(0x5FBF03B5, 0xA92E, 0x11CF, 0x8EE300C00C205365ull);
constexpr Guid kContentDescription = guid(0x75B22633, 0x668E, 0x11CF, 0xA6D900AA0062CE6Cull);
constexpr Guid kExtendedContentDescription = guid(0xD2D0A440, 0xE307, 0x11D2, 0x97F000A0C95EA850ull);
constexpr Guid kContentEncryption = guid(0x2211B3FB, 0xBD23, 0x11D2, 0xB4B700A0C955FC6Eull);
constexpr Guid kExtendedContentEncryption = guid(0x298AE614, 0x2622, 0x4C17, 0xB935DAE07EE9289Cull);
constexpr Guid kAdvancedContentEncryption = guid(0x43058533, 0x6981, 0x49E6, 0x9B74AD12CB86D58Cull);
constexpr Guid kExtendedStreamProperties = guid(0x14E6A5CB, 0xC672, 0x4332, 0x8399A96952065B5Aull);
constexpr Guid kMetadata = guid(0xC5F8CBEA, 0x5BAF, 0x4877, 0x8467AA8C44FA4CCAull);
constexpr Guid kMetadataLibrary = guid(0x44231C94, 0x9498, 0x49D1, 0xA1411D134E457054ull);
constexpr Guid kLanguageList = guid(0x7C4346A9, 0xEFE0, 0x4BFC, 0xB229393EDE415C85ull);
constexpr Guid kAudio = guid(0xF8699E40, 0x5B4D, 0x11CF, 0xA8FD00805F5C442Bull);
constexpr Guid kVideo = guid(0xBC19EFC0, 0x5B4D, 0x11CF, 0xA8FD00805F5C442Bull);
constexpr Guid kAudioSpread = guid(0xBFC3CD50, 0x618F, 0x11CF, 0x8BB200AA00B4E220ull);

const uint64_t kLargestHeader = 256u << 20;
const uint32_t kLargestObject = 256u << 20;
const uint32_t kLargestPacket = 1u << 20;
const size_t kMostPieces = 1u << 23;
const uint32_t kMostChunks = 1u << 16;
const uint32_t kPacketFrames = 4096;
const int64_t kFiletimeEpoch = 116444736000000000;

struct Cursor {
    const uint8_t* p = nullptr;
    size_t n = 0;
    size_t at = 0;
    bool ok = true;

    bool need(size_t k) {
        if (!ok || n - at < k) ok = false;
        return ok;
    }
    uint32_t u8() { return need(1) ? p[at++] : 0; }
    uint32_t u16() {
        if (!need(2)) return 0;
        at += 2;
        return le16(p + at - 2);
    }
    uint32_t u32() {
        if (!need(4)) return 0;
        at += 4;
        return le32(p + at - 4);
    }
    uint64_t u64() {
        if (!need(8)) return 0;
        at += 8;
        return le64(p + at - 8);
    }
    uint32_t sized(uint32_t type) { return type == 1 ? u8() : type == 2 ? u16() : type == 3 ? u32() : 0; }
    bool is(const Guid& g) { return need(16) && memcmp(p + at, g.data(), 16) == 0; }
    void skip(uint64_t k) {
        if (k > n - at) ok = false;
        else if (ok) at += size_t(k);
    }
    Cursor sub(uint64_t k) {
        if (!ok || k > n - at) {
            ok = false;
            return {nullptr, 0, 0, false};
        }
        Cursor inner{p + at, size_t(k)};
        at += size_t(k);
        return inner;
    }
    size_t left() const { return ok ? n - at : 0; }
};

struct Piece {
    uint64_t at = 0;
    uint32_t size = 0;
};

struct Building {
    bool open = false;
    uint32_t number = 0;
    uint32_t size = 0;
    uint32_t filled = 0;
    int64_t time = 0;
    bool key = false;
    uint32_t packet = 0;
    std::vector<Piece> pieces;
};

struct Stream {
    Track track;
    bool audio = false;
    uint16_t tag = 0;
    uint32_t handler = 0;
    std::vector<uint8_t> format;
    uint32_t span = 1;
    uint32_t virtualPacket = 0;
    uint32_t virtualChunk = 0;
    int64_t offset = 0;
    bool keyed = false;
    std::vector<Piece> pieces;
    std::vector<uint64_t> ends;
    uint64_t total = 0;
    std::vector<uint32_t> packets;
    std::vector<uint32_t> raw;
    Building building;
};

struct Extended {
    int language = -1;
    uint64_t frameTime = 0;
};

uint32_t wmaCodec(uint16_t tag) {
    switch (tag) {
        case 0x0160: return fourcc("wma1");
        case 0x0161: return fourcc("wma2");
        case 0x0162: return fourcc("wmap");
        case 0x0163: return fourcc("wmal");
        case 0x000A: return fourcc("wmas");
        default: return 0;
    }
}

uint32_t wmvCodec(uint32_t handler) {
    char name[5] = {char(handler >> 24), char(handler >> 16), char(handler >> 8), char(handler), 0};
    for (char& c : name) if (c >= 'a' && c <= 'z') c = char(c - 32);
    for (const char* known : {"WMV1", "WMV2", "WMV3", "WVC1", "WMVA", "MPG4", "MP42", "MP43"}) if (memcmp(name, known, 4) == 0) return be32(reinterpret_cast<const uint8_t*>(name));
    if (memcmp(name, "DIV3", 4) == 0 || memcmp(name, "DIV4", 4) == 0) return fourcc("MP43");
    return 0;
}

std::string hexTag(uint16_t tag) {
    static const char digits[] = "0123456789abcdef";
    return {digits[tag >> 12], digits[(tag >> 8) & 15], digits[(tag >> 4) & 15], digits[tag & 15]};
}

std::string filetime(uint64_t value) {
    if (value == 0 || value > uint64_t(INT64_MAX)) return {};
    int64_t ticks = int64_t(value) - kFiletimeEpoch;
    int64_t seconds = ticks >= 0 ? ticks / 10000000 : -((-ticks + 9999999) / 10000000);
    int64_t micro = (ticks - seconds * 10000000) / 10;
    time_t whole = time_t(seconds);
    struct tm parts;
    if (!gmtime_r(&whole, &parts)) return {};
    char text[64];
    snprintf(text, sizeof text, "%04d-%02d-%02dT%02d:%02d:%02d.%06lldZ", parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday, parts.tm_hour,
             parts.tm_min, parts.tm_sec, (long long)micro);
    return text;
}

std::string tagName(const std::string& name) {
    static const char* const names[][2] = {{"Title", "title"}, {"Author", "artist"}, {"Description", "comment"}, {"Copyright", "copyright"},
        {"WM/AlbumTitle", "album"}, {"WM/AlbumArtist", "album_artist"}, {"WM/Year", "date"}, {"WM/Genre", "genre"}, {"WM/TrackNumber", "track"},
        {"WM/Composer", "composer"}, {"WM/Publisher", "publisher"}, {"WM/EncodingSettings", "encoder"}, {"WM/Language", "language"}};
    for (const auto& row : names) if (name == row[0]) return row[1];
    return name;
}

bool sliceOrder(const uint8_t* nal, size_t size, const VideoInfo& sps, int64_t& lsb) {
    if (sps.orderType != 0 || sps.orderBits < 4 || sps.orderBits > 16) return false;
    std::vector<uint8_t> plain = unescape(nal, std::min<size_t>(size, 64));
    if (plain.size() < 2) return false;
    BitReader r{plain.data() + 1, plain.size() - 1};
    r.ue();
    r.ue();
    r.ue();
    if (sps.separatePlanes) r.skip(2);
    r.skip(size_t(sps.frameNumBits));
    if (sps.interlaced && r.bit()) r.skip(1);
    if ((nal[0] & 31) == 5) r.ue();
    lsb = r.bits(sps.orderBits);
    return r.ok;
}

class AsfReader : public FileReader {
public:
    std::vector<Stream> streams;
    int slot[128];
    std::vector<Extended> extended = std::vector<Extended>(128);
    std::vector<std::string> languages;
    std::vector<std::vector<std::pair<uint32_t, int64_t>>> simpleIndexes;
    std::vector<std::pair<uint32_t, std::vector<std::pair<uint32_t, int64_t>>>> indexed;
    uint32_t packetSize = 0;
    uint32_t largestPacket = 0;
    int64_t preroll = 0;
    uint64_t playTime = 0;
    bool broadcast = false;
    bool protectedContent = false;
    uint64_t firstPacket = 0;
    uint64_t dataEnd = 0;
    bool cut = false;
    std::vector<uint8_t> scratch;

    AsfReader() { std::fill(std::begin(slot), std::end(slot), -1); }

    double statedSeconds() const override {
        if (broadcast || !playTime || cut) return -1;
        double seconds = double(playTime) / 1e7 - double(preroll) / 1000.0;
        return seconds > 0 ? seconds : -1;
    }

    const Stream* streamOf(const Track& track) const {
        if (track.id >= 128 || slot[track.id] < 0) return nullptr;
        return &streams[size_t(slot[track.id])];
    }

    bool fetch(const Stream& s, uint64_t from, uint8_t* out, size_t count) {
        size_t piece = size_t(std::upper_bound(s.ends.begin(), s.ends.end(), from) - s.ends.begin());
        while (count) {
            if (piece >= s.pieces.size()) return false;
            const uint64_t begins = piece ? s.ends[piece - 1] : 0;
            const uint64_t inside = from - begins;
            const size_t take = size_t(std::min<uint64_t>(count, s.pieces[piece].size - inside));
            if (!source.read(s.pieces[piece].at + inside, out, take)) return false;
            out += take;
            from += take;
            count -= take;
            ++piece;
        }
        return true;
    }

    bool read(const Track& track, uint32_t index, uint8_t* out) override {
        const Stream* s = streamOf(track);
        if (!s || index >= track.samples.size()) return false;
        const Sample& sample = track.samples[index];
        if (s->raw.empty()) return fetch(*s, sample.offset, out, sample.size);
        if (index >= s->raw.size()) return false;
        scratch.resize(s->raw[index]);
        if (!fetch(*s, sample.offset, scratch.data(), scratch.size())) return false;
        size_t total = 0;
        bool fits = true;
        annexNals(scratch.data(), scratch.size(), [&](const uint8_t* nal, size_t size) {
            const int type = nal[0] & 31;
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

    void streamProperties(Cursor c) {
        Cursor type = c.sub(16);
        Cursor correction = c.sub(16);
        const uint64_t offset = c.u64();
        const uint32_t typeLength = c.u32();
        const uint32_t correctionLength = c.u32();
        const uint32_t flags = c.u16();
        c.u32();
        Cursor specific = c.sub(typeLength);
        Cursor spread = c.sub(correctionLength);
        if (!c.ok) return;
        const uint32_t number = flags & 0x7F;
        if (flags & 0x8000) protectedContent = true;
        if (number == 0 || slot[number] >= 0 || streams.size() >= 127) return;
        Stream s;
        s.track.id = number;
        s.offset = int64_t(std::min<uint64_t>(offset, 1ull << 50) / 10000);
        if (type.is(kAudio)) {
            if (specific.left() < 16) return;
            s.audio = true;
            s.track.kind = Kind::Audio;
            s.format.assign(specific.p, specific.p + specific.n);
            s.tag = uint16_t(specific.u16());
            s.track.channels = int(specific.u16());
            s.track.sampleRate = int(specific.u32());
            specific.u32();
            specific.u16();
            s.track.bits = int(specific.u16());
            if (s.tag == 0xFFFE && specific.n >= 40) {
                s.track.channelMask = le32(specific.p + 20);
                s.tag = uint16_t(le16(specific.p + 24));
            }
            if (correction.is(kAudioSpread) && spread.n >= 5) {
                s.span = spread.u8();
                s.virtualPacket = spread.u16();
                s.virtualChunk = spread.u16();
            }
        } else if (type.is(kVideo)) {
            const uint32_t width = specific.u32();
            const uint32_t height = specific.u32();
            specific.u8();
            const uint32_t length = specific.u16();
            Cursor bitmap = specific.sub(std::min<uint64_t>(length, specific.left()));
            if (!specific.ok || bitmap.n < 40) return;
            s.track.kind = Kind::Video;
            s.format.assign(bitmap.p, bitmap.p + bitmap.n);
            const int32_t bitmapWidth = int32_t(le32(bitmap.p + 4));
            const int32_t bitmapHeight = int32_t(le32(bitmap.p + 8));
            s.track.width = bitmapWidth > 0 ? bitmapWidth : int(std::min<uint32_t>(width, 1u << 20));
            s.track.height = bitmapHeight != 0 && bitmapHeight > INT32_MIN ? std::abs(bitmapHeight) : int(std::min<uint32_t>(height, 1u << 20));
            s.handler = be32(bitmap.p + 16);
        } else {
            return;
        }
        slot[number] = int(streams.size());
        streams.push_back(std::move(s));
    }

    void extendedStream(Cursor c) {
        c.skip(8 + 8 + 4 * 7 + 4);
        const uint32_t number = c.u16() & 0x7F;
        const uint32_t language = c.u16();
        const uint64_t frameTime = c.u64();
        const uint32_t names = c.u16();
        const uint32_t systems = c.u16();
        for (uint32_t i = 0; i < names && c.ok; ++i) {
            c.u16();
            c.skip(c.u16());
        }
        for (uint32_t i = 0; i < systems && c.ok; ++i) {
            c.skip(16 + 2);
            c.skip(c.u32());
        }
        if (!c.ok) return;
        extended[number].language = int(language);
        extended[number].frameTime = frameTime;
        if (c.left() >= 24 && c.is(kStreamProperties)) {
            const uint64_t size = le64(c.p + c.at + 16);
            if (size >= 24 && size <= c.left()) {
                c.skip(24);
                streamProperties(c.sub(size - 24));
            }
        }
    }

    void languageList(Cursor c) {
        const uint32_t count = c.u16();
        for (uint32_t i = 0; i < count && c.ok; ++i) {
            Cursor text = c.sub(c.u8());
            if (c.ok) languages.push_back(utf16Text(text.p, text.n));
        }
    }

    void contentDescription(Cursor c) {
        uint32_t lengths[5];
        for (uint32_t& length : lengths) length = c.u16();
        static const char* const keys[5] = {"title", "artist", "copyright", "comment", "rating"};
        for (int i = 0; i < 5 && c.ok; ++i) {
            Cursor text = c.sub(lengths[i]);
            std::string value = c.ok ? utf16Text(text.p, text.n) : std::string();
            if (!value.empty()) found[keys[i]] = value;
        }
    }

    void keepTag(const Cursor& name, uint32_t type, const Cursor& value) {
        std::string text;
        if (type == 0) text = utf16Text(value.p, value.n);
        else if ((type == 2 || type == 3) && value.n >= 4) text = std::to_string(le32(value.p));
        else if ((type == 2 || type == 5) && value.n >= 2) text = std::to_string(le16(value.p));
        else if (type == 4 && value.n >= 8) text = std::to_string(le64(value.p));
        const std::string key = tagName(utf16Text(name.p, name.n));
        if (!key.empty() && !text.empty() && !found.count(key)) found[key] = text;
    }

    void extendedContent(Cursor c) {
        const uint32_t count = c.u16();
        for (uint32_t i = 0; i < count && c.ok; ++i) {
            Cursor name = c.sub(c.u16());
            const uint32_t type = c.u16();
            Cursor value = c.sub(c.u16());
            if (c.ok) keepTag(name, type, value);
        }
    }

    void metadata(Cursor c) {
        const uint32_t count = c.u16();
        for (uint32_t i = 0; i < count && c.ok; ++i) {
            c.skip(4);
            const uint32_t nameLength = c.u16();
            const uint32_t type = c.u16();
            const uint32_t valueLength = c.u32();
            Cursor name = c.sub(nameLength);
            Cursor value = c.sub(valueLength);
            if (c.ok) keepTag(name, type, value);
        }
    }

    void headerExtension(Cursor c) {
        c.skip(16 + 2);
        Cursor inner = c.sub(std::min<uint64_t>(c.u32(), c.left()));
        while (inner.left() >= 24) {
            Cursor id = inner.sub(16);
            const uint64_t size = inner.u64();
            if (size < 24 || size - 24 > inner.left()) break;
            Cursor body = inner.sub(size - 24);
            if (id.is(kExtendedStreamProperties)) extendedStream(body);
            else if (id.is(kLanguageList)) languageList(body);
            else if (id.is(kMetadata) || id.is(kMetadataLibrary)) metadata(body);
            else if (id.is(kAdvancedContentEncryption)) protectedContent = true;
        }
    }

    bool header(uint64_t at, uint64_t size) {
        std::vector<uint8_t> bytes;
        if (size < 30 || !source.load(at, size, bytes, kLargestHeader)) return false;
        Cursor c{bytes.data(), bytes.size()};
        c.skip(24 + 6);
        bool described = false;
        while (c.left() >= 24) {
            Cursor id = c.sub(16);
            const uint64_t length = c.u64();
            if (length < 24 || length - 24 > c.left()) break;
            Cursor body = c.sub(length - 24);
            if (id.is(kFileProperties) && body.n >= 80) {
                body.skip(16 + 8);
                tagTime(body.u64());
                body.u64();
                playTime = body.u64();
                body.u64();
                preroll = int64_t(std::min<uint64_t>(body.u64(), 1ull << 40));
                broadcast = body.u32() & 1;
                const uint32_t least = body.u32();
                const uint32_t most = body.u32();
                packetSize = least == most ? least : 0;
                largestPacket = most;
                described = true;
            } else if (id.is(kStreamProperties)) {
                streamProperties(body);
            } else if (id.is(kHeaderExtension)) {
                headerExtension(body);
            } else if (id.is(kContentDescription)) {
                contentDescription(body);
            } else if (id.is(kExtendedContentDescription)) {
                extendedContent(body);
            } else if (id.is(kContentEncryption) || id.is(kExtendedContentEncryption)) {
                protectedContent = true;
            }
        }
        return described;
    }

    void tagTime(uint64_t value) {
        std::string text = filetime(value);
        if (!text.empty()) found["creation_time"] = text;
    }

    void appendRange(Stream& s, const Building& b, const std::vector<uint32_t>& starts, uint32_t from, uint32_t count) {
        size_t piece = size_t(std::upper_bound(starts.begin(), starts.end(), from) - starts.begin()) - 1;
        for (; count && piece < b.pieces.size(); ++piece) {
            const uint32_t inside = from - starts[piece];
            const uint32_t take = std::min(count, b.pieces[piece].size - inside);
            const Piece part{b.pieces[piece].at + inside, take};
            if (!s.pieces.empty() && s.pieces.back().at + s.pieces.back().size == part.at && s.pieces.back().size <= UINT32_MAX - take) s.pieces.back().size += take;
            else s.pieces.push_back(part);
            from += take;
            count -= take;
        }
    }

    void complete(Stream& s) {
        Building& b = s.building;
        b.open = false;
        const uint32_t group = s.span * s.virtualPacket;
        const bool spread = s.audio && s.span > 1 && s.virtualChunk > 0 && s.virtualPacket % s.virtualChunk == 0 && group > 0 && b.size / s.virtualChunk <= kMostChunks;
        if (s.pieces.size() + b.pieces.size() + (spread ? b.size / s.virtualChunk + 1 : 0) > kMostPieces) return;
        std::vector<uint32_t> starts(b.pieces.size());
        for (size_t i = 1; i < starts.size(); ++i) starts[i] = starts[i - 1] + b.pieces[i - 1].size;
        uint32_t done = 0;
        if (spread) {
            const uint32_t across = s.virtualPacket / s.virtualChunk;
            for (; b.size - done >= group; done += group) {
                for (uint32_t out = 0; out < s.span * across; ++out) {
                    const uint32_t stored = (out % across) * s.span + out / across;
                    appendRange(s, b, starts, done + stored * s.virtualChunk, s.virtualChunk);
                }
            }
        }
        appendRange(s, b, starts, done, b.size - done);
        Sample sample;
        sample.offset = s.total;
        sample.size = b.size;
        sample.pts = sample.dts = b.time - preroll + s.offset;
        sample.sync = b.key;
        s.keyed = s.keyed || b.key;
        s.total += b.size;
        s.track.samples.push_back(sample);
        s.packets.push_back(b.packet);
    }

    void deliver(uint32_t number, uint32_t object, uint32_t offset, uint32_t size, int64_t time, bool key, uint64_t at, uint32_t length, uint32_t packet) {
        if (number >= 128 || slot[number] < 0) return;
        Stream& s = streams[size_t(slot[number])];
        Building& b = s.building;
        if (offset == 0) {
            if (!size) size = length;
            b.open = size <= kLargestObject && size > 0;
            b.number = object;
            b.size = size;
            b.filled = 0;
            b.time = time;
            b.key = key;
            b.packet = packet;
            b.pieces.clear();
        } else if (!b.open || b.number != object || b.filled != offset) {
            b.open = false;
            return;
        }
        if (!b.open || length > b.size - b.filled || b.pieces.size() >= 1u << 20) {
            b.open = false;
            return;
        }
        if (length) b.pieces.push_back({at, length});
        b.filled += length;
        if (b.filled == b.size) complete(s);
    }

    uint64_t packet(uint64_t at, uint32_t number) {
        const size_t want = size_t(std::min<uint64_t>(packetSize ? packetSize : std::max(largestPacket, 1u), std::min<uint64_t>(dataEnd - at, kLargestPacket)));
        const uint8_t* p = source.view(at, want);
        if (!p || want < 2) return 0;
        Cursor c{p, want};
        uint32_t flags = c.u8();
        if (flags & 0x80) {
            if (flags & 0x60) return packetSize;
            c.skip(flags & 0x0F);
            flags = c.u8();
        }
        const uint32_t property = c.u8();
        const uint32_t stated = c.sized((flags >> 5) & 3);
        c.sized((flags >> 1) & 3);
        const uint32_t padding = c.sized((flags >> 3) & 3);
        const int64_t send = c.u32();
        c.u16();
        if (!c.ok) return packetSize;
        size_t length = packetSize ? packetSize : stated;
        if (stated && stated < length) length = stated;
        if (!packetSize && !length) return 0;
        const uint64_t step = packetSize ? packetSize : length;
        if (padding > length || length - padding < c.at) return step;
        const size_t end = std::min(length - padding, want);
        c.n = end;
        const bool multiple = flags & 1;
        uint32_t count = 1, lengthType = 2;
        if (multiple) {
            const uint32_t payloadFlags = c.u8();
            count = payloadFlags & 0x3F;
            lengthType = payloadFlags >> 6;
        }
        for (uint32_t i = 0; i < count && c.ok; ++i) {
            const uint32_t stream = c.u8();
            const uint32_t object = c.sized((property >> 4) & 3);
            const uint32_t offset = c.sized((property >> 2) & 3);
            const uint32_t replicated = c.sized(property & 3);
            if (replicated == 1) {
                const int64_t delta = c.u8();
                const uint64_t size = multiple ? c.sized(lengthType) : c.left();
                Cursor parts = c.sub(size);
                for (uint32_t k = 0; parts.left() > 0 && parts.ok; ++k) {
                    const uint32_t part = parts.u8();
                    const size_t from = parts.at;
                    parts.skip(part);
                    if (parts.ok) deliver(stream & 0x7F, object + k, 0, part, int64_t(offset) + delta * k, stream & 0x80, at + size_t(parts.p - p) + from, part, number);
                }
                continue;
            }
            Cursor extra = c.sub(replicated);
            const uint32_t size = extra.n >= 8 ? le32(extra.p) : 0;
            const int64_t time = extra.n >= 8 ? int64_t(le32(extra.p + 4)) : send;
            const uint64_t data = multiple ? c.sized(lengthType) : c.left();
            if (!c.ok || data > c.left()) break;
            deliver(stream & 0x7F, object, offset, size, time, stream & 0x80, at + c.at, uint32_t(data), number);
            c.skip(data);
        }
        return step;
    }

    void index(uint64_t at, uint64_t size, bool simple) {
        std::vector<uint8_t> bytes;
        if (!source.load(at, size, bytes, 64u << 20)) return;
        Cursor c{bytes.data(), bytes.size()};
        c.skip(24);
        if (simple) {
            c.skip(16);
            const int64_t interval = int64_t(std::min<uint64_t>(c.u64() / 10000, 1u << 30));
            c.u32();
            const uint32_t count = c.u32();
            std::vector<std::pair<uint32_t, int64_t>> packets;
            for (uint32_t i = 0; i < count && c.left() >= 6; ++i) {
                packets.push_back({c.u32(), int64_t(i) * interval - preroll});
                c.u16();
            }
            simpleIndexes.push_back(std::move(packets));
            return;
        }
        const int64_t interval = c.u32();
        const uint32_t kinds = c.u16();
        const uint32_t blocks = c.u32();
        std::vector<uint32_t> numbers;
        for (uint32_t i = 0; i < kinds && c.ok; ++i) {
            numbers.push_back(c.u16() & 0x7F);
            c.u16();
        }
        if (!c.ok || kinds == 0 || !packetSize) return;
        std::vector<std::vector<std::pair<uint32_t, int64_t>>> lists(kinds);
        int64_t entry = 0;
        for (uint32_t block = 0; block < blocks && c.ok; ++block) {
            const uint32_t entries = c.u32();
            std::vector<uint64_t> bases;
            for (uint32_t k = 0; k < kinds; ++k) bases.push_back(c.u64());
            if (!c.ok || uint64_t(entries) * kinds * 4 > c.left()) break;
            for (uint32_t e = 0; e < entries; ++e, ++entry) {
                for (uint32_t k = 0; k < kinds; ++k) lists[k].push_back({uint32_t(std::min<uint64_t>((bases[k] + c.u32()) / packetSize, UINT32_MAX)), entry * interval - preroll});
            }
        }
        for (uint32_t k = 0; k < kinds; ++k) indexed.push_back({numbers[k], std::move(lists[k])});
    }

    void outline(std::string& problem) {
        const uint64_t total = source.size();
        uint64_t at = 0;
        bool headed = false, data = false;
        while (total - at >= 24) {
            const uint8_t* h = source.view(at, 24);
            if (!h) break;
            uint64_t size = le64(h + 16);
            const bool open = size < 24 || size > total - at;
            if (size > total - at && memcmp(h, kData.data(), 16) == 0) cut = true;
            if (open) size = total - at;
            if (memcmp(h, kHeader.data(), 16) == 0 && !headed) {
                headed = header(at, size);
                if (!headed) {
                    problem = "the header of this Windows Media file is damaged";
                    return;
                }
            } else if (memcmp(h, kData.data(), 16) == 0 && !data && headed) {
                data = true;
                firstPacket = at + 50;
                dataEnd = at + size;
            } else if (memcmp(h, kSimpleIndex.data(), 16) == 0) {
                index(at, size, true);
            } else if (memcmp(h, kIndex.data(), 16) == 0) {
                index(at, size, false);
            } else if (!headed) {
                break;
            }
            if (open) break;
            at += size;
        }
        if (!headed) problem = "this Windows Media file has no header";
        else if (!data || firstPacket > total) problem = "this Windows Media file has no data";
    }

    void scan() {
        dataEnd = std::min(dataEnd, source.size());
        uint64_t at = firstPacket;
        for (uint32_t number = 0; at < dataEnd && dataEnd - at >= 8; ++number) {
            const uint64_t step = packet(at, number);
            if (!step) break;
            at += step;
        }
    }

    void markFromIndex(Stream& s, size_t video) {
        std::vector<std::pair<uint32_t, int64_t>> keys;
        for (auto& [number, list] : indexed) if (number == s.track.id) keys.insert(keys.end(), list.begin(), list.end());
        if (keys.empty() && video < simpleIndexes.size()) keys = simpleIndexes[video];
        std::sort(keys.begin(), keys.end());
        for (size_t k = 0; k < keys.size(); ++k) {
            if (k && keys[k].first == keys[k - 1].first) continue;
            const auto from = std::lower_bound(s.packets.begin(), s.packets.end(), keys[k].first);
            const auto to = std::upper_bound(from, s.packets.end(), keys[k].first);
            if (from == to) continue;
            size_t chosen = size_t(from - s.packets.begin());
            for (size_t i = chosen; i < size_t(to - s.packets.begin()); ++i) {
                const int64_t pts = s.track.samples[i].pts;
                if (pts <= keys[k].second && pts >= s.track.samples[chosen].pts) chosen = i;
            }
            s.track.samples[chosen].sync = true;
        }
        if (!s.track.samples.empty()) s.track.samples[0].sync = true;
    }

    void avc(Stream& s) {
        Track& t = s.track;
        const std::vector<uint8_t> extra(s.format.begin() + 40, s.format.end());
        t.configType = fourcc("avcC");
        if (!extra.empty() && extra[0] == 1) {
            t.config = extra;
            return;
        }
        NalList sps, pps;
        VideoInfo info;
        auto gather = [&](const uint8_t* nal, size_t size) {
            const int type = nal[0] & 31;
            NalList* list = type == 7 ? &sps : type == 8 ? &pps : nullptr;
            if (!list || list->size() >= 32 || size > 65535) return;
            std::vector<uint8_t> one(nal, nal + size);
            if (type == 7 && sps.empty()) info = parseAvcSps(nal, size);
            if (std::find(list->begin(), list->end(), one) == list->end()) list->push_back(std::move(one));
        };
        annexNals(extra.data(), extra.size(), gather);
        std::vector<Sample> kept;
        std::vector<uint32_t> raw;
        std::vector<int64_t> order;
        std::vector<bool> idr;
        for (const Sample& sample : t.samples) {
            scratch.resize(sample.size);
            if (!fetch(s, sample.offset, scratch.data(), scratch.size())) continue;
            size_t packed = 0;
            bool fresh = false;
            int64_t lsb = -1;
            annexNals(scratch.data(), scratch.size(), [&](const uint8_t* nal, size_t size) {
                const int type = nal[0] & 31;
                if (type == 9 || type == 12) return;
                packed += size + 4;
                gather(nal, size);
                if (type == 5) fresh = true;
                int64_t value = 0;
                if ((type == 1 || type == 5) && lsb < 0 && sliceOrder(nal, size, info, value)) lsb = value;
            });
            if (!packed || packed > 0x7FFFFFFF) continue;
            Sample copy = sample;
            copy.size = uint32_t(packed);
            if (!s.keyed) copy.sync = fresh;
            kept.push_back(copy);
            raw.push_back(sample.size);
            order.push_back(lsb);
            idr.push_back(fresh);
        }
        t.samples = std::move(kept);
        s.raw = std::move(raw);
        t.config = buildAvcConfig(sps, pps);
        bool monotonic = true;
        for (size_t i = 1; i < t.samples.size(); ++i) monotonic = monotonic && t.samples[i].pts >= t.samples[i - 1].pts;
        if (!monotonic || info.orderType != 0 || info.orderBits < 4 || info.orderBits > 16) return;
        const int64_t span = int64_t(1) << info.orderBits;
        std::vector<size_t> rank(t.samples.size());
        size_t begin = 0;
        int64_t high = 0, before = 0;
        std::vector<std::pair<int64_t, size_t>> group;
        auto close = [&] {
            std::stable_sort(group.begin(), group.end());
            for (size_t k = 0; k < group.size(); ++k) rank[group[k].second] = begin + k;
            begin += group.size();
            group.clear();
        };
        for (size_t i = 0; i < t.samples.size(); ++i) {
            if (idr[i] || order[i] < 0) {
                close();
                high = before = 0;
            }
            int64_t value = std::max<int64_t>(order[i], 0);
            if (value < before && before - value >= span / 2) high += span;
            else if (value > before && value - before > span / 2) high -= span;
            before = value;
            group.push_back({value + high, i});
        }
        close();
        std::vector<int64_t> slots(t.samples.size());
        for (size_t i = 0; i < slots.size(); ++i) slots[i] = t.samples[i].pts;
        stamp(t, slots, rank);
    }

    static void stamp(Track& t, const std::vector<int64_t>& slots, const std::vector<size_t>& rank) {
        const size_t count = t.samples.size();
        size_t late = 0;
        for (size_t i = 0; i < count; ++i) if (i > rank[i]) late = std::max(late, i - rank[i]);
        const int64_t usual = count > 1 ? std::max<int64_t>(1, (slots.back() - slots.front()) / int64_t(count - 1)) : 1;
        for (size_t i = 0; i < count; ++i) {
            Sample& s = t.samples[i];
            s.pts = slots[rank[i]];
            s.dts = i >= late ? slots[i - late] : slots[0] - int64_t(late - i) * usual;
        }
    }

    void timeVideo(Stream& s, uint64_t frameTime) {
        Track& t = s.track;
        const size_t count = t.samples.size();
        if (!count) return;
        bool monotonic = true;
        for (size_t i = 1; i < count; ++i) monotonic = monotonic && t.samples[i].pts >= t.samples[i - 1].pts;
        if (!monotonic) {
            std::vector<uint32_t> byTime(count);
            std::iota(byTime.begin(), byTime.end(), 0);
            std::stable_sort(byTime.begin(), byTime.end(), [&](uint32_t a, uint32_t b) { return t.samples[a].pts < t.samples[b].pts; });
            std::vector<int64_t> slots(count);
            std::vector<size_t> rank(count);
            for (size_t k = 0; k < count; ++k) {
                slots[k] = t.samples[byTime[k]].pts;
                rank[byTime[k]] = k;
            }
            stamp(t, slots, rank);
        }
        std::vector<uint32_t> byTime(count);
        std::iota(byTime.begin(), byTime.end(), 0);
        std::stable_sort(byTime.begin(), byTime.end(), [&](uint32_t a, uint32_t b) { return t.samples[a].pts < t.samples[b].pts; });
        const int64_t stated = int64_t(std::min<uint64_t>(frameTime / 10000, 1u << 30));
        for (size_t k = 0; k < count; ++k) {
            Sample& sample = t.samples[byTime[k]];
            int64_t gap = k + 1 < count ? t.samples[byTime[k + 1]].pts - sample.pts : stated > 0 ? stated : k > 0 ? t.samples[byTime[k - 1]].duration : 1;
            sample.duration = uint32_t(std::clamp<int64_t>(gap, 0, 0x7FFFFFFF));
        }
    }

    void timeObjects(Stream& s) {
        Track& t = s.track;
        const uint32_t bytesPerSecond = le32(s.format.data() + 8);
        for (size_t i = 0; i < t.samples.size(); ++i) {
            Sample& sample = t.samples[i];
            sample.sync = true;
            int64_t gap = i + 1 < t.samples.size() ? t.samples[i + 1].pts - sample.pts : bytesPerSecond ? int64_t(uint64_t(sample.size) * 1000 / bytesPerSecond) : i ? t.samples[i - 1].duration : 0;
            sample.duration = uint32_t(std::clamp<int64_t>(gap, 0, 0x7FFFFFFF));
        }
    }

    bool recut(Stream& s) {
        Track& t = s.track;
        if (t.samples.empty() || t.sampleRate < 1) return false;
        const int64_t startMs = t.samples[0].pts;
        std::vector<Sample> frames;
        auto base = [&](int rate) { return int64_t(std::llround(double(startMs) * rate / 1000.0)); };
        if (s.tag == 1 || s.tag == 3) {
            const uint64_t frameBytes = uint64_t(t.channels) * uint64_t((t.bits + 7) / 8);
            const bool sized = s.tag == 3 ? t.bits == 32 || t.bits == 64 : t.bits == 8 || t.bits == 16 || t.bits == 24 || t.bits == 32;
            if (!sized || t.channels < 1 || t.channels > 64) return false;
            t.codec = fourcc("lpcm");
            t.floating = s.tag == 3;
            t.timescale = uint32_t(t.sampleRate);
            const int64_t first = base(t.sampleRate);
            const uint64_t count = s.total / frameBytes;
            for (uint64_t done = 0; done < count; done += kPacketFrames) {
                Sample sample;
                const uint64_t take = std::min<uint64_t>(kPacketFrames, count - done);
                sample.offset = done * frameBytes;
                sample.size = uint32_t(take * frameBytes);
                sample.pts = sample.dts = first + int64_t(done);
                sample.duration = uint32_t(take);
                sample.sync = true;
                frames.push_back(sample);
            }
        } else {
            const bool mpeg = s.tag == 0x55 || s.tag == 0x50;
            t.bits = 0;
            uint64_t at = 0;
            uint64_t lost = 0;
            int64_t time = 0;
            uint8_t head[8];
            const size_t need = mpeg ? 4 : 8;
            while (s.total - at >= need && lost < (1u << 20)) {
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
                if (size < need || size > s.total - at || rate < 1 || (!frames.empty() && (codec != t.codec || rate != t.sampleRate))) {
                    ++at;
                    ++lost;
                    continue;
                }
                lost = 0;
                if (frames.empty()) {
                    t.codec = codec;
                    t.sampleRate = rate;
                    t.channels = channels;
                    t.timescale = uint32_t(rate);
                    time = base(rate);
                }
                Sample sample;
                sample.offset = at;
                sample.size = size;
                sample.pts = sample.dts = time;
                sample.duration = samples;
                sample.sync = true;
                time += samples;
                frames.push_back(sample);
                at += size;
            }
        }
        t.samples = std::move(frames);
        return !t.samples.empty();
    }

    void finish(Stream& s, size_t video) {
        Track& t = s.track;
        s.ends.reserve(s.pieces.size());
        uint64_t total = 0;
        for (const Piece& piece : s.pieces) s.ends.push_back(total += piece.size);
        const Extended& more = extended[t.id];
        if (more.language >= 0 && size_t(more.language) < languages.size()) t.language = languages[size_t(more.language)];
        t.timescale = 1000;
        if (s.audio) {
            t.configType = fourcc("wfex");
            t.config = s.format;
            t.codec = wmaCodec(s.tag);
            const bool cut = s.tag == 1 || s.tag == 3 || s.tag == 0x55 || s.tag == 0x50 || s.tag == 0x2000;
            if (cut) {
                t.configType = 0;
                t.config.clear();
                if (!recut(s)) t.samples.clear();
            } else {
                if (!t.codec) {
                    const std::string text = hexTag(s.tag);
                    t.codec = be32(reinterpret_cast<const uint8_t*>(text.data()));
                }
                timeObjects(s);
            }
        } else {
            t.codec = wmvCodec(s.handler);
            if (!s.keyed) markFromIndex(s, video);
            if (t.codec) {
                t.configType = fourcc("bmih");
                t.config = s.format;
            } else {
                t.codec = videoFourcc(s.handler, t.height);
                if (t.codec == fourcc("avc1")) {
                    avc(s);
                } else if (t.codec == fourcc("mp4v")) {
                    std::vector<uint8_t> headers(s.format.begin() + 40, s.format.end());
                    bool described = false;
                    for (size_t i = 0; i + 3 < headers.size(); ++i) described = described || (headers[i] == 0 && headers[i + 1] == 0 && headers[i + 2] == 1 && (headers[i + 3] & 0xF0) == 0x20);
                    if (!described && !t.samples.empty() && t.samples[0].size >= 4) {
                        headers.resize(std::min<uint32_t>(t.samples[0].size, 4096));
                        if (fetch(s, t.samples[0].offset, headers.data(), headers.size())) {
                            for (size_t i = 0; i + 3 < headers.size(); ++i) {
                                if (headers[i] == 0 && headers[i + 1] == 0 && headers[i + 2] == 1 && headers[i + 3] == 0xB6) {
                                    headers.resize(i);
                                    break;
                                }
                            }
                        } else {
                            headers.clear();
                        }
                    }
                    t.configType = fourcc("esds");
                    t.config = mpeg4Descriptor(headers.data(), headers.size());
                } else if (t.codec != fourcc("h263") && t.codec != fourcc("jpeg")) {
                    t.codec = s.handler;
                    t.configType = fourcc("bmih");
                    t.config = s.format;
                }
            }
            timeVideo(s, more.frameTime);
            if ((t.codec == fourcc("avc1") && t.config.empty()) || t.width <= 0 || t.height <= 0) t.samples.clear();
        }
        int64_t first = t.samples.empty() ? 0 : INT64_MAX;
        for (const Sample& sample : t.samples) first = std::min(first, sample.pts);
        t.mediaStart = t.delay = first;
        indexTrack(t);
    }
};

}

const char* windowsMediaName(uint32_t codec) {
    static const std::pair<uint32_t, const char*> names[] = {{fourcc("wma1"), "Windows Media Audio 1"}, {fourcc("wma2"), "Windows Media Audio 2"},
        {fourcc("wmap"), "Windows Media Audio 9 Professional"}, {fourcc("wmal"), "Windows Media Audio 9 Lossless"}, {fourcc("wmas"), "Windows Media Audio 9 Voice"},
        {fourcc("WMV1"), "Windows Media Video 7"}, {fourcc("WMV2"), "Windows Media Video 8"}, {fourcc("WMV3"), "Windows Media Video 9"},
        {fourcc("WVC1"), "VC-1 (Windows Media Video 9 Advanced Profile)"}, {fourcc("WMVA"), "Windows Media Video 9 Advanced Profile"},
        {fourcc("MPG4"), "Microsoft MPEG-4 version 1"}, {fourcc("MP42"), "Microsoft MPEG-4 version 2"}, {fourcc("MP43"), "Microsoft MPEG-4 version 3"},
        {fourcc("DIV3"), "Microsoft MPEG-4 version 3 (DivX 3)"}, {fourcc("DIV4"), "Microsoft MPEG-4 version 3 (DivX 3)"}};
    for (const auto& [code, name] : names) if (code == codec) return name;
    return nullptr;
}

std::unique_ptr<Reader> openAsf(const std::string& path, std::string* error) {
    auto fail = [&](const std::string& text) {
        if (error) *error = text;
        return nullptr;
    };
    auto reader = std::make_unique<AsfReader>();
    if (!reader->source.open(path)) return fail("cannot open file");
    reader->name = "asf";
    std::string problem;
    reader->outline(problem);
    if (!problem.empty()) return fail(problem);
    if (reader->protectedContent) return fail("this Windows Media file is protected by DRM (digital rights management) and cannot be played");
    if (!reader->packetSize && reader->largestPacket == 0) return fail("this Windows Media file gives no packet size");
    reader->scan();
    size_t video = 0;
    for (Stream& s : reader->streams) {
        reader->finish(s, video);
        if (!s.audio) ++video;
        reader->list.push_back(std::move(s.track));
    }
    if (reader->list.empty()) return fail("no picture or sound streams in this Windows Media file");
    return reader;
}

}

#include "read_common.h"
#include "readers.h"
#include <algorithm>
#include <array>
#include <cstring>
#include "portable.h"
#if defined(__ANDROID__)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace absent {

namespace {

#if defined(__ANDROID__)
struct Window {
    int fd;
    off64_t at;
};

int windowRead(void* cookie, char* out, int count) {
    auto* w = static_cast<Window*>(cookie);
    const ssize_t got = pread64(w->fd, out, size_t(count), w->at);
    if (got > 0) w->at += got;
    return int(got);
}

fpos_t windowSeek(void* cookie, fpos_t offset, int whence) {
    auto* w = static_cast<Window*>(cookie);
    off64_t base = whence == SEEK_CUR ? w->at : 0;
    if (whence == SEEK_END) {
        struct stat status;
        if (fstat(w->fd, &status) != 0) return -1;
        base = status.st_size;
    }
    if (base + offset < 0) return -1;
    return w->at = base + offset;
}

int windowClose(void* cookie) {
    auto* w = static_cast<Window*>(cookie);
    close(w->fd);
    delete w;
    return 0;
}
#endif

const size_t kWindow = 16384;

void put(Tags& tags, const std::string& key, const std::string& value) {
    if (!value.empty() && !tags.count(key)) tags[key] = value;
}

void appendUtf8(std::string& out, uint32_t c) {
    if (c < 0x80) {
        out += char(c);
    } else if (c < 0x800) {
        out += char(0xC0 | (c >> 6));
        out += char(0x80 | (c & 63));
    } else if (c < 0x10000) {
        out += char(0xE0 | (c >> 12));
        out += char(0x80 | ((c >> 6) & 63));
        out += char(0x80 | (c & 63));
    } else {
        out += char(0xF0 | (c >> 18));
        out += char(0x80 | ((c >> 12) & 63));
        out += char(0x80 | ((c >> 6) & 63));
        out += char(0x80 | (c & 63));
    }
}

std::string latin(const uint8_t* p, size_t n) {
    std::string out;
    for (size_t i = 0; i < n && p[i]; ++i) appendUtf8(out, p[i]);
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string wide(const uint8_t* p, size_t n, bool big) {
    std::string out;
    if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) {
        big = false;
        p += 2;
        n -= 2;
    } else if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) {
        big = true;
        p += 2;
        n -= 2;
    }
    for (size_t i = 0; i + 1 < n; i += 2) {
        uint32_t c = big ? be16(p + i) : le16(p + i);
        if (!c) break;
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < n) {
            uint32_t low = big ? be16(p + i + 2) : le16(p + i + 2);
            if (low >= 0xDC00 && low < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            }
        }
        appendUtf8(out, c);
    }
    return out;
}

std::string id3Text(const uint8_t* p, size_t n) {
    if (n < 2) return {};
    uint8_t encoding = p[0];
    ++p;
    --n;
    if (encoding == 1 || encoding == 2) return wide(p, n, encoding == 2);
    if (encoding == 0) return latin(p, n);
    std::string out(reinterpret_cast<const char*>(p), strnlen(reinterpret_cast<const char*>(p), n));
    return out;
}

uint32_t syncsafe(const uint8_t* p) {
    return (uint32_t(p[0] & 0x7F) << 21) | (uint32_t(p[1] & 0x7F) << 14) | (uint32_t(p[2] & 0x7F) << 7) | (p[3] & 0x7F);
}

void unsynchronise(std::vector<uint8_t>& data) {
    size_t out = 0;
    for (size_t i = 0; i < data.size(); ++i) {
        data[out++] = data[i];
        if (data[i] == 0xFF && i + 1 < data.size() && data[i + 1] == 0) ++i;
    }
    data.resize(out);
}

const char* id3Key(const uint8_t* id, bool three) {
    static const char* const table[][3] = {{"TIT2", "TT2", "title"}, {"TPE1", "TP1", "artist"}, {"TALB", "TAL", "album"}, {"TDRC", "TYE", "date"},
        {"TYER", "TYE", "date"}, {"TCON", "TCO", "genre"}, {"TRCK", "TRK", "track"}};
    for (const auto& row : table) if (memcmp(id, three ? row[1] : row[0], three ? 3 : 4) == 0) return row[2];
    return nullptr;
}

uint8_t crc8(const uint8_t* p, size_t n) {
    static const std::array<uint8_t, 256> table = [] {
        std::array<uint8_t, 256> filled{};
        for (int i = 0; i < 256; ++i) {
            uint8_t c = uint8_t(i);
            for (int k = 0; k < 8; ++k) c = uint8_t((c << 1) ^ (c & 0x80 ? 0x07 : 0));
            filled[size_t(i)] = c;
        }
        return filled;
    }();
    uint8_t c = 0;
    for (size_t i = 0; i < n; ++i) c = table[c ^ p[i]];
    return c;
}

}

FILE* openRead(const std::string& path) {
#if defined(__ANDROID__)
    int fd = -1;
    char rest = 0;
    if (sscanf(path.c_str(), "/proc/self/fd/%d%c", &fd, &rest) == 1) {
        const int own = dup(fd);
        if (own < 0) return nullptr;
        auto* window = new Window{own, 0};
        if (FILE* file = funopen(window, windowRead, nullptr, windowSeek, windowClose)) return file;
        close(own);
        delete window;
        return nullptr;
    }
#endif
    return openFile(path, "rb");
}

Source::~Source() { if (file) fclose(file); }

bool Source::open(const std::string& path) {
    file = openRead(path);
    if (!file) return false;
    setvbuf(file, nullptr, _IONBF, 0);
    if (fseeko(file, 0, SEEK_END) != 0) return false;
    int64_t end = ftello(file);
    if (end < 0) return false;
    total = uint64_t(end);
    return true;
}

const uint8_t* Source::view(uint64_t at, size_t count) {
    static const uint8_t nothing = 0;
    if (at > total || count > total - at) return nullptr;
    if (!count) return &nothing;
    if (at >= heldAt && at - heldAt <= held.size() && count <= held.size() - size_t(at - heldAt)) return held.data() + size_t(at - heldAt);
    size_t want = size_t(std::min<uint64_t>(total - at, std::max(count, kWindow)));
    held.resize(want);
    heldAt = at;
    if (want && (fseeko(file, int64_t(at), SEEK_SET) != 0 || fread(held.data(), 1, want, file) != want)) {
        held.clear();
        return nullptr;
    }
    return held.data();
}

bool Source::read(uint64_t at, uint8_t* out, size_t count) {
    if (at > total || count > total - at) return false;
    if (!count) return true;
    return fseeko(file, int64_t(at), SEEK_SET) == 0 && fread(out, 1, count, file) == count;
}

bool Source::load(uint64_t at, uint64_t count, std::vector<uint8_t>& out, uint64_t limit) {
    if (at > total || count > total - at || count > limit) return false;
    out.resize(size_t(count));
    return read(at, out.data(), out.size());
}

uint64_t Source::find(uint64_t from, uint64_t end, const uint8_t* pattern, size_t length) {
    end = std::min(end, total);
    while (from < end && end - from >= length) {
        size_t count = size_t(std::min<uint64_t>(end - from, 1 << 16));
        const uint8_t* p = view(from, count);
        if (!p) return end;
        for (size_t i = 0; i + length <= count; ++i) {
            if (p[i] == pattern[0] && memcmp(p + i, pattern, length) == 0) return from + i;
        }
        if (count < length) break;
        from += count - (length - 1);
    }
    return end;
}

std::string utf16Text(const uint8_t* p, size_t n) { return wide(p, n, false); }

bool FileReader::read(const Track& track, uint32_t index, uint8_t* out) {
    if (index >= track.samples.size()) return false;
    return source.read(track.samples[index].offset, out, track.samples[index].size);
}

void indexTrack(Track& track) {
    track.order.clear();
    track.syncs.clear();
    for (uint32_t i = 0; i < track.samples.size(); ++i) {
        if (track.samples[i].pts + (track.kind == Kind::Video ? 0 : int64_t(track.samples[i].duration) - 1) >= track.mediaStart) track.order.push_back(i);
        if (track.samples[i].sync) track.syncs.push_back(i);
    }
    std::stable_sort(track.order.begin(), track.order.end(), [&](uint32_t a, uint32_t b) { return track.samples[a].pts < track.samples[b].pts; });
}

bool mpegAudio(const uint8_t* p, MpegAudio& out) {
    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return false;
    int version = (p[1] >> 3) & 3, layer = (p[1] >> 1) & 3, rateIndex = p[2] >> 4, frequency = (p[2] >> 2) & 3, padding = (p[2] >> 1) & 1;
    if (version == 1 || layer == 0 || rateIndex == 0 || rateIndex == 15 || frequency == 3) return false;
    static const int rates[3] = {44100, 48000, 32000};
    static const int kilobits[5][15] = {{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384}, {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320},
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256}, {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}};
    bool one = version == 3;
    out.layer = 4 - layer;
    out.version = one ? 1 : version == 2 ? 2 : 3;
    out.sampleRate = rates[frequency] >> (out.version - 1);
    int bits = kilobits[one ? out.layer - 1 : out.layer == 1 ? 3 : 4][rateIndex] * 1000;
    if (out.layer == 1) {
        out.samples = 384;
        out.size = (12 * bits / out.sampleRate + padding) * 4;
    } else {
        out.samples = out.layer == 3 && !one ? 576 : 1152;
        out.size = out.samples / 8 * bits / out.sampleRate + padding;
    }
    out.channels = (p[3] >> 6) == 3 ? 1 : 2;
    out.checksum = !(p[1] & 1);
    return out.size >= 4;
}

bool flacFrame(const uint8_t* p, size_t n, FlacFrame& out) {
    if (n < 6 || p[0] != 0xFF || (p[1] & 0xFE) != 0xF8) return false;
    int blockCode = p[2] >> 4, rateCode = p[2] & 15, channelCode = p[3] >> 4, bitsCode = (p[3] >> 1) & 7;
    if (blockCode == 0 || rateCode == 15 || channelCode > 10 || bitsCode == 3 || (p[3] & 1)) return false;
    out.variable = p[1] & 1;
    size_t at = 4;
    int more = 0;
    uint8_t lead = p[at++];
    if (lead == 0xFF) return false;
    while (more < 7 && (lead & (0x80 >> more))) ++more;
    if (more == 1) return false;
    uint64_t number = more ? lead & (0x7F >> more) : lead;
    for (int i = 1; i < more; ++i) {
        if (at >= n || (p[at] & 0xC0) != 0x80) return false;
        number = (number << 6) | (p[at++] & 0x3F);
    }
    out.number = number;
    size_t tail = (blockCode == 6 ? 1 : blockCode == 7 ? 2 : 0) + (rateCode == 12 ? 1 : rateCode >= 13 ? 2 : 0) + 1;
    if (n - at < tail) return false;
    if (blockCode == 1) out.blockSize = 192;
    else if (blockCode <= 5) out.blockSize = 576u << (blockCode - 2);
    else if (blockCode == 6) out.blockSize = p[at++] + 1u;
    else if (blockCode == 7) {
        out.blockSize = be16(p + at) + 1;
        at += 2;
    } else out.blockSize = 256u << (blockCode - 8);
    static const int rates[12] = {0, 88200, 176400, 192000, 8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000};
    if (rateCode < 12) out.sampleRate = rates[rateCode];
    else if (rateCode == 12) out.sampleRate = p[at++] * 1000;
    else {
        out.sampleRate = int(be16(p + at)) * (rateCode == 14 ? 10 : 1);
        at += 2;
    }
    static const int depths[8] = {0, 8, 12, 0, 16, 20, 24, 32};
    out.bits = depths[bitsCode];
    out.channels = channelCode < 8 ? channelCode + 1 : 2;
    if (crc8(p, at) != p[at]) return false;
    out.headerSize = at + 1;
    return true;
}

bool flacInfo(const uint8_t* p, size_t n, FlacInfo& out) {
    if (n < 34) return false;
    out.minBlock = be16(p);
    out.maxBlock = be16(p + 2);
    out.sampleRate = int(be24(p + 10) >> 4);
    out.channels = ((p[12] >> 1) & 7) + 1;
    out.bits = (((p[12] & 1) << 4) | (p[13] >> 4)) + 1;
    out.total = (uint64_t(p[13] & 15) << 32) | be32(p + 14);
    return out.sampleRate > 0;
}

uint32_t opusSamples(const uint8_t* p, size_t n) {
    if (!n) return 0;
    int config = p[0] >> 3;
    uint32_t frame = config < 12 ? (config & 3) == 3 ? 2880 : 480u << (config & 3) : config < 16 ? 480u << (config & 1) : 120u << (config & 3);
    int code = p[0] & 3;
    uint32_t count = code == 0 ? 1 : code < 3 ? 2 : n >= 2 ? p[1] & 63 : 0;
    uint32_t total = frame * count;
    return total > 5760 ? 0 : total;
}

bool VorbisModes::parse(const uint8_t* identity, size_t identitySize, const uint8_t* setup, size_t setupSize) {
    if (identitySize < 30 || identity[0] != 1 || memcmp(identity + 1, "vorbis", 6) != 0) return false;
    channels = identity[11];
    sampleRate = int(le32(identity + 12));
    int small = identity[28] & 15, big = identity[28] >> 4;
    if (small < 6 || big > 13 || small > big || sampleRate <= 0 || channels <= 0) return false;
    blocks[0] = 1u << small;
    blocks[1] = 1u << big;
    if (setupSize < 8 || setup[0] != 5) return false;
    int64_t end = int64_t(setupSize) * 8 - 1;
    auto bit = [&](int64_t at) { return (setup[at >> 3] >> (at & 7)) & 1; };
    while (end >= 0 && !bit(end)) --end;
    int count = 0;
    while (count < 64) {
        int64_t base = end - 41 * int64_t(count + 1);
        if (base < 56 + 6) break;
        bool zero = true;
        for (int i = 1; i <= 32 && zero; ++i) zero = !bit(base + i);
        if (!zero) break;
        ++count;
    }
    for (; count > 0; --count) {
        int64_t base = end - 41 * int64_t(count);
        int stored = 0;
        for (int i = 0; i < 6; ++i) stored |= bit(base - 6 + i) << i;
        if (stored == count - 1) break;
    }
    if (count <= 0) return false;
    large.clear();
    for (int i = 0; i < count; ++i) large.push_back(bit(end - 41 * int64_t(count - i)));
    modeBits = 0;
    while ((1 << modeBits) < count) ++modeBits;
    previous = 0;
    return true;
}

uint32_t VorbisModes::next(const uint8_t* p, size_t n) {
    if (!n || (p[0] & 1) || large.empty()) return 0;
    size_t mode = (p[0] >> 1) & ((1u << modeBits) - 1);
    if (mode >= large.size()) return 0;
    uint32_t block = blocks[large[mode] ? 1 : 0];
    uint32_t samples = previous ? (previous + block) / 4 : 0;
    previous = block;
    return samples;
}

bool splitXiph(const std::vector<uint8_t>& laced, std::vector<View>& packets) {
    packets.clear();
    if (laced.empty()) return false;
    size_t count = size_t(laced[0]) + 1;
    size_t at = 1;
    std::vector<size_t> sizes;
    size_t used = 0;
    for (size_t i = 0; i + 1 < count; ++i) {
        size_t size = 0;
        for (;;) {
            if (at >= laced.size()) return false;
            uint8_t b = laced[at++];
            size += b;
            if (b != 255) break;
        }
        sizes.push_back(size);
        used += size;
    }
    if (used > laced.size() - at) return false;
    sizes.push_back(laced.size() - at - used);
    for (size_t size : sizes) {
        packets.push_back(View{laced.data() + at, size});
        at += size;
    }
    return true;
}

void vorbisComments(const uint8_t* p, size_t n, Tags& tags) {
    if (n < 8) return;
    size_t vendor = le32(p);
    if (vendor > n - 8) return;
    put(tags, "encoder", std::string(reinterpret_cast<const char*>(p + 4), vendor));
    size_t at = 4 + vendor;
    uint32_t count = le32(p + at);
    at += 4;
    for (uint32_t i = 0; i < count && n - at >= 4; ++i) {
        size_t size = le32(p + at);
        at += 4;
        if (size > n - at) return;
        const char* text = reinterpret_cast<const char*>(p + at);
        const char* equals = static_cast<const char*>(memchr(text, '=', size));
        if (equals && size <= 65536) {
            std::string key(text, equals);
            for (char& c : key) c = char(c >= 'A' && c <= 'Z' ? c + 32 : c);
            put(tags, key, std::string(equals + 1, text + size));
        }
        at += size;
    }
}

uint64_t id3v2(Source& source, uint64_t at, Tags& tags) {
    const uint8_t* h = source.view(at, 10);
    if (!h || memcmp(h, "ID3", 3) != 0 || h[3] == 0xFF || h[4] == 0xFF || ((h[6] | h[7] | h[8] | h[9]) & 0x80)) return 0;
    int version = h[3];
    uint8_t flags = h[5];
    uint32_t size = syncsafe(h + 6);
    uint64_t whole = 10 + uint64_t(size) + (flags & 0x10 ? 10 : 0);
    std::vector<uint8_t> body;
    if (version < 2 || version > 4 || !source.load(at + 10, size, body, 64u << 20)) return whole;
    if ((flags & 0x80) && version < 4) unsynchronise(body);
    const bool three = version == 2;
    const size_t head = three ? 6 : 10;
    size_t pos = 0;
    if ((flags & 0x40) && !three && body.size() >= 4) pos = version == 4 ? syncsafe(body.data()) : 4 + size_t(be32(body.data()));
    while (pos <= body.size() && body.size() - pos >= head && body[pos]) {
        const uint8_t* f = body.data() + pos;
        size_t length = three ? be24(f + 3) : version == 4 ? syncsafe(f + 4) : be32(f + 4);
        uint8_t format = three ? 0 : f[9];
        pos += head;
        if (length > body.size() - pos) break;
        const char* key = id3Key(f, three);
        if (key && length && !(format & (version == 3 ? 0xE0 : 0x4C))) {
            std::vector<uint8_t> content(body.begin() + pos, body.begin() + pos + length);
            if (version == 4 && ((format & 0x02) || (flags & 0x80))) unsynchronise(content);
            size_t skip = version == 4 && (format & 0x01) ? 4 : 0;
            if (content.size() > skip) put(tags, key, id3Text(content.data() + skip, content.size() - skip));
        }
        pos += length;
    }
    return whole;
}

void id3v1(Source& source, Tags& tags) {
    if (source.size() < 128) return;
    const uint8_t* p = source.view(source.size() - 128, 128);
    if (!p || memcmp(p, "TAG", 3) != 0) return;
    put(tags, "title", latin(p + 3, 30));
    put(tags, "artist", latin(p + 33, 30));
    put(tags, "album", latin(p + 63, 30));
    put(tags, "date", latin(p + 93, 4));
}

}

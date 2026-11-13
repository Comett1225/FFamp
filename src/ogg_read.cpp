#include "read_common.h"
#include "readers.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <map>

namespace absent {

namespace {

struct Piece {
    uint64_t offset;
    uint32_t size;
};

uint32_t pageChecksum(const uint8_t* p, size_t n) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> filled{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i << 24;
            for (int k = 0; k < 8; ++k) c = (c << 1) ^ (c & 0x80000000u ? 0x04C11DB7u : 0);
            filled[i] = c;
        }
        return filled;
    }();
    uint32_t c = 0;
    for (size_t i = 0; i < n; ++i) {
        uint8_t byte = i >= 22 && i < 26 ? 0 : p[i];
        c = (c << 8) ^ table[(c >> 24) ^ byte];
    }
    return c;
}

enum class Stream { None, Opus, Vorbis, Flac };

struct PageEnd {
    size_t samples;
    int64_t granule;
};

class OggReader : public FileReader {
public:
    std::vector<Piece> pieces;
    std::vector<uint32_t> starts;

    bool read(const Track& track, uint32_t index, uint8_t* out) override {
        if (index >= track.samples.size() || index + 1 >= starts.size()) return false;
        for (uint32_t i = starts[index]; i < starts[index + 1]; ++i) {
            if (!source.read(pieces[i].offset, out, pieces[i].size)) return false;
            out += pieces[i].size;
        }
        return true;
    }

    std::vector<uint8_t> gather(const std::vector<Piece>& packet, size_t limit) {
        std::vector<uint8_t> data;
        for (const Piece& piece : packet) {
            size_t take = std::min<size_t>(piece.size, limit - data.size());
            if (!take) break;
            size_t at = data.size();
            data.resize(at + take);
            if (!source.read(piece.offset, data.data() + at, take)) {
                data.resize(at);
                break;
            }
        }
        return data;
    }
};

}

std::unique_ptr<Reader> openOgg(const std::string& path, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    auto reader = std::make_unique<OggReader>();
    Source& source = reader->source;
    if (!source.open(path)) return fail("cannot open file");
    reader->name = "ogg";
    const uint64_t total = source.size();
    static const uint8_t capture[4] = {'O', 'g', 'g', 'S'};

    Stream stream = Stream::None;
    uint32_t serial = 0;
    Track track;
    track.id = 1;
    track.kind = Kind::Audio;
    std::vector<Piece> partial;
    bool broken = false;
    size_t headersWanted = 0, headersSeen = 0;
    std::vector<std::vector<uint8_t>> vorbisHeaders;
    VorbisModes modes;
    FlacInfo flacStream;
    int64_t preSkip = 0;
    std::vector<PageEnd> pageEnds;
    int64_t time = 0;

    auto packet = [&](const std::vector<Piece>& parts) {
        uint64_t size = 0;
        for (const Piece& piece : parts) size += piece.size;
        if (!size || size > 0x7FFFFFFF) return;
        if (headersSeen < headersWanted || (stream == Stream::Flac && headersWanted == 0)) {
            std::vector<uint8_t> data = reader->gather(parts, 16u << 20);
            if (data.size() != size) return;
            if (stream == Stream::Opus) {
                if (headersSeen == 0) {
                    track.config = data;
                    track.channels = data.size() > 9 ? data[9] : 0;
                    preSkip = data.size() >= 12 ? int64_t(le16(data.data() + 10)) : 0;
                } else if (data.size() > 8 && memcmp(data.data(), "OpusTags", 8) == 0) {
                    vorbisComments(data.data() + 8, data.size() - 8, reader->found);
                }
                ++headersSeen;
                return;
            }
            if (stream == Stream::Vorbis) {
                if (data.size() > 7 && data[0] == 3) vorbisComments(data.data() + 7, data.size() - 7, reader->found);
                vorbisHeaders.push_back(std::move(data));
                ++headersSeen;
                return;
            }
            if (headersSeen == 0) {
                if (data.size() >= 13 + 4 + 34) track.config.assign(data.begin() + 17, data.begin() + 51);
                ++headersSeen;
                return;
            }
            if (data[0] != 0xFF) {
                if ((data[0] & 0x7F) == 4 && data.size() > 4) vorbisComments(data.data() + 4, data.size() - 4, reader->found);
                ++headersSeen;
                return;
            }
            headersWanted = headersSeen;
        }
        if (stream == Stream::Vorbis && modes.large.empty()) {
            if (vorbisHeaders.size() < 3 || !modes.parse(vorbisHeaders[0].data(), vorbisHeaders[0].size(), vorbisHeaders[2].data(), vorbisHeaders[2].size())) {
                broken = true;
                return;
            }
        }
        std::vector<uint8_t> head = reader->gather(parts, 16);
        Sample s;
        if (stream == Stream::Opus) {
            s.duration = opusSamples(head.data(), head.size());
        } else if (stream == Stream::Vorbis) {
            if (head.empty() || (head[0] & 1)) return;
            s.duration = modes.next(head.data(), head.size());
        } else {
            FlacFrame f;
            if (!flacFrame(head.data(), head.size(), f)) return;
            s.duration = f.blockSize;
        }
        s.offset = reader->pieces.size();
        s.size = uint32_t(size);
        s.pts = s.dts = time;
        s.sync = true;
        time += s.duration;
        reader->starts.push_back(uint32_t(reader->pieces.size()));
        reader->pieces.insert(reader->pieces.end(), parts.begin(), parts.end());
        track.samples.push_back(s);
    };

    uint64_t at = 0;
    bool lost = false, numbered = false;
    uint32_t lastNumber = 0;
    while (!broken && total - at >= 27) {
        const uint8_t* h = source.view(at, 27);
        if (!h) break;
        size_t segments = h[26];
        const uint8_t* table = memcmp(h, capture, 4) == 0 && h[4] == 0 ? source.view(at, 27 + segments) : nullptr;
        size_t body = 0;
        for (size_t i = 0; table && i < segments; ++i) body += table[27 + i];
        const size_t pageSize = 27 + segments + body;
        const uint8_t* page = table ? source.view(at, pageSize) : nullptr;
        if (!page || pageChecksum(page, pageSize) != le32(page + 22)) {
            uint64_t next = source.find(at + 1, total, capture, 4);
            if (next >= total) break;
            at = next;
            lost = true;
            continue;
        }
        const uint8_t flags = page[5];
        const int64_t granule = int64_t(le64(page + 6));
        const uint32_t pageSerial = le32(page + 14);
        const uint32_t pageNumber = le32(page + 18);
        const uint8_t* lacing = page + 27;
        const uint64_t bodyAt = at + 27 + segments;
        const uint8_t* content = page + 27 + segments;
        at += pageSize;
        if ((flags & 2) && stream == Stream::None) {
            size_t first = 0;
            for (size_t i = 0; i < segments; ++i) {
                first += lacing[i];
                if (lacing[i] < 255) break;
            }
            if (first >= 19 && memcmp(content, "OpusHead", 8) == 0) {
                stream = Stream::Opus;
                headersWanted = 2;
                track.codec = fourcc("Opus");
                track.configType = fourcc("OpHd");
                track.sampleRate = 48000;
            } else if (first >= 30 && memcmp(content, "\x01vorbis", 7) == 0) {
                stream = Stream::Vorbis;
                headersWanted = 3;
                track.codec = fourcc("vorb");
                track.configType = fourcc("vorb");
            } else if (first >= 51 && memcmp(content, "\x7F" "FLAC", 5) == 0 && memcmp(content + 9, "fLaC", 4) == 0) {
                stream = Stream::Flac;
                track.codec = fourcc("fLaC");
                track.configType = fourcc("fLaC");
            }
            if (stream != Stream::None) serial = pageSerial;
        } else if ((flags & 2) && pageSerial == serial && stream != Stream::None) {
            break;
        }
        if (stream == Stream::None || pageSerial != serial) continue;
        size_t index = 0;
        uint64_t offset = bodyAt;
        if (numbered && pageNumber != lastNumber + 1) lost = true;
        numbered = true;
        lastNumber = pageNumber;
        if (lost || !(flags & 1)) partial.clear();
        if ((flags & 1) && partial.empty()) {
            while (index < segments && lacing[index] == 255) offset += lacing[index++];
            if (index < segments) offset += lacing[index++];
        }
        lost = false;
        const size_t before = track.samples.size();
        uint32_t run = 0;
        for (; index < segments; ++index) {
            run += lacing[index];
            if (lacing[index] == 255) continue;
            if (run) partial.push_back({offset, run});
            packet(partial);
            partial.clear();
            offset += run;
            run = 0;
        }
        if (run) partial.push_back({offset, run});
        if (track.samples.size() > before && granule >= 0) pageEnds.push_back({track.samples.size(), granule});
        if (flags & 4) break;
    }
    reader->starts.push_back(uint32_t(reader->pieces.size()));
    if (stream == Stream::None) return fail("no Opus, Vorbis or FLAC stream in this Ogg file");
    if (stream == Stream::Opus) {
        if (track.config.size() < 19 || track.channels < 1) return fail("broken Opus header");
        track.mediaStart = preSkip;
    } else if (stream == Stream::Vorbis) {
        if (vorbisHeaders.size() < 3 || modes.large.empty()) return fail("broken Vorbis headers");
        track.sampleRate = modes.sampleRate;
        track.channels = modes.channels;
        Bytes config;
        config.u8(2);
        for (int i = 0; i < 2; ++i) {
            size_t size = vorbisHeaders[i].size();
            for (; size >= 255; size -= 255) config.u8(255);
            config.u8(uint32_t(size));
        }
        for (int i = 0; i < 3; ++i) config.raw(vorbisHeaders[i].data(), vorbisHeaders[i].size());
        track.config = std::move(config.d);
    } else {
        if (!flacInfo(track.config.data(), track.config.size(), flacStream)) return fail("broken FLAC header");
        track.sampleRate = flacStream.sampleRate;
        track.channels = flacStream.channels;
        track.bits = flacStream.bits;
    }
    track.timescale = uint32_t(track.sampleRate);
    if (track.samples.empty()) return fail("no sound packets in this Ogg file");
    if (!pageEnds.empty()) {
        auto gapAt = [&](const PageEnd& page) { return (page.samples < track.samples.size() ? track.samples[page.samples].pts : time) - page.granule; };
        std::map<int64_t, size_t> votes;
        for (size_t i = 0; i + 1 < pageEnds.size(); ++i) ++votes[gapAt(pageEnds[i])];
        int64_t shift = 0;
        size_t most = 0;
        for (const auto& [gap, count] : votes) {
            if (count > most) {
                most = count;
                shift = gap;
            }
        }
        if (shift > 0) track.mediaStart += shift;
        if (shift < 0 && shift > -(int64_t(1) << 40)) {
            for (Sample& s : track.samples) {
                s.pts -= shift;
                s.dts -= shift;
            }
            track.mediaStart -= shift;
            time -= shift;
            shift = 0;
        }
        const __int128 end = __int128(pageEnds.back().granule) + shift;
        if (end >= track.mediaStart && end < time) track.mediaEnd = int64_t(end);
    }
    indexTrack(track);
    reader->list.push_back(std::move(track));
    return reader;
}

}

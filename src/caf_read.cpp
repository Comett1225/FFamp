#include "aac_config.h"
#include "read_common.h"
#include "readers.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace absent {

namespace {

const uint32_t kPacketFrames = 4096;
const uint64_t kMostPackets = 1u << 27;

}

std::unique_ptr<Reader> openCaf(const std::string& path, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    auto reader = std::make_unique<FileReader>();
    Source& source = reader->source;
    if (!source.open(path)) return fail("cannot open file");
    reader->name = "caf";
    const uint64_t total = source.size();
    double rate = 0;
    uint32_t format = 0, flags = 0, packetBytes = 0, packetFrames = 0, channels = 0, bits = 0;
    uint64_t dataAt = 0, dataSize = 0, tableAt = 0, tableSize = 0;
    std::vector<uint8_t> cookie;
    bool described = false;
    uint64_t at = 8;
    while (total - at >= 12) {
        const uint8_t* h = source.view(at, 12);
        if (!h) break;
        uint32_t id = be32(h);
        uint64_t size = be64(h + 4);
        uint64_t body = at + 12;
        if (size > total - body) size = total - body;
        if (id == fourcc("desc") && size >= 32) {
            const uint8_t* d = source.view(body, 32);
            if (!d) break;
            uint64_t raw = be64(d);
            memcpy(&rate, &raw, 8);
            format = be32(d + 8);
            flags = be32(d + 12);
            packetBytes = be32(d + 16);
            packetFrames = be32(d + 20);
            channels = be32(d + 24);
            bits = be32(d + 28);
            described = true;
        } else if (id == fourcc("data") && size >= 4 && !dataAt) {
            dataAt = body + 4;
            dataSize = size - 4;
        } else if (id == fourcc("pakt") && size >= 24) {
            tableAt = body;
            tableSize = size;
        } else if (id == fourcc("kuki")) {
            source.load(body, size, cookie, 1 << 20);
        } else if (id == fourcc("info") && size >= 4) {
            std::vector<uint8_t> text;
            if (source.load(body, size, text, 1 << 20)) {
                std::vector<std::string> words;
                for (size_t i = 4; i < text.size();) {
                    size_t end = size_t(std::find(text.begin() + long(i), text.end(), uint8_t(0)) - text.begin());
                    words.emplace_back(text.begin() + long(i), text.begin() + long(end));
                    i = end + 1;
                }
                for (size_t i = 0; i + 1 < words.size(); i += 2) {
                    if (!words[i].empty() && !words[i + 1].empty() && !reader->found.count(words[i])) reader->found[words[i]] = words[i + 1];
                }
            }
        }
        at = body + size;
    }
    if (!described) return fail("no sound format description in this CAF file");
    if (!dataAt) return fail("no sound data in this CAF file");
    if (!(rate >= 1 && rate <= 768000) || channels < 1 || channels > 64) return fail("unsupported sound layout in this CAF file");
    Track track;
    track.id = 1;
    track.kind = Kind::Audio;
    track.sampleRate = int(std::lround(rate));
    track.timescale = uint32_t(track.sampleRate);
    track.channels = int(channels);
    if (format == fourcc("lpcm")) {
        const bool floating = flags & 1;
        const bool sized = floating ? bits == 32 || bits == 64 : bits == 8 || bits == 16 || bits == 24 || bits == 32;
        const uint64_t frameBytes = uint64_t(channels) * (bits / 8);
        if (!sized || packetBytes != frameBytes) return fail("unsupported PCM layout in this CAF file");
        track.codec = fourcc("lpcm");
        track.bits = int(bits);
        track.floating = floating;
        track.bigEndian = !(flags & 2) && bits > 8;
        const uint64_t frames = dataSize / frameBytes;
        for (uint64_t done = 0; done < frames; done += kPacketFrames) {
            Sample s;
            uint64_t count = std::min<uint64_t>(kPacketFrames, frames - done);
            s.offset = dataAt + done * frameBytes;
            s.size = uint32_t(count * frameBytes);
            s.pts = s.dts = int64_t(done);
            s.duration = uint32_t(count);
            s.sync = true;
            track.samples.push_back(s);
        }
        if (bits == 8) return fail("8-bit PCM in CAF files is not supported");
    } else {
        if (format == fourcc("aac ") || format == fourcc("aach") || format == fourcc("aacp")) {
            track.codec = fourcc("mp4a");
            track.configType = fourcc("esds");
            track.config.assign(4, 0);
            track.config.insert(track.config.end(), cookie.begin(), cookie.end());
            if (cookie.empty()) return fail("AAC in this CAF file has no decoder configuration");
            AacConfig aac;
            if (aacConfigOf(track.configType, track.config, aac) && aac.outputRate > 0) track.sampleRate = aac.outputRate;
        } else if (format == fourcc("alac")) {
            size_t from = cookie.size() >= 48 && memcmp(cookie.data() + 4, "frma", 4) == 0 ? 24 : 0;
            if (cookie.size() < from + 24) return fail("ALAC in this CAF file has no decoder configuration");
            track.codec = fourcc("alac");
            track.configType = fourcc("alac");
            track.config.assign(cookie.begin() + long(from), cookie.begin() + long(from) + 24);
            track.bits = track.config[5];
        } else if (format == fourcc("flac")) {
            static const uint8_t mark[4] = {'d', 'f', 'L', 'a'};
            auto found = std::search(cookie.begin(), cookie.end(), mark, mark + 4);
            FlacInfo info;
            if (cookie.end() - found < 46 || !flacInfo(&found[12], 34, info)) return fail("FLAC in this CAF file has no stream description");
            track.codec = fourcc("fLaC");
            track.configType = fourcc("fLaC");
            track.config.assign(found + 12, found + 46);
            track.bits = info.bits;
        } else if (format == fourcc("opus") && channels <= 2 && track.sampleRate == 48000) {
            static const uint8_t head[19] = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 0, 0, 0, 0x80, 0xBB, 0, 0, 0, 0, 0};
            track.codec = fourcc("Opus");
            track.configType = fourcc("OpHd");
            track.config.assign(head, head + 19);
            track.config[9] = uint8_t(channels);
        } else if (format == fourcc(".mp3") || format == fourcc(".mp2") || format == fourcc(".mp1") || format == fourcc("ac-3") || format == fourcc("ec-3") ||
                   format == fourcc("samr") || format == fourcc("sawb")) {
            track.codec = format;
        } else {
            if (error) *error = "sound codec " + fourccText(format) + " in this CAF file is not supported";
            return nullptr;
        }
        int64_t priming = 0, valid = -1;
        uint64_t packets = packetBytes ? dataSize / packetBytes : 0;
        std::vector<uint8_t> table;
        if (tableAt) {
            const uint8_t* t = source.view(tableAt, 24);
            if (!t) return fail("the packet table of this CAF file cannot be read");
            if (!packetBytes || !packetFrames) packets = be64(t);
            valid = int64_t(be64(t + 8));
            priming = int32_t(be32(t + 16));
            if (!source.load(tableAt + 24, tableSize - 24, table, 1u << 30)) return fail("the packet table of this CAF file cannot be read");
        }
        if ((!packetBytes || !packetFrames) && !tableAt) return fail("this CAF file has no packet table");
        if (packets > kMostPackets) return fail("too many packets in this CAF file");
        size_t next = 0;
        bool whole = true;
        auto number = [&]() {
            uint64_t value = 0;
            for (int i = 0; i < 9; ++i) {
                if (next >= table.size()) {
                    whole = false;
                    return uint64_t(0);
                }
                uint8_t b = table[next++];
                value = (value << 7) | (b & 0x7F);
                if (!(b & 0x80)) return value;
            }
            whole = false;
            return uint64_t(0);
        };
        uint64_t offset = 0;
        int64_t time = 0;
        for (uint64_t i = 0; i < packets; ++i) {
            uint64_t size = packetBytes ? packetBytes : number();
            uint64_t frames = packetFrames ? packetFrames : number();
            if (!whole || size > dataSize - offset || size > 0x7FFFFFFF || frames > 0x7FFFFFFF) break;
            Sample s;
            s.offset = dataAt + offset;
            s.size = uint32_t(size);
            s.pts = s.dts = time;
            s.duration = uint32_t(frames);
            s.sync = true;
            track.samples.push_back(s);
            offset += size;
            time += int64_t(frames);
        }
        if (priming > 0 && priming < time) track.mediaStart = priming;
        if (track.codec == fourcc("Opus")) {
            track.config[10] = uint8_t(track.mediaStart);
            track.config[11] = uint8_t(track.mediaStart >> 8);
        }
        if (valid >= 0 && track.mediaStart + valid < time) track.mediaEnd = track.mediaStart + valid;
    }
    if (track.samples.empty()) return fail("no sound packets in this CAF file");
    indexTrack(track);
    reader->list.push_back(std::move(track));
    return reader;
}

}

#include "read_common.h"
#include "readers.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace absent {

void riffInfo(Source& source, uint64_t at, uint64_t size, Tags& tags);

namespace {

const uint32_t kPacketFrames = 4096;

class WaveReader : public FileReader {
public:
    bool flipSign = false;

    bool read(const Track& track, uint32_t index, uint8_t* out) override {
        if (!FileReader::read(track, index, out)) return false;
        if (flipSign) for (uint32_t i = 0; i < track.samples[index].size; ++i) out[i] ^= 0x80;
        return true;
    }
};

struct Layout {
    int channels = 0;
    double rate = 0;
    int bytes = 0;
    bool floating = false;
    bool bigEndian = false;
    uint64_t mask = 0;
    uint64_t dataAt = 0;
    uint64_t dataSize = 0;
    uint64_t frames = ~0ull;
    uint32_t codec = 0;
    uint32_t unitBytes = 0;
    uint32_t unitFrames = 0;
    std::vector<uint8_t> format;
    bool described = false;
    bool located = false;
    const char* refused = nullptr;
};

std::string chunkText(Source& source, uint64_t at, uint64_t size) {
    std::vector<uint8_t> text;
    if (!source.load(at, size, text, 1 << 16)) return {};
    std::string out(text.begin(), std::find(text.begin(), text.end(), uint8_t(0)));
    while (!out.empty() && (out.back() == ' ' || out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

void keep(Tags& tags, const char* key, const std::string& value) {
    if (!value.empty() && !tags.count(key)) tags[key] = value;
}

}

void riffInfo(Source& source, uint64_t at, uint64_t size, Tags& tags) {
    static const char* const names[][2] = {{"INAM", "title"}, {"IART", "artist"}, {"IPRD", "album"}, {"ICRD", "date"}, {"IGNR", "genre"},
        {"ICMT", "comment"}, {"ISFT", "encoder"}, {"ITRK", "track"}, {"IPRT", "track"}, {"ICOP", "copyright"}};
    uint64_t end = at + size;
    while (end - at >= 8) {
        const uint8_t* h = source.view(at, 8);
        if (!h) return;
        uint64_t length = le32(h + 4);
        char id[5] = {char(h[0]), char(h[1]), char(h[2]), char(h[3]), 0};
        at += 8;
        if (length > end - at) return;
        for (const auto& name : names) if (strcmp(id, name[0]) == 0) keep(tags, name[1], chunkText(source, at, length));
        at += length + (length & 1);
        if (at > end) return;
    }
}

namespace {

void riff(Source& source, Layout& w, Tags& tags) {
    const uint64_t total = source.size();
    uint64_t wideData = 0;
    uint64_t at = 12;
    while (total - at >= 8) {
        const uint8_t* h = source.view(at, 8);
        if (!h) break;
        uint32_t id = be32(h);
        uint64_t size = le32(h + 4);
        uint64_t body = at + 8;
        uint64_t left = total - body;
        if (id == fourcc("ds64") && size >= 28 && left >= 28) {
            const uint8_t* d = source.view(body, 28);
            if (d) wideData = le64(d + 8);
        } else if (id == fourcc("fmt ") && size >= 16 && left >= 16 && !w.described) {
            size_t have = size_t(std::min<uint64_t>({size, left, 40}));
            const uint8_t* f = source.view(body, have);
            if (!f) break;
            source.load(body, std::min<uint64_t>({size, left, 65536}), w.format, 65536);
            uint32_t tag = le16(f);
            w.channels = int(le16(f + 2));
            w.rate = le32(f + 4);
            uint32_t align = le16(f + 12), bits = le16(f + 14);
            if (tag == 0xFFFE && have >= 40) w.mask = le32(f + 20);
            tag = extensibleTag(f, have);
            w.bytes = w.channels > 0 && align % uint32_t(w.channels) == 0 && align ? int(align) / w.channels : int(bits + 7) / 8;
            w.floating = tag == 3;
            w.described = true;
            if (tag == 0xF1AC) {
                w.codec = fourcc("fLaC");
                w.unitBytes = 1;
            } else if (tag == 0x55 || tag == 0x50 || tag == 0x2000) {
                w.codec = tag == 0x2000 ? fourcc("ac-3") : fourcc(".mp3");
                w.unitBytes = 1;
            } else if (tag != 1 && tag != 3) {
                w.codec = waveAudio(tag);
                if (tag == 2 && w.channels > 2 && align * uint32_t(w.channels) <= 0xFFFF && w.format.size() >= 14) {
                    align *= uint32_t(w.channels);
                    w.format[12] = uint8_t(align);
                    w.format[13] = uint8_t(align >> 8);
                }
                w.unitBytes = std::max<uint32_t>(1, align);
                if (!w.codec) w.refused = "this WAV file holds sound in a format that is not supported";
            }
        } else if (id == fourcc("data") && !w.located) {
            w.dataAt = body;
            w.dataSize = size == 0xFFFFFFFF && wideData ? wideData : size;
            if ((size == 0xFFFFFFFF && !wideData) || size == 0 || w.dataSize > left) w.dataSize = left;
            w.located = true;
            size = w.dataSize;
        } else if (id == fourcc("LIST") && size >= 4 && left >= size) {
            const uint8_t* kind = source.view(body, 4);
            if (kind && be32(kind) == fourcc("INFO")) riffInfo(source, body + 4, size - 4, tags);
        } else if ((id == fourcc("id3 ") || id == fourcc("ID3 ")) && left >= size) {
            id3v2(source, body, tags);
        }
        if (size > left) break;
        at = body + size;
        if ((size & 1) && at < total) ++at;
    }
}

double extended(const uint8_t* p) {
    int exponent = int(be16(p) & 0x7FFF);
    double value = std::ldexp(double(be64(p + 2)), exponent - 16383 - 63);
    return std::isfinite(value) ? value : 0;
}

void aiff(Source& source, bool compressed, Layout& w, Tags& tags, bool& flip) {
    const uint64_t total = source.size();
    uint64_t at = 12;
    w.bigEndian = true;
    while (total - at >= 8) {
        const uint8_t* h = source.view(at, 8);
        if (!h) break;
        uint32_t id = be32(h);
        uint64_t size = be32(h + 4);
        uint64_t body = at + 8;
        uint64_t left = total - body;
        if (id == fourcc("COMM") && size >= 18 && left >= 18 && !w.described) {
            size_t have = size_t(std::min<uint64_t>({size, left, 22}));
            const uint8_t* c = source.view(body, have);
            if (!c) break;
            w.channels = int(be16(c));
            w.frames = be32(c + 2);
            int bits = int(be16(c + 6));
            w.rate = extended(c + 8);
            w.bytes = (bits + 7) / 8;
            w.described = true;
            uint32_t kind = compressed && have >= 22 ? be32(c + 18) : fourcc("NONE");
            if (kind == fourcc("sowt")) {
                w.bigEndian = false;
            } else if (kind == fourcc("fl32") || kind == fourcc("FL32")) {
                w.floating = true;
                w.bytes = 4;
            } else if (kind == fourcc("fl64") || kind == fourcc("FL64")) {
                w.floating = true;
                w.bytes = 8;
            } else if (kind == fourcc("in24")) {
                w.bytes = 3;
            } else if (kind == fourcc("in32")) {
                w.bytes = 4;
            } else if (kind == fourcc("alaw") || kind == fourcc("ALAW") || kind == fourcc("ulaw") || kind == fourcc("ULAW")) {
                w.codec = (kind | 0x20202020) == fourcc("alaw") ? fourcc("alaw") : fourcc("ulaw");
                w.unitBytes = uint32_t(w.channels);
                w.unitFrames = 1;
            } else if (kind == fourcc("ima4")) {
                w.codec = kind;
                w.unitBytes = 34 * uint32_t(w.channels);
                w.unitFrames = 64;
            } else if (kind != fourcc("raw ") && kind != fourcc("NONE") && kind != fourcc("twos")) {
                w.refused = "this AIFF file holds compressed sound, only PCM is supported";
            }
            flip = w.bytes == 1 && kind != fourcc("raw ") && !w.codec;
        } else if (id == fourcc("SSND") && left >= 8 && !w.located) {
            const uint8_t* d = source.view(body, 8);
            if (!d) break;
            uint64_t skip = be32(d);
            uint64_t available = left - 8;
            uint64_t content = size >= 8 && size - 8 <= available ? size - 8 : available;
            if (skip <= content) {
                w.dataAt = body + 8 + skip;
                w.dataSize = content - skip;
                w.located = true;
            }
        } else if (id == fourcc("NAME")) {
            keep(tags, "title", chunkText(source, body, std::min(size, left)));
        } else if (id == fourcc("AUTH")) {
            keep(tags, "artist", chunkText(source, body, std::min(size, left)));
        } else if (id == fourcc("ANNO")) {
            keep(tags, "comment", chunkText(source, body, std::min(size, left)));
        } else if (id == fourcc("(c) ")) {
            keep(tags, "copyright", chunkText(source, body, std::min(size, left)));
        } else if (id == fourcc("ID3 ") && left >= size) {
            id3v2(source, body, tags);
        }
        if (size > left) break;
        at = body + size;
        if ((size & 1) && at < total) ++at;
    }
}

}

std::unique_ptr<Reader> openWave(const std::string& path, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    auto reader = std::make_unique<WaveReader>();
    Source& source = reader->source;
    if (!source.open(path)) return fail("cannot open file");
    const uint8_t* head = source.view(0, 12);
    if (!head) return fail("file is too short");
    Layout w;
    if (memcmp(head, "FORM", 4) == 0) {
        reader->name = "aiff";
        aiff(source, memcmp(head + 8, "AIFC", 4) == 0, w, reader->found, reader->flipSign);
    } else {
        reader->name = "wav";
        riff(source, w, reader->found);
    }
    if (!w.described) return fail("no sound format description in this file");
    if (w.refused) return fail(w.refused);
    if (!w.located) return fail("no sound data in this file");
    if (w.codec) {
        if (w.channels < 1 || w.channels > 255 || !(w.rate >= 1 && w.rate <= 16777216) || w.unitBytes < 1 || w.unitBytes > (1u << 20)) return fail("unsupported sound layout");
        Track track;
        track.id = 1;
        track.kind = Kind::Audio;
        track.codec = w.codec;
        track.sampleRate = int(std::lround(w.rate));
        track.timescale = uint32_t(track.sampleRate);
        track.channels = w.channels;
        track.channelMask = w.mask;
        if (!w.format.empty()) {
            track.configType = fourcc("wfex");
            track.config = std::move(w.format);
        }
        if (track.codec == fourcc("fLaC")) {
            FlacInfo info;
            if (track.config.size() < 18 + 34 || !flacInfo(track.config.data() + 18, 34, info)) return fail("FLAC in this WAV file has no stream description");
            track.config.erase(track.config.begin(), track.config.begin() + 18);
            track.config.resize(34);
            track.configType = fourcc("fLaC");
            track.bits = info.bits;
            if (flacFrames(source, w.dataAt, w.dataAt + w.dataSize, info, track) < 0) return fail("no sound frames in this file");
            indexTrack(track);
            reader->list.push_back(std::move(track));
            return reader;
        }
        if (track.codec == fourcc("ac-3") || track.codec == fourcc(".mp3")) {
            const bool mpeg = track.codec == fourcc(".mp3");
            track.configType = 0;
            track.config.clear();
            uint64_t at = 0, lost = 0;
            int64_t time = 0;
            while (w.dataSize - at >= 8 && lost < (1u << 20)) {
                const uint8_t* head = source.view(w.dataAt + at, 8);
                if (!head) break;
                MpegAudio m;
                DolbyFrame d;
                uint32_t size = 0, samples = 0, codec = 0;
                int rate = 0, channels = 0;
                if (mpeg && mpegAudio(head, m)) {
                    size = uint32_t(m.size), samples = uint32_t(m.samples), rate = m.sampleRate, channels = m.channels;
                    codec = m.layer == 3 ? fourcc(".mp3") : m.layer == 2 ? fourcc(".mp2") : fourcc(".mp1");
                } else if (!mpeg && dolbyFrame(head, d) && !d.dependent) {
                    size = d.bytes, samples = d.samples, rate = d.rate, channels = d.channels, codec = d.codec;
                }
                if (size < 8 || size > w.dataSize - at || rate < 1 || (!track.samples.empty() && (codec != track.codec || rate != track.sampleRate))) {
                    ++at;
                    ++lost;
                    continue;
                }
                lost = 0;
                if (track.samples.empty()) {
                    track.codec = codec;
                    track.sampleRate = rate;
                    track.channels = channels;
                    track.timescale = uint32_t(rate);
                }
                Sample s;
                s.offset = w.dataAt + at;
                s.size = size;
                s.pts = s.dts = time;
                s.duration = samples;
                s.sync = true;
                time += samples;
                track.samples.push_back(s);
                at += size;
            }
            if (track.samples.empty()) return fail("no sound frames in this file");
            indexTrack(track);
            reader->list.push_back(std::move(track));
            return reader;
        }
        const uint64_t units = w.dataSize / w.unitBytes + (w.unitFrames ? 0 : w.dataSize % w.unitBytes != 0);
        const uint64_t group = std::max<uint64_t>(1, (w.unitFrames ? kPacketFrames / w.unitFrames : 4096 / w.unitBytes));
        uint64_t time = 0;
        for (uint64_t done = 0; done < units; done += group) {
            Sample s;
            s.offset = w.dataAt + done * w.unitBytes;
            s.size = uint32_t(std::min<uint64_t>(group * w.unitBytes, w.dataAt + w.dataSize - s.offset));
            const uint64_t frames = w.unitFrames ? std::min(group, units - done) * w.unitFrames : uint64_t(waveFrames(track, s.size));
            s.pts = s.dts = int64_t(time);
            s.duration = uint32_t(frames);
            s.sync = true;
            time += frames;
            track.samples.push_back(s);
        }
        if (track.samples.empty()) return fail("no sound data in this file");
        indexTrack(track);
        reader->list.push_back(std::move(track));
        return reader;
    }
    const bool sized = w.floating ? w.bytes == 4 || w.bytes == 8 : (w.bytes >= 1 && w.bytes <= 4) || w.bytes == 8;
    if (w.channels < 1 || w.channels > 255 || !sized || !(w.rate >= 1 && w.rate <= 16777216)) return fail("unsupported PCM layout");
    const uint64_t frameBytes = uint64_t(w.channels) * w.bytes;
    const uint64_t frames = std::min(w.frames, w.dataSize / frameBytes);
    Track track;
    track.id = 1;
    track.kind = Kind::Audio;
    track.codec = fourcc("lpcm");
    track.sampleRate = int(std::lround(w.rate));
    track.timescale = uint32_t(track.sampleRate);
    track.channels = w.channels;
    track.bits = w.bytes * 8;
    track.floating = w.floating;
    track.bigEndian = w.bigEndian && w.bytes > 1;
    track.channelMask = w.mask;
    track.samples.reserve(size_t((frames + kPacketFrames - 1) / kPacketFrames));
    for (uint64_t done = 0; done < frames; done += kPacketFrames) {
        Sample s;
        uint64_t count = std::min<uint64_t>(kPacketFrames, frames - done);
        s.offset = w.dataAt + done * frameBytes;
        s.size = uint32_t(count * frameBytes);
        s.pts = s.dts = int64_t(done);
        s.duration = uint32_t(count);
        s.sync = true;
        track.samples.push_back(s);
    }
    indexTrack(track);
    reader->list.push_back(std::move(track));
    return reader;
}

}

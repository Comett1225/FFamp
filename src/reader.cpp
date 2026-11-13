#include "absent/media.h"
#include "readers.h"
#include <cstdio>
#include <cstring>

namespace absent {

namespace {

enum class Sniffed { Unknown, Mp4, Matroska, Ogg, Wave, RawAudio, TransportStream };

Sniffed sniff(const uint8_t* b, size_t n) {
    auto at = [&](size_t i, const char* text) { size_t k = strlen(text); return n >= i + k && memcmp(b + i, text, k) == 0; };
    if (at(4, "ftyp") || at(4, "moov") || at(4, "mdat") || at(4, "free") || at(4, "wide") || at(4, "skip")) return Sniffed::Mp4;
    if (n >= 4 && b[0] == 0x1A && b[1] == 0x45 && b[2] == 0xDF && b[3] == 0xA3) return Sniffed::Matroska;
    if (at(0, "OggS")) return Sniffed::Ogg;
    if ((at(0, "RIFF") || at(0, "RF64")) && at(8, "WAVE")) return Sniffed::Wave;
    if (at(0, "FORM") && (at(8, "AIFF") || at(8, "AIFC"))) return Sniffed::Wave;
    if (n >= 189 && b[0] == 0x47 && b[188] == 0x47 && (n < 377 || b[376] == 0x47)) return Sniffed::TransportStream;
    if (n >= 389 && b[4] == 0x47 && b[196] == 0x47 && b[388] == 0x47) return Sniffed::TransportStream;
    if (n >= 205 && b[0] == 0x47 && b[204] == 0x47) return Sniffed::TransportStream;
    if (at(0, "fLaC") || at(0, "ID3")) return Sniffed::RawAudio;
    if (n >= 2 && b[0] == 0xFF && (b[1] & 0xE0) == 0xE0) return Sniffed::RawAudio;
    return Sniffed::Unknown;
}

const std::map<std::string, std::string> kNoTags;

}

const std::map<std::string, std::string>& Reader::tags() const { return kNoTags; }

const Track* Reader::first(Kind kind) const {
    for (const Track& t : tracks()) if (t.kind == kind && !t.samples.empty()) return &t;
    return nullptr;
}

std::unique_ptr<Reader> Reader::open(const std::string& path, std::string* error) {
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) {
        if (error) *error = "cannot open file";
        return nullptr;
    }
    uint8_t head[400] = {};
    size_t n = fread(head, 1, sizeof head, file);
    fclose(file);
    switch (sniff(head, n)) {
        case Sniffed::Mp4: return Mp4Reader::open(path, error);
        case Sniffed::Matroska: return openMatroska(path, error);
        case Sniffed::Ogg: return openOgg(path, error);
        case Sniffed::Wave: return openWave(path, error);
        case Sniffed::RawAudio: return openRawAudio(path, error);
        case Sniffed::TransportStream: return openTransportStream(path, error);
        case Sniffed::Unknown: break;
    }
    if (error) *error = "file format not recognised";
    return nullptr;
}

}

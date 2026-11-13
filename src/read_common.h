#pragma once
#include "absent/media.h"
#include "bytes.h"
#include <cstdio>

namespace absent {

inline uint32_t le16(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8); }
inline uint32_t le32(const uint8_t* p) { return le16(p) | (le16(p + 2) << 16); }
inline uint64_t le64(const uint8_t* p) { return uint64_t(le32(p)) | (uint64_t(le32(p + 4)) << 32); }
inline uint32_t be16(const uint8_t* p) { return (uint32_t(p[0]) << 8) | p[1]; }
inline uint32_t be24(const uint8_t* p) { return (uint32_t(p[0]) << 16) | be16(p + 1); }
inline uint32_t be32(const uint8_t* p) { return (be16(p) << 16) | be16(p + 2); }
inline uint64_t be64(const uint8_t* p) { return (uint64_t(be32(p)) << 32) | be32(p + 4); }

using Tags = std::map<std::string, std::string>;

class Source {
public:
    ~Source();
    bool open(const std::string& path);
    uint64_t size() const { return total; }
    const uint8_t* view(uint64_t at, size_t count);
    bool read(uint64_t at, uint8_t* out, size_t count);
    bool load(uint64_t at, uint64_t count, std::vector<uint8_t>& out, uint64_t limit);
    uint64_t find(uint64_t from, uint64_t end, const uint8_t* pattern, size_t length);

private:
    FILE* file = nullptr;
    uint64_t total = 0;
    uint64_t heldAt = 0;
    std::vector<uint8_t> held;
};

class FileReader : public Reader {
public:
    const char* format() const override { return name; }
    const std::vector<Track>& tracks() const override { return list; }
    const Tags& tags() const override { return found; }
    bool read(const Track& track, uint32_t index, uint8_t* out) override;

    Source source;
    const char* name = "";
    std::vector<Track> list;
    Tags found;
};

void indexTrack(Track& track);

struct MpegAudio {
    int version = 0;
    int layer = 0;
    int sampleRate = 0;
    int channels = 0;
    int samples = 0;
    int size = 0;
    bool checksum = false;
};

bool mpegAudio(const uint8_t* p, MpegAudio& out);

struct FlacFrame {
    bool variable = false;
    uint64_t number = 0;
    uint32_t blockSize = 0;
    int sampleRate = 0;
    int channels = 0;
    int bits = 0;
    size_t headerSize = 0;
};

bool flacFrame(const uint8_t* p, size_t n, FlacFrame& out);

struct FlacInfo {
    uint32_t minBlock = 0;
    uint32_t maxBlock = 0;
    int sampleRate = 0;
    int channels = 0;
    int bits = 0;
    uint64_t total = 0;
};

bool flacInfo(const uint8_t* p, size_t n, FlacInfo& out);

uint32_t opusSamples(const uint8_t* p, size_t n);

struct VorbisModes {
    uint32_t blocks[2] = {0, 0};
    std::vector<bool> large;
    int modeBits = 0;
    int sampleRate = 0;
    int channels = 0;
    uint32_t previous = 0;

    bool parse(const uint8_t* identity, size_t identitySize, const uint8_t* setup, size_t setupSize);
    uint32_t next(const uint8_t* p, size_t n);
};

bool splitXiph(const std::vector<uint8_t>& laced, std::vector<View>& packets);
void vorbisComments(const uint8_t* p, size_t n, Tags& tags);
uint64_t id3v2(Source& source, uint64_t at, Tags& tags);
void id3v1(Source& source, Tags& tags);
std::string utf16Text(const uint8_t* p, size_t n);

}

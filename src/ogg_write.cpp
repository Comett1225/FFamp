#include "writers.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>

namespace absent {

namespace {

const uint32_t kOpusRate = 48000;
const int64_t kPageSamples = 48000;

struct Checksums {
    uint32_t table[256];
    Checksums() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t value = i << 24;
            for (int k = 0; k < 8; ++k) value = (value << 1) ^ ((value & 0x80000000u) ? 0x04C11DB7u : 0);
            table[i] = value;
        }
    }
};

class OggWriter : public Writer {
public:
    std::string path;
    FILE* file = nullptr;
    TrackSpec spec;
    bool added = false;
    bool started = false;
    bool failed = false;
    uint32_t serial = 0;
    uint32_t sequence = 0;
    std::vector<uint8_t> lacing;
    std::vector<uint8_t> body;
    int64_t decoded = 0;
    int64_t pageStart = 0;
    int64_t written = 0;
    bool trimmed = false;

    ~OggWriter() override {
        if (file) {
            fclose(file);
            remove(path.c_str());
        }
    }

    int add(const TrackSpec& track) override {
        if (added || track.codec != fourcc("Opus")) return -1;
        spec = track;
        added = true;
        return 0;
    }

    void configure(int track, uint32_t type, const std::vector<uint8_t>& config) override {
        if (track != 0) return;
        spec.configType = type;
        spec.config = config;
    }

    void trim(int track, int64_t skip, int64_t length) override {
        if (track != 0) return;
        spec.skip = skip;
        spec.length = length;
        trimmed = true;
    }

    int64_t ticks(int64_t value) const { return spec.timescale ? value * int64_t(kOpusRate) / int64_t(spec.timescale) : 0; }

    void page(int64_t granule, int flags) {
        static const Checksums sums;
        std::vector<uint8_t> head = {'O', 'g', 'g', 'S', 0, uint8_t(flags)};
        for (int i = 0; i < 8; ++i) head.push_back(uint8_t(uint64_t(granule) >> (8 * i)));
        for (int i = 0; i < 4; ++i) head.push_back(uint8_t(serial >> (8 * i)));
        for (int i = 0; i < 4; ++i) head.push_back(uint8_t(sequence >> (8 * i)));
        head.insert(head.end(), 4, 0);
        head.push_back(uint8_t(lacing.size()));
        head.insert(head.end(), lacing.begin(), lacing.end());
        uint32_t sum = 0;
        for (uint8_t byte : head) sum = (sum << 8) ^ sums.table[(sum >> 24) ^ byte];
        for (uint8_t byte : body) sum = (sum << 8) ^ sums.table[(sum >> 24) ^ byte];
        for (int i = 0; i < 4; ++i) head[size_t(22 + i)] = uint8_t(sum >> (8 * i));
        if (fwrite(head.data(), 1, head.size(), file) != head.size() || fwrite(body.data(), 1, body.size(), file) != body.size()) failed = true;
        ++sequence;
        lacing.clear();
        body.clear();
    }

    bool fits(size_t size) const { return lacing.size() + size / 255 + 1 <= 255; }

    void packet(const uint8_t* data, size_t size) {
        for (size_t left = size; ; left -= 255) {
            lacing.push_back(uint8_t(std::min<size_t>(left, 255)));
            if (left < 255) break;
        }
        body.insert(body.end(), data, data + size);
    }

    bool start() {
        if (spec.config.size() < 19 || memcmp(spec.config.data(), "OpusHead", 8) != 0) return false;
        std::vector<uint8_t> head = spec.config;
        if (spec.skip > 0) {
            int64_t skip = std::min<int64_t>(ticks(spec.skip), 0xFFFF);
            head[10] = uint8_t(skip);
            head[11] = uint8_t(skip >> 8);
        }
        decoded = 0;
        packet(head.data(), head.size());
        page(0, 2);
        const char vendor[] = "absent-media";
        std::vector<uint8_t> tags = {'O', 'p', 'u', 's', 'T', 'a', 'g', 's', uint8_t(sizeof(vendor) - 1), 0, 0, 0};
        tags.insert(tags.end(), vendor, vendor + sizeof(vendor) - 1);
        tags.insert(tags.end(), 4, 0);
        packet(tags.data(), tags.size());
        page(0, 0);
        started = true;
        return true;
    }

    static int64_t samples(const uint8_t* data, size_t size) {
        if (!size) return 0;
        static const int silk[4] = {480, 960, 1920, 2880}, hybrid[2] = {480, 960}, celt[4] = {120, 240, 480, 960};
        int config = data[0] >> 3;
        int one = config < 12 ? silk[config & 3] : config < 16 ? hybrid[config & 1] : celt[config & 3];
        int code = data[0] & 3;
        int count = code == 0 ? 1 : code < 3 ? 2 : size > 1 ? data[1] & 63 : 0;
        return int64_t(one) * count;
    }

    bool write(int track, const uint8_t* data, size_t size, int64_t, int64_t, uint32_t, bool) override {
        if (track != 0 || !added || failed || !file || size / 255 + 1 > 255) return false;
        if (!started && !start()) return false;
        if (!lacing.empty() && (!fits(size) || decoded - pageStart >= kPageSamples)) {
            page(decoded, 0);
            written = decoded;
            pageStart = decoded;
        }
        packet(data, size);
        decoded += samples(data, size);
        return !failed;
    }

    bool finish(std::string* error) override {
        auto fail = [&](const char* text) {
            if (error) *error = text;
            if (file) fclose(file);
            file = nullptr;
            remove(path.c_str());
            return false;
        };
        if (!file || failed) return fail("writing the output file failed");
        if (!started) return fail("nothing was written");
        int64_t end = decoded;
        if (trimmed || spec.length > 0) end = std::clamp<int64_t>(ticks(spec.skip) + ticks(spec.length), written, decoded);
        page(end, 4);
        if (failed) return fail("writing the output file failed");
        bool ok = fclose(file) == 0;
        file = nullptr;
        if (!ok) return fail("writing the output file failed");
        return true;
    }
};

}

std::unique_ptr<Writer> openOggWriter(const std::string& path, std::string* error) {
    auto writer = std::make_unique<OggWriter>();
    writer->path = path;
    writer->file = fopen(path.c_str(), "wb");
    if (!writer->file) {
        if (error) *error = "cannot create output file";
        return nullptr;
    }
    writer->serial = std::random_device{}();
    return writer;
}

}

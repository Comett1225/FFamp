#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace absent {

constexpr uint32_t fourcc(const char (&s)[5]) {
    return (uint32_t(uint8_t(s[0])) << 24) | (uint32_t(uint8_t(s[1])) << 16) | (uint32_t(uint8_t(s[2])) << 8) | uint32_t(uint8_t(s[3]));
}

std::string fourccText(uint32_t value);

enum class Kind { Video, Audio, Other };

struct Sample {
    uint64_t offset = 0;
    uint32_t size = 0;
    int64_t dts = 0;
    int64_t pts = 0;
    uint32_t duration = 0;
    bool sync = false;
};

struct Color {
    bool known = false;
    int primaries = 1;
    int transfer = 1;
    int matrix = 1;
    bool full = false;
};

struct Track {
    uint32_t id = 0;
    Kind kind = Kind::Other;
    uint32_t codec = 0;
    uint32_t timescale = 0;
    int64_t mediaStart = 0;
    int width = 0;
    int height = 0;
    int rotation = 0;
    int sampleRate = 0;
    int channels = 0;
    uint32_t configType = 0;
    std::vector<uint8_t> config;
    Color color;
    std::vector<Sample> samples;
    std::vector<uint32_t> order;
    std::vector<uint32_t> syncs;
    double seconds() const;
    double rate() const;
};

class Mp4Reader {
public:
    static std::unique_ptr<Mp4Reader> open(const std::string& path, std::string* error = nullptr);
    ~Mp4Reader();
    const std::vector<Track>& tracks() const;
    const Track* first(Kind kind) const;
    bool read(const Track& track, uint32_t index, uint8_t* out);

private:
    Mp4Reader();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

struct TrackSpec {
    Kind kind = Kind::Video;
    uint32_t codec = 0;
    uint32_t timescale = 0;
    int width = 0;
    int height = 0;
    int rotation = 0;
    int sampleRate = 0;
    int channels = 0;
    uint32_t configType = 0;
    std::vector<uint8_t> config;
    Color color;
    int64_t skip = 0;
    int64_t length = 0;
};

class Writer {
public:
    static std::unique_ptr<Writer> open(const std::string& path, std::string* error = nullptr);
    virtual ~Writer() = default;
    virtual int add(const TrackSpec& spec) = 0;
    virtual void configure(int track, uint32_t type, const std::vector<uint8_t>& config) = 0;
    virtual bool write(int track, const uint8_t* data, size_t size, int64_t pts, int64_t dts, uint32_t duration, bool sync) = 0;
    virtual bool finish(std::string* error = nullptr) = 0;
};

class Mp4Writer : public Writer {
public:
    static std::unique_ptr<Mp4Writer> open(const std::string& path, std::string* error = nullptr);
    ~Mp4Writer() override;
    int add(const TrackSpec& spec) override;
    void configure(int track, uint32_t type, const std::vector<uint8_t>& config) override;
    bool write(int track, const uint8_t* data, size_t size, int64_t pts, int64_t dts, uint32_t duration, bool sync) override;
    bool finish(std::string* error = nullptr) override;

private:
    Mp4Writer();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

class MkvWriter : public Writer {
public:
    static std::unique_ptr<MkvWriter> open(const std::string& path, std::string* error = nullptr);
    ~MkvWriter() override;
    int add(const TrackSpec& spec) override;
    void configure(int track, uint32_t type, const std::vector<uint8_t>& config) override;
    bool write(int track, const uint8_t* data, size_t size, int64_t pts, int64_t dts, uint32_t duration, bool sync) override;
    bool finish(std::string* error = nullptr) override;

private:
    MkvWriter();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

struct Frame {
    std::shared_ptr<void> native;
    int width = 0;
    int height = 0;
    int64_t pts = 0;
    explicit operator bool() const { return native != nullptr; }
};

bool copyI420(const Frame& frame, std::vector<uint8_t>& out);

class VideoDecoder {
public:
    static std::unique_ptr<VideoDecoder> open(Mp4Reader& file, const Track& track, std::string* error = nullptr);
    ~VideoDecoder();
    uint32_t frames() const;
    Frame frame(uint32_t index);
    uint64_t run(bool async);
    uint64_t fed() const;
    uint64_t restarts() const;

private:
    VideoDecoder();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

struct Packet {
    std::vector<uint8_t> data;
    int64_t pts = 0;
    int64_t dts = 0;
    uint32_t duration = 0;
    bool sync = false;
};

struct EncodeSpec {
    int width = 0;
    int height = 0;
    uint32_t timescale = 0;
    double fps = 30;
    int bitrate = 8000000;
    int keyInterval = 30;
    bool hevc = false;
    bool reorder = true;
    bool bgra = false;
};

class VideoEncoder {
public:
    static std::unique_ptr<VideoEncoder> open(const EncodeSpec& spec, std::string* error = nullptr);
    ~VideoEncoder();
    bool encodeI420(const uint8_t* data, int64_t pts, int64_t duration);
    bool encodeBGRA(const uint8_t* data, int64_t pts, int64_t duration);
    bool encode(const Frame& frame, int64_t pts, int64_t duration);
    bool finish();
    bool take(Packet& out);
    uint32_t configType() const;
    std::vector<uint8_t> config() const;

private:
    VideoEncoder();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

using AudioSink = std::function<void(const uint8_t* data, size_t size)>;

class OpusEncoder {
public:
    static std::unique_ptr<OpusEncoder> open(int channels, int bitrate, std::string* error = nullptr);
    ~OpusEncoder();
    bool encode(const int16_t* pcm, size_t frames, const AudioSink& sink);
    bool finish(const AudioSink& sink);
    std::vector<uint8_t> config() const;
    int priming() const;

private:
    OpusEncoder();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

class AacEncoder {
public:
    static std::unique_ptr<AacEncoder> open(int rate, int channels, int bitrate, std::string* error = nullptr);
    ~AacEncoder();
    bool encode(const int16_t* pcm, size_t frames, const AudioSink& sink);
    bool finish(const AudioSink& sink);
    std::vector<uint8_t> config() const;
    int priming() const;

private:
    AacEncoder();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}

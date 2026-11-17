#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
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
    int bits = 0;
    bool floating = false;
    bool bigEndian = false;
    uint64_t channelMask = 0;
    bool interlaced = false;
    bool alpha = false;
    int64_t mediaEnd = -1;
    int64_t delay = 0;
    uint32_t frameStep = 0;
    std::string language;
    uint32_t configType = 0;
    std::vector<uint8_t> config;
    Color color;
    std::vector<Sample> samples;
    std::vector<uint32_t> order;
    std::vector<uint32_t> syncs;
    double seconds() const;
    double rate() const;
};

class Reader {
public:
    static std::unique_ptr<Reader> open(const std::string& path, std::string* error = nullptr);
    virtual ~Reader() = default;
    virtual const char* format() const = 0;
    virtual const std::vector<Track>& tracks() const = 0;
    virtual bool read(const Track& track, uint32_t index, uint8_t* out) = 0;
    virtual bool readAlpha(const Track& track, uint32_t index, std::vector<uint8_t>& out);
    virtual const std::map<std::string, std::string>& tags() const;
    virtual double statedSeconds() const;
    const Track* first(Kind kind) const;
};

class Mp4Reader : public Reader {
public:
    static std::unique_ptr<Mp4Reader> open(const std::string& path, std::string* error = nullptr);
    ~Mp4Reader() override;
    const char* format() const override;
    const std::vector<Track>& tracks() const override;
    bool read(const Track& track, uint32_t index, uint8_t* out) override;
    const std::map<std::string, std::string>& tags() const override;

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
    Color color{true};
    int64_t skip = 0;
    int64_t length = 0;
    int bits = 0;
    bool floating = false;
    bool bigEndian = false;
    int64_t delay = 0;
};

class Writer {
public:
    static std::unique_ptr<Writer> open(const std::string& path, std::string* error = nullptr);
    virtual ~Writer() = default;
    virtual int add(const TrackSpec& spec) = 0;
    virtual void configure(int track, uint32_t type, const std::vector<uint8_t>& config) = 0;
    virtual bool write(int track, const uint8_t* data, size_t size, int64_t pts, int64_t dts, uint32_t duration, bool sync) = 0;
    virtual bool finish(std::string* error = nullptr) = 0;
    virtual void trim(int, int64_t, int64_t) {}
    virtual void tags(const std::map<std::string, std::string>&) {}
};

enum class Mp4Brand { Mp4, QuickTime, M4a };

class Mp4Writer : public Writer {
public:
    static std::unique_ptr<Mp4Writer> open(const std::string& path, std::string* error = nullptr);
    static std::unique_ptr<Mp4Writer> open(const std::string& path, Mp4Brand brand, std::string* error = nullptr);
    ~Mp4Writer() override;
    void trim(int track, int64_t skip, int64_t length) override;
    int add(const TrackSpec& spec) override;
    void configure(int track, uint32_t type, const std::vector<uint8_t>& config) override;
    bool write(int track, const uint8_t* data, size_t size, int64_t pts, int64_t dts, uint32_t duration, bool sync) override;
    bool finish(std::string* error = nullptr) override;
    void tags(const std::map<std::string, std::string>& values) override;

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
    void tags(const std::map<std::string, std::string>& values) override;
    void trim(int track, int64_t skip, int64_t length) override;

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
    int64_t duration = 0;
    int bits = 8;
    int chromaWidth = 0;
    int chromaHeight = 0;
    Color color;
    explicit operator bool() const { return native != nullptr; }
};

bool copyI420(const Frame& frame, std::vector<uint8_t>& out);
bool copyYuv(const Frame& frame, std::vector<uint8_t>& out);
bool copyBGRA(const Frame& frame, std::vector<uint8_t>& out);
void bgraToI420(const uint8_t* pixels, size_t stride, int width, int height, uint8_t* out, bool redFirst = false);

class VideoDecoder {
public:
    static std::unique_ptr<VideoDecoder> open(Reader& file, const Track& track, std::string* error = nullptr, bool software = false);
    ~VideoDecoder();
    const char* backend() const;
    uint32_t frames() const;
    int64_t pts(uint32_t index) const;
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
    uint32_t codec = 0;
    int speed = -1;
};

class VideoEncoder {
public:
    static std::unique_ptr<VideoEncoder> open(const EncodeSpec& spec, std::string* error = nullptr);
    ~VideoEncoder();
    bool encodeI420(const uint8_t* data, int64_t pts, int64_t duration);
    bool encodeBGRA(const uint8_t* data, int64_t pts, int64_t duration, bool redFirst = false);
    bool encode(const Frame& frame, int64_t pts, int64_t duration);
    bool encodePixelBuffer(void* pixelBuffer, int64_t pts, int64_t duration);
    bool encodeSurface(void* surface, int64_t pts, int64_t duration);
    const char* backend() const;
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

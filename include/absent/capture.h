#pragma once
#include "absent/audio.h"

namespace absent {

enum class PixelLayout { Rgba, Rgb24, I420, Bgra, Gray };

bool pixelLayoutNamed(const std::string& name, PixelLayout& layout);
size_t pixelBytes(PixelLayout layout, int width, int height);

struct FrameReaderSpec {
    int width = 0;
    int height = 0;
    PixelLayout layout = PixelLayout::Rgba;
    bool rotate = true;
    bool software = false;
};

class FrameReader {
public:
    static std::unique_ptr<FrameReader> open(const std::string& path, const FrameReaderSpec& spec, std::string* error = nullptr);
    ~FrameReader();
    int width() const;
    int height() const;
    int sourceWidth() const;
    int sourceHeight() const;
    int rotation() const;
    size_t frameBytes() const;
    uint32_t frames() const;
    double seconds(uint32_t index) const;
    double length() const;
    double rate() const;
    uint32_t indexAt(double seconds) const;
    std::vector<uint32_t> steady(double start, double fps, double length, long count) const;
    bool read(uint32_t index, std::vector<uint8_t>& out);

private:
    FrameReader();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

struct FrameWriterSpec {
    int width = 0;
    int height = 0;
    PixelLayout layout = PixelLayout::Rgba;
    int64_t rateTop = 30;
    int64_t rateBottom = 1;
    double start = 0;
    bool hevc = false;
    int bitrate = 0;
    std::string audioFrom;
    double audioStart = 0;
    double audioLength = -1;
    double limit = -1;
    int audioBitrate = 160000;
};

bool frameRateFrom(const std::string& text, int64_t& top, int64_t& bottom);
int frameWriterBitrate(int width, int height, double fps);

class FrameWriter {
public:
    static std::unique_ptr<FrameWriter> open(const std::string& path, const FrameWriterSpec& spec, std::string* error = nullptr);
    ~FrameWriter();
    size_t frameBytes() const;
    int width() const;
    int height() const;
    bool write(const uint8_t* pixels);
    long frames() const;
    bool finish(std::string* error = nullptr);

private:
    FrameWriter();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

std::vector<float> stretchSound(const std::vector<float>& mono, int rate, double speed);
std::vector<float> shapeSpeech(const std::vector<float>& mono, int rate, double speed, double pitch);
bool convertSpeech(const std::string& in, const std::string& out, int rate, double speed, double pitch, std::string* error = nullptr);

struct CaptureDevice {
    std::string id;
    std::string name;
    bool video = false;
};

enum class CaptureAccess { Authorized, Undetermined, Denied, Restricted };

extern const char* const kTestDevice;

std::vector<CaptureDevice> captureDevices();
CaptureAccess captureAccess(bool video);
const char* captureAccessName(CaptureAccess access);

using PreviewSink = std::function<void(const uint8_t* bgra, int width, int height, double seconds)>;

struct CaptureSpec {
    std::string video;
    std::string audio;
    std::string record;
    int previewWidth = 0;
    int previewHeight = 0;
    double previewFps = 15;
    PreviewSink preview;
    int bitrate = 10000000;
    int audioBitrate = 160000;
    bool ask = false;
};

struct CaptureTotals {
    long videoFrames = 0;
    long audioFrames = 0;
    long previewFrames = 0;
    long silenceAdded = 0;
    long soundDropped = 0;
    int width = 0;
    int height = 0;
    double seconds = 0;
};

class CaptureSession {
public:
    static std::unique_ptr<CaptureSession> open(const CaptureSpec& spec, std::string* error = nullptr);
    ~CaptureSession();
    bool failed(std::string* error = nullptr) const;
    CaptureTotals totals() const;
    bool finish(std::string* error = nullptr);

private:
    CaptureSession();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}

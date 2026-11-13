#pragma once
#include "frames.h"
#include <atomic>
#include <mutex>
#include <thread>

namespace absent {

class CaptureRecorder {
public:
    CaptureRecorder(const CaptureSpec& spec, bool sound);
    void video(const Picture& picture, void* pixelBuffer, double seconds);
    void audio(const int16_t* pcm, size_t frames, int rate, int channels, double seconds);
    void fail(const std::string& text);
    bool failed(std::string* error) const;
    CaptureTotals totals() const;
    bool finish(std::string* error);

private:
    struct Waiting {
        double seconds = 0;
        std::vector<int16_t> pcm;
    };
    void preview(const Picture& picture, double seconds);
    bool begin(const Picture* picture);
    bool drain();
    void sound(const int16_t* pcm, size_t frames, double seconds);
    void packets(const int16_t* pcm, size_t frames);

    CaptureSpec spec;
    bool wantSound = false;
    bool wantPictures = true;
    mutable std::mutex lock;
    std::string problem;
    CaptureTotals sums;
    std::unique_ptr<VideoEncoder> encoder;
    std::unique_ptr<AacEncoder> aac;
    std::unique_ptr<Writer> writer;
    int videoTrack = -1;
    int audioTrack = -1;
    bool configured = false;
    bool started = false;
    bool finished = false;
    double origin = 0;
    double lastVideo = 0;
    double previewDue = -1;
    int rate = 0;
    int channels = 0;
    int64_t soundPackets = 0;
    bool soundSeen = false;
    std::vector<Waiting> waiting;
    std::vector<uint8_t> planar;
    std::vector<uint8_t> shown;
    std::vector<int16_t> quiet;
};

class TestSource {
public:
    TestSource(CaptureRecorder& recorder, bool sound);
    ~TestSource();

private:
    std::atomic<bool> stop{false};
    std::thread pictures;
    std::thread tones;
};

}

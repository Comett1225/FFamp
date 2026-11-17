#pragma once
#include "absent/media.h"

namespace absent {

class AudioDecoder {
public:
    static std::unique_ptr<AudioDecoder> open(Reader& file, const Track& track, std::string* error = nullptr);
    ~AudioDecoder();
    int sampleRate() const;
    int channels() const;
    uint64_t layout() const;
    int64_t length() const;
    bool read(int64_t start, size_t frames, float* out);

private:
    AudioDecoder();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

class Resampler {
public:
    Resampler(int inRate, int inChannels, int outRate, int outChannels);
    ~Resampler();
    void push(const float* in, size_t frames);
    size_t pull(float* out, size_t frames);
    void finish();
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

double mixPeak(int inChannels, int outChannels);

class PcmReader {
public:
    static std::unique_ptr<PcmReader> open(const std::string& path, int rate, int channels, std::string* error = nullptr);
    ~PcmReader();
    int sampleRate() const;
    int channels() const;
    int64_t length() const;
    double mixPeak() const;
    bool read(int64_t start, size_t frames, float* out);

private:
    PcmReader();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

enum class AudioFormat { Wav16, Wav24, WavFloat, Flac, Aac, Opus, Mp3, Flac24 };

class AudioFileWriter {
public:
    static std::unique_ptr<AudioFileWriter> open(const std::string& path, AudioFormat format, int rate, int channels, int bitrate, std::string* error = nullptr);
    ~AudioFileWriter();
    bool write(const float* pcm, size_t frames);
    bool finish(std::string* error = nullptr);
    void alwaysRf64();

private:
    AudioFileWriter();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}

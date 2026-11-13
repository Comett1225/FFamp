#include "absent/audio.h"
#include "flac_encode.h"
#include "writers.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace absent {

namespace {

int32_t toFixed(float value) {
    float scaled = value * 2147483648.0f;
    if (scaled != scaled) return 0;
    if (scaled >= 2147483648.0f) return INT32_MAX;
    if (scaled <= -2147483648.0f) return INT32_MIN;
    return int32_t(scaled);
}

int16_t toShort(float value) { return int16_t(std::min<int64_t>((int64_t(toFixed(value)) + 0x8000) >> 16, 32767)); }

int32_t toDeep(float value) { return toFixed(value) >> 8; }

struct Target {
    virtual ~Target() = default;
    virtual bool write(const float* pcm, size_t frames) = 0;
    virtual bool finish(std::string* error) = 0;
    virtual void wide() {}
};

struct Little {
    std::vector<uint8_t> d;
    void text(const char* four) { d.insert(d.end(), four, four + 4); }
    void u16(uint32_t v) { d.push_back(uint8_t(v)); d.push_back(uint8_t(v >> 8)); }
    void u32(uint32_t v) { u16(v); u16(v >> 16); }
    void u64(uint64_t v) { u32(uint32_t(v)); u32(uint32_t(v >> 32)); }
};

bool problem(std::string* error, const char* text) {
    if (error) *error = text;
    return false;
}

struct WaveTarget : Target {
    std::string path;
    FILE* file = nullptr;
    int rate = 0;
    int channels = 0;
    int bits = 0;
    bool floating = false;
    bool forced = false;
    bool failed = false;
    uint64_t frames = 0;
    std::vector<uint8_t> packed;

    ~WaveTarget() override {
        if (file) {
            fclose(file);
            remove(path.c_str());
        }
    }

    void wide() override { forced = true; }

    std::vector<uint8_t> header() const {
        const uint32_t frameBytes = uint32_t(channels * bits / 8);
        const uint64_t dataBytes = frames * frameBytes;
        const bool extensible = channels > 2 || (!floating && bits > 16);
        const uint32_t formatBytes = extensible ? 40 : floating ? 18 : 16;
        const uint64_t headBytes = 12 + 36 + 8 + formatBytes + (floating ? 12 : 0) + 8;
        const uint64_t riffBytes = headBytes - 8 + dataBytes + (dataBytes & 1);
        const bool large = forced || riffBytes > 0xFFFFFFFFull;
        static const uint32_t masks[9] = {0, 0x4, 0x3, 0x7, 0x33, 0x37, 0x3F, 0x13F, 0x63F};
        Little h;
        h.text(large ? "RF64" : "RIFF");
        h.u32(large ? 0xFFFFFFFFu : uint32_t(riffBytes));
        h.text("WAVE");
        h.text(large ? "ds64" : "JUNK");
        h.u32(28);
        h.u64(large ? riffBytes : 0);
        h.u64(large ? dataBytes : 0);
        h.u64(large ? frames : 0);
        h.u32(0);
        h.text("fmt ");
        h.u32(formatBytes);
        h.u16(extensible ? 0xFFFE : floating ? 3 : 1);
        h.u16(uint32_t(channels));
        h.u32(uint32_t(rate));
        h.u32(uint32_t(rate) * frameBytes);
        h.u16(frameBytes);
        h.u16(uint32_t(bits));
        if (extensible) {
            h.u16(22);
            h.u16(uint32_t(bits));
            h.u32(channels <= 8 ? masks[channels] : 0);
            h.u16(floating ? 3 : 1);
            static const uint8_t tail[14] = {0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71};
            h.d.insert(h.d.end(), tail, tail + 14);
        } else if (floating) {
            h.u16(0);
        }
        if (floating) {
            h.text("fact");
            h.u32(4);
            h.u32(large || frames > 0xFFFFFFFFull ? 0xFFFFFFFFu : uint32_t(frames));
        }
        h.text("data");
        h.u32(large ? 0xFFFFFFFFu : uint32_t(dataBytes));
        return h.d;
    }

    bool write(const float* pcm, size_t count) override {
        if (failed) return false;
        const size_t values = count * size_t(channels);
        const size_t width = size_t(bits / 8);
        packed.resize(values * width);
        if (floating) {
            memcpy(packed.data(), pcm, values * 4);
        } else if (bits == 16) {
            for (size_t i = 0; i < values; ++i) {
                int16_t v = toShort(pcm[i]);
                packed[i * 2] = uint8_t(v);
                packed[i * 2 + 1] = uint8_t(v >> 8);
            }
        } else {
            for (size_t i = 0; i < values; ++i) {
                int32_t v = toDeep(pcm[i]);
                packed[i * 3] = uint8_t(v);
                packed[i * 3 + 1] = uint8_t(v >> 8);
                packed[i * 3 + 2] = uint8_t(v >> 16);
            }
        }
        failed = fwrite(packed.data(), 1, packed.size(), file) != packed.size();
        frames += count;
        return !failed;
    }

    bool finish(std::string* error) override {
        if ((frames * uint64_t(channels * bits / 8)) & 1) failed = failed || fputc(0, file) == EOF;
        std::vector<uint8_t> head = header();
        failed = failed || fseeko(file, 0, SEEK_SET) != 0 || fwrite(head.data(), 1, head.size(), file) != head.size();
        failed = fclose(file) != 0 || failed;
        file = nullptr;
        if (failed) {
            remove(path.c_str());
            return problem(error, "writing the output file failed");
        }
        return true;
    }
};

struct FlacTarget : Target {
    static constexpr int kSeekPoints = 100;
    struct Point {
        uint64_t sample;
        uint64_t offset;
        uint32_t frames;
    };
    std::string path;
    FILE* file = nullptr;
    int channels = 0;
    int bits = 0;
    bool failed = false;
    std::unique_ptr<FlacEncoder> encoder;
    std::vector<int32_t> whole;
    std::vector<Point> points;
    uint64_t sample = 0;
    uint64_t offset = 0;
    FlacSink sink = [this](const uint8_t* data, size_t size, uint32_t count) {
        points.push_back({sample, offset, count});
        failed = failed || fwrite(data, 1, size, file) != size;
        sample += count;
        offset += size;
    };

    ~FlacTarget() override {
        if (file) {
            fclose(file);
            remove(path.c_str());
        }
    }

    std::vector<uint8_t> head() const {
        std::vector<uint8_t> h = {'f', 'L', 'a', 'C', 0, 0, 0, 34};
        std::vector<uint8_t> info = encoder->streamInfo();
        h.insert(h.end(), info.begin(), info.end());
        const uint32_t tableBytes = kSeekPoints * 18;
        h.insert(h.end(), {3, uint8_t(tableBytes >> 16), uint8_t(tableBytes >> 8), uint8_t(tableBytes)});
        const size_t stride = (points.size() + kSeekPoints - 1) / kSeekPoints;
        for (size_t i = 0; i < size_t(kSeekPoints); ++i) {
            const bool real = stride && i * stride < points.size();
            const Point point = real ? points[i * stride] : Point{~0ull, 0, 0};
            for (int k = 7; k >= 0; --k) h.push_back(uint8_t(point.sample >> (8 * k)));
            for (int k = 7; k >= 0; --k) h.push_back(uint8_t(point.offset >> (8 * k)));
            h.push_back(uint8_t(point.frames >> 8));
            h.push_back(uint8_t(point.frames));
        }
        const char vendor[] = "absent-media";
        const uint32_t n = sizeof(vendor) - 1;
        h.insert(h.end(), {0x84, 0, 0, uint8_t(8 + n), uint8_t(n), 0, 0, 0});
        h.insert(h.end(), vendor, vendor + n);
        h.insert(h.end(), 4, 0);
        return h;
    }

    bool write(const float* pcm, size_t count) override {
        if (failed) return false;
        const size_t values = count * size_t(channels);
        whole.resize(values);
        if (bits == 16) for (size_t i = 0; i < values; ++i) whole[i] = toShort(pcm[i]);
        else for (size_t i = 0; i < values; ++i) whole[i] = toDeep(pcm[i]);
        encoder->encode(whole.data(), count, sink);
        return !failed;
    }

    bool finish(std::string* error) override {
        encoder->finish(sink);
        std::vector<uint8_t> h = head();
        failed = failed || fseeko(file, 0, SEEK_SET) != 0 || fwrite(h.data(), 1, h.size(), file) != h.size();
        failed = fclose(file) != 0 || failed;
        file = nullptr;
        if (failed) {
            remove(path.c_str());
            return problem(error, "writing the output file failed");
        }
        return true;
    }
};

template <class Encoder>
struct PacketTarget : Target {
    std::unique_ptr<Writer> writer;
    std::unique_ptr<Encoder> encoder;
    int channels = 0;
    int track = 0;
    uint32_t step = 0;
    int64_t time = 0;
    int64_t fed = 0;
    bool failed = false;
    std::vector<int16_t> whole;
    AudioSink sink = [this](const uint8_t* data, size_t size) {
        failed = failed || !writer->write(track, data, size, time, time, step, true);
        time += step;
    };

    bool write(const float* pcm, size_t count) override {
        if (failed) return false;
        whole.resize(count * size_t(channels));
        for (size_t i = 0; i < whole.size(); ++i) whole[i] = toShort(pcm[i]);
        fed += int64_t(count);
        return encoder->encode(whole.data(), count, sink) && !failed;
    }

    bool finish(std::string* error) override {
        if (!encoder->finish(sink) || failed) return problem(error, "encoding the sound failed");
        writer->trim(track, encoder->priming(), fed);
        return writer->finish(error);
    }
};

struct Mp3Target : Target {
    std::unique_ptr<Mp3File> file;
    bool write(const float* pcm, size_t count) override { return file->write(pcm, count); }
    bool finish(std::string* error) override { return file->finish() || problem(error, "writing the output file failed"); }
};

template <class Encoder>
std::unique_ptr<Target> packets(std::unique_ptr<Writer> writer, std::unique_ptr<Encoder> encoder, uint32_t codec, uint32_t configType, int rate, int channels, uint32_t step) {
    if (!writer || !encoder) return nullptr;
    auto target = std::make_unique<PacketTarget<Encoder>>();
    TrackSpec spec;
    spec.kind = Kind::Audio;
    spec.codec = codec;
    spec.timescale = uint32_t(rate);
    spec.sampleRate = rate;
    spec.channels = channels;
    spec.configType = configType;
    spec.config = encoder->config();
    spec.skip = encoder->priming();
    target->track = writer->add(spec);
    target->writer = std::move(writer);
    target->encoder = std::move(encoder);
    target->channels = channels;
    target->step = step;
    return target;
}

}

#ifndef ABSENT_LAME
std::unique_ptr<Mp3File> Mp3File::open(const std::string&, int, int, int, std::string* error) {
    if (error) *error = "this build has no MP3 encoder";
    return nullptr;
}
#endif

struct AudioFileWriter::Impl {
    std::unique_ptr<Target> target;
};

AudioFileWriter::AudioFileWriter() : impl(new Impl) {}
AudioFileWriter::~AudioFileWriter() = default;

std::unique_ptr<AudioFileWriter> AudioFileWriter::open(const std::string& path, AudioFormat format, int rate, int channels, int bitrate, std::string* error) {
    auto fail = [&](const char* text) {
        if (error && (text[0] || error->empty())) *error = text[0] ? text : "the sound file could not be started";
        return nullptr;
    };
    if (rate < 1 || rate > 655350 || channels < 1 || channels > 8) return fail("audio layout not supported");
    if (bitrate <= 0) bitrate = 128000;
    std::unique_ptr<AudioFileWriter> writer(new AudioFileWriter);
    if (format == AudioFormat::Wav16 || format == AudioFormat::Wav24 || format == AudioFormat::WavFloat) {
        auto wave = std::make_unique<WaveTarget>();
        wave->path = path;
        wave->rate = rate;
        wave->channels = channels;
        wave->bits = format == AudioFormat::Wav16 ? 16 : format == AudioFormat::Wav24 ? 24 : 32;
        wave->floating = format == AudioFormat::WavFloat;
        wave->file = fopen(path.c_str(), "wb");
        std::vector<uint8_t> head = wave->header();
        if (!wave->file || fwrite(head.data(), 1, head.size(), wave->file) != head.size()) return fail("cannot create output file");
        writer->impl->target = std::move(wave);
    } else if (format == AudioFormat::Flac || format == AudioFormat::Flac24) {
        auto flac = std::make_unique<FlacTarget>();
        flac->path = path;
        flac->channels = channels;
        flac->bits = format == AudioFormat::Flac ? 16 : 24;
        flac->encoder = std::make_unique<FlacEncoder>(rate, channels, flac->bits);
        flac->file = fopen(path.c_str(), "wb");
        std::vector<uint8_t> head = flac->head();
        if (!flac->file || fwrite(head.data(), 1, head.size(), flac->file) != head.size()) return fail("cannot create output file");
        writer->impl->target = std::move(flac);
    } else if (format == AudioFormat::Aac) {
        auto encoder = AacEncoder::open(rate, channels, bitrate, error);
        if (!encoder) return fail("");
        writer->impl->target = packets<AacEncoder>(Mp4Writer::open(path, Mp4Brand::M4a, error), std::move(encoder), fourcc("mp4a"), fourcc("esds"), rate, channels, 1024);
    } else if (format == AudioFormat::Opus) {
#ifdef ABSENT_OPUS
        if (rate != 48000) return fail("Opus sound is written at 48000 Hz only");
        auto encoder = OpusEncoder::open(channels, bitrate, error);
        if (!encoder) return fail("");
        writer->impl->target = packets<OpusEncoder>(openOggWriter(path, error), std::move(encoder), fourcc("Opus"), fourcc("OpHd"), rate, channels, 960);
#else
        return fail("this build has no Opus encoder");
#endif
    } else {
        auto mp3 = std::make_unique<Mp3Target>();
        mp3->file = Mp3File::open(path, rate, channels, bitrate, error);
        if (!mp3->file) return fail("");
        writer->impl->target = std::move(mp3);
    }
    if (!writer->impl->target) return fail("");
    return writer;
}

bool AudioFileWriter::write(const float* pcm, size_t frames) { return impl->target->write(pcm, frames); }

bool AudioFileWriter::finish(std::string* error) { return impl->target->finish(error); }

void AudioFileWriter::alwaysRf64() { impl->target->wide(); }

}

#include "frames.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

namespace absent {

namespace {

const int kAacRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000};

void padPlane(const uint8_t* in, int width, int height, size_t unit, int evenWidth, int evenHeight, uint8_t* out) {
    for (int y = 0; y < evenHeight; ++y) {
        const uint8_t* from = in + size_t(std::min(y, height - 1)) * size_t(width) * unit;
        uint8_t* to = out + size_t(y) * size_t(evenWidth) * unit;
        memcpy(to, from, size_t(width) * unit);
        if (evenWidth > width) memcpy(to + size_t(width) * unit, from + size_t(width - 1) * unit, unit);
    }
}

}

bool frameRateFrom(const std::string& text, int64_t& top, int64_t& bottom) {
    long long a = 0, b = 1;
    if (text.find('/') != std::string::npos) {
        if (sscanf(text.c_str(), "%lld/%lld", &a, &b) != 2) return false;
    } else {
        const double value = atof(text.c_str());
        if (!(value > 0) || value > 100000) return false;
        const double whole = std::round(value), film = std::round(value * 1.001);
        if (std::fabs(value - whole) < 1e-6) {
            a = (long long)whole;
        } else if (std::fabs(value - film / 1.001) < 2e-4) {
            a = (long long)film * 1000;
            b = 1001;
        } else {
            a = std::llround(value * 1000000);
            b = 1000000;
        }
    }
    if (a <= 0 || b <= 0) return false;
    const long long shared = std::gcd(a, b);
    a /= shared;
    b /= shared;
    if (a > 0x7FFFFFFF || b > 0x7FFFFFFF || double(a) / double(b) > 100000) return false;
    top = a;
    bottom = b;
    return true;
}

int frameWriterBitrate(int width, int height, double fps) {
    const double bits = double(width) * double(height) * std::clamp(fps, 1.0, 240.0) * 0.8;
    return int(std::clamp(bits, 400000.0, 400000000.0));
}

struct FrameWriter::Impl {
    FrameWriterSpec spec;
    std::unique_ptr<VideoEncoder> encoder;
    std::unique_ptr<Writer> writer;
    int videoTrack = -1;
    int audioTrack = -1;
    uint32_t timescale = 0;
    int64_t tick = 0;
    int64_t startTicks = 0;
    long count = 0;
    bool configured = false;
    bool failed = false;
    std::vector<uint8_t> planar;
    std::vector<uint8_t> even;
    int width = 0;
    int height = 0;

    const uint8_t* evened(const uint8_t* pixels) {
        if (width == spec.width && height == spec.height) return pixels;
        even.resize(pixelBytes(spec.layout, width, height));
        if (spec.layout != PixelLayout::I420) {
            padPlane(pixels, spec.width, spec.height, pixelBytes(spec.layout, 1, 1), width, height, even.data());
            return even.data();
        }
        const size_t luma = size_t(spec.width) * size_t(spec.height), chroma = size_t(width / 2) * size_t(height / 2);
        padPlane(pixels, spec.width, spec.height, 1, width, height, even.data());
        memcpy(even.data() + size_t(width) * size_t(height), pixels + luma, 2 * chroma);
        return even.data();
    }

    std::unique_ptr<Reader> soundFile;
    const Track* soundTrack = nullptr;
    uint32_t nextPacket = 0;
    int64_t copySkip = 0;
    int64_t copied = 0;
    std::unique_ptr<PcmReader> pcm;
    std::unique_ptr<AacEncoder> aac;
    int rate = 0;
    int channels = 0;
    int64_t soundFrom = 0;
    int64_t soundTotal = 0;
    int64_t soundDone = 0;
    int64_t soundPackets = 0;
    std::vector<float> floats;
    std::vector<int16_t> shorts;
    std::vector<uint8_t> packet;

    bool soundOpen(std::string* error) {
        soundFile = Reader::open(spec.audioFrom, error);
        if (!soundFile) return false;
        soundTrack = soundFile->first(Kind::Audio);
        if (!soundTrack || soundTrack->samples.empty() || !soundTrack->timescale) {
            soundFile.reset();
            soundTrack = nullptr;
            return true;
        }
        const Track& t = *soundTrack;
        TrackSpec sound;
        sound.kind = Kind::Audio;
        if (t.codec == fourcc("mp4a") && spec.audioStart <= 0 && t.sampleRate > 0) {
            int64_t first = t.samples.front().pts;
            for (const Sample& s : t.samples) first = std::min(first, s.pts);
            copySkip = std::max<int64_t>(0, t.mediaStart - first);
            sound.codec = t.codec;
            sound.timescale = t.timescale;
            sound.sampleRate = t.sampleRate;
            sound.channels = t.channels;
            sound.configType = t.configType;
            sound.config = t.config;
            sound.skip = copySkip;
            audioTrack = writer->add(sound);
            return true;
        }
        rate = 48000;
        for (int known : kAacRates) if (known == t.sampleRate) rate = known;
        channels = t.channels == 1 ? 1 : 2;
        soundTrack = nullptr;
        soundFile.reset();
        pcm = PcmReader::open(spec.audioFrom, rate, channels, error);
        if (!pcm) return false;
        aac = AacEncoder::open(rate, channels, spec.audioBitrate, error);
        if (!aac) return false;
        soundFrom = std::llround(std::max(spec.audioStart, 0.0) * rate);
        soundTotal = std::max<int64_t>(pcm->length() - soundFrom, 0);
        if (spec.audioLength >= 0) soundTotal = std::min<int64_t>(soundTotal, std::llround(spec.audioLength * rate));
        sound.codec = fourcc("mp4a");
        sound.timescale = uint32_t(rate);
        sound.sampleRate = rate;
        sound.channels = channels;
        sound.configType = fourcc("esds");
        sound.config = aac->config();
        sound.skip = aac->priming();
        audioTrack = writer->add(sound);
        return true;
    }

    double soundLimit(double seconds) const {
        double limit = seconds;
        if (spec.limit >= 0) limit = std::min(limit, spec.limit);
        if (soundTrack && spec.audioLength >= 0) limit = std::min(limit, spec.audioLength);
        return limit;
    }

    bool soundUntil(double seconds) {
        if (audioTrack < 0) return true;
        const double limit = soundLimit(seconds);
        if (soundTrack) {
            const Track& t = *soundTrack;
            const int64_t end = t.mediaStart + std::llround(limit * t.timescale);
            while (nextPacket < t.samples.size()) {
                const Sample& s = t.samples[nextPacket];
                if (s.pts >= end || (t.mediaEnd > t.mediaStart && s.pts >= t.mediaEnd)) break;
                packet.resize(s.size);
                if (!soundFile->read(t, nextPacket, packet.data())) return false;
                if (!writer->write(audioTrack, packet.data(), s.size, s.pts, s.dts, s.duration, true)) return false;
                copied = std::max(copied, s.pts + int64_t(s.duration));
                ++nextPacket;
            }
            return true;
        }
        const int64_t target = std::min<int64_t>(soundTotal, std::llround(limit * rate));
        AudioSink sink = [&](const uint8_t* data, size_t size) {
            if (!writer->write(audioTrack, data, size, soundPackets * 1024, soundPackets * 1024, 1024, true)) failed = true;
            ++soundPackets;
        };
        while (soundDone < target) {
            const size_t frames = size_t(std::min<int64_t>(target - soundDone, 8192));
            floats.resize(frames * size_t(channels));
            shorts.resize(floats.size());
            if (!pcm->read(soundFrom + soundDone, frames, floats.data())) return false;
            for (size_t i = 0; i < floats.size(); ++i) shorts[i] = int16_t(std::lround(std::clamp(floats[i], -1.0f, 1.0f) * 32767.0f));
            if (!aac->encode(shorts.data(), frames, sink)) return false;
            soundDone += int64_t(frames);
        }
        return !failed;
    }

    bool soundFinish(double seconds) {
        if (audioTrack < 0) return true;
        if (!soundUntil(seconds)) return false;
        if (soundTrack) {
            const Track& t = *soundTrack;
            int64_t length = std::min<int64_t>(copied - t.mediaStart, std::llround(soundLimit(seconds) * t.timescale));
            if (t.mediaEnd > t.mediaStart) length = std::min(length, t.mediaEnd - t.mediaStart);
            writer->trim(audioTrack, copySkip, std::max<int64_t>(length, 0));
            return true;
        }
        AudioSink sink = [&](const uint8_t* data, size_t size) {
            if (!writer->write(audioTrack, data, size, soundPackets * 1024, soundPackets * 1024, 1024, true)) failed = true;
            ++soundPackets;
        };
        if (!aac->finish(sink) || failed) return false;
        writer->trim(audioTrack, aac->priming(), soundDone);
        return true;
    }

    bool drain() {
        Packet packet;
        while (encoder->take(packet)) {
            if (!configured) {
                writer->configure(videoTrack, encoder->configType(), encoder->config());
                configured = true;
            }
            if (!writer->write(videoTrack, packet.data.data(), packet.data.size(), packet.pts, packet.dts, packet.duration ? packet.duration : uint32_t(tick), packet.sync)) return false;
        }
        return true;
    }

    int64_t timeOf(long frame) const { return startTicks > 0 ? (frame ? startTicks + (frame - 1) * tick : 0) : frame * tick; }
    double endOf(long frames) const { return double(timeOf(frames)) / timescale; }
};

FrameWriter::FrameWriter() : impl(new Impl) {}
FrameWriter::~FrameWriter() = default;

std::unique_ptr<FrameWriter> FrameWriter::open(const std::string& path, const FrameWriterSpec& spec, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    if (spec.width <= 0 || spec.height <= 0 || spec.width > 16384 || spec.height > 16384) return fail("bad frame size");
    if (spec.rateTop <= 0 || spec.rateBottom <= 0 || spec.rateTop > 0x7FFFFFFF || spec.rateBottom > 0x7FFFFFFF) return fail("bad frame rate");
    if (!(spec.start >= 0) || spec.start > 86400) return fail("bad start time");
    std::unique_ptr<FrameWriter> out(new FrameWriter);
    Impl& w = *out->impl;
    w.spec = spec;
    int64_t scale = 1;
    while (spec.rateTop * scale < 1000 || (spec.start > 0 && spec.rateTop * scale < 100000 && spec.rateBottom * scale * 10 < 0x7FFFFFFF)) scale *= 10;
    if (spec.rateTop * scale > 0x7FFFFFFF || spec.rateBottom * scale > 0x7FFFFFFF) return fail("bad frame rate");
    w.timescale = uint32_t(spec.rateTop * scale);
    w.tick = spec.rateBottom * scale;
    w.startTicks = std::llround(spec.start * w.timescale);
    const double fps = double(spec.rateTop) / double(spec.rateBottom);
    EncodeSpec video;
    w.width = (spec.width + 1) / 2 * 2;
    w.height = (spec.height + 1) / 2 * 2;
    video.width = w.width;
    video.height = w.height;
    video.timescale = w.timescale;
    video.fps = fps;
    video.hevc = spec.hevc;
    video.bitrate = spec.bitrate > 0 ? spec.bitrate : frameWriterBitrate(w.width, w.height, fps);
    video.keyInterval = std::max(1, int(fps + 0.5));
    video.bgra = spec.layout != PixelLayout::I420 && spec.layout != PixelLayout::Gray;
    w.encoder = VideoEncoder::open(video, error);
    if (!w.encoder) return nullptr;
    w.writer = Writer::open(path, error);
    if (!w.writer) return nullptr;
    TrackSpec picture;
    picture.kind = Kind::Video;
    picture.codec = spec.hevc ? fourcc("hvc1") : fourcc("avc1");
    picture.timescale = w.timescale;
    picture.width = w.width;
    picture.height = w.height;
    w.videoTrack = w.writer->add(picture);
    if (!spec.audioFrom.empty() && !w.soundOpen(error)) {
        w.writer.reset();
        remove(path.c_str());
        return nullptr;
    }
    return out;
}

size_t FrameWriter::frameBytes() const { return pixelBytes(impl->spec.layout, impl->spec.width, impl->spec.height); }
int FrameWriter::width() const { return impl->width; }
int FrameWriter::height() const { return impl->height; }
long FrameWriter::frames() const { return impl->count; }

bool FrameWriter::write(const uint8_t* pixels) {
    Impl& w = *impl;
    if (w.failed || !pixels) return false;
    const int64_t pts = w.timeOf(w.count);
    const int64_t duration = w.timeOf(w.count + 1) - pts;
    if (w.spec.limit >= 0 && double(pts) / w.timescale >= w.spec.limit - 1e-9) return true;
    pixels = w.evened(pixels);
    bool ok;
    if (w.spec.layout == PixelLayout::Rgba || w.spec.layout == PixelLayout::Bgra) {
        ok = w.encoder->encodeBGRA(pixels, pts, duration, w.spec.layout == PixelLayout::Rgba);
    } else if (w.spec.layout == PixelLayout::Rgb24) {
        const size_t area = size_t(w.width) * size_t(w.height);
        w.planar.resize(area * 4);
        for (size_t i = 0; i < area; ++i) {
            w.planar[i * 4] = pixels[i * 3 + 2];
            w.planar[i * 4 + 1] = pixels[i * 3 + 1];
            w.planar[i * 4 + 2] = pixels[i * 3];
            w.planar[i * 4 + 3] = 255;
        }
        ok = w.encoder->encodeBGRA(w.planar.data(), pts, duration, false);
    } else {
        layoutToI420(pixels, w.width, w.height, w.spec.layout, w.planar);
        ok = w.encoder->encodeI420(w.planar.data(), pts, duration);
    }
    ++w.count;
    if (!ok || !w.drain() || !w.soundUntil(w.endOf(w.count))) w.failed = true;
    return !w.failed;
}

bool FrameWriter::finish(std::string* error) {
    Impl& w = *impl;
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return false;
    };
    if (w.failed) return fail("writing the frames failed");
    if (!w.count) return fail("no frames were written");
    if (!w.encoder->finish() || !w.drain()) return fail("the video encoder failed");
    if (!w.soundFinish(w.endOf(w.count))) return fail("the sound could not be added");
    return w.writer->finish(error);
}

}

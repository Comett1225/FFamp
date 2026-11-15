#include "capture.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include "portable.h"

namespace absent {

namespace {

const uint32_t kClock = 90000;
const double kNominalFrame = 1.0 / 30;
const double kSlack = 0.020;
const size_t kWaitingLimit = 400;

}

const char* const kTestDevice = "test";

const char* captureAccessName(CaptureAccess access) {
    switch (access) {
        case CaptureAccess::Authorized: return "authorized";
        case CaptureAccess::Undetermined: return "undetermined";
        case CaptureAccess::Denied: return "denied";
        default: return "restricted";
    }
}

CaptureRecorder::CaptureRecorder(const CaptureSpec& given, bool sound) : spec(given), wantSound(sound && !given.record.empty()), wantPictures(!given.video.empty()) {}

void CaptureRecorder::fail(const std::string& text) {
    std::lock_guard<std::mutex> hold(lock);
    if (problem.empty()) problem = text;
}

bool CaptureRecorder::failed(std::string* error) const {
    std::lock_guard<std::mutex> hold(lock);
    if (problem.empty()) return false;
    if (error) *error = problem;
    return true;
}

CaptureTotals CaptureRecorder::totals() const {
    std::lock_guard<std::mutex> hold(lock);
    return sums;
}

void CaptureRecorder::preview(const Picture& picture, double seconds) {
    if (!spec.preview) return;
    const double interval = 1.0 / std::clamp(spec.previewFps, 0.1, 240.0);
    if (previewDue >= 0 && seconds + 1e-4 < previewDue) return;
    previewDue = previewDue < 0 || previewDue + interval < seconds ? seconds + interval : previewDue + interval;
    int width = spec.previewWidth, height = spec.previewHeight;
    if (width <= 0 || height <= 0) {
        width = std::min(picture.width, 1920);
        height = width == picture.width ? picture.height : std::max(2, int(std::lround(double(picture.height) * width / picture.width / 2)) * 2);
    }
    if (!pictureToLayout(picture, width, height, PixelLayout::Bgra, shown)) return;
    ++sums.previewFrames;
    spec.preview(shown.data(), width, height, seconds);
}

bool CaptureRecorder::begin(const Picture* picture) {
    std::string error;
    if (picture) {
        EncodeSpec video;
        video.width = picture->width;
        video.height = picture->height;
        video.timescale = kClock;
        video.fps = 30;
        video.bitrate = spec.bitrate;
        video.keyInterval = 30;
        video.reorder = false;
        encoder = VideoEncoder::open(video, &error);
    }
    if (encoder || !picture) writer = Writer::open(spec.record, &error);
    if (!writer) {
        problem = error;
        return false;
    }
    if (picture) {
        TrackSpec track;
        track.kind = Kind::Video;
        track.codec = fourcc("avc1");
        track.timescale = kClock;
        track.width = picture->width;
        track.height = picture->height;
        videoTrack = writer->add(track);
        sums.width = picture->width;
        sums.height = picture->height;
    }
    if (!wantSound) return true;
    aac = AacEncoder::open(rate, channels, spec.audioBitrate, &error);
    if (!aac) {
        problem = error;
        return false;
    }
    TrackSpec voice;
    voice.kind = Kind::Audio;
    voice.codec = fourcc("mp4a");
    voice.timescale = uint32_t(rate);
    voice.sampleRate = rate;
    voice.channels = channels;
    voice.configType = fourcc("esds");
    voice.config = aac->config();
    voice.skip = aac->priming();
    audioTrack = writer->add(voice);
    return true;
}

bool CaptureRecorder::drain() {
    Packet packet;
    while (encoder->take(packet)) {
        if (!configured) {
            writer->configure(videoTrack, encoder->configType(), encoder->config());
            configured = true;
        }
        if (!writer->write(videoTrack, packet.data.data(), packet.data.size(), packet.pts, packet.dts, packet.duration ? packet.duration : uint32_t(kClock * kNominalFrame), packet.sync)) return false;
    }
    return true;
}

void CaptureRecorder::video(const Picture& picture, void* pixelBuffer, double seconds) {
    std::lock_guard<std::mutex> hold(lock);
    if (!problem.empty() || finished) return;
    preview(picture, seconds);
    if (spec.record.empty()) {
        sums.width = picture.width;
        sums.height = picture.height;
        return;
    }
    if (!started) {
        if (wantSound && !soundSeen) return;
        if (!begin(&picture)) return;
        started = true;
        origin = seconds;
        std::vector<Waiting> held;
        held.swap(waiting);
        for (const Waiting& piece : held) sound(piece.pcm.data(), piece.pcm.size() / size_t(channels), piece.seconds);
    }
    if (picture.width != sums.width || picture.height != sums.height) {
        problem = "the camera changed its picture size while recording";
        return;
    }
    const int64_t pts = std::llround((seconds - origin) * kClock);
    if (sums.videoFrames && seconds <= lastVideo) return;
    const int64_t duration = std::llround(kClock * kNominalFrame);
    bool ok;
    if (pixelBuffer) {
        ok = encoder->encodePixelBuffer(pixelBuffer, pts, duration);
    } else {
        pictureToI420(picture, planar);
        ok = encoder->encodeI420(planar.data(), pts, duration);
    }
    if (!ok || !drain()) {
        problem = "the video encoder refused a frame";
        return;
    }
    lastVideo = seconds;
    ++sums.videoFrames;
    sums.seconds = seconds - origin + kNominalFrame;
}

void CaptureRecorder::packets(const int16_t* pcm, size_t frames) {
    AudioSink sink = [&](const uint8_t* data, size_t size) {
        if (!writer->write(audioTrack, data, size, soundPackets * 1024, soundPackets * 1024, 1024, true)) problem = "writing the sound failed";
        ++soundPackets;
    };
    if (!aac->encode(pcm, frames, sink) && problem.empty()) problem = "the audio encoder failed";
    sums.audioFrames += long(frames);
}

void CaptureRecorder::sound(const int16_t* pcm, size_t frames, double seconds) {
    const int64_t place = std::llround((seconds - origin) * rate);
    const int64_t gap = place - sums.audioFrames;
    const int64_t slack = std::llround(kSlack * rate);
    const bool opening = sums.audioFrames == 0;
    if (gap > slack || (opening && gap > 0)) {
        quiet.assign(size_t(std::min<int64_t>(gap, rate)) * size_t(channels), 0);
        for (int64_t left = gap; left > 0;) {
            const int64_t part = std::min<int64_t>(left, rate);
            packets(quiet.data(), size_t(part));
            left -= part;
        }
        sums.silenceAdded += long(gap);
    } else if (gap < -slack || (opening && gap < 0)) {
        const size_t drop = size_t(std::min<int64_t>(-gap, int64_t(frames)));
        pcm += drop * size_t(channels);
        frames -= drop;
        sums.soundDropped += long(drop);
    }
    if (frames) packets(pcm, frames);
}

void CaptureRecorder::audio(const int16_t* pcm, size_t frames, int givenRate, int givenChannels, double seconds) {
    std::lock_guard<std::mutex> hold(lock);
    if (!problem.empty() || finished || !wantSound || !frames) return;
    if (!soundSeen) {
        rate = givenRate;
        channels = givenChannels;
        soundSeen = true;
    }
    if (givenRate != rate || givenChannels != channels) {
        problem = "the microphone changed its format while recording";
        return;
    }
    if (!started && !wantPictures) {
        if (!begin(nullptr)) return;
        started = true;
        origin = seconds;
    }
    if (started) {
        sound(pcm, frames, seconds);
        if (!wantPictures) sums.seconds = double(sums.audioFrames) / rate;
        return;
    }
    if (waiting.size() >= kWaitingLimit) waiting.erase(waiting.begin());
    Waiting piece;
    piece.seconds = seconds;
    piece.pcm.assign(pcm, pcm + frames * size_t(channels));
    waiting.push_back(std::move(piece));
}

bool CaptureRecorder::finish(std::string* error) {
    std::lock_guard<std::mutex> hold(lock);
    auto fail = [&](const std::string& text) {
        if (error) *error = text;
        if (problem.empty()) problem = text;
        writer.reset();
        if (!spec.record.empty()) removeFile(spec.record);
        return false;
    };
    if (finished) return problem.empty();
    finished = true;
    if (!problem.empty()) return fail(problem);
    if (spec.record.empty()) return true;
    if (!started || (wantPictures && !sums.videoFrames)) return fail(wantSound && !soundSeen ? "the microphone gave no sound" : "the camera gave no pictures");
    if (encoder && (!encoder->finish() || !drain())) return fail("the video encoder failed");
    if (aac) {
        AudioSink sink = [&](const uint8_t* data, size_t size) {
            writer->write(audioTrack, data, size, soundPackets * 1024, soundPackets * 1024, 1024, true);
            ++soundPackets;
        };
        if (!aac->finish(sink)) return fail("the audio encoder failed");
        writer->trim(audioTrack, aac->priming(), sums.audioFrames);
    }
    std::string trouble;
    if (!writer->finish(&trouble)) return fail(trouble);
    return true;
}

TestSource::TestSource(CaptureRecorder& recorder, bool sound) {
    const auto began = std::chrono::steady_clock::now();
    auto flash = [](double seconds) { return seconds - std::floor(seconds) < 0.1; };
    pictures = std::thread([this, &recorder, began, flash] {
        const int width = 1280, height = 720, cw = width / 2, ch = height / 2;
        std::vector<uint8_t> pixels(size_t(width) * height + 2 * size_t(cw) * ch);
        Picture picture;
        picture.plane[0] = pixels.data();
        picture.plane[1] = pixels.data() + size_t(width) * height;
        picture.plane[2] = picture.plane[1] + size_t(cw) * ch;
        picture.stride[0] = size_t(width);
        picture.stride[1] = picture.stride[2] = size_t(cw);
        picture.width = width;
        picture.height = height;
        picture.chromaWidth = cw;
        picture.chromaHeight = ch;
        picture.color.known = true;
        for (long n = 0; !stop; ++n) {
            const double seconds = double(n) / 30;
            std::this_thread::sleep_until(began + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds)));
            const bool bright = flash(seconds + 1e-6);
            for (int y = 0; y < height; ++y) {
                uint8_t* row = pixels.data() + size_t(y) * width;
                for (int x = 0; x < width; ++x) row[x] = bright ? 235 : uint8_t(40 + ((x + y + n * 8) & 63));
            }
            for (int y = 0; y < ch; ++y) {
                for (int x = 0; x < cw; ++x) {
                    pixels[size_t(width) * height + size_t(y) * cw + x] = bright ? 128 : uint8_t(96 + x * 64 / cw);
                    pixels[size_t(width) * height + size_t(cw) * ch + size_t(y) * cw + x] = bright ? 128 : uint8_t(96 + y * 64 / ch);
                }
            }
            recorder.video(picture, nullptr, seconds);
        }
    });
    if (!sound) return;
    tones = std::thread([this, &recorder, began, flash] {
        const int rate = 48000, block = 1024;
        const double late = 0.35;
        std::vector<int16_t> pcm(size_t(block) * 2);
        for (long n = 0; !stop; ++n) {
            const double seconds = late + double(n) * block / rate;
            std::this_thread::sleep_until(began + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds + double(block) / rate)));
            for (int i = 0; i < block; ++i) {
                const double at = seconds + double(i) / rate;
                const double value = flash(at) ? 0.5 * std::sin(2 * 3.14159265358979323846 * 1000 * at) : 0.0;
                pcm[size_t(i) * 2] = pcm[size_t(i) * 2 + 1] = int16_t(std::lround(value * 32767));
            }
            recorder.audio(pcm.data(), size_t(block), rate, 2, seconds);
        }
    });
}

TestSource::~TestSource() {
    stop = true;
    if (pictures.joinable()) pictures.join();
    if (tones.joinable()) tones.join();
}

}

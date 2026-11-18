#include "tools_common.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <set>
#include "portable.h"

namespace absent {

namespace {

bool fail(std::string* error, const char* text) {
    if (error) *error = text;
    return false;
}

int even(double value) { return std::max(2, int(std::lround(value / 2)) * 2); }

struct Buffer {
    void* p = nullptr;
    Buffer() = default;
    explicit Buffer(void* buffer) : p(buffer) {}
    Buffer(Buffer&& other) noexcept : p(other.p) { other.p = nullptr; }
    Buffer& operator=(Buffer&& other) noexcept {
        std::swap(p, other.p);
        return *this;
    }
    ~Buffer() { releaseBuffer(p); }
};

struct VideoOut {
    std::unique_ptr<VideoEncoder> encoder;
    Writer* writer = nullptr;
    int track = -1;
    bool configured = false;
    bool failed = false;
    uint32_t usual = 1;

    bool open(const EncodeSpec& spec, int rotation, Writer& file, std::string* error) {
        encoder = VideoEncoder::open(spec, error);
        if (!encoder) return false;
        writer = &file;
        TrackSpec picture;
        picture.kind = Kind::Video;
        picture.codec = spec.codec;
        picture.timescale = spec.timescale;
        picture.width = spec.width;
        picture.height = spec.height;
        picture.rotation = rotation;
        track = file.add(picture);
        return track >= 0 || fail(error, kCannotHold);
    }
    void drain() {
        Packet packet;
        while (encoder->take(packet)) {
            if (!configured) writer->configure(track, encoder->configType(), encoder->config());
            configured = true;
            if (!writer->write(track, packet.data.data(), packet.data.size(), packet.pts, packet.dts, packet.duration ? packet.duration : usual, packet.sync)) failed = true;
        }
    }
    bool push(void* buffer, int64_t pts, int64_t duration) {
        if (!buffer || !encoder->encodePixelBuffer(buffer, pts, duration)) return false;
        drain();
        return !failed;
    }
    bool pushBGRA(const uint8_t* pixels, int64_t pts, int64_t duration) {
        if (!encoder->encodeBGRA(pixels, pts, duration)) return false;
        drain();
        return !failed;
    }
    bool finish() {
        if (!encoder->finish()) return false;
        drain();
        return !failed && configured;
    }
    const char* problem() const { return failed ? "writing the output file failed" : "the encoder refused a frame"; }
};

struct SoundOut {
    std::unique_ptr<PcmReader> pcm;
    std::unique_ptr<AacEncoder> aac;
    Writer* writer = nullptr;
    int track = -1;
    int rate = 0;
    int channels = 0;
    int64_t first = 0;
    int64_t total = 0;
    int64_t done = 0;
    int64_t packets = 0;
    bool backwards = false;
    bool failed = false;
    std::vector<float> block;
    std::vector<int16_t> shorts;

    bool open(const std::string& path, const SoundPlan& plan, int bitrate, std::string* error) {
        rate = plan.rate;
        channels = plan.channels;
        pcm = PcmReader::open(path, rate, channels, error);
        if (!pcm) return false;
        aac = AacEncoder::open(rate, channels, bitrate, error);
        return aac != nullptr;
    }
    void begin(int64_t from, int64_t count, bool reversed, Writer& file) {
        writer = &file;
        first = from;
        total = count;
        backwards = reversed;
        TrackSpec sound;
        sound.kind = Kind::Audio;
        sound.codec = fourcc("mp4a");
        sound.configType = fourcc("esds");
        sound.config = aac->config();
        sound.timescale = uint32_t(rate);
        sound.sampleRate = rate;
        sound.channels = channels;
        sound.skip = aac->priming();
        sound.length = count;
        track = file.add(sound);
    }
    AudioSink sink() {
        return [this](const uint8_t* data, size_t size) {
            if (!writer->write(track, data, size, packets * 1024, packets * 1024, 1024, true)) failed = true;
            ++packets;
        };
    }
    bool advance(double seconds) {
        const int64_t target = seconds >= 1e17 ? total : std::min<int64_t>(total, std::llround(seconds * rate));
        while (done < target && !failed) {
            const size_t frames = size_t(std::min<int64_t>(4096, target - done));
            block.resize(frames * size_t(channels));
            const int64_t at = backwards ? first + total - done - int64_t(frames) : first + done;
            if (!pcm->read(at, frames, block.data())) return false;
            shorts.resize(block.size());
            for (size_t i = 0; i < frames; ++i) {
                const size_t from = backwards ? frames - 1 - i : i;
                for (int c = 0; c < channels; ++c) shorts[i * size_t(channels) + size_t(c)] = int16_t(std::clamp(std::lround(block[from * size_t(channels) + size_t(c)] * 32768.0f), -32768l, 32767l));
            }
            if (!aac->encode(shorts.data(), frames, sink())) return false;
            done += int64_t(frames);
        }
        return !failed;
    }
    bool finish() { return advance(1e18) && aac->finish(sink()) && !failed; }
};

struct Clip {
    std::unique_ptr<Reader> reader;
    const Track* track = nullptr;
    std::unique_ptr<VideoDecoder> decoder;
    double zero = 0;

    bool open(const std::string& path, std::string* error) {
        reader = Reader::open(path, error);
        if (!reader) return false;
        track = reader->first(Kind::Video);
        if (!track || !track->timescale) return fail(error, "no video track");
        decoder = VideoDecoder::open(*reader, *track, error);
        if (!decoder) return false;
        if (!decoder->frames()) return fail(error, "no video track");
        zero = zeroSeconds(*reader);
        return true;
    }
    double time(int64_t tick) const { return double(tick - track->mediaStart + track->delay) / track->timescale - zero; }
    double at(uint32_t index) const { return time(decoder->pts(index)); }
    int64_t tick(double seconds) const { return std::llround((seconds + zero) * track->timescale) - track->delay + track->mediaStart; }
    uint32_t firstAtOrAfter(double seconds) const {
        uint32_t low = 0, high = decoder->frames();
        while (low < high) {
            const uint32_t middle = low + (high - low) / 2;
            if (at(middle) < seconds - 1e-7) low = middle + 1;
            else high = middle;
        }
        return low;
    }
    int64_t sourceBitrate() const {
        uint64_t bytes = 0;
        for (const Sample& s : track->samples) bytes += s.size;
        const double seconds = track->seconds();
        return seconds > 0 ? int64_t(double(bytes) * 8 / seconds) : 0;
    }
};

double frameRate(const Track& track) {
    const double rate = track.rate();
    return rate > 0 && rate < 1000 ? rate : 30;
}

}

bool compress(const CompressJob& job, const Progress& progress, std::string* error) {
    if (sameFile(job.input, job.output)) return fail(error, "the output would replace the input");
    Clip in;
    if (!in.open(job.input, error)) return false;
    VideoDecoder& decoder = *in.decoder;
    const uint32_t count = decoder.frames();
    const double start = std::max<int64_t>(job.startUs, 0) / 1e6;
    const bool ranged = job.endUs > job.startUs;
    const double span = ranged ? (job.endUs - std::max<int64_t>(job.startUs, 0)) / 1e6 : std::numeric_limits<double>::infinity();
    uint32_t first = in.firstAtOrAfter(start);
    double shown = start;
    if (job.startUs > 0 && ranged && (first >= count || in.at(first) >= start + span)) {
        if (first > 0) {
            --first;
            shown = in.at(first);
        }
    }
    if (first >= count) return fail(error, "nothing lies between the two times");
    uint32_t last = first;
    while (last + 1 < count && in.at(last + 1) - shown < span - 1e-7) ++last;

    int width = job.width, height = job.height;
    const int sourceWidth = in.track->width > 0 ? in.track->width : 2, sourceHeight = in.track->height > 0 ? in.track->height : 2;
    if (width <= 0 && height <= 0) {
        width = sourceWidth;
        height = sourceHeight;
    } else if (width <= 0) {
        width = even(double(height) * sourceWidth / sourceHeight);
    } else if (height <= 0) {
        height = even(double(width) * sourceHeight / sourceWidth);
    } else if (job.keepAspect) {
        const double scale = std::min(double(width) / sourceWidth, double(height) / sourceHeight);
        width = even(sourceWidth * scale);
        height = even(sourceHeight * scale);
    }
    width = even(width);
    height = even(height);
    if (width > 16384 || height > 16384) return fail(error, "the size is too large");

    const bool fixed = job.fpsNumerator > 0 && job.fpsDenominator > 0;
    const double fps = fixed ? double(job.fpsNumerator) / job.fpsDenominator : frameRate(*in.track);
    if (fixed && (fps > 1000 || fps < 0.01)) return fail(error, "bad frame rate");
    const uint32_t scale = fixed ? (job.fpsDenominator == 1 ? uint32_t(job.fpsNumerator) * 1000u : uint32_t(job.fpsNumerator)) : in.track->timescale;
    const int64_t tick = fixed ? (job.fpsDenominator == 1 ? 1000 : job.fpsDenominator) : 1;

    auto writer = Writer::open(job.output, error);
    if (!writer) return false;
    EncodeSpec spec;
    spec.width = width;
    spec.height = height;
    spec.timescale = scale;
    spec.fps = fps;
    spec.bitrate = job.bitrate > 0 ? job.bitrate : int(std::clamp(double(width) * height * fps * 0.1, 150000.0, 100e6));
    spec.keyInterval = std::max(1, int(std::lround(fps)));
    spec.codec = job.codec ? job.codec : fourcc("avc1");
    VideoOut video;
    if (!video.open(spec, in.track->rotation, *writer, error)) return false;
    video.usual = uint32_t(std::max<int64_t>(tick, 1));

    const Frame probe = decoder.frame(last);
    if (!probe) return fail(error, "a frame did not decode");
    const double finish = std::min(span, in.time(decoder.pts(last) + std::max<int64_t>(probe.duration, 1)) - shown);

    const Track* audio = in.reader->first(Kind::Audio);
    std::vector<Piece> copied;
    SoundOut sound;
    bool encodeSound = false;
    if (audio && job.sound == SoundChoice::Copy) {
        Piece piece;
        if (planPiece(*in.reader, *audio, in.zero, start, start + span, false, piece)) {
            copied.push_back(std::move(piece));
            settle(copied, start, matroskaPath(job.output), *writer, false);
        }
    } else if (audio && job.sound == SoundChoice::Aac) {
        SoundPlan plan;
        if (!soundPlan(*in.reader, false, plan, error)) return false;
        if (!sound.open(job.input, plan, job.audioBitrate, error)) return false;
        const int64_t from = std::llround((start - plan.start) * plan.rate);
        const int64_t length = sound.pcm->length();
        const int64_t until = std::min<int64_t>(length, std::isinf(span) ? length : from + std::llround(span * plan.rate));
        if (until > from && until > 0) {
            sound.begin(from, until - from, false, *writer);
            encodeSound = true;
        }
    } else if (job.sound == SoundChoice::Mp3) {
        return fail(error, "sound is copied or encoded to AAC here");
    }
    if ((!copied.empty() && copied[0].out < 0) || (encodeSound && sound.track < 0)) return fail(error, kCannotHold);

    auto soundUntil = [&](double seconds) {
        if (encodeSound) return sound.advance(seconds);
        if (copied.empty()) return true;
        Piece& piece = copied[0];
        while (!piece.done() && piece.time() <= seconds) if (!piece.step(*writer)) return false;
        return true;
    };

    Scaler scaler(width, height);
    Ticker ticker{progress};
    const int64_t limit = fixed && ranged ? int64_t(std::ceil(span * fps - 1e-6)) : std::numeric_limits<int64_t>::max();
    const int64_t shownTick = shown == start ? in.tick(start) : decoder.pts(first);
    const int64_t lastEnd = decoder.pts(last) + std::max<int64_t>(probe.duration, 1);
    auto slotOf = [&](int64_t sourceTick) {
        const __int128 scaled = static_cast<__int128>(std::max<int64_t>(sourceTick - shownTick, 0)) * job.fpsNumerator * 2;
        const __int128 unit = static_cast<__int128>(job.fpsDenominator) * in.track->timescale;
        return int64_t((scaled + unit) / (unit * 2));
    };
    int64_t next = 0;
    int64_t written = 0;
    const int64_t origin = shownTick;
    for (uint32_t i = first; i <= last; ++i) {
        const double ends = i < last ? in.at(i + 1) - shown : finish;
        double shownAt = 0;
        if (fixed) {
            const int64_t until = std::min<int64_t>(limit, i < last ? slotOf(decoder.pts(i + 1)) : std::max<int64_t>(slotOf(lastEnd), next == 0 ? 1 : 0));
            if (until <= next) continue;
            Frame frame = decoder.frame(i);
            Buffer buffer(frame ? scaler.fit(frame) : nullptr);
            if (!buffer.p) return fail(error, "a frame did not decode");
            for (; next < until; ++next) if (!video.push(buffer.p, next * tick, tick)) return fail(error, video.problem());
            shownAt = double(next) / fps;
        } else {
            Frame frame = decoder.frame(i);
            Buffer buffer(frame ? scaler.fit(frame) : nullptr);
            if (!buffer.p) return fail(error, "a frame did not decode");
            const int64_t begin = i == first ? origin : decoder.pts(i);
            const int64_t pts = begin - origin;
            const int64_t length = std::max<int64_t>(1, (i < last ? decoder.pts(i + 1) : decoder.pts(i) + std::max<int64_t>(probe.duration, 1)) - begin);
            if (!video.push(buffer.p, pts, length)) return fail(error, video.problem());
            written = pts + length;
            shownAt = ends;
        }
        if (!soundUntil(shownAt)) return fail(error, "writing the sound failed");
        if (!ticker(finish > 0 ? shownAt / finish * 0.99 : 0)) break;
    }
    if (ticker.stopped(error)) return false;
    writer->trim(video.track, 0, fixed ? next * tick : written);
    if (!video.finish()) return fail(error, "the encoder failed");
    if (!soundUntil(1e18) || (encodeSound && !sound.finish())) return fail(error, "writing the sound failed");
    if (!writer->finish(error)) return false;
    if (progress) progress(1.0);
    return true;
}

namespace {

struct Scratch {
    std::string path;
    ~Scratch() {
        removeFile(path);
        removeFile(path + ".part");
    }
};

}

bool reverse(const ReverseJob& job, const Progress& progress, ReverseResult* result, std::string* error) {
    if (sameFile(job.input, job.output)) return fail(error, "the output would replace the input");
    Clip in;
    if (!in.open(job.input, error)) return false;
    VideoDecoder& decoder = *in.decoder;
    const uint32_t count = decoder.frames();
    const bool ranged = job.endUs > job.startUs;
    const uint32_t first = ranged ? in.firstAtOrAfter(std::max<int64_t>(job.startUs, 0) / 1e6) : 0;
    const uint32_t end = ranged ? in.firstAtOrAfter(job.endUs / 1e6) : count;
    if (end <= first) return fail(error, "nothing lies between the two times");

    const int width = even(in.track->width), height = even(in.track->height);
    const double fps = frameRate(*in.track);
    auto writer = Writer::open(job.output, error);
    if (!writer) return false;
    EncodeSpec spec;
    spec.width = width;
    spec.height = height;
    spec.timescale = in.track->timescale;
    spec.fps = fps;
    const int64_t source = in.sourceBitrate();
    spec.bitrate = job.bitrate > 0 ? job.bitrate : int(std::clamp<int64_t>(source * (in.track->codec == fourcc("avc1") ? 3 : 4) / 2, 1000000, 80000000));
    spec.keyInterval = std::max(1, int(std::lround(fps)));
    spec.codec = fourcc("avc1");
    VideoOut video;
    if (!video.open(spec, in.track->rotation, *writer, error)) return false;

    Scaler scaler(width, height);
    Frame tail = decoder.frame(end - 1);
    if (!tail) return fail(error, "a frame did not decode");
    const int64_t finish = end < count ? decoder.pts(end) : decoder.pts(end - 1) + std::max<int64_t>(tail.duration, 1);
    tail = Frame();
    const double seconds = double(finish - decoder.pts(first)) / in.track->timescale;
    video.usual = uint32_t(std::max<int64_t>(1, std::llround(in.track->timescale / fps)));

    SoundOut sound;
    bool hasSound = false;
    if (in.reader->first(Kind::Audio)) {
        SoundPlan plan;
        if (!soundPlan(*in.reader, false, plan, error)) return false;
        const int64_t from = std::max<int64_t>(0, std::llround((in.at(first) - plan.start) * plan.rate));
        const int64_t until = std::llround((in.time(finish) - plan.start) * plan.rate);
        if (until > from) {
            if (!sound.open(job.input, plan, job.audioBitrate, error)) return false;
            sound.begin(from, until - from, true, *writer);
            if (sound.track < 0) return fail(error, kCannotHold);
            hasSound = true;
        }
    }

    std::set<int64_t> keys;
    for (uint32_t index : in.track->syncs) if (index < in.track->samples.size()) keys.insert(in.track->samples[index].pts);
    const uint64_t frameBytes = uint64_t(width) * uint64_t(height) * 3 / 2;
    const uint64_t room = std::max<uint64_t>(1, job.memoryBytes / std::max<uint64_t>(frameBytes, 1));
    Scratch scratch{job.output + ".spill.mp4"};
    Ticker ticker{progress};
    const uint32_t total = end - first;
    uint32_t done = 0, spilled = 0;

    auto place = [&](uint32_t index, void* buffer) {
        const int64_t later = index + 1 < end ? decoder.pts(index + 1) : finish;
        const int64_t pts = finish - later;
        const int64_t length = std::max<int64_t>(1, later - decoder.pts(index));
        if (!video.push(buffer, pts, length)) return fail(error, video.problem());
        if (hasSound && !sound.advance(double(pts + length) / in.track->timescale)) return fail(error, "writing the sound failed");
        ++done;
        return true;
    };

    for (uint32_t stop = end; stop > first && !ticker.cancelled;) {
        uint32_t from = stop - 1;
        while (from > first && !keys.count(decoder.pts(from))) --from;
        const uint32_t held = stop - from;
        if (held <= room) {
            std::vector<Buffer> frames;
            frames.reserve(held);
            for (uint32_t i = from; i < stop; ++i) {
                Frame frame = decoder.frame(i);
                frames.emplace_back(frame ? scaler.fit(frame) : nullptr);
                if (!frames.back().p) return fail(error, "a frame did not decode");
            }
            for (uint32_t i = stop; i-- > from;) {
                if (!place(i, frames[i - from].p)) return false;
                frames[i - from] = Buffer();
                if (!ticker(double(done) / total * 0.99)) break;
            }
        } else {
            ++spilled;
            {
                auto store = Mp4Writer::open(scratch.path, error);
                if (!store) return false;
                EncodeSpec plain = spec;
                plain.timescale = 600;
                plain.keyInterval = 1;
                plain.reorder = false;
                plain.bitrate = int(std::clamp(double(width) * height * fps * 2.0, 20e6, 400e6));
                VideoOut keep;
                if (!keep.open(plain, 0, *store, error)) return false;
                for (uint32_t i = from; i < stop; ++i) {
                    Frame frame = decoder.frame(i);
                    Buffer buffer(frame ? scaler.fit(frame) : nullptr);
                    if (!buffer.p) return fail(error, "a frame did not decode");
                    if (!keep.push(buffer.p, int64_t(i - from) * 20, 20)) return fail(error, keep.problem());
                    if (!ticker((done + (i - from) * 0.5) / total * 0.99)) break;
                }
                if (ticker.cancelled) break;
                if (!keep.finish() || !store->finish(error)) return fail(error, "writing the scratch file failed");
            }
            auto stored = Reader::open(scratch.path, error);
            const Track* track = stored ? stored->first(Kind::Video) : nullptr;
            auto replay = track ? VideoDecoder::open(*stored, *track, error) : nullptr;
            if (!replay || replay->frames() != held) return fail(error, "reading the scratch file failed");
            for (uint32_t i = stop; i-- > from;) {
                Frame frame = replay->frame(i - from);
                if (!frame) return fail(error, "reading the scratch file failed");
                if (!place(i, frame.native.get())) return false;
                if (!ticker(double(done) / total * 0.99)) break;
            }
            replay.reset();
            stored.reset();
            removeFile(scratch.path);
        }
        stop = from;
    }
    if (ticker.stopped(error)) return false;
    writer->trim(video.track, 0, finish - decoder.pts(first));
    if (!video.finish()) return fail(error, "the encoder failed");
    if (hasSound && !sound.finish()) return fail(error, "writing the sound failed");
    if (!writer->finish(error)) return false;
    if (result) {
        result->width = width;
        result->height = height;
        result->frames = total;
        result->durationUs = std::llround(seconds * 1e6);
        result->spilledGroups = spilled;
    }
    if (progress) progress(1.0);
    return true;
}

int gifDelayMs(double seconds) {
    const long hundredths = std::isfinite(seconds) && seconds > 0 ? std::lround(std::min(seconds, 655.35) * 100) : 0;
    return hundredths <= 1 ? 100 : int(hundredths) * 10;
}

bool gifVideoSize(int width, int height, int& videoWidth, int& videoHeight) {
    if (width <= 0 || height <= 0) return false;
    auto even = [](double value) { return std::max(2, int(std::floor(value / 2 + 0.5)) * 2); };
    double scale = 1.0;
    for (;;) {
        videoWidth = even(width * scale);
        videoHeight = even(height * scale);
        if (videoWidth <= 8192 && videoHeight <= 8192) return true;
        scale *= 0.75;
        if (std::min(width, height) * scale < 16) return false;
    }
}

bool gifToVideo(const std::string& input, const std::string& colour, const std::string& matte, GifInfo* out, const Progress& progress, std::string* error) {
    GifInfo info;
    std::unique_ptr<Writer> colourFile, matteFile;
    VideoOut colourVideo, matteVideo;
    std::vector<uint8_t> shown, mask;
    std::vector<int> across, down;
    int width = 0, height = 0;
    int64_t at = 0;
    bool broken = false;
    Ticker ticker{progress};
    auto start = [&]() {
        if (info.delaysMs.size() < 2) return fail(error, "the GIF is a still picture");
        const int least = std::min(info.width, info.height);
        std::vector<double> scales = {1.0};
        for (double scale = 0.75; least * scale >= 16; scale *= 0.75) scales.push_back(scale);
        for (double scale : {2.0, 4.0, 8.0}) scales.push_back(scale);
        colourFile = Writer::open(colour, error);
        if (!colourFile) return false;
        EncodeSpec spec;
        spec.timescale = 100;
        spec.fps = info.fps;
        spec.bitrate = 20000000;
        spec.keyInterval = std::max(1, info.fps);
        spec.codec = fourcc("avc1");
        spec.bgra = true;
        spec.reorder = false;
        bool opened = false;
        for (double scale : scales) {
            spec.width = even(info.width * scale);
            spec.height = even(info.height * scale);
            if (spec.width > 8192 || spec.height > 8192) continue;
            if (colourVideo.open(spec, 0, *colourFile, error)) {
                opened = true;
                break;
            }
        }
        if (!opened) return fail(error, "no video size fits this GIF");
        width = info.videoWidth = spec.width;
        height = info.videoHeight = spec.height;
        if (!matte.empty()) {
            matteFile = Writer::open(matte, error);
            if (!matteFile || !matteVideo.open(spec, 0, *matteFile, error)) return false;
        }
        across.resize(size_t(width));
        down.resize(size_t(height));
        for (int x = 0; x < width; ++x) across[size_t(x)] = std::min(int((int64_t(x) * 2 + 1) * info.width / (int64_t(width) * 2)), info.width - 1);
        for (int y = 0; y < height; ++y) down[size_t(y)] = std::min(int((int64_t(y) * 2 + 1) * info.height / (int64_t(height) * 2)), info.height - 1);
        shown.resize(size_t(width) * size_t(height) * 4);
        mask.resize(shown.size());
        return true;
    };
    auto frame = [&](uint32_t index, const uint8_t* canvas) {
        if (index == 0 && !start()) {
            broken = true;
            return false;
        }
        for (int y = 0; y < height; ++y) {
            const uint8_t* row = canvas + size_t(down[size_t(y)]) * size_t(info.width) * 4;
            uint8_t* to = &shown[size_t(y) * size_t(width) * 4];
            uint8_t* alpha = &mask[size_t(y) * size_t(width) * 4];
            for (int x = 0; x < width; ++x) {
                const uint8_t* from = row + size_t(across[size_t(x)]) * 4;
                to[0] = from[0];
                to[1] = from[1];
                to[2] = from[2];
                to[3] = 255;
                alpha[0] = alpha[1] = alpha[2] = from[3];
                alpha[3] = 255;
                to += 4;
                alpha += 4;
            }
        }
        const int delay = info.delaysMs[index] / 10;
        if (!colourVideo.pushBGRA(shown.data(), at, delay) || (matteFile && !matteVideo.pushBGRA(mask.data(), at, delay))) {
            broken = true;
            if (error) *error = "the encoder refused a frame";
            return false;
        }
        at += delay;
        return ticker(double(index + 1) / double(info.delaysMs.size()) * 0.99);
    };
    std::string problem;
    if (!gifRead(input, info, frame, &problem)) {
        if (ticker.stopped(error)) return false;
        if (error && (!broken || error->empty())) *error = problem;
        return false;
    }
    if (!colourFile) return fail(error, "the GIF is a still picture");
    colourFile->trim(colourVideo.track, 0, at);
    if (!colourVideo.finish() || !colourFile->finish(error)) return fail(error, "writing the colour file failed");
    if (matteFile) matteFile->trim(matteVideo.track, 0, at);
    if (matteFile && info.transparent && (!matteVideo.finish() || !matteFile->finish(error))) {
        removeFile(colour);
        return fail(error, "writing the matte file failed");
    }
    if (out) *out = info;
    if (progress) progress(1.0);
    return true;
}

}

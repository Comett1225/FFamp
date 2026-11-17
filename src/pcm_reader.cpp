#include "absent/audio.h"
#include "resample.h"
#include <algorithm>
#include <future>

namespace absent {

namespace {

const size_t kAhead = 65536;

}

struct PcmReader::Impl {
    std::unique_ptr<Reader> file;
    std::unique_ptr<AudioDecoder> decoder;
    std::unique_ptr<SincKernel> kernel;
    std::unique_ptr<ChannelMix> mix;
    int rate = 0;
    int channels = 0;
    int width = 0;
    bool mixFirst = true;
    int64_t length = 0;
    std::vector<float> native;
    std::vector<float> mixed;
    std::vector<float> shaped;
    std::vector<float> window;
    int64_t windowStart = 0;
    int64_t windowEnd = 0;

    std::vector<float> early;
    int64_t earlyFrom = 0;
    size_t earlyFrames = 0;
    std::future<void> earlyTask;

    void decode(int64_t from, size_t frames, std::vector<float>& into) {
        native.resize(frames * size_t(decoder->channels()));
        decoder->read(from, frames, native.data());
        if (!mixFirst) return into.swap(native);
        into.resize(frames * size_t(width));
        mix->apply(native.data(), frames, into.data());
    }

    const float* source(int64_t from, size_t frames) {
        bool ready = earlyTask.valid() && earlyFrom == from && earlyFrames == frames;
        if (earlyTask.valid()) earlyTask.get();
        if (ready) mixed.swap(early);
        else decode(from, frames, mixed);
        return mixed.data();
    }

    void prepare(int64_t start, size_t frames) {
        earlyFrom = kernel->firstInput(start);
        earlyFrames = size_t(kernel->endInput(start + int64_t(frames) - 1) - earlyFrom);
        earlyTask = std::async(std::launch::async, [this] { decode(earlyFrom, earlyFrames, early); });
    }

    void step(int64_t start, size_t frames, float* out, bool more) {
        float* target = out;
        if (!mixFirst) {
            shaped.resize(frames * size_t(width));
            target = shaped.data();
        }
        if (kernel->identity()) {
            const float* in = source(start, frames);
            std::copy(in, in + frames * size_t(width), target);
        } else {
            int64_t from = kernel->firstInput(start);
            int64_t to = kernel->endInput(start + int64_t(frames) - 1);
            const float* in = source(from, size_t(to - from));
            int64_t next = start + int64_t(frames);
            if (more && next < length) prepare(next, size_t(std::min<int64_t>(int64_t(kAhead), length - next)));
            kernel->render(in, from, width, start, frames, target);
        }
        if (!mixFirst) mix->apply(target, frames, out);
    }
};

PcmReader::PcmReader() : impl(new Impl) {}
PcmReader::~PcmReader() = default;

std::unique_ptr<PcmReader> PcmReader::open(const std::string& path, int rate, int channels, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    if (rate < 1 || rate > 768000 || channels < 1 || channels > 64) return fail("output audio layout not supported");
    std::unique_ptr<PcmReader> reader(new PcmReader);
    Impl& r = *reader->impl;
    r.file = Reader::open(path, error);
    if (!r.file) return nullptr;
    const Track* track = r.file->first(Kind::Audio);
    if (!track) return fail("no audio track");
    r.decoder = AudioDecoder::open(*r.file, *track, error);
    if (!r.decoder) return nullptr;
    r.rate = rate;
    r.channels = channels;
    r.kernel = std::make_unique<SincKernel>(r.decoder->sampleRate(), rate);
    r.mix = std::make_unique<ChannelMix>(r.decoder->channels(), channels, r.decoder->layout(), r.decoder->mixLevels());
    r.mixFirst = channels <= r.decoder->channels();
    r.width = r.mixFirst ? channels : r.decoder->channels();
    r.length = r.kernel->identity() ? r.decoder->length() : r.kernel->outputLength(r.decoder->length());
    return reader;
}

int PcmReader::sampleRate() const { return impl->rate; }
int PcmReader::channels() const { return impl->channels; }
int64_t PcmReader::length() const { return impl->length; }
double PcmReader::mixPeak() const { return impl->mix->identity ? 1.0 : impl->mix->peak(); }

bool PcmReader::read(int64_t start, size_t frames, float* out) {
    Impl& r = *impl;
    const size_t channels = size_t(r.channels);
    std::fill(out, out + frames * channels, 0.0f);
    if (start >= r.length || start < -(int64_t(1) << 62)) return true;
    uint64_t skip = start < 0 ? std::min<uint64_t>(frames, uint64_t(-start)) : 0;
    int64_t from = start + int64_t(skip);
    int64_t to = from + int64_t(std::min<uint64_t>(frames - skip, uint64_t(r.length - from)));
    for (int64_t at = from; at < to;) {
        if (at < r.windowStart || at >= r.windowEnd) {
            size_t wanted = at == r.windowEnd && at > 0 ? kAhead : size_t(std::min<int64_t>(int64_t(kAhead), to - at));
            size_t made = size_t(std::min<int64_t>(int64_t(wanted), r.length - at));
            r.window.resize(made * channels);
            r.step(at, made, r.window.data(), wanted == kAhead);
            r.windowStart = at;
            r.windowEnd = at + int64_t(made);
        }
        size_t count = size_t(std::min(to, r.windowEnd) - at);
        const float* ready = r.window.data() + size_t(at - r.windowStart) * channels;
        std::copy(ready, ready + count * channels, out + size_t(at - start) * channels);
        at += int64_t(count);
    }
    return true;
}

}

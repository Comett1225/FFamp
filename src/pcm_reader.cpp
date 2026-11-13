#include "absent/audio.h"
#include "resample.h"
#include <algorithm>

namespace absent {

namespace {

const size_t kStep = 4096;

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

    const float* source(int64_t from, size_t frames) {
        native.resize(frames * size_t(decoder->channels()));
        decoder->read(from, frames, native.data());
        if (!mixFirst) return native.data();
        mixed.resize(frames * size_t(width));
        mix->apply(native.data(), frames, mixed.data());
        return mixed.data();
    }

    void step(int64_t start, size_t frames, float* out) {
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
            kernel->render(source(from, size_t(to - from)), from, width, start, frames, target);
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
    r.mix = std::make_unique<ChannelMix>(r.decoder->channels(), channels);
    r.mixFirst = channels <= r.decoder->channels();
    r.width = r.mixFirst ? channels : r.decoder->channels();
    r.length = r.kernel->identity() ? r.decoder->length() : r.kernel->outputLength(r.decoder->length());
    return reader;
}

int PcmReader::sampleRate() const { return impl->rate; }
int PcmReader::channels() const { return impl->channels; }
int64_t PcmReader::length() const { return impl->length; }

bool PcmReader::read(int64_t start, size_t frames, float* out) {
    Impl& r = *impl;
    const size_t channels = size_t(r.channels);
    std::fill(out, out + frames * channels, 0.0f);
    if (start >= r.length || start < -(int64_t(1) << 62)) return true;
    uint64_t skip = start < 0 ? std::min<uint64_t>(frames, uint64_t(-start)) : 0;
    int64_t from = start + int64_t(skip);
    int64_t to = from + int64_t(std::min<uint64_t>(frames - skip, uint64_t(r.length - from)));
    for (int64_t at = from; at < to;) {
        size_t count = size_t(std::min<int64_t>(int64_t(kStep), to - at));
        r.step(at, count, out + size_t(at - start) * channels);
        at += int64_t(count);
    }
    return true;
}

}

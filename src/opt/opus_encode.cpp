#ifdef ABSENT_OPUS
#include "absent/media.h"
#include <opus/opus.h>
#include <cstring>

namespace absent {

namespace {

const int kFrame = 960;

}

struct OpusEncoder::Impl {
    ::OpusEncoder* encoder = nullptr;
    int channels = 0;
    int lookahead = 0;
    int64_t fed = 0;
    int64_t coded = 0;
    std::vector<int16_t> pending;
    std::vector<uint8_t> packet = std::vector<uint8_t>(4000);

    ~Impl() { if (encoder) opus_encoder_destroy(encoder); }

    bool drain(const AudioSink& sink) {
        size_t at = 0;
        while (pending.size() - at >= size_t(kFrame * channels)) {
            int size = opus_encode(encoder, pending.data() + at, kFrame, packet.data(), opus_int32(packet.size()));
            if (size < 0) return false;
            sink(packet.data(), size_t(size));
            at += size_t(kFrame * channels);
            coded += kFrame;
        }
        pending.erase(pending.begin(), pending.begin() + long(at));
        return true;
    }
};

OpusEncoder::OpusEncoder() : impl(new Impl) {}
OpusEncoder::~OpusEncoder() = default;

std::unique_ptr<OpusEncoder> OpusEncoder::open(int channels, int bitrate, std::string* error) {
    std::unique_ptr<OpusEncoder> encoder(new OpusEncoder);
    int status = OPUS_OK;
    if (channels >= 1 && channels <= 2) encoder->impl->encoder = opus_encoder_create(48000, channels, OPUS_APPLICATION_AUDIO, &status);
    if (!encoder->impl->encoder || status != OPUS_OK) {
        if (error) *error = "the Opus encoder is not available for this layout";
        return nullptr;
    }
    encoder->impl->channels = channels;
    opus_encoder_ctl(encoder->impl->encoder, OPUS_SET_BITRATE(bitrate));
    opus_int32 lookahead = 0;
    opus_encoder_ctl(encoder->impl->encoder, OPUS_GET_LOOKAHEAD(&lookahead));
    encoder->impl->lookahead = int(lookahead);
    return encoder;
}

bool OpusEncoder::encode(const int16_t* pcm, size_t frames, const AudioSink& sink) {
    impl->pending.insert(impl->pending.end(), pcm, pcm + frames * size_t(impl->channels));
    impl->fed += int64_t(frames);
    return impl->drain(sink);
}

bool OpusEncoder::finish(const AudioSink& sink) {
    int64_t needed = impl->fed + impl->lookahead;
    int64_t queued = impl->coded + int64_t(impl->pending.size()) / impl->channels;
    int64_t whole = (needed + kFrame - 1) / kFrame * kFrame;
    if (whole > queued) impl->pending.insert(impl->pending.end(), size_t(whole - queued) * size_t(impl->channels), 0);
    return impl->drain(sink);
}

int OpusEncoder::priming() const { return impl->lookahead; }

std::vector<uint8_t> OpusEncoder::config() const {
    std::vector<uint8_t> head = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, uint8_t(impl->channels), uint8_t(impl->lookahead), uint8_t(impl->lookahead >> 8), 0x80, 0xBB, 0, 0, 0, 0, 0};
    return head;
}

}

#endif

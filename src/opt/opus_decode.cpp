#ifdef ABSENT_OPUS
#include "../audio_codec.h"
#include <cstring>
#include <opus/opus_multistream.h>

namespace absent {

namespace {

const int kLongestPacket = 5760;
const uint8_t kWavOrder[9][8] = {{}, {0}, {0, 1}, {0, 2, 1}, {0, 1, 2, 3}, {0, 2, 1, 3, 4}, {0, 2, 1, 5, 3, 4}, {0, 2, 1, 6, 5, 3, 4}, {0, 2, 1, 7, 5, 6, 3, 4}};

struct OpusAudio : PacketDecoder {
    OpusMSDecoder* decoder = nullptr;

    ~OpusAudio() override { if (decoder) opus_multistream_decoder_destroy(decoder); }

    void reset() override { opus_multistream_decoder_ctl(decoder, OPUS_RESET_STATE); }

    void feed(const std::vector<uint8_t>& packet, std::vector<float>& out) override {
        size_t at = out.size();
        out.resize(at + size_t(kLongestPacket) * size_t(channels));
        int frames = opus_multistream_decode_float(decoder, packet.data(), opus_int32(packet.size()), out.data() + at, kLongestPacket, 0);
        out.resize(at + size_t(frames > 0 ? frames : 0) * size_t(channels));
    }
};

}

std::unique_ptr<PacketDecoder> openOpusAudio(const Track& track, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    const std::vector<uint8_t>& head = track.config;
    if (head.size() < 19 || memcmp(head.data(), "OpusHead", 8) != 0) return fail("Opus track without its header");
    int channels = head[9];
    int family = head[18];
    int streams = 1;
    int coupled = channels == 2 ? 1 : 0;
    uint8_t mapping[8] = {0, 1};
    if (family == 0) {
        if (channels < 1 || channels > 2) return fail("Opus channel layout not supported");
    } else if (family == 1) {
        if (channels < 1 || channels > 8 || head.size() < size_t(21 + channels)) return fail("Opus channel layout not supported");
        streams = head[19];
        coupled = head[20];
        for (int i = 0; i < channels; ++i) mapping[i] = head[size_t(21 + kWavOrder[channels][i])];
    } else {
        return fail("Opus channel layout not supported");
    }
    auto audio = std::make_unique<OpusAudio>();
    int status = 0;
    audio->decoder = opus_multistream_decoder_create(48000, channels, streams, coupled, mapping, &status);
    if (!audio->decoder || status != OPUS_OK) return fail("Opus header refused");
    opus_multistream_decoder_ctl(audio->decoder, OPUS_SET_GAIN(int16_t(head[16] | (head[17] << 8))));
    audio->rate = 48000;
    audio->channels = channels;
    audio->prerollFrames = 48000;
    audio->longestPacket = kLongestPacket;
    return audio;
}

}
#endif

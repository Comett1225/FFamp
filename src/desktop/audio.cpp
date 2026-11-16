#include "../audio_codec.h"
#include "../bytes.h"
#include <algorithm>

namespace absent {

std::unique_ptr<PacketDecoder> openPlatformAudio(const Track& track, const std::vector<uint8_t>& firstPacket, std::string* error);
std::unique_ptr<AacCore> openPlatformAac(int rate, int channels, int bitrate, std::string* error);

namespace {

const int kRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};

}

std::unique_ptr<PacketDecoder> openSystemAudio(const Track& track, const std::vector<uint8_t>& firstPacket, std::string* error) {
    if (track.codec == fourcc("fLaC")) return openFlacAudio(track, error);
    if (track.codec == fourcc("alac")) return openAlacAudio(track, error);
    if (track.codec == fourcc("ac-3") || track.codec == fourcc("ec-3")) return openAc3Audio(track, firstPacket, error);
    if (track.codec == fourcc("samr") || track.codec == fourcc("sawb")) return openAmrAudio(track, error);
    const bool aac = track.codec == fourcc("mp4a"), mpeg = track.codec == fourcc(".mp3") || track.codec == fourcc(".mp2") || track.codec == fourcc(".mp1");
    if (!aac && !mpeg) {
        if (error) *error = "no decoder for this audio codec on this system yet";
        return nullptr;
    }
    std::string problem;
    if (auto system = openPlatformAudio(track, firstPacket, &problem)) return system;
    return aac ? openAacAudio(track, firstPacket, error) : openMpg123Audio(track, firstPacket, error);
}

struct AacEncoder::Impl {
    std::unique_ptr<AacCore> core;
    int rate = 0;
    int channels = 0;
    int bitrate = 0;
};

AacEncoder::AacEncoder() : impl(new Impl) {}
AacEncoder::~AacEncoder() = default;

std::unique_ptr<AacEncoder> AacEncoder::open(int rate, int channels, int bitrate, std::string* error) {
    if (channels < 1 || channels > 2 || std::find(std::begin(kRates), std::end(kRates), rate) == std::end(kRates)) {
        if (error) *error = "audio layout not supported";
        return nullptr;
    }
    std::unique_ptr<AacEncoder> encoder(new AacEncoder);
    Impl& a = *encoder->impl;
    a.rate = rate;
    a.channels = channels;
    a.bitrate = bitrate;
    std::string problem;
    a.core = openPlatformAac(rate, channels, bitrate, &problem);
    if (!a.core) a.core = openVoAac(rate, channels, bitrate, error);
    if (!a.core) return nullptr;
    return encoder;
}

bool AacEncoder::encode(const int16_t* pcm, size_t frames, const AudioSink& sink) { return impl->core->encode(pcm, frames, sink); }
bool AacEncoder::finish(const AudioSink& sink) { return impl->core->finish(sink); }
int AacEncoder::priming() const { return impl->core->priming; }

std::vector<uint8_t> AacEncoder::config() const {
    const int index = int(std::find(std::begin(kRates), std::end(kRates), impl->rate) - std::begin(kRates));
    Bytes b;
    b.u32(0);
    b.u8(0x03);
    b.u8(25);
    b.u16(0);
    b.u8(0);
    b.u8(0x04);
    b.u8(17);
    b.u8(0x40);
    b.u8(0x15);
    b.u24(0);
    b.u32(uint32_t(impl->bitrate));
    b.u32(uint32_t(impl->bitrate));
    b.u8(0x05);
    b.u8(2);
    b.u8((2 << 3) | (index >> 1));
    b.u8(((index & 1) << 7) | (impl->channels << 3));
    b.u8(0x06);
    b.u8(1);
    b.u8(0x02);
    return b.d;
}

}

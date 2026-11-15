#include "audio_codec.h"
#ifdef ABSENT_FAAD
#include "aac_config.h"
#include <neaacdec.h>

namespace absent {

namespace {

const int kOrder[9][8] = {
    {}, {FRONT_CHANNEL_CENTER}, {FRONT_CHANNEL_LEFT, FRONT_CHANNEL_RIGHT}, {FRONT_CHANNEL_LEFT, FRONT_CHANNEL_RIGHT, FRONT_CHANNEL_CENTER},
    {FRONT_CHANNEL_LEFT, FRONT_CHANNEL_RIGHT, FRONT_CHANNEL_CENTER, BACK_CHANNEL_CENTER},
    {FRONT_CHANNEL_LEFT, FRONT_CHANNEL_RIGHT, FRONT_CHANNEL_CENTER, BACK_CHANNEL_LEFT, BACK_CHANNEL_RIGHT},
    {FRONT_CHANNEL_LEFT, FRONT_CHANNEL_RIGHT, FRONT_CHANNEL_CENTER, LFE_CHANNEL, BACK_CHANNEL_LEFT, BACK_CHANNEL_RIGHT},
    {FRONT_CHANNEL_LEFT, FRONT_CHANNEL_RIGHT, FRONT_CHANNEL_CENTER, LFE_CHANNEL, BACK_CHANNEL_CENTER, SIDE_CHANNEL_LEFT, SIDE_CHANNEL_RIGHT},
    {FRONT_CHANNEL_LEFT, FRONT_CHANNEL_RIGHT, FRONT_CHANNEL_CENTER, LFE_CHANNEL, BACK_CHANNEL_LEFT, BACK_CHANNEL_RIGHT, SIDE_CHANNEL_LEFT, SIDE_CHANNEL_RIGHT}};

struct FaadAudio : PacketDecoder {
    NeAACDecHandle handle = nullptr;
    std::vector<uint8_t> specific;
    bool plainRate = false;

    ~FaadAudio() override {
        if (handle) NeAACDecClose(handle);
    }

    bool open() {
        if (handle) NeAACDecClose(handle);
        handle = NeAACDecOpen();
        if (!handle) return false;
        NeAACDecConfigurationPtr settings = NeAACDecGetCurrentConfiguration(handle);
        settings->outputFormat = FAAD_FMT_FLOAT;
        settings->downMatrix = 0;
        settings->dontUpSampleImplicitSBR = plainRate ? 1 : 0;
        NeAACDecSetConfiguration(handle, settings);
        unsigned long rateFound = 0;
        unsigned char channelsFound = 0;
        if (NeAACDecInit2(handle, specific.data(), (unsigned long)specific.size(), &rateFound, &channelsFound) < 0) return false;
        if (!rate) {
            rate = int(rateFound);
            channels = channelsFound;
        }
        return true;
    }

    void reset() override { open(); }

    void feed(const std::vector<uint8_t>& packet, std::vector<float>& out) override {
        if (!handle || packet.empty()) return;
        NeAACDecFrameInfo info = {};
        auto* pcm = static_cast<const float*>(NeAACDecDecode(handle, &info, const_cast<unsigned char*>(packet.data()), (unsigned long)packet.size()));
        if (info.error || !pcm || !info.samples || !info.channels) return;
        const size_t given = info.channels, frames = info.samples / given, wanted = size_t(channels);
        const size_t at = out.size();
        out.resize(at + frames * wanted, 0.0f);
        float* to = out.data() + at;
        int from[8];
        for (size_t c = 0; c < wanted; ++c) {
            from[c] = -1;
            if (given == wanted && wanted <= 2) from[c] = int(c);
            else if (wanted < 9) for (size_t k = 0; k < given; ++k) if (info.channel_position[k] == kOrder[wanted][c]) from[c] = int(k);
            if (from[c] < 0 && given == 1) from[c] = 0;
            if (from[c] < 0 && c < given) from[c] = int(c);
        }
        for (size_t f = 0; f < frames; ++f) {
            for (size_t c = 0; c < wanted; ++c) to[f * wanted + c] = from[c] >= 0 ? pcm[f * given + size_t(from[c])] : 0.0f;
        }
    }
};

}

std::unique_ptr<PacketDecoder> openFaadAudio(const Track& track, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    auto decoder = std::make_unique<FaadAudio>();
    if (track.configType == fourcc("ASC ")) decoder->specific = track.config;
    else if (track.configType == fourcc("esds") && track.config.size() > 4) {
        ElementaryStream stream;
        if (parseElementaryStream(View{track.config.data(), track.config.size()}, stream) && stream.specific.p) decoder->specific.assign(stream.specific.p, stream.specific.p + stream.specific.n);
    }
    AacConfig described;
    if (decoder->specific.empty() || !parseAacConfig(View{decoder->specific.data(), decoder->specific.size()}, described)) return fail("AAC track without a decoder configuration");
    decoder->plainRate = described.outputRate <= described.rate;
    if (!decoder->open()) return fail("AAC configuration not understood");
    if (described.outputRate > 0) decoder->rate = described.outputRate;
    if (described.outputChannels > 0) decoder->channels = described.outputChannels;
    if (decoder->channels < 1 || decoder->channels > 8) return fail("audio layout not supported");
    decoder->packetFrames = described.frameLength * (described.outputRate > described.rate ? 2 : 1);
    decoder->prerollPackets = 4;
    return decoder;
}

}
#else

namespace absent {

std::unique_ptr<PacketDecoder> openFaadAudio(const Track&, std::string* error) {
    if (error) *error = "built without an AAC software decoder";
    return nullptr;
}

}
#endif

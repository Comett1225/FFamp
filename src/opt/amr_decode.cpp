#include "../audio_codec.h"
#ifdef ABSENT_AMR
#include <opencore-amrnb/interf_dec.h>
#include <opencore-amrwb/dec_if.h>

namespace absent {

namespace {

const uint8_t kNarrowBytes[16] = {13, 14, 16, 18, 20, 21, 27, 32, 6, 1, 1, 1, 1, 1, 1, 1};
const uint8_t kWideBytes[16] = {18, 24, 33, 37, 41, 47, 51, 59, 61, 6, 6, 0, 0, 0, 1, 1};

struct AmrAudio : PacketDecoder {
    bool wide = false;
    void* state = nullptr;

    ~AmrAudio() override { close(); }

    void close() {
        if (!state) return;
        if (wide) D_IF_exit(state);
        else Decoder_Interface_exit(state);
        state = nullptr;
    }

    void reset() override {
        close();
        state = wide ? D_IF_init() : Decoder_Interface_init();
    }

    void feed(const std::vector<uint8_t>& packet, std::vector<float>& out) override {
        if (!state) return;
        short pcm[320];
        for (size_t at = 0; at < packet.size();) {
            const unsigned mode = (packet[at] >> 3) & 15;
            const size_t bytes = wide ? kWideBytes[mode] : kNarrowBytes[mode];
            if (!bytes || bytes > packet.size() - at) return;
            if (wide) D_IF_decode(state, packet.data() + at, pcm, 0);
            else Decoder_Interface_Decode(state, packet.data() + at, pcm, 0);
            for (int i = 0; i < (wide ? 320 : 160); i++) out.push_back(float(pcm[i]) / 32768.0f);
            at += bytes;
        }
    }
};

}

std::unique_ptr<PacketDecoder> openAmrAudio(const Track& track, std::string* error) {
    auto audio = std::make_unique<AmrAudio>();
    audio->wide = track.codec == fourcc("sawb");
    audio->rate = audio->wide ? 16000 : 8000;
    audio->channels = 1;
    audio->prerollPackets = 8;
    audio->reset();
    if (!audio->state) {
        if (error) *error = "the AMR decoder could not start";
        return nullptr;
    }
    return audio;
}

}
#else

namespace absent {

std::unique_ptr<PacketDecoder> openAmrAudio(const Track&, std::string* error) {
    if (error) *error = "AMR needs opencore-amr, which was not found when building";
    return nullptr;
}

}
#endif

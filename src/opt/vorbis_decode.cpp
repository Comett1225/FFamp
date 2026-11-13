#ifdef ABSENT_VORBIS
#include "../audio_codec.h"
#include "../read_common.h"
#include <vorbis/codec.h>

namespace absent {

namespace {

const uint8_t kWavOrder[9][8] = {{}, {0}, {0, 1}, {0, 2, 1}, {0, 1, 2, 3}, {0, 2, 1, 3, 4}, {0, 2, 1, 5, 3, 4}, {0, 2, 1, 6, 5, 3, 4}, {0, 2, 1, 7, 5, 6, 3, 4}};

struct VorbisAudio : PacketDecoder {
    vorbis_info info;
    vorbis_comment comment;
    vorbis_dsp_state state;
    vorbis_block block;
    bool running = false;
    int64_t number = 3;

    VorbisAudio() {
        vorbis_info_init(&info);
        vorbis_comment_init(&comment);
    }

    ~VorbisAudio() override {
        if (running) {
            vorbis_block_clear(&block);
            vorbis_dsp_clear(&state);
        }
        vorbis_comment_clear(&comment);
        vorbis_info_clear(&info);
    }

    ogg_packet wrap(const uint8_t* data, size_t size, bool first) {
        ogg_packet packet = {};
        packet.packet = const_cast<unsigned char*>(data);
        packet.bytes = long(size);
        packet.b_o_s = first ? 1 : 0;
        packet.granulepos = -1;
        packet.packetno = number++;
        return packet;
    }

    void reset() override { vorbis_synthesis_restart(&state); }

    void feed(const std::vector<uint8_t>& data, std::vector<float>& out) override {
        ogg_packet packet = wrap(data.data(), data.size(), false);
        if (vorbis_synthesis(&block, &packet) != 0 || vorbis_synthesis_blockin(&state, &block) != 0) return;
        float** lanes = nullptr;
        for (int frames; (frames = vorbis_synthesis_pcmout(&state, &lanes)) > 0; vorbis_synthesis_read(&state, frames)) {
            size_t at = out.size();
            out.resize(at + size_t(frames) * size_t(channels));
            for (int c = 0; c < channels; ++c) {
                const float* lane = lanes[channels <= 8 ? kWavOrder[channels][c] : c];
                for (int i = 0; i < frames; ++i) out[at + size_t(i) * size_t(channels) + size_t(c)] = lane[i];
            }
        }
    }
};

}

std::unique_ptr<PacketDecoder> openVorbisAudio(const Track& track, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    std::vector<View> headers;
    if (!splitXiph(track.config, headers) || headers.size() != 3) return fail("Vorbis track without its three headers");
    auto audio = std::make_unique<VorbisAudio>();
    for (size_t i = 0; i < 3; ++i) {
        ogg_packet packet = audio->wrap(headers[i].p, headers[i].n, i == 0);
        packet.packetno = long(i);
        if (vorbis_synthesis_headerin(&audio->info, &audio->comment, &packet) != 0) return fail("Vorbis headers refused");
    }
    if (vorbis_synthesis_init(&audio->state, &audio->info) != 0) return fail("Vorbis headers refused");
    vorbis_block_init(&audio->state, &audio->block);
    audio->running = true;
    audio->rate = int(audio->info.rate);
    audio->channels = audio->info.channels;
    audio->prerollPackets = 1;
    audio->longestPacket = int(vorbis_info_blocksize(&audio->info, 1) / 2);
    return audio;
}

}
#endif

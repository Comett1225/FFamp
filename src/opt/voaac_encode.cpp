#include "audio_codec.h"
#ifdef ABSENT_VOAAC
#include <vo-aacenc/cmnMemory.h>
#include <vo-aacenc/voAAC.h>

namespace absent {

namespace {

const size_t kFrame = 1024;
const int kDelay = 1600;

struct VoAac : AacCore {
    VO_AUDIO_CODECAPI api = {};
    VO_MEM_OPERATOR memory = {};
    VO_HANDLE handle = nullptr;
    int channels = 0;
    std::vector<int16_t> pending;
    std::vector<uint8_t> packet;
    int64_t fed = 0;
    int64_t made = 0;

    ~VoAac() override {
        if (handle) api.Uninit(handle);
    }
    const char* name() const override { return "vo-aacenc"; }

    bool frame(const int16_t* pcm, const AudioSink& sink) {
        VO_CODECBUFFER in = {};
        in.Buffer = reinterpret_cast<VO_PBYTE>(const_cast<int16_t*>(pcm));
        in.Length = VO_U32(kFrame * size_t(channels) * 2);
        if (api.SetInputData(handle, &in) != VO_ERR_NONE) return false;
        for (;;) {
            VO_CODECBUFFER out = {};
            out.Buffer = packet.data();
            out.Length = VO_U32(packet.size());
            VO_AUDIO_OUTPUTINFO info = {};
            const VO_U32 status = api.GetOutputData(handle, &out, &info);
            if (status == VO_ERR_INPUT_BUFFER_SMALL) return true;
            if (status != VO_ERR_NONE) return false;
            if (out.Length) {
                sink(packet.data(), out.Length);
                ++made;
            }
        }
    }

    bool encode(const int16_t* pcm, size_t frames, const AudioSink& sink) override {
        pending.insert(pending.end(), pcm, pcm + frames * size_t(channels));
        fed += int64_t(frames);
        const size_t step = kFrame * size_t(channels);
        size_t at = 0;
        for (; pending.size() - at >= step; at += step) if (!frame(pending.data() + at, sink)) return false;
        pending.erase(pending.begin(), pending.begin() + long(at));
        return true;
    }

    bool finish(const AudioSink& sink) override {
        const int64_t wanted = (fed + kDelay + int64_t(kFrame) - 1) / int64_t(kFrame);
        std::vector<int16_t> quiet(kFrame * size_t(channels), 0);
        if (!pending.empty()) {
            pending.resize(kFrame * size_t(channels), 0);
            if (!frame(pending.data(), sink)) return false;
            pending.clear();
        }
        for (int guard = 0; made < wanted && guard < 8; ++guard) if (!frame(quiet.data(), sink)) return false;
        return true;
    }
};

}

std::unique_ptr<AacCore> openVoAac(int rate, int channels, int bitrate, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    auto encoder = std::make_unique<VoAac>();
    voGetAACEncAPI(&encoder->api);
    encoder->memory.Alloc = cmnMemAlloc;
    encoder->memory.Copy = cmnMemCopy;
    encoder->memory.Free = cmnMemFree;
    encoder->memory.Set = cmnMemSet;
    encoder->memory.Check = cmnMemCheck;
    VO_CODEC_INIT_USERDATA user = {};
    user.memflag = VO_IMF_USERMEMOPERATOR;
    user.memData = &encoder->memory;
    if (encoder->api.Init(&encoder->handle, VO_AUDIO_CodingAAC, &user) != VO_ERR_NONE) {
        encoder->handle = nullptr;
        return fail("the AAC encoder did not start");
    }
    AACENC_PARAM settings = {};
    settings.sampleRate = rate;
    settings.bitRate = bitrate;
    settings.nChannels = short(channels);
    settings.adtsUsed = 0;
    if (encoder->api.SetParam(encoder->handle, VO_PID_AAC_ENCPARAM, &settings) != VO_ERR_NONE) return fail("the AAC encoder refused these settings");
    encoder->channels = channels;
    encoder->priming = kDelay;
    encoder->packet.resize(8192);
    return encoder;
}

}
#else

namespace absent {

std::unique_ptr<AacCore> openVoAac(int, int, int, std::string* error) {
    if (error) *error = "built without an AAC software encoder";
    return nullptr;
}

}
#endif

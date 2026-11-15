#include "../aac_config.h"
#include "../audio_codec.h"
#include "mf.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace absent {

namespace {

struct MfAudio : PacketDecoder {
    ComPtr<IMFTransform> transform;
    GUID input{};
    std::vector<uint8_t> userData;
    int sourceRate = 0;
    int sourceChannels = 0;
    bool floats = false;
    int64_t clock = 0;

    bool configure() {
        const MediaFoundation* mf = mediaFoundation();
        ComPtr<IMFMediaType> in;
        if (FAILED(mf->mediaType(&in))) return false;
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        in->SetGUID(MF_MT_SUBTYPE, input);
        in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, UINT32(sourceRate));
        in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, UINT32(sourceChannels));
        if (input == MFAudioFormat_AAC) {
            in->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
            in->SetBlob(MF_MT_USER_DATA, userData.data(), UINT32(userData.size()));
        }
        if (FAILED(transform->SetInputType(0, in.Get(), 0))) return false;
        for (int pass = 0; pass < 2; ++pass) {
            for (DWORD i = 0;; ++i) {
                ComPtr<IMFMediaType> out;
                if (FAILED(transform->GetOutputAvailableType(0, i, &out))) break;
                GUID subtype{};
                UINT32 bits = 0;
                out->GetGUID(MF_MT_SUBTYPE, &subtype);
                out->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
                const bool wanted = pass == 0 ? subtype == MFAudioFormat_Float : (subtype == MFAudioFormat_PCM && bits == 16);
                if (!wanted || FAILED(transform->SetOutputType(0, out.Get(), 0))) continue;
                UINT32 value = 0;
                if (SUCCEEDED(out->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &value))) rate = int(value);
                if (SUCCEEDED(out->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &value))) channels = int(value);
                floats = pass == 0;
                transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
                transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
                return true;
            }
        }
        return false;
    }

    void reset() override {
        transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        clock = 0;
    }

    void collect(std::vector<float>& out) {
        MFT_OUTPUT_STREAM_INFO info{};
        transform->GetOutputStreamInfo(0, &info);
        const bool provides = info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES);
        for (;;) {
            MFT_OUTPUT_DATA_BUFFER output{};
            ComPtr<IMFSample> own;
            if (!provides) {
                ComPtr<IMFMediaBuffer> buffer;
                if (FAILED(mediaFoundation()->memoryBuffer(std::max<DWORD>(info.cbSize, 1 << 16), &buffer)) || FAILED(mediaFoundation()->sample(&own))) return;
                own->AddBuffer(buffer.Get());
                output.pSample = own.Get();
            }
            DWORD status = 0;
            const HRESULT result = transform->ProcessOutput(0, 1, &output, &status);
            if (output.pEvents) output.pEvents->Release();
            if (FAILED(result)) return;
            ComPtr<IMFMediaBuffer> buffer;
            BYTE* data = nullptr;
            DWORD length = 0;
            if (output.pSample && SUCCEEDED(output.pSample->ConvertToContiguousBuffer(&buffer)) && SUCCEEDED(buffer->Lock(&data, nullptr, &length))) {
                const size_t at = out.size();
                if (floats) {
                    out.resize(at + length / 4);
                    memcpy(out.data() + at, data, length / 4 * 4);
                } else {
                    out.resize(at + length / 2);
                    for (size_t i = 0; i < length / 2; ++i) out[at + i] = float(int16_t(data[2 * i] | (data[2 * i + 1] << 8))) / 32768.0f;
                }
                buffer->Unlock();
            }
            if (provides && output.pSample) output.pSample->Release();
        }
    }

    void feed(const std::vector<uint8_t>& packet, std::vector<float>& out) override {
        if (packet.empty()) return;
        HRESULT result = S_OK;
        if (!sendSample(transform.Get(), packet.data(), packet.size(), clock, 0, true, &result) && result == MF_E_NOTACCEPTING) {
            collect(out);
            sendSample(transform.Get(), packet.data(), packet.size(), clock, 0, true);
        }
        clock += 1;
        collect(out);
    }

    void finish(std::vector<float>& out) override {
        transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        collect(out);
    }
};

struct MfAac : AacCore {
    ComPtr<IMFTransform> transform;
    int rate = 0;
    int channels = 0;
    int64_t clock = 0;
    bool started = false;

    const char* name() const override { return "mediafoundation"; }

    bool pull(const AudioSink& sink) {
        MFT_OUTPUT_STREAM_INFO info{};
        transform->GetOutputStreamInfo(0, &info);
        const bool provides = info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES);
        for (;;) {
            MFT_OUTPUT_DATA_BUFFER output{};
            ComPtr<IMFSample> own;
            if (!provides) {
                ComPtr<IMFMediaBuffer> buffer;
                if (FAILED(mediaFoundation()->memoryBuffer(std::max<DWORD>(info.cbSize, 8192), &buffer)) || FAILED(mediaFoundation()->sample(&own))) return false;
                own->AddBuffer(buffer.Get());
                output.pSample = own.Get();
            }
            DWORD status = 0;
            const HRESULT result = transform->ProcessOutput(0, 1, &output, &status);
            if (output.pEvents) output.pEvents->Release();
            if (result == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
            if (FAILED(result)) return false;
            ComPtr<IMFMediaBuffer> buffer;
            BYTE* data = nullptr;
            DWORD length = 0;
            if (output.pSample && SUCCEEDED(output.pSample->ConvertToContiguousBuffer(&buffer)) && SUCCEEDED(buffer->Lock(&data, nullptr, &length))) {
                if (length) sink(data, length);
                buffer->Unlock();
            }
            if (provides && output.pSample) output.pSample->Release();
        }
    }

    bool lead(const AudioSink& sink) {
        if (started) return true;
        started = true;
        const std::vector<int16_t> quiet(size_t(priming) * size_t(channels), 0);
        return encode(quiet.data(), size_t(priming), sink);
    }

    bool encode(const int16_t* pcm, size_t frames, const AudioSink& sink) override {
        if (!frames) return true;
        if (!lead(sink)) return false;
        const size_t bytes = frames * size_t(channels) * 2;
        const int64_t duration = int64_t(frames) * 10000000 / rate;
        HRESULT result = S_OK;
        if (!sendSample(transform.Get(), reinterpret_cast<const uint8_t*>(pcm), bytes, clock, duration, true, &result)) {
            if (result != MF_E_NOTACCEPTING || !pull(sink) || !sendSample(transform.Get(), reinterpret_cast<const uint8_t*>(pcm), bytes, clock, duration, true)) return false;
        }
        clock += duration;
        return pull(sink);
    }

    bool finish(const AudioSink& sink) override {
        if (!lead(sink)) return false;
        transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        return pull(sink);
    }
};

}

std::unique_ptr<PacketDecoder> openPlatformAudio(const Track& track, const std::vector<uint8_t>&, std::string* error) {
    if (!mediaFoundation() || std::getenv("FFAMP_NO_SYSTEM_AUDIO")) return nullptr;
    auto decoder = std::make_unique<MfAudio>();
    if (track.codec == fourcc("mp4a")) {
        std::vector<uint8_t> specific;
        if (track.configType == fourcc("ASC ")) specific = track.config;
        else if (track.configType == fourcc("esds") && track.config.size() > 4) {
            ElementaryStream stream;
            if (parseElementaryStream(View{track.config.data(), track.config.size()}, stream) && stream.specific.p) specific.assign(stream.specific.p, stream.specific.p + stream.specific.n);
        }
        AacConfig described;
        if (specific.empty() || !parseAacConfig(View{specific.data(), specific.size()}, described)) return nullptr;
        decoder->input = MFAudioFormat_AAC;
        decoder->sourceRate = described.rate;
        decoder->sourceChannels = described.channels;
        decoder->userData = {0, 0, 0xFE, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        decoder->userData.insert(decoder->userData.end(), specific.begin(), specific.end());
        decoder->packetFrames = described.frameLength * (described.outputRate > described.rate ? 2 : 1);
        decoder->prerollPackets = 4;
        decoder->lead = decoder->packetFrames;
    } else {
        return nullptr;
    }
    decoder->transform = findTransform(MFT_CATEGORY_AUDIO_DECODER, MFMediaType_Audio, decoder->input, GUID_NULL, false, nullptr);
    if (!decoder->transform || !decoder->configure()) {
        if (error) *error = "the system audio decoder refused this stream";
        return nullptr;
    }
    return decoder;
}

std::unique_ptr<AacCore> openPlatformAac(int rate, int channels, int bitrate, std::string* error) {
    if (!mediaFoundation() || std::getenv("FFAMP_NO_SYSTEM_AUDIO")) return nullptr;
    static const int allowed[] = {96000, 128000, 160000, 192000};
    if ((rate != 44100 && rate != 48000) || std::find(std::begin(allowed), std::end(allowed), bitrate) == std::end(allowed)) {
        if (error) *error = "the system AAC encoder takes only 44.1 or 48 kHz at 96, 128, 160 or 192 kbit/s";
        return nullptr;
    }
    auto encoder = std::make_unique<MfAac>();
    encoder->rate = rate;
    encoder->channels = channels;
    encoder->transform = findTransform(MFT_CATEGORY_AUDIO_ENCODER, MFMediaType_Audio, MFAudioFormat_PCM, MFAudioFormat_AAC, false, nullptr);
    if (!encoder->transform) return nullptr;
    const MediaFoundation* mf = mediaFoundation();
    ComPtr<IMFMediaType> out, in;
    if (FAILED(mf->mediaType(&out)) || FAILED(mf->mediaType(&in))) return nullptr;
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, UINT32(rate));
    out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, UINT32(channels));
    out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, UINT32(bitrate / 8));
    out->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, UINT32(rate));
    in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, UINT32(channels));
    in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, UINT32(channels * 2));
    in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, UINT32(rate * channels * 2));
    if (FAILED(encoder->transform->SetOutputType(0, out.Get(), 0)) || FAILED(encoder->transform->SetInputType(0, in.Get(), 0))) {
        if (error) *error = "the system AAC encoder refused these settings";
        return nullptr;
    }
    encoder->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    encoder->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    encoder->priming = 1024;
    return encoder;
}

}

#pragma once
#include "absent/media.h"

namespace absent {

struct PacketDecoder {
    int rate = 0;
    int channels = 0;
    int packetFrames = 0;
    int longestPacket = 0;
    int prerollPackets = 0;
    int prerollFrames = 0;
    int64_t lead = 0;
    int64_t skip = 0;
    bool stateless = false;
    const char* refusal = nullptr;
    virtual ~PacketDecoder() = default;
    virtual void reset() = 0;
    virtual void feed(const std::vector<uint8_t>& packet, std::vector<float>& out) = 0;
    virtual void finish(std::vector<float>&) {}
    virtual void skim(const std::vector<uint8_t>& packet) {
        std::vector<float> none;
        feed(packet, none);
    }
    virtual bool counts(Reader&, const Track&, std::vector<int64_t>&) { return false; }
    virtual bool save(std::vector<uint8_t>&) const { return false; }
    virtual void load(const std::vector<uint8_t>&) {}
};

struct AacCore {
    int priming = 0;
    virtual ~AacCore() = default;
    virtual const char* name() const = 0;
    virtual bool encode(const int16_t* pcm, size_t frames, const AudioSink& sink) = 0;
    virtual bool finish(const AudioSink& sink) = 0;
};

std::unique_ptr<AacCore> openVoAac(int rate, int channels, int bitrate, std::string* error);
std::unique_ptr<PacketDecoder> openFlacAudio(const Track& track, std::string* error);
std::unique_ptr<PacketDecoder> openAlacAudio(const Track& track, std::string* error);
std::unique_ptr<PacketDecoder> openFaadAudio(const Track& track, std::string* error);
std::unique_ptr<PacketDecoder> openMpg123Audio(const Track& track, const std::vector<uint8_t>& firstPacket, std::string* error);
std::unique_ptr<PacketDecoder> openSystemAudio(const Track& track, const std::vector<uint8_t>& firstPacket, std::string* error);
std::unique_ptr<PacketDecoder> openOpusAudio(const Track& track, std::string* error);
std::unique_ptr<PacketDecoder> openVorbisAudio(const Track& track, std::string* error);
std::unique_ptr<PacketDecoder> openWmaAudio(const Track& track, std::string* error);
std::unique_ptr<PacketDecoder> openWmaProAudio(const Track& track, std::string* error);
std::unique_ptr<PacketDecoder> openWmaLosslessAudio(const Track& track, std::string* error);
std::unique_ptr<PacketDecoder> openWmaVoiceAudio(const Track& track, std::string* error);
std::unique_ptr<PacketDecoder> openWaveAudio(const Track& track, std::string* error);
std::unique_ptr<PacketDecoder> openTtaAudio(const Track& track, std::string* error);
std::unique_ptr<PacketDecoder> openAc3Audio(const Track& track, const std::vector<uint8_t>& firstPacket, std::string* error);

}

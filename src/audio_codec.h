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
    bool stateless = false;
    virtual ~PacketDecoder() = default;
    virtual void reset() = 0;
    virtual void feed(const std::vector<uint8_t>& packet, std::vector<float>& out) = 0;
    virtual void finish(std::vector<float>&) {}
};

std::unique_ptr<PacketDecoder> openSystemAudio(const Track& track, const std::vector<uint8_t>& firstPacket, std::string* error);
std::unique_ptr<PacketDecoder> openOpusAudio(const Track& track, std::string* error);

}

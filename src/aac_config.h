#pragma once
#include "bytes.h"

namespace absent {

struct AacConfig {
    int objectType = 0;
    int rate = 0;
    int channels = 0;
    int channelConfiguration = 0;
    int outputRate = 0;
    int outputChannels = 0;
    int frameLength = 1024;
    bool spectralBandReplication = false;
    bool parametricStereo = false;
};

struct ElementaryStream {
    int objectTypeIndication = 0;
    View specific;
};

bool parseElementaryStream(View esdsBody, ElementaryStream& out);
bool parseAacConfig(View specific, AacConfig& out);
bool aacConfigOf(uint32_t configType, const std::vector<uint8_t>& config, AacConfig& out);

}

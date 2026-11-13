#pragma once
#include "absent/media.h"

namespace absent {

std::unique_ptr<Reader> openMatroska(const std::string& path, std::string* error);
std::unique_ptr<Reader> openOgg(const std::string& path, std::string* error);
std::unique_ptr<Reader> openWave(const std::string& path, std::string* error);
std::unique_ptr<Reader> openRawAudio(const std::string& path, std::string* error);
std::unique_ptr<Reader> openTransportStream(const std::string& path, std::string* error);
std::unique_ptr<Reader> openIvf(const std::string& path, std::string* error);
std::unique_ptr<Reader> openAvi(const std::string& path, std::string* error);
std::unique_ptr<Reader> openCaf(const std::string& path, std::string* error);
std::unique_ptr<Reader> openAsf(const std::string& path, std::string* error);
const char* windowsMediaName(uint32_t codec);
int vc1PictureType(uint32_t codec, const std::vector<uint8_t>& config, const uint8_t* sample, size_t size);
inline bool vc1Codec(uint32_t codec) { return codec == fourcc("WMV3") || codec == fourcc("WVC1") || codec == fourcc("WMVA"); }

struct DolbyFrame {
    uint32_t codec = 0;
    int rate = 0;
    int channels = 0;
    int mode = 0;
    bool lfe = false;
    bool dependent = false;
    uint32_t bytes = 0;
    uint32_t samples = 0;
};

bool dolbyFrame(const uint8_t* header, DolbyFrame& out);
uint32_t amrFrameBytes(bool wide, uint8_t first);
std::vector<uint8_t> mpeg4Descriptor(const uint8_t* headers, size_t size);
uint32_t videoFourcc(uint32_t handler, int height);

}

#pragma once
#include "absent/media.h"

namespace absent {

std::unique_ptr<Writer> openOggWriter(const std::string& path, std::string* error);

class Mp3File {
public:
    static std::unique_ptr<Mp3File> open(const std::string& path, int rate, int channels, int bitrate, std::string* error);
    virtual ~Mp3File() = default;
    virtual bool write(const float* pcm, size_t frames) = 0;
    virtual bool finish() = 0;
};

}

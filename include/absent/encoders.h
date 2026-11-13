#pragma once
#include "absent/media.h"

namespace absent {

struct EncoderInfo {
    std::string name;
    uint32_t codec = 0;
    Kind kind = Kind::Video;
    bool hardware = false;
};

std::vector<EncoderInfo> encoders();
bool encoderWorks(const EncodeSpec& spec, std::string* error = nullptr);

}

#pragma once
#include "absent/media.h"

namespace absent::android {

std::string mime(const Track& track);
std::vector<std::vector<uint8_t>> codecConfig(const Track& track);
int nalLengthSize(const Track& track);
size_t annexB(const uint8_t* in, size_t size, int lengthSize, uint8_t* out, size_t capacity);

struct SoftPicture {
    std::vector<uint8_t> i420;
    int width = 0;
    int height = 0;
    int64_t stamp = 0;
};

class SoftVideo {
public:
    static std::unique_ptr<SoftVideo> open(const Track& track, std::string* error = nullptr);
    virtual ~SoftVideo() = default;
    virtual const char* name() const = 0;
    virtual bool decode(const uint8_t* data, size_t size, int64_t stamp, std::vector<SoftPicture>& out) = 0;
    virtual void drain(std::vector<SoftPicture>& out) = 0;
    virtual void reset() = 0;
};

}

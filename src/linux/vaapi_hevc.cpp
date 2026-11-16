#ifdef ABSENT_VAAPI
#include "vaapi.h"

namespace absent {

std::unique_ptr<SoftDecoder> openVaapiHevc(const std::vector<uint8_t>&, std::string* error) {
    if (error) *error = "HEVC is not decoded on the GPU yet";
    return nullptr;
}

}
#endif

#ifdef ABSENT_VAAPI
#include "vaapi.h"

namespace absent {

std::unique_ptr<SoftDecoder> openVaapiH264(const std::vector<uint8_t>&, std::string* error) {
    if (error) *error = "H.264 is not decoded on the GPU yet";
    return nullptr;
}

}
#endif

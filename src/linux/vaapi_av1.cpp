#ifdef ABSENT_VAAPI
#include "vaapi.h"

namespace absent {

std::unique_ptr<SoftDecoder> openVaapiAv1(const std::vector<uint8_t>&, const StreamShape&, int, int, std::string* error) {
    if (error) *error = "AV1 is not decoded on the GPU yet";
    return nullptr;
}

}
#endif

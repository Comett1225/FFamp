#include "readers.h"

namespace absent {

std::unique_ptr<Reader> openWave(const std::string&, std::string* error) {
    if (error) *error = "WAV and AIFF files are not supported yet";
    return nullptr;
}

}

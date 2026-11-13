#include "readers.h"

namespace absent {

std::unique_ptr<Reader> openTransportStream(const std::string&, std::string* error) {
    if (error) *error = "MPEG transport stream files are not supported yet";
    return nullptr;
}

}

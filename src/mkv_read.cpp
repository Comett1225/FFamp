#include "readers.h"

namespace absent {

std::unique_ptr<Reader> openMatroska(const std::string&, std::string* error) {
    if (error) *error = "Matroska and WebM files are not supported yet";
    return nullptr;
}

}

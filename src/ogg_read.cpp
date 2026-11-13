#include "readers.h"

namespace absent {

std::unique_ptr<Reader> openOgg(const std::string&, std::string* error) {
    if (error) *error = "Ogg files are not supported yet";
    return nullptr;
}

}

#include "readers.h"

namespace absent {

std::unique_ptr<Reader> openRawAudio(const std::string&, std::string* error) {
    if (error) *error = "MP3, ADTS AAC and FLAC files are not supported yet";
    return nullptr;
}

}

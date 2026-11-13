#pragma once
#include "absent/media.h"

namespace absent {

std::unique_ptr<Reader> openMatroska(const std::string& path, std::string* error);
std::unique_ptr<Reader> openOgg(const std::string& path, std::string* error);
std::unique_ptr<Reader> openWave(const std::string& path, std::string* error);
std::unique_ptr<Reader> openRawAudio(const std::string& path, std::string* error);
std::unique_ptr<Reader> openTransportStream(const std::string& path, std::string* error);

}

#pragma once
#include <string>

namespace absent {

std::string probeJson(const std::string& path, bool packets, std::string* error = nullptr);

}

#pragma once
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

#ifdef _WIN32
#define fseeko _fseeki64
#define ftello _ftelli64
#endif

namespace absent {

FILE* openFile(const std::string& path, const char* mode);
void removeFile(const std::string& path);
bool renameFile(const std::string& from, const std::string& to);
int64_t fileBytes(const std::string& path);
bool utcParts(time_t seconds, struct tm& parts);

}

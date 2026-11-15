#include "portable.h"
#ifdef _WIN32
#include <sys/stat.h>
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace absent {

#ifdef _WIN32
namespace {

std::wstring wide(const std::string& text) {
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring out(size_t(count > 0 ? count : 0), L'\0');
    if (count > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), count);
    return out;
}

}

FILE* openFile(const std::string& path, const char* mode) {
    const std::wstring how = wide(mode);
    return _wfopen(wide(path).c_str(), how.c_str());
}

void removeFile(const std::string& path) { _wremove(wide(path).c_str()); }

int64_t fileBytes(const std::string& path) {
    struct _stat64 status;
    return _wstat64(wide(path).c_str(), &status) == 0 ? int64_t(status.st_size) : 0;
}

bool utcParts(time_t seconds, struct tm& parts) { return gmtime_s(&parts, &seconds) == 0; }
#else
FILE* openFile(const std::string& path, const char* mode) { return fopen(path.c_str(), mode); }

void removeFile(const std::string& path) { remove(path.c_str()); }

int64_t fileBytes(const std::string& path) {
    struct stat status;
    return stat(path.c_str(), &status) == 0 ? int64_t(status.st_size) : 0;
}

bool utcParts(time_t seconds, struct tm& parts) { return gmtime_r(&seconds, &parts) != nullptr; }
#endif

}

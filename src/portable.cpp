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
    static const int streams = _getmaxstdio() < 8192 ? _setmaxstdio(8192) : _getmaxstdio();
    (void)streams;
    const std::wstring how = wide(mode);
    return _wfopen(wide(path).c_str(), how.c_str());
}

void removeFile(const std::string& path) { _wremove(wide(path).c_str()); }

bool renameFile(const std::string& from, const std::string& to) { return MoveFileExW(wide(from).c_str(), wide(to).c_str(), MOVEFILE_REPLACE_EXISTING) != 0; }

bool sameFile(const std::string& a, const std::string& b) {
    if (a == b) return true;
    BY_HANDLE_FILE_INFORMATION info[2]{};
    const std::string* paths[2] = {&a, &b};
    for (int i = 0; i < 2; ++i) {
        HANDLE handle = CreateFileW(wide(*paths[i]).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return false;
        const bool known = GetFileInformationByHandle(handle, &info[i]) != 0;
        CloseHandle(handle);
        if (!known) return false;
    }
    return info[0].dwVolumeSerialNumber == info[1].dwVolumeSerialNumber && info[0].nFileIndexHigh == info[1].nFileIndexHigh && info[0].nFileIndexLow == info[1].nFileIndexLow;
}

int64_t fileBytes(const std::string& path) {
    struct _stat64 status;
    return _wstat64(wide(path).c_str(), &status) == 0 ? int64_t(status.st_size) : 0;
}

bool utcParts(time_t seconds, struct tm& parts) { return gmtime_s(&parts, &seconds) == 0; }
#else
FILE* openFile(const std::string& path, const char* mode) { return fopen(path.c_str(), mode); }

void removeFile(const std::string& path) { remove(path.c_str()); }

bool renameFile(const std::string& from, const std::string& to) { return rename(from.c_str(), to.c_str()) == 0; }

bool sameFile(const std::string& a, const std::string& b) {
    struct stat first, second;
    return a == b || (stat(a.c_str(), &first) == 0 && stat(b.c_str(), &second) == 0 && first.st_dev == second.st_dev && first.st_ino == second.st_ino);
}

int64_t fileBytes(const std::string& path) {
    struct stat status;
    return stat(path.c_str(), &status) == 0 ? int64_t(status.st_size) : 0;
}

bool utcParts(time_t seconds, struct tm& parts) { return gmtime_r(&seconds, &parts) != nullptr; }
#endif

}

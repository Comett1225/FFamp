#include "cli.h"
#include <cstdio>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <shellapi.h>
#endif

namespace cli {

std::map<std::string, Command>& commands() {
    static std::map<std::string, Command> all;
    return all;
}

}

int main(int argc, char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
    std::vector<std::string> utf8;
    std::vector<char*> pointers;
    int count = 0;
    if (LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count)) {
        for (int i = 0; i < count; ++i) {
            const int size = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
            std::string text(size_t(size > 0 ? size - 1 : 0), '\0');
            if (size > 1) WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, text.data(), size, nullptr, nullptr);
            utf8.push_back(std::move(text));
        }
        LocalFree(wide);
        for (std::string& text : utf8) pointers.push_back(text.data());
        pointers.push_back(nullptr);
        argc = count;
        argv = pointers.data();
    }
#endif
    auto& all = cli::commands();
    if (argc < 2 || !all.count(argv[1])) {
        std::string names;
        for (auto& item : all) names += (names.empty() ? "" : ", ") + item.first;
        return cli::fail(argc < 2 ? "commands: " + names : "unknown command " + std::string(argv[1]) + "; commands: " + names);
    }
    return all[argv[1]](cli::parse(argc, argv));
}

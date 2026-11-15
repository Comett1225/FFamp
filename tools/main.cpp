#include "cli.h"
#include <cstdio>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
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
#endif
    auto& all = cli::commands();
    if (argc < 2 || !all.count(argv[1])) {
        std::string names;
        for (auto& item : all) names += (names.empty() ? "" : ", ") + item.first;
        return cli::fail(argc < 2 ? "commands: " + names : "unknown command " + std::string(argv[1]) + "; commands: " + names);
    }
    return all[argv[1]](cli::parse(argc, argv));
}

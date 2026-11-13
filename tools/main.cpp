#include "cli.h"
#include <cstdio>

namespace cli {

std::map<std::string, Command>& commands() {
    static std::map<std::string, Command> all;
    return all;
}

}

int main(int argc, char** argv) {
    auto& all = cli::commands();
    if (argc < 2 || !all.count(argv[1])) {
        std::string names;
        for (auto& item : all) names += (names.empty() ? "" : ", ") + item.first;
        return cli::fail(argc < 2 ? "commands: " + names : "unknown command " + std::string(argv[1]) + "; commands: " + names);
    }
    return all[argv[1]](cli::parse(argc, argv));
}

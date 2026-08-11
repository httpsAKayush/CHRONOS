#include "chronos/cli/commands.hpp"
#include <filesystem>
#include <iostream>
#include <thread>
#include <chrono>

namespace fs = std::filesystem;

namespace chronos {

CmdClean::CmdClean(const CliContext& ctx) : ctx_(ctx) {}

int CmdClean::execute(int argc, char** argv) {
    bool force = false;
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--force") force = true;
    }

    if (system("pgrep -f chronos-daemon > /dev/null") == 0) {
        std::cerr << "[!] chronos-daemon is currently running. Stopping daemon...\n";
        system("pkill -f chronos-daemon");
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    if (!force) {
        std::cout << "[!] This will destroy the local index. Are you sure? (y/N) ";
        std::string ans;
        std::getline(std::cin, ans);
        if (ans != "y" && ans != "Y") {
            std::cout << "Aborted.\n";
            return 0;
        }
    }

    fs::path chronosDir = fs::path(ctx_.repoRoot) / ".chronos";
    if (fs::exists(chronosDir)) {
        fs::remove_all(chronosDir);
        std::cout << "Cleaned " << chronosDir << "\n";
    } else {
        std::cout << ".chronos directory not found.\n";
    }

    return 0;
}

} // namespace chronos
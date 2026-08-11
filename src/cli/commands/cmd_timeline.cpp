#include "chronos/cli/commands.hpp"
#include "chronos/cli/cli_util.hpp"
#include <iostream>

namespace chronos {

CmdTimeline::CmdTimeline(const CliContext& ctx) : ctx_(ctx) {}

int CmdTimeline::execute(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: chronos timeline <target>\n";
        return 2;
    }
    Codex& codex = *ctx_.storage;
    std::string target = argv[2];
    std::string rootId = codex.resolveAlias(target);
    auto history = codex.getHistory(rootId);

    if (history.empty()) {
        std::cerr << "chronos timeline: No history found for target '" << target << "'\n";
        return 1;
    }

    std::cout << "Temporal Timeline for " << target << " (" << rootId << ")\n";
    std::cout << "--------------------------------------------------------\n";
    for (const auto& rec : history) {
        std::string cmd = "git -C " + ctx_.repoRoot + " show -s --format=\"%h %cd: %s\" --date=short " + rec.commitHash;
        std::system(cmd.c_str());
        if (!rec.syntheticMsg.empty()) {
            std::cout << "  [AI]: " << rec.syntheticMsg << "\n";
        }
        std::cout << "--------------------------------------------------------\n";
    }
    return 0;
}

} // namespace chronos
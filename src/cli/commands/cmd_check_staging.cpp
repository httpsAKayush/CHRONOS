#include "chronos/cli/commands.hpp"
#include "chronos/cli/cli_util.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <map>
#include <set>
#include <tuple>
#include <algorithm>

namespace fs = std::filesystem;

namespace chronos {

CmdCheckStaging::CmdCheckStaging(const CliContext& ctx) : ctx_(ctx) {}

int CmdCheckStaging::execute(int argc, char** argv) {
    std::string targetRepo = ctx_.repoRoot;
    bool strict = false;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--strict") {
            strict = true;
        } else if (arg[0] != '-') {
            targetRepo = arg;
        }
    }

    fs::path chronosDb = fs::path(targetRepo) / ".chronos" / "codex.db";
    if (!fs::exists(chronosDb)) {
        return 0;
    }

    std::string diffCmd = "git -C \"" + targetRepo + "\" diff --cached -U3 2>/dev/null";
    std::string diffOutput = execCmdOutput(diffCmd);
    if (diffOutput.empty()) {
        return 0;
    }

    std::map<std::string, std::vector<std::string>> stagedAdditions;
    std::stringstream ss(diffOutput);
    std::string line;
    std::string currentFile;

    while (std::getline(ss, line)) {
        if (line.rfind("diff --git", 0) == 0) {
            currentFile.clear();
        } else if (line.rfind("+++ ", 0) == 0) {
            std::string path = line.substr(4);
            if (path.rfind("b/", 0) == 0) {
                path = path.substr(2);
            }
            if (path != "/dev/null") {
                if (!path.empty() && path.back() == '\r') path.pop_back();
                currentFile = path;
            } else {
                currentFile.clear();
            }
        } else if (!currentFile.empty() && !line.empty() && line[0] == '+' && line.rfind("+++", 0) != 0) {
            if (line.back() == '\r') line.pop_back();
            stagedAdditions[currentFile].push_back(line);
        }
    }

    Codex codex(targetRepo);
    int collisionCount = 0;
    std::set<std::tuple<std::string, std::string, std::string>> reported;

    for (const auto& [filePath, addedLines] : stagedAdditions) {
        auto history = codex.getHistoryForFile(filePath);
        if (history.empty()) {
            std::string normPath = fs::path(filePath).lexically_normal().string();
            if (normPath != filePath) {
                history = codex.getHistoryForFile(normPath);
            }
        }

        if (history.empty()) continue;

        for (const auto& stagedLine : addedLines) {
            for (const auto& rec : history) {
                if (rec.syntheticMsg.empty()) continue;
                if (checkStagingCollision(stagedLine, rec.syntheticMsg)) {
                    std::tuple<std::string, std::string, std::string> key = {filePath, stagedLine, rec.commitHash};
                    if (reported.count(key)) continue;
                    reported.insert(key);

                    std::string shortHash = rec.commitHash.substr(0, std::min<size_t>(7, rec.commitHash.length()));

                    std::cout << "================================================================================\n";
                    std::cout << "[TEMPORAL COLLISION WARNING]\n";
                    std::cout << "File: " << filePath << "\n";
                    std::cout << "Staged Modification:\n";
                    std::cout << "  " << stagedLine << "\n";
                    std::cout << "Historical Constraint Violation:\n";
                    std::cout << "  - Commit " << shortHash << ": \"" << rec.syntheticMsg << "\"\n";
                    std::cout << "Action: Please review historical constraint before committing.\n";
                    std::cout << "================================================================================\n";

                    collisionCount++;
                }
            }
        }
    }

    if (strict && collisionCount > 0) {
        return 1;
    }
    return 0;
}

} // namespace chronos
#include "chronos/cli/commands.hpp"
#include "chronos/cli/cli_util.hpp"
#include "chronos/infrastructure/config.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdlib>

namespace fs = std::filesystem;

namespace chronos {

CmdStatus::CmdStatus(const CliContext& ctx) : ctx_(ctx) {}

int CmdStatus::execute(int argc, char** argv) {
    (void)argc; (void)argv;
    const std::string& repoRoot = ctx_.repoRoot;

    fs::path codexPath = fs::path(repoRoot) / ".chronos" / "codex.db";
    if (!fs::exists(codexPath)) {
        std::cerr << "Index not found.\n";
        return 1;
    }
    auto sizeBytes = fs::file_size(codexPath);
    double sizeMB = static_cast<double>(sizeBytes) / (1024.0 * 1024.0);

    Codex& codex = *ctx_.storage;
    int nodeCount = 0, edgeCount = 0;
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(codex.raw(), "SELECT COUNT(*) FROM nodes", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) nodeCount = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    if (sqlite3_prepare_v2(codex.raw(), "SELECT COUNT(*) FROM edges", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) edgeCount = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }

    std::string commitHash = "unknown";
    fs::path headPath = fs::path(repoRoot) / ".git" / "HEAD";
    if (fs::exists(headPath)) {
        std::ifstream in(headPath);
        std::string line;
        if (std::getline(in, line)) {
            if (line.rfind("ref: ", 0) == 0) {
                std::string refPath = line.substr(5);
                fs::path refFile = fs::path(repoRoot) / ".git" / refPath;
                if (fs::exists(refFile)) {
                    std::ifstream refIn(refFile);
                    if (std::getline(refIn, commitHash)) {
                        commitHash = commitHash.substr(0, 8);
                    }
                }
            } else {
                commitHash = line.substr(0, 8);
            }
        }
    }

    std::string statusStr = "SLEEPING";
    int pid = 0;
    if (system("pgrep -f chronos-daemon > /dev/null") == 0) {
        statusStr = "RUNNING";
        FILE* pipe = popen("pgrep -f chronos-daemon", "r");
        if (pipe) {
            char buf[128];
            if (fgets(buf, sizeof(buf), pipe)) {
                pid = std::stoi(buf);
            }
            pclose(pipe);
        }
    }

    fs::path vectorPath = fs::path(repoRoot) / ".chronos" / "vectors.bin";
    double vectorSizeMB = 0;
    if (fs::exists(vectorPath)) {
        vectorSizeMB = static_cast<double>(fs::file_size(vectorPath)) / (1024.0 * 1024.0);
    }
    size_t embedCount = ctx_.vectors ? ctx_.vectors->size() : 0;

    int historyDepth = 0;
    std::string gitCmd = "git -C \"" + repoRoot + "\" rev-list --count HEAD 2>/dev/null";
    FILE* gpipe = popen(gitCmd.c_str(), "r");
    if (gpipe) {
        char buf[128];
        if (fgets(buf, sizeof(buf), gpipe)) {
            historyDepth = std::stoi(buf);
        }
        pclose(gpipe);
    }

    int keyframes = 0;
    if (sqlite3_prepare_v2(codex.raw(), "SELECT COUNT(DISTINCT commit_hash) FROM history", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) keyframes = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }

    std::string activeLLM = "llama3 (local)";
    std::string llmMode = "auto";
    if (ctx_.config) {
        auto cfg = static_cast<Config&>(*ctx_.config);
        std::string model = cfg.model();
        if (!model.empty()) activeLLM = model;
        llmMode = cfg.providerMode();
    }

    std::cout << "[ CHRONOS SYSTEM STATUS ]\n\n";

    std::cout << "--- Structural Graph (codex.db) ---\n";
    std::cout << "Database Size : " << sizeMB << " MB\n";
    std::cout << "Graph Nodes   : " << nodeCount << "\n";
    std::cout << "Graph Edges   : " << edgeCount << "\n\n";

    std::cout << "--- Semantic Index (vectors.bin) ---\n";
    std::cout << "Vector Size   : " << vectorSizeMB << " MB\n";
    std::cout << "Embeddings    : " << embedCount << "\n\n";

    std::cout << "--- Temporal Index ---\n";
    std::cout << "History Depth : " << historyDepth << " commits indexed\n";
    std::cout << "Keyframes     : " << keyframes << " structural mutations mapped\n\n";

    std::cout << "--- System Health ---\n";
    if (pid > 0) {
        std::cout << "Daemon State  : RUNNING (PID " << pid << ")\n";
    } else {
        std::cout << "Daemon State  : " << statusStr << "\n";
    }
    std::cout << "Synced Commit : " << commitHash << "\n";
    std::cout << "LLM Mode      : " << llmMode << " (auto = cloud first, local fallback)\n";
    std::cout << "Active LLM    : " << activeLLM << "\n";
    return 0;
}

} // namespace chronos
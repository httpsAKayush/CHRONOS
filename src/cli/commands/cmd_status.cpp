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
        std::cerr << "Index not found. Run `chronos init` first.\n";
        return 1;
    }
    auto sizeBytes = fs::file_size(codexPath);
    double sizeMB = static_cast<double>(sizeBytes) / (1024.0 * 1024.0);

    Codex& codex = *ctx_.storage;
    int nodeCount = 0, edgeCount = 0, functionCount = 0, contextCount = 0;
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(codex.raw(), "SELECT COUNT(*) FROM nodes WHERE is_active=1", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) nodeCount = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    if (sqlite3_prepare_v2(codex.raw(), "SELECT COUNT(*) FROM edges", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) edgeCount = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    if (sqlite3_prepare_v2(codex.raw(), "SELECT COUNT(*) FROM nodes WHERE is_active=1 AND parse_confidence > 0.0", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) functionCount = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    if (sqlite3_prepare_v2(codex.raw(), "SELECT COUNT(*) FROM nodes WHERE is_active=1 AND kind = 'context'", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) contextCount = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }

    int activeFiles = 0;
    if (sqlite3_prepare_v2(codex.raw(), "SELECT COUNT(DISTINCT file_path) FROM nodes WHERE is_active=1", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) activeFiles = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }

    std::string commitHash = "unknown";
    std::string branchName = "detached";
    fs::path headPath = fs::path(repoRoot) / ".git" / "HEAD";
    if (fs::exists(headPath)) {
        std::ifstream in(headPath);
        std::string line;
        if (std::getline(in, line)) {
            if (line.rfind("ref: refs/heads/", 0) == 0) {
                branchName = line.substr(16);
                fs::path refFile = fs::path(repoRoot) / ".git" / line.substr(5);
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

    std::string activeLLM = "none";
    std::string llmMode = "auto";
    std::string cloudUrl = "not configured";
    std::string localUrl = "not configured";
    bool cloudConfigured = false;
    bool localConfigured = false;
    if (ctx_.config) {
        auto cfg = static_cast<Config&>(*ctx_.config);
        std::string model = cfg.model();
        if (!model.empty()) activeLLM = model;
        llmMode = cfg.providerMode();
        cloudUrl = cfg.cloudUrl();
        localUrl = cfg.localUrl();
        cloudConfigured = cfg.cloudConfigured();
        localConfigured = cfg.localConfigured();
    }

    std::cout << R"(
  ┌──────────────────────────────────────────────────────────────────────────┐
  │  CHRONOS SYSTEM STATUS                                                  │
  └──────────────────────────────────────────────────────────────────────────┘)";

    std::cout << "\n\n  ┌─ Structural Graph ──────────────────────────────────────────────────────\n";
    std::cout << "  │ Database    : " << sizeMB << " MB\n";
    std::cout << "  │ Nodes       : " << nodeCount << " (" << functionCount << " functions, " << contextCount << " context)\n";
    std::cout << "  │ Edges       : " << edgeCount << "\n";
    std::cout << "  │ Files       : " << activeFiles << " indexed\n";
    std::cout << "  └──────────────────────────────────────────────────────────────────────\n";

    std::cout << "\n  ┌─ Semantic Index ──────────────────────────────────────────────────────\n";
    std::cout << "  │ Vector DB   : " << vectorSizeMB << " MB\n";
    std::cout << "  │ Embeddings  : " << embedCount << "\n";
    std::cout << "  └──────────────────────────────────────────────────────────────────────\n";

    std::cout << "\n  ┌─ Temporal Index ─────────────────────────────────────────────────────\n";
    std::cout << "  │ Commits     : " << historyDepth << " indexed\n";
    std::cout << "  │ Keyframes   : " << keyframes << " structural mutations\n";
    std::cout << "  └──────────────────────────────────────────────────────────────────────\n";

    std::cout << "\n  ┌─ Git ─────────────────────────────────────────────────────────────────\n";
    std::cout << "  │ Branch      : " << branchName << "\n";
    std::cout << "  │ HEAD        : " << commitHash << "\n";
    std::cout << "  └──────────────────────────────────────────────────────────────────────\n";

    std::cout << "\n  ┌─ LLM Provider ────────────────────────────────────────────────────────\n";
    std::cout << "  │ Mode        : " << llmMode;
    if (llmMode == "auto") std::cout << " (cloud first, local fallback)";
    std::cout << "\n";
    std::cout << "  │ Active      : " << activeLLM << "\n";
    std::cout << "  │ Local       : " << (localConfigured ? localUrl : "(not configured)") << "\n";
    std::cout << "  │ Cloud       : " << (cloudConfigured ? cloudUrl : "(not configured)") << "\n";
    std::cout << "  └──────────────────────────────────────────────────────────────────────\n";

    std::cout << "\n  ┌─ Daemon ──────────────────────────────────────────────────────────────\n";
    if (pid > 0) {
        std::cout << "  │ State       : RUNNING (PID " << pid << ")\n";
    } else {
        std::cout << "  │ State       : " << statusStr << "\n";
    }
    std::cout << "  └──────────────────────────────────────────────────────────────────────\n\n";

    return 0;
}

} // namespace chronos

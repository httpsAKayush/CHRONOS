#include "chronos/cli/commands.hpp"
#include "chronos/cli/cli_util.hpp"
#include "chronos/infrastructure/llm/ipc_llm_client.hpp"
#include <unistd.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

namespace chronos {

namespace {

constexpr size_t kMaxDiffChars = 30000;

const char* kCommitSystemPrompt =
    "You are an expert developer writing a Conventional Commit message for a "
    "git diff.\n\n"
    "Format: <type>(<scope>): <subject>\n\n"
    "Rules:\n"
    "- type: one of feat, fix, refactor, docs, chore, test, perf, build, ci, "
    "style, revert\n"
    "- scope: the primary module or area affected (optional but preferred when "
    "obvious)\n"
    "- subject: imperative mood (\"add\", \"fix\", \"refactor\"), lowercase, "
    "under 50 characters, no trailing period\n"
    "- If the change is non-trivial, add a concise body separated by a blank "
    "line, wrapped at 72 characters\n"
    "- Describe ONLY what the diff actually changes. Never invent unrelated "
    "features.\n\n"
    "Output ONLY the commit message. Do not wrap it in code fences, quotes, or "
    "markdown. Do not add a preamble.";

void trim(std::string& s) {
    s.erase(0, s.find_first_not_of(" \t\r\n"));
    s.erase(s.find_last_not_of(" \t\r\n") + 1);
}

std::string sanitizeMessage(const std::string& raw) {
    std::stringstream ss(raw);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    while (!lines.empty()) {
        std::string t = lines.front();
        trim(t);
        if (t.empty() || t.rfind("```", 0) == 0) {
            lines.erase(lines.begin());
        } else {
            break;
        }
    }
    while (!lines.empty()) {
        std::string t = lines.back();
        trim(t);
        if (t.empty() || t.rfind("```", 0) == 0) {
            lines.pop_back();
        } else {
            break;
        }
    }
    std::string msg;
    for (const auto& l : lines) msg += l + "\n";
    trim(msg);
    return msg;
}

std::string editMessage(const std::string& proposed) {
    fs::path tmp = fs::temp_directory_path() /
                   ("chronos-commit-" + std::to_string(static_cast<long>(getpid())) + ".msg");
    {
        std::ofstream out(tmp);
        out << proposed << "\n";
    }
    const char* editor = std::getenv("EDITOR");
    std::string ed = (editor && *editor) ? editor : "vi";
    std::string cmd = ed + " \"" + tmp.string() + "\"";
    std::system(cmd.c_str());
    std::ifstream in(tmp);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    fs::remove(tmp);
    if (content.empty()) return proposed;
    std::string trimmed = content;
    trim(trimmed);
    return trimmed.empty() ? proposed : content;
}

void printUsage() {
    std::cout << "usage: chronos commit [options]\n"
              << "  Write a Conventional Commit message for your staged changes and commit.\n\n"
              << "Options:\n"
              << "  -a, --all    Stage all changes (tracked + untracked) before committing\n"
              << "      --amend  Amend the last commit instead of creating a new one\n"
              << "  -y, --yes    Accept the generated message without confirmation\n"
              << "  -h, --help   Show this help\n";
}

} // namespace

CmdCommit::CmdCommit(const CliContext& ctx) : ctx_(ctx) {}

int CmdCommit::execute(int argc, char** argv) {
    bool yes = false;
    bool amend = false;
    bool all = false;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-y" || arg == "--yes") {
            yes = true;
        } else if (arg == "--amend") {
            amend = true;
        } else if (arg == "-a" || arg == "--all") {
            all = true;
        } else if (arg == "-h" || arg == "--help") {
            printUsage();
            return 0;
        } else {
            std::cerr << "[!] Unknown option: " << arg << "\n";
            printUsage();
            return 2;
        }
    }

    const std::string& repo = ctx_.repoRoot;

    if (execCmdOutput("git -C \"" + repo + "\" rev-parse --git-dir 2>/dev/null").empty()) {
        std::cerr << "[!] Not a git repository: " << repo << "\n";
        return 1;
    }

    std::cout << "[1/3] Collecting staged changes...\n";

    if (all) {
        std::string addCmd = "git -C \"" + repo + "\" add -A 2>/dev/null";
        if (std::system(addCmd.c_str()) != 0) {
            std::cerr << "[!] Failed to stage changes.\n";
            return 1;
        }
    }

    std::string diff = execCmdOutput("git -C \"" + repo + "\" diff --cached -U3 2>/dev/null");
    if (diff.empty()) {
        std::string porcelain = execCmdOutput("git -C \"" + repo + "\" status --porcelain 2>/dev/null");
        std::cerr << "[!] No staged changes to commit.\n";
        if (!porcelain.empty()) {
            std::cerr << "    Changes are present but unstaged. Run `git add <file>` "
                         "(or `chronos commit --all`) first.\n";
        } else {
            std::cerr << "    Working tree is clean.\n";
        }
        return 1;
    }

    if (diff.size() > kMaxDiffChars) {
        diff = diff.substr(0, kMaxDiffChars) + "\n...[diff truncated]";
    }

    std::string stagedFiles = execCmdOutput("git -C \"" + repo + "\" diff --cached --name-only 2>/dev/null");

    std::cout << "[2/3] Waking LLM daemon...\n";
    IpcLLMClient llm(ctx_.repoRoot);
    if (!llm.ensureDaemon()) {
        std::cerr << "[!] LLM daemon could not be started.\n";
        return 1;
    }

    std::cout << "[3/3] Generating commit message...\n" << std::flush;
    std::string message = llm.complete(
        kCommitSystemPrompt,
        "Generate a Conventional Commit message for this staged diff:\n\n" + diff,
        1024);

    bool llmFailed = message.empty() ||
                     message.rfind("[LLM Unavailable]", 0) == 0 ||
                     message.rfind("[API Error]", 0) == 0 ||
                     message.find("\"error\":") != std::string::npos;
    if (llmFailed) {
        std::cerr << "\n[!] " << llmUnavailableMessage(static_cast<const Config&>(*ctx_.config)) << "\n";
        std::cerr << "    Verify with `chronos status` and `chronos config list`, then re-run `chronos commit`.\n";
        return 1;
    }
    message = sanitizeMessage(message);
    if (message.empty()) {
        std::cerr << "\n[!] LLM produced an empty commit message.\n";
        return 1;
    }

    std::cout << "\n--- Staged files ---\n";
    std::cout << (stagedFiles.empty() ? "  (none)\n" : stagedFiles);
    std::cout << "\n--- Proposed commit message ---\n";
    std::cout << message << "\n";
    std::cout << std::flush;

    if (!yes) {
        while (true) {
            std::cout << "\nAccept this commit message? [Y/n/e] " << std::flush;
            std::string answer;
            std::getline(std::cin, answer);
            if (answer.empty()) break;
            char c = answer[0];
            if (c == 'y' || c == 'Y') break;
            if (c == 'n' || c == 'N') {
                std::cout << "Commit aborted.\n";
                return 1;
            }
            if (c == 'e' || c == 'E') {
                message = editMessage(message);
                std::cout << "\n--- Edited commit message ---\n" << message << "\n";
                break;
            }
            std::cout << "Invalid choice.\n";
        }
    }

    fs::path tmp = fs::temp_directory_path() /
                   ("chronos-commit-" + std::to_string(static_cast<long>(getpid())) + ".msg");
    {
        std::ofstream out(tmp);
        out << message << "\n";
    }
    std::string commitCmd = "git -C \"" + repo + "\" commit" +
                            (amend ? " --amend" : "") + " -F \"" + tmp.string() + "\"";
    int rc = std::system(commitCmd.c_str());
    fs::remove(tmp);
    return (rc == 0) ? 0 : 1;
}

} // namespace chronos

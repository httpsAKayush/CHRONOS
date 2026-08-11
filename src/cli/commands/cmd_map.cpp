#include "chronos/cli/commands.hpp"
#include "chronos/cli/cli_util.hpp"
#include <iostream>
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

namespace chronos {

CmdMap::CmdMap(const CliContext& ctx) : ctx_(ctx) {}

int CmdMap::execute(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: chronos map <target> [--upstream|--downstream|--both] [--depth N]\n"
                  << "\n"
                  << "  <target>       Function name (auto-inferred) or file path\n"
                  << "  --upstream     Show callers only (impact radius)\n"
                  << "  --downstream   Show callees only (behavior analysis)\n"
                  << "  --both         Show full skeleton (default)\n"
                  << "  --depth N      Max traversal depth (default: 4)\n";
        return 2;
    }

    Codex& codex = *ctx_.storage;
    const std::string& repoRoot = ctx_.repoRoot;
    std::string rawTarget = argv[2];
    MapMode mode = MapMode::BOTH;
    int maxDepth = 4;
    for (int i = 3; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--upstream")   mode = MapMode::UPSTREAM;
        else if (arg == "--downstream") mode = MapMode::DOWNSTREAM;
        else if (arg == "--both")  mode = MapMode::BOTH;
        else if (arg == "--depth" && i + 1 < argc) maxDepth = std::stoi(argv[++i]);
    }

    std::string resolvedTarget = rawTarget;
    bool isSymbol = (rawTarget.find('/') == std::string::npos &&
                     rawTarget.find('.') == std::string::npos);
    if (isSymbol && rawTarget.find("sym:") != 0) {
        resolvedTarget = "sym:" + rawTarget;
    }

    std::string rootId;
    try {
        rootId = codex.resolveAlias(resolvedTarget);
    } catch (const std::exception&) {
        std::cerr << "\n[!] Error: Symbol '" << rawTarget << "' not found in Codex graph.\n"
                  << "    Run `chronos sync` to rebuild the index if this symbol exists.\n\n";
        return 1;
    }

    auto startNode = codex.getNode(rootId);
    if (!startNode) {
        std::cerr << "\n[!] Error: Symbol '" << rawTarget << "' not found in Codex graph.\n\n";
        return 1;
    }

    std::vector<BfsRow> upstreamRows, downstreamRows;
    if (mode == MapMode::BOTH || mode == MapMode::UPSTREAM) {
        upstreamRows = bfsCollect(codex, rootId, false, maxDepth);
    }
    if (mode == MapMode::BOTH || mode == MapMode::DOWNSTREAM) {
        downstreamRows = bfsCollect(codex, rootId, true, maxDepth);
    }

    std::string nodeShortId = rootId.substr(0, 8);
    int coreLine = countLinesTo(repoRoot, startNode->file_path, startNode->byte_start);

    std::cout << "\nArchitect Flow for `" << rawTarget << "` [node:" << nodeShortId << "]\n";
    std::cout << "Location: " << startNode->file_path << " : Line " << coreLine << "\n";
    std::cout << "================================================================================\n";

    if (mode == MapMode::BOTH || mode == MapMode::UPSTREAM) {
        std::cout << "[UPSTREAM CALL STACK] (How we arrive at this function)\n\n";

        if (upstreamRows.empty()) {
            std::cout << "  (no callers found in project)\n";
        } else {
            int maxActualDepth = 1;
            for (const auto& r : upstreamRows) maxActualDepth = std::max(maxActualDepth, r.depth);

            for (auto it = upstreamRows.rbegin(); it != upstreamRows.rend(); ++it) {
                const auto& row = *it;
                auto node = codex.getNode(row.nodeId);
                std::string callerFile = node ? fs::path(node->file_path).filename().string() : "unknown";
                std::string callerSym = node ? nodeLabel(*node, row.nodeId, &codex) : row.nodeId.substr(0, 8);
                size_t pos = callerSym.find(" :: ");
                if (pos != std::string::npos) callerSym = callerSym.substr(pos + 4);

                std::string indentStr;
                if (row.depth == maxActualDepth) {
                    indentStr = "";
                } else {
                    indentStr = std::string(1 + (maxActualDepth - row.depth - 1) * 6, ' ') + "└──> ";
                }
                std::string pipeIndent = std::string(1 + (maxActualDepth - row.depth) * 6, ' ');

                std::cout << indentStr << callerFile << " : Line " << row.startLine << " :: " << callerSym << "()\n";
                std::cout << pipeIndent << "│  ↳ " << row.callSiteText << "\n";
                std::cout << pipeIndent << "│\n";
            }
            std::string targetIndent = std::string(1 + (maxActualDepth - 1) * 6, ' ') + "└──> ";
            std::cout << targetIndent << "[TARGET] " << fs::path(startNode->file_path).filename().string()
                      << " : Line " << coreLine << " :: " << rawTarget << "()\n";
        }
        std::cout << "================================================================================\n";
    }

    if (mode == MapMode::BOTH || mode == MapMode::DOWNSTREAM) {
        if (mode == MapMode::BOTH) std::cout << "\n";
        std::cout << "[DOWNSTREAM EXECUTION PATH] (Chronological Step-by-Step)\n\n";

        if (downstreamRows.empty()) {
            std::cout << "  (no callees found in project)\n";
        } else {
            for (const auto& row : downstreamRows) {
                auto node = codex.getNode(row.nodeId);
                std::string calleeFile = node ? node->file_path : "unknown";
                std::string calleeSym = node ? nodeLabel(*node, row.nodeId, &codex) : row.nodeId.substr(0, 8);
                size_t pos = calleeSym.find(" :: ");
                if (pos != std::string::npos) calleeSym = calleeSym.substr(pos + 4);

                int targetLine = node ? countLinesTo(repoRoot, node->file_path, node->byte_start) : 0;
                std::string annotation = node ? nodeAnnotation(*node, repoRoot, calleeSym) : "";
                if (annotation.empty()) annotation = "def " + calleeSym + "(...)";

                std::cout << "[Line " << row.startLine << "] ──> " << calleeSym << "()\n";
                std::cout << " │            (" << calleeFile << " : Line " << targetLine << ")\n";
                std::cout << " │            ↳ " << annotation << "\n │\n";
            }
        }
        std::cout << "================================================================================\n\n";
    }

    return 0;
}

} // namespace chronos
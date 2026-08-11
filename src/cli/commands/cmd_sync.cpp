#include "chronos/cli/commands.hpp"
#include "chronos/cli/cli_util.hpp"
#include "chronos/domain/ast_indexer.hpp"
#include "chronos/domain/vendor_filter.hpp"
#include "chronos/infrastructure/git_indexer.hpp"
#include "chronos/use_cases/repo_profiler.hpp"
#include "chronos/use_cases/hierarchical_summarizer.hpp"
#include <filesystem>
#include <iostream>
#include <thread>
#include <chrono>
#include <algorithm>

namespace fs = std::filesystem;

namespace chronos {

CmdSync::CmdSync(const CliContext& ctx) : ctx_(ctx) {}

int CmdSync::execute(int argc, char** argv) {
    bool historyMode = false;
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--history") historyMode = true;
    }

    Codex& codex = *ctx_.storage;
    VectorIndex& vectors = *ctx_.vectors;
    AstIndexer indexer(codex, vectors, ctx_.repoRoot);

    int count = 0;
    for (auto& entry : fs::recursive_directory_iterator(ctx_.repoRoot)) {
        if (isVendorPath(entry.path().string())) continue;
        auto ext = entry.path().extension().string();
        if (ext == ".cpp" || ext == ".h" || ext == ".hpp" || ext == ".cc" || ext == ".py" || ext == ".md" || ext == ".ts" || ext == ".js" || ext == ".css" || ext == ".rs" || ext == ".go") {
            ++count;
        }
    }

    int syncDepthChoice = 1;
    int histCommits = 0;
    int histMutations = 0;
    int hardwareFilesPerSec = 850;

    if (historyMode) {
        auto startBench = std::chrono::steady_clock::now();
        int bCount = 0;
        for (auto& entry : fs::recursive_directory_iterator(ctx_.repoRoot)) {
            if (isVendorPath(entry.path().string())) continue;
            auto ext = entry.path().extension().string();
            if (ext == ".cpp" || ext == ".py") {
                std::string rel = fs::relative(entry.path(), ctx_.repoRoot).string();
                indexer.indexFile(rel, "sync");
                bCount++;
                if (bCount >= 10) break;
            }
        }
        auto endBench = std::chrono::steady_clock::now();
        double elapsedSec = std::chrono::duration<double>(endBench - startBench).count();
        if (elapsedSec > 0.01 && bCount > 0) {
            hardwareFilesPerSec = static_cast<int>(bCount / elapsedSec);
        }

        std::string gitCmd = "git -C \"" + ctx_.repoRoot + "\" rev-list --count HEAD 2>/dev/null";
        FILE* pipe = popen(gitCmd.c_str(), "r");
        if (pipe) {
            char buf[128];
            if (fgets(buf, sizeof(buf), pipe) != nullptr) {
                histCommits = std::stoi(buf);
            }
            pclose(pipe);
        }

        histMutations = histCommits * 12;

        std::cout << "\n[ PRE-FLIGHT PROFILER ]\n";
        std::cout << "Hardware Benchmark: " << hardwareFilesPerSec << " files/sec (" << std::thread::hardware_concurrency() << " threads)\n";
        std::cout << "Current Codebase: " << count << " source files\n";
        std::cout << "Historical Weight: " << histCommits << " commits (est. " << histMutations << " file mutations)\n";
        std::cout << "========================================================\n\n";

        std::cout << "Choose your Temporal Sync depth:\n\n";
        std::cout << "[1] Current State Only (Recommended for instant setup)\n";
        std::cout << "    ↳ Indexes the repo exactly as it is right now.\n";
        std::cout << "    ↳ ETA: ~" << std::max(1, count / std::max(1, hardwareFilesPerSec)) << " seconds\n\n";

        std::cout << "[2] Smart Keyframing (Recommended for historical debugging)\n";
        std::cout << "    ↳ Skips formatting/noise. Indexes major structural mutations.\n";
        std::cout << "    ↳ ETA: ~" << std::max(1, (histMutations / 3) / std::max(1, hardwareFilesPerSec)) << " seconds\n\n";

        std::cout << "[3] Deep Archive (Warning: Heavy CPU Load)\n";
        std::cout << "    ↳ Calculates AST Mutation Scores for all " << histCommits << " commits since project inception.\n";
        std::cout << "    ↳ ETA: ~" << std::max(1, histMutations / std::max(1, hardwareFilesPerSec)) << " seconds\n\n";

        std::cout << "[4] Custom Depth\n";
        std::cout << "    ↳ e.g., \"Last 50 commits\" or \"Score threshold > 20\"\n\n";

        std::cout << "> Select option (1-4): ";
        std::string input;
        std::getline(std::cin, input);
        if (!input.empty() && input[0] >= '1' && input[0] <= '4') {
            syncDepthChoice = input[0] - '0';
        }
    }

    std::cout << "\n[1/3] Building Structural Graph (Parsing ASTs)... Done.\n";
    std::cout << "[2/3] Generating Semantic Index (Vectorizing)... Done.\n";

    for (auto& entry : fs::recursive_directory_iterator(ctx_.repoRoot)) {
        if (isVendorPath(entry.path().string())) continue;
        auto ext = entry.path().extension().string();
        if (ext == ".cpp" || ext == ".h" || ext == ".hpp" || ext == ".cc" || ext == ".py" || ext == ".md" || ext == ".ts" || ext == ".js" || ext == ".css" || ext == ".rs" || ext == ".go") {
            std::string rel = fs::relative(entry.path(), ctx_.repoRoot).string();
            indexer.indexFile(rel, "sync");
        }
    }
    vectors.save();

    if (syncDepthChoice >= 2) {
        GitIndexer gitIndexer(ctx_.repoRoot, codex, indexer);
        std::cout << "[3/3] Building Temporal Index (Calculating AST Mutations)...\n";
        gitIndexer.indexHistory(syncDepthChoice);
    }

    std::cout << "chronos sync: re-checked " << count << " files ("
              << indexer.stats().nodesSkippedIdempotent << " already up to date, "
              << indexer.stats().nodesUpserted << " updated)\n";

    // Only run hierarchical context ingestion if:
    // 1. There were actual node updates (new/modified files), OR
    // 2. Context nodes don't exist yet (first run)
    bool contextExists = false;
    try {
        auto globalNode = codex.getNode("[GLOBAL:REPO]");
        contextExists = globalNode && globalNode->is_active;
    } catch (...) {}

    bool hasUpdates = indexer.stats().nodesUpserted > 0;

    if (ctx_.llm && ctx_.llm->isAvailable() && (hasUpdates || !contextExists)) {
        std::cout << "\n[ Hierarchical Context Ingestion ]\n";
        std::cout << "Profiling subsystems...\n";
        RepoProfiler profiler(ctx_.repoRoot);
        auto profiles = profiler.profile();
        HierarchicalSummarizer summarizer(codex, *ctx_.llm);
        int contextNodes = summarizer.run(profiles);
        std::cout << "Wrote " << contextNodes << " context nodes ([CONTEXT:DIR:...] + [GLOBAL:REPO]).\n";
    } else if (contextExists) {
        std::cout << "\n[ Hierarchical Context ] Up to date — skipping.\n";
    } else {
        std::cout << "\n[ Hierarchical Context ] LLM unavailable — skipping subsystem summaries.\n";
    }

    return 0;
}

} // namespace chronos
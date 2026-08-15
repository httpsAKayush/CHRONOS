#include "chronos/cli/commands.hpp"
#include "chronos/cli/cli_util.hpp"
#include "chronos/use_cases/context_builder.hpp"
#include "chronos/use_cases/oracle.hpp"
#include <iostream>
#include <thread>
#include <chrono>

namespace chronos {

CmdExplain::CmdExplain(const CliContext& ctx) : ctx_(ctx) {}

int CmdExplain::execute(int argc, char** argv) {
    if (argc < 3) {
        std::cerr
            << "usage: chronos explain <target> [--query \"<question>\"]\n\n"
            << "  <target> formats:\n"
            << "    agent/decision.py                  -- explain whole file\n"
            << "    agent/decision.py::learn            -- explain specific function\n"
            << "    agent/decision.py::QLearningDecision-- explain class + all methods\n"
            << "    QLearningDecision                   -- explain bare symbol (any file)\n\n"
            << "  Options:\n"
            << "    --query \"<question>\"  Focus the explanation on a specific question\n";
        return 2;
    }

    std::string target = argv[2];
    std::string query  = "";
    for (int i = 3; i < argc; ++i) {
        if (std::string(argv[i]) == "--query" && i + 1 < argc) {
            query = argv[++i];
        }
    }

    Codex& codex       = *ctx_.storage;
    VectorIndex& vectors = *ctx_.vectors;
    ContextBuilder builder(codex, vectors, ctx_.repoRoot);
    Oracle oracle(codex, ctx_.repoRoot);

    std::cout << "[1/3] Waking daemon...\n";
    std::string sockPath = socketPathForRepo(ctx_.repoRoot);
    IpcClient client;
    bool daemonUp = client.connect(sockPath);
    if (!daemonUp) {
        wakeDaemon(ctx_.repoRoot);
        for (int i = 0; i < 20 && !daemonUp; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            daemonUp = client.connect(sockPath);
        }
    }

    std::cout << "[2/3] Analyzing structure...\n";
    BuildResult built = builder.buildExplain(target, query);

    if (!built.ok) {
        std::cerr << built.reason << "\n";
        return 1;
    }

    // Show what we assembled so the user can see the retrieval worked
    std::cerr << "[Explain] Assembled " << built.request.context.size()
              << " context block(s) for target: " << target << "\n";

    if (!daemonUp) {
        std::cout << "[!] LLM daemon unavailable -- falling back to Oracle-Only Mode.\n\n";
        std::cout << oracle.renderTrace(built.rawTrace);
        return 0;
    }

    std::cout << "[3/3] Interrogating Codebase...\n\n";
    std::string fullResponse;
    client.sendAndStream(built.request, [&](const ChronosResponseChunk& chunk) {
        std::cout << chunk.textDelta << std::flush;
        fullResponse += chunk.textDelta;
    });

    std::cout << "\n\nTraceability ID: " << built.request.traceId
              << "  (run `chronos trace " << built.request.traceId << "` later)\n";
    return 0;
}

} // namespace chronos
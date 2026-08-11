#include "chronos/cli/commands.hpp"
#include "chronos/cli/cli_util.hpp"
#include "chronos/use_cases/context_builder.hpp"
#include "chronos/use_cases/oracle.hpp"
#include "chronos/use_cases/diagnose.hpp"
#include "chronos/use_cases/ask_engine.hpp"
#include "chronos/infrastructure/llm/ipc_llm_client.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>

namespace chronos {

CmdAsk::CmdAsk(const CliContext& ctx) : ctx_(ctx) {}

int CmdAsk::execute(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: chronos ask \"<query>\" [--crash <file>]\n";
        return 2;
    }
    std::string query = argv[2];
    std::string crashFile;

    size_t crashPos = query.find("--crash");
    if (crashPos != std::string::npos) {
        std::string rem = query.substr(crashPos);
        query = query.substr(0, crashPos);
        if (rem == "--crash" && argc > 3) {
            crashFile = argv[3];
        } else if (rem.length() > 7 && rem[7] == '=') {
            crashFile = rem.substr(8);
        } else if (rem.length() > 7) {
            crashFile = rem.substr(7);
        }
    } else {
        for (int i = 3; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--crash" && i + 1 < argc) {
                crashFile = argv[++i];
            } else if (arg.rfind("--crash=", 0) == 0) {
                crashFile = arg.substr(8);
            } else if (arg.length() > 7 && arg.substr(0, 7) == "--crash") {
                crashFile = arg.substr(7);
            }
        }
    }

    Codex& codex = *ctx_.storage;
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

    std::cout << "[2/3] Analyzing graph...\n";
    BuildResult built;

    if (!crashFile.empty()) {
        std::ifstream file(crashFile);
        if (!file.is_open()) {
            std::cerr << "[!] Could not open crash log: " << crashFile << "\n";
            return 1;
        }
        std::string crashText((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        DiagnoseEngine engine(codex, vectors, ctx_.repoRoot);
        auto diagResult = engine.diagnose(crashText, 1);

        if (!diagResult.ok || diagResult.candidates.empty()) {
            std::cerr << "[!] Diagnosis engine failed to find candidates. Falling back to normal ask...\n";
            built = builder.build(query);
        } else {
            auto& topCand = diagResult.candidates.front();

            std::string culpritCode;
            auto node = codex.getNode(topCand.nodeId);
            if (node) {
                culpritCode = readSnippetForNode(*node, ctx_.repoRoot);
            }

            std::string diffCmd = "git -C \"" + ctx_.repoRoot + "\" log -p -1 " + topCand.commitHash + " -- \"" + topCand.filePath + "\" 2>/dev/null";
            char buf[2048];
            std::string gitDiff;
            FILE* pipe = popen(diffCmd.c_str(), "r");
            if (pipe) {
                while (fgets(buf, sizeof(buf), pipe)) gitDiff += buf;
                pclose(pipe);
            }

            std::ostringstream payload;
            payload << "You are an expert debugger. Determine if this is a codebase bug or a system/environment issue.\n\n";
            payload << "--- CRASH LOG ---\n" << crashText << "\n\n";
            payload << "--- CULPRIT NODE (" << topCand.nodeId << ") AST ---\n" << culpritCode << "\n\n";
            payload << "--- RECENT GIT HISTORY FOR " << topCand.filePath << " ---\n" << gitDiff << "\n\n";
            payload << "--- USER QUERY ---\n" << query << "\n";

            built = builder.build(query);
            if (built.ok) {
                built.request.userQuery = payload.str();
                built.request.traceId = "crash-" + topCand.commitHash;

                if (node) {
                    bool found = false;
                    for (const auto& c : built.request.context) {
                        if (c.nodeId == node->id) { found = true; break; }
                    }
                    if (!found) {
                        built.request.context.push_back({
                            node->id,
                            node->file_path,
                            culpritCode,
                            false
                        });
                    }
                }
            }
        }
    } else {
        // Phase 4: Use AskEngine's 4-step HyDE pipeline instead of naive embedText
        IpcLLMClient ipcLlm(ctx_.repoRoot);
        AskEngine askEngine(codex, vectors, ipcLlm, ctx_.repoRoot);
        auto askResult = askEngine.run(query);

        if (!askResult.ok) {
            std::cout << askResult.reason << "\n";
            return 0;
        }

        built.ok = askResult.ok;
        built.request = std::move(askResult.request);
        built.rawTrace = std::move(askResult.rawTrace);
    }

    if (!built.ok) {
        std::cout << built.reason << "\n";
        return 0;
    }

    if (!daemonUp) {
        std::cout << "[!] LLM daemon unavailable -- falling back to Oracle-Only Mode.\n\n";
        std::cout << oracle.renderTrace(built.rawTrace);
        std::cout << "\nTraceability ID: " << built.request.traceId
                  << "  (run `chronos trace " << built.request.traceId << "` later)\n";
        return 0;
    }

    std::cout << "[3/3] Generating response...\n" << std::flush;
    bool gotAnyText = false;
    std::string fullText;
    client.sendAndStream(built.request, [&](const ChronosResponseChunk& chunk) {
        if (!chunk.textDelta.empty()) {
            gotAnyText = true;
            fullText += chunk.textDelta;
            std::cout << chunk.textDelta << std::flush;
        }
    });
    std::cout << "\n";

    bool isApiError = (fullText.find("[API Error]") != std::string::npos ||
                       fullText.find("[LLM Unavailable]") != std::string::npos ||
                       fullText.find("\"error\":") != std::string::npos);

    if (!gotAnyText || isApiError) {
        if (isApiError) {
            std::cout << "\n[!] LLM returned an API error -- falling back to Oracle-Only Mode.\n\n";
        } else {
            std::cout << "\n[!] LLM produced no response -- falling back to Oracle-Only Mode.\n\n";
        }
        std::cout << oracle.renderTrace(built.rawTrace);
        std::cout << "\nTraceability ID: " << built.request.traceId
                  << "  (run `chronos trace " << built.request.traceId << "` later)\n";
        return 0;
    }

    auto check = oracle.verifyCitations(fullText);
    if (!check.allValid) {
        std::cout << "\n[Unverified] This answer contains citations that don't match the "
                     "Codex graph. Would you like to see the deterministic trace for this "
                     "region instead? Run: chronos ask --oracle-only \"" << query << "\"\n";
    }

    std::cout << "\nTraceability ID: " << built.request.traceId
              << "  (run `chronos trace " << built.request.traceId << "` later)\n";
    return 0;
}

} // namespace chronos
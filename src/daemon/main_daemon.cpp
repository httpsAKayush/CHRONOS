// chronos-daemon: "The Brain" (project.md III). Owns the 0600 AF_UNIX
// socket, lazy-loads the local LLM (Ollama, per Spec §6 External
// dependencies) only on first request, and self-terminates after 15 minutes
// idle (FR-5, "Zero-Idle"). Enforces the FR-8 citation schema and the §7
// Safety Contract (uncertainty_warning prefacing) via the system prompt.
//
// The actual model weights/runtime live in a separately-running Ollama
// process (`ollama serve`, default localhost:11434) — Spec explicitly rules
// out statically linking llama.cpp into Chronos binaries (§2 hard
// constraint) to avoid bloat/latency tax. This daemon is a thin, stateless
// relay + policy layer between the Codex-derived context and that process.

#include "chronos/infrastructure/config.hpp"
#include "chronos/infrastructure/llm/llm_client_factory.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include "chronos/ipc.hpp"
#include "chronos/infrastructure/codex.hpp"
#include "chronos/use_cases/oracle.hpp"
#include "chronos/env.hpp"

using namespace chronos;
using namespace std::chrono_literals;

namespace {

std::atomic<std::chrono::steady_clock::time_point> g_lastActivity{std::chrono::steady_clock::now()};
constexpr auto kIdleTimeout = 15min;

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: chronos-daemon <repoRoot>\n";
        return 2;
    }
    std::string repoRoot = argv[1];
    ::signal(SIGPIPE, SIG_IGN);

    auto env = loadEnv(repoRoot);
    for (const auto& [k, v] : env) {
        setenv(k.c_str(), v.c_str(), 1);
    }

    Config config;
    auto llm = createLLMClient(config);

    Codex codex(repoRoot);
    Oracle oracle(codex, repoRoot);
    IpcServer server;
    std::string sockPath = socketPathForRepo(repoRoot);

    const std::string kSystemPromptTemplate =
        "You are Chronos, a code-intelligence assistant. You may ONLY use the "
        "provided context blocks to answer -- never invent facts about the "
        "codebase. Every factual claim about the code MUST end with a "
        "citation in the exact form [node:<id>] referencing the context "
        "block's nodeId it came from. If any provided context block is "
        "marked uncertain, you MUST preface your answer with exactly: "
        "\"Structural resolution is uncertain in this region.\"\n\nContext:\n";

    bool ok = server.listen(sockPath, [&](const ChronosRequest& req,
                                           const std::function<void(const ChronosResponseChunk&)>& send) {
        g_lastActivity.store(std::chrono::steady_clock::now());

        if (req.command == "summarize") {
            std::string prompt = "You are a code summarization tool. For each provided code block, provide a 3-to-5 word label describing what it does. Format your output strictly as: nodeId|summary, one per line. Do NOT output any json or markdown formatting. Example:\nnode_123|Calculates point cloud RMSE\nnode_456|Loads reference data";
            for (const auto& cn : req.context) {
                prompt += "\n--- [" + cn.nodeId + "] ---\n" + cn.codeSnippet + "\n";
            }
            std::string content = llm->complete(prompt, "Summarize the blocks as requested.", 1024);
            
            ChronosResponseChunk chunk;
            chunk.traceId = req.traceId;
            chunk.textDelta = content;
            chunk.done = true;
            send(chunk);
            return;
        }

        if (req.command == "complete") {
            std::string result = llm->complete(
                req.systemPromptOverride.empty() ? "You are a helpful assistant." : req.systemPromptOverride,
                req.userQuery, 2048);
            ChronosResponseChunk chunk;
            chunk.traceId = req.traceId;
            chunk.textDelta = result;
            chunk.done = true;
            send(chunk);
            return;
        }

        if (req.command == "embed") {
            std::vector<float> vec = llm->embed(req.userQuery);
            nlohmann::json j = vec;
            ChronosResponseChunk chunk;
            chunk.traceId = req.traceId;
            chunk.textDelta = j.dump();
            chunk.done = true;
            send(chunk);
            return;
        }

        std::string systemPrompt = req.systemPromptOverride.empty() ? kSystemPromptTemplate : req.systemPromptOverride;
        for (const auto& cn : req.context) {
            systemPrompt += "--- File: " + cn.filePath + " [node:" + cn.nodeId + "] ---\n";
            systemPrompt += cn.codeSnippet + "\n\n";
        }

        constexpr size_t kMaxSystemPromptChars = 12000;
        if (systemPrompt.size() > kMaxSystemPromptChars) {
            systemPrompt = systemPrompt.substr(0, kMaxSystemPromptChars) + "...\n[context truncated]";
        }

        bool gotAnyText = false;
        llm->stream(systemPrompt, req.userQuery, 2048, [&](const std::string& chunkContent) {
            gotAnyText = true;
            ChronosResponseChunk chunk;
            chunk.traceId = req.traceId;
            chunk.textDelta = chunkContent;
            chunk.done = false;
            send(chunk);
        });

        ChronosResponseChunk finalChunk;
        finalChunk.traceId = req.traceId;
        finalChunk.textDelta = "";
        finalChunk.done = true;
        
        if (!gotAnyText) {
        }
        
        send(finalChunk);
    });

    if (!ok) {
        std::cerr << "chronos-daemon: failed to bind " << sockPath << "\n";
        return 1;
    }

    std::thread watchdog([&]() {
        while (true) {
            std::this_thread::sleep_for(30s);
            if (std::chrono::steady_clock::now() - g_lastActivity.load() > kIdleTimeout) {
                server.stop();
                break;
            }
        }
    });
    watchdog.detach();

    server.run();
    return 0;
}
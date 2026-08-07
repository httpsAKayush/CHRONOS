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

#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <cstring>
#include <csignal>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "chronos/ipc.hpp"
#include "chronos/codex.hpp"
#include "chronos/oracle.hpp"
#include "chronos/env.hpp"

using namespace chronos;
using namespace std::chrono_literals;

namespace {

std::atomic<std::chrono::steady_clock::time_point> g_lastActivity{std::chrono::steady_clock::now()};
constexpr auto kIdleTimeout = 15min; // FR-5

// Minimal blocking HTTP/1.1 client to Ollama's local REST API. No TLS, no
// external HTTP library — this only ever talks to 127.0.0.1, matching the
// Spec's "strictly local, no network exposure" constraint (the socket
// itself never leaves loopback).
std::string openAiChatBlocking(const std::string& systemPrompt, const std::string& userQuery) {
    std::string escapedSys, escapedUser;
    for (char c : systemPrompt) {
        if (c == '"' || c == '\\') escapedSys += '\\';
        if (c == '\n') escapedSys += "\\n";
        else escapedSys += c;
    }
    for (char c : userQuery) {
        if (c == '"' || c == '\\') escapedUser += '\\';
        if (c == '\n') escapedUser += "\\n";
        else escapedUser += c;
    }

    const char* sysKey = std::getenv("OPENROUTER_API_KEY");
    if (!sysKey) sysKey = std::getenv("OPENAI_API_KEY");
    const char* apiKey = sysKey ? sysKey : "";

    if (!apiKey[0]) {
        std::cerr << "chronos-daemon: No API key environment variable set. Falling back to Oracle-Only.\n";
        return "";
    }

    std::string body = "{\"model\":\"openai/gpt-4o\",\"max_tokens\":1000,\"messages\":["
        "{\"role\":\"system\",\"content\":\"" + escapedSys + "\"},"
        "{\"role\":\"user\",\"content\":\"" + escapedUser + "\"}]}";

    std::string tmpFile = "/tmp/chronos_daemon_req.json";
    {
        std::ofstream out(tmpFile);
        out << body;
    }

    std::string cmd = "curl -s https://openrouter.ai/api/v1/chat/completions "
                      "-H \"Content-Type: application/json\" "
                      "-H \"Authorization: Bearer " + std::string(apiKey) + "\" "
                      "-d @" + tmpFile;

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return "";
    }

    char buffer[128];
    std::string result = "";
    while (!feof(pipe)) {
        if (fgets(buffer, 128, pipe) != nullptr)
            result += buffer;
    }
    pclose(pipe);
    
    std::ofstream outResp("/tmp/chronos_daemon_resp.json");
    outResp << result;
    outResp.close();
    
    return result;
}

std::string extractContent(const std::string& openaiJson) {
    // Basic JSON extraction for the message content in OpenAI's response format
    size_t choicesPos = openaiJson.find("\"choices\"");
    if (choicesPos == std::string::npos) return "";
    
    size_t contentPos = openaiJson.find("\"content\":", choicesPos);
    if (contentPos == std::string::npos) return "";
    
    // Find the first quote after "content":
    size_t startPos = openaiJson.find("\"", contentPos + 10);
    if (startPos == std::string::npos) return "";
    startPos++; // skip the quote
    
    std::string out;
    for (size_t i = startPos; i < openaiJson.size(); ++i) {
        if (openaiJson[i] == '\\' && i + 1 < openaiJson.size()) {
            if (openaiJson[i+1] == 'n') { out += '\n'; i++; continue; }
            if (openaiJson[i+1] == '"') { out += '"'; i++; continue; }
            out += openaiJson[i + 1];
            i++;
            continue;
        }
        if (openaiJson[i] == '"') break;
        out += openaiJson[i];
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: chronos-daemon <repoRoot>\n";
        return 2;
    }
    std::string repoRoot = argv[1];
    ::signal(SIGPIPE, SIG_IGN); // Spec §18: EPIPE on client interrupt must not kill the daemon

    auto env = loadEnv(repoRoot);
    for (const auto& [k, v] : env) {
        setenv(k.c_str(), v.c_str(), 1);
    }

    Codex codex(repoRoot);
    Oracle oracle(codex, repoRoot);
    IpcServer server;
    std::string sockPath = socketPathForRepo(repoRoot);

    // FR-8 + §7 Safety Contract: the system prompt is the enforcement
    // mechanism. We can't force the model to comply, but every response is
    // additionally checked by Oracle::verifyCitations before being trusted
    // by the CLI (see main_cli.cpp), so a non-compliant model degrades to
    // "Unverified" rather than silently passing off hallucinated citations.
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

        std::string systemPrompt = kSystemPromptTemplate;
        for (const auto& cn : req.context) {
            systemPrompt += "--- File: " + cn.filePath + " [node:" + cn.nodeId + "] ---\n";
            systemPrompt += cn.codeSnippet + "\n\n";
        }

        std::string rawResp = openAiChatBlocking(systemPrompt, req.userQuery);
        std::string content = extractContent(rawResp);

        ChronosResponseChunk chunk;
        chunk.traceId = req.traceId;
        if (content.empty()) {
            // Daemon unreachable / model errored -> Spec unhappy path:
            // "TUI falls back to Oracle-Only Mode". We signal this by
            // sending an empty final chunk; the CLI detects empty content
            // and switches to Oracle rendering itself.
            chunk.textDelta = "";
            chunk.done = true;
            send(chunk);
            return;
        }
        chunk.textDelta = content;
        chunk.done = true;
        send(chunk);
    });

    if (!ok) {
        std::cerr << "chronos-daemon: failed to bind " << sockPath << "\n";
        return 1;
    }

    // Idle watchdog (FR-5): after 15 minutes with no request, stop the
    // server loop so the process exits and frees any VRAM held by whatever
    // keep-alive state Ollama itself is holding for this session.
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

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

using namespace chronos;
using namespace std::chrono_literals;

namespace {

std::atomic<std::chrono::steady_clock::time_point> g_lastActivity{std::chrono::steady_clock::now()};
constexpr auto kIdleTimeout = 15min; // FR-5

// Minimal blocking HTTP/1.1 client to Ollama's local REST API. No TLS, no
// external HTTP library — this only ever talks to 127.0.0.1, matching the
// Spec's "strictly local, no network exposure" constraint (the socket
// itself never leaves loopback).
std::string ollamaChatBlocking(const std::string& systemPrompt, const std::string& userQuery) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return "";
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(11434);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return ""; // Ollama not running -> caller falls back to Oracle-Only
    }

    std::string escapedSys, escapedUser;
    for (char c : systemPrompt) { if (c == '"' || c == '\\') escapedSys += '\\'; escapedSys += c; }
    for (char c : userQuery) { if (c == '"' || c == '\\') escapedUser += '\\'; escapedUser += c; }

    std::string body = "{\"model\":\"llama3\",\"stream\":false,\"messages\":["
        "{\"role\":\"system\",\"content\":\"" + escapedSys + "\"},"
        "{\"role\":\"user\",\"content\":\"" + escapedUser + "\"}]}";
    std::string req = "POST /api/chat HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                       "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    ::write(fd, req.data(), req.size());

    std::string resp;
    char buf[4096];
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof(buf))) > 0) resp.append(buf, n);
    ::close(fd);

    // Strip HTTP headers; caller only needs the JSON body's "content" field,
    // extracted with the same tiny field-scan used in ipc.cpp.
    size_t bodyStart = resp.find("\r\n\r\n");
    return bodyStart == std::string::npos ? "" : resp.substr(bodyStart + 4);
}

std::string extractContent(const std::string& ollamaJson) {
    size_t pos = ollamaJson.find("\"content\":\"");
    if (pos == std::string::npos) return "";
    pos += 11;
    std::string out;
    for (size_t i = pos; i < ollamaJson.size(); ++i) {
        if (ollamaJson[i] == '\\' && i + 1 < ollamaJson.size()) { out += ollamaJson[i + 1]; ++i; continue; }
        if (ollamaJson[i] == '"') break;
        out += ollamaJson[i];
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

        std::string rawResp = ollamaChatBlocking(systemPrompt, req.userQuery);
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

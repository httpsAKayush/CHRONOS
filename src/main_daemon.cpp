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

#include "chronos/llm_config.hpp"
#include <iostream>
#include <fstream>
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
    auto escape = [](const std::string& str) {
        std::string out;
        for (char c : str) {
            if (c == '"' || c == '\\') { out += '\\'; out += c; }
            else if (c == '\n') out += "\\n";
            else if (c == '\r') out += "\\r";
            else if (c == '\t') out += "\\t";
            else out += c;
        }
        return out;
    };
    std::string escapedSys = escape(systemPrompt);
    std::string escapedUser = escape(userQuery);

    LlmConfig config = loadLlmConfig();
    if (!config.isValid()) {
        std::cerr << "chronos-daemon: No API key environment variable set. Falling back to Oracle-Only.\n";
        return "";
    }

    std::string body = "{\"model\":\"" + config.modelName + "\",\"max_tokens\":4096,\"messages\":["
        "{\"role\":\"system\",\"content\":\"" + escapedSys + "\"},"
        "{\"role\":\"user\",\"content\":\"" + escapedUser + "\"}]}";

    std::string tmpFile = "/tmp/chronos_daemon_req.json";
    {
        std::ofstream out(tmpFile);
        out << body;
    }

    std::string cmd = "curl -s " + config.apiUrl + " "
                      "-H \"Content-Type: application/json\" "
                      "-H \"Authorization: Bearer " + config.apiKey + "\" "
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

void openAiChatStreaming(const std::string& systemPrompt, const std::string& userQuery, std::function<void(const std::string&)> onChunk) {
    auto escape = [](const std::string& str) {
        std::string out;
        for (char c : str) {
            if (c == '"' || c == '\\') { out += '\\'; out += c; }
            else if (c == '\n') out += "\\n";
            else if (c == '\r') out += "\\r";
            else if (c == '\t') out += "\\t";
            else out += c;
        }
        return out;
    };
    std::string escapedSys = escape(systemPrompt);
    std::string escapedUser = escape(userQuery);

    LlmConfig config = loadLlmConfig();
    if (!config.isValid()) {
        std::cerr << "chronos-daemon: No API key environment variable set. Falling back to Oracle-Only.\n";
        return;
    }

    std::string body = "{\"model\":\"" + config.modelName + "\",\"max_tokens\":4096,\"stream\":true,\"messages\":["
        "{\"role\":\"system\",\"content\":\"" + escapedSys + "\"},"
        "{\"role\":\"user\",\"content\":\"" + escapedUser + "\"}]}";

    std::string tmpFile = "/tmp/chronos_daemon_req.json";
    {
        std::ofstream out(tmpFile);
        out << body;
    }

    std::string cmd = "curl -N -s " + config.apiUrl + " "
                      "-H \"Content-Type: application/json\" "
                      "-H \"Authorization: Bearer " + config.apiKey + "\" "
                      "-d @" + tmpFile;

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return;
    }

    char buffer[4096];
    std::string lineBuffer;
    while (!feof(pipe)) {
        if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            lineBuffer += buffer;
            size_t newlinePos;
            while ((newlinePos = lineBuffer.find('\n')) != std::string::npos) {
                std::string line = lineBuffer.substr(0, newlinePos);
                lineBuffer = lineBuffer.substr(newlinePos + 1);
                
                if (line.find("data: ") == 0) {
                    std::string data = line.substr(6);
                    if (data == "[DONE]") continue;
                    
                    std::string chunkContent = extractContent(data);
                    if (!chunkContent.empty()) {
                        onChunk(chunkContent);
                    }
                }
            }
        }
    }
    pclose(pipe);
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

        if (req.command == "summarize") {
            std::string prompt = "You are a code summarization tool. For each provided code block, provide a 3-to-5 word label describing what it does. Format your output strictly as: nodeId|summary, one per line. Do NOT output any json or markdown formatting. Example:\nnode_123|Calculates point cloud RMSE\nnode_456|Loads reference data";
            for (const auto& cn : req.context) {
                prompt += "\n--- [" + cn.nodeId + "] ---\n" + cn.codeSnippet + "\n";
            }
            std::string rawResp = openAiChatBlocking(prompt, "Summarize the blocks as requested.");
            std::string content = extractContent(rawResp);
            
            ChronosResponseChunk chunk;
            chunk.traceId = req.traceId;
            chunk.textDelta = content;
            chunk.done = true;
            send(chunk);
            return;
        }

        std::string systemPrompt = req.systemPromptOverride.empty() ? kSystemPromptTemplate : req.systemPromptOverride;
        for (const auto& cn : req.context) {
            systemPrompt += "--- File: " + cn.filePath + " [node:" + cn.nodeId + "] ---\n";
            systemPrompt += cn.codeSnippet + "\n\n";
        }

        bool gotAnyText = false;
        openAiChatStreaming(systemPrompt, req.userQuery, [&](const std::string& chunkContent) {
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
            // Daemon unreachable / model errored
            // TUI falls back to Oracle-Only Mode
        }
        
        send(finalChunk);
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

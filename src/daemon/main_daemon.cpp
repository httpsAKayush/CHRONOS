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

#include "chronos/infrastructure/llm_config.hpp"
#include <nlohmann/json.hpp>
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
#include "chronos/infrastructure/codex.hpp"
#include "chronos/use_cases/oracle.hpp"
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

    // Truncate system prompt to prevent context bloat crashing a local LLM.
    // Hard cap: 12,000 chars of system context (~3,000 tokens) for 8B models.
    constexpr size_t kMaxSystemPromptChars = 12000;
    if (escapedSys.size() > kMaxSystemPromptChars) {
        escapedSys = escapedSys.substr(0, kMaxSystemPromptChars) + "...\\n[context truncated]";
    }

    std::string body = "{\"model\":\"" + config.modelName + "\",\"max_tokens\":2048,\"messages\":["
        "{\"role\":\"system\",\"content\":\"" + escapedSys + "\"},"
        "{\"role\":\"user\",\"content\":\"" + escapedUser + "\"}]}";

    std::string tmpFile = "/tmp/chronos_daemon_req.json";
    {
        std::ofstream out(tmpFile);
        out << body;
    }

    // --connect-timeout: fail fast if Ollama isn't running.
    // --max-time: hard ceiling so a slow model cannot hang forever.
    std::string cmd = "curl -s --connect-timeout 5 --max-time 120 " + config.apiUrl + " "
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
    // Proper JSON extraction of the assistant message content:
    //   streamed:     choices[0].delta.content
    //   non-streamed: choices[0].message.content
    // Reasoning fields (reasoning_content) and null content are ignored —
    // only the final answer text is relayed.
    try {
        auto j = nlohmann::json::parse(openaiJson);
        if (!j.is_object()) return "";
        auto it = j.find("choices");
        if (it == j.end() || !it->is_array() || it->empty()) return "";
        const auto& first = (*it)[0];
        if (!first.is_object()) return "";
        for (const char* key : {"message", "delta"}) {
            auto m = first.find(key);
            if (m == first.end() || !m->is_object()) continue;
            auto c = m->find("content");
            if (c != m->end() && c->is_string()) return c->get<std::string>();
        }
    } catch (...) {}
    return "";
}

// Best-effort extraction of an error message from a non-SSE response body
// (e.g. OpenAI/DeepSeek `{"error":{"message":"..."}}`, or a proxy 4xx/5xx
// body). Returns a truncated raw body when no "message" field is present.
// SSE payloads (which contain "data: " framing, e.g. keepalive events) are
// NOT error bodies and yield "" — they mean the upstream started streaming
// but never delivered content, which is reported differently.
// Used so an LLM failure surfaces the real reason to the CLI instead of
// being silently swallowed as "produced no response".
std::string extractErrorMessage(const std::string& body) {
    if (body.empty()) return "";
    if (body.find("data: ") != std::string::npos) return "";
    size_t msgPos = body.find("\"message\"");
    if (msgPos != std::string::npos) {
        size_t colon = body.find(':', msgPos);
        size_t start = colon == std::string::npos ? std::string::npos : body.find('"', colon + 1);
        if (start != std::string::npos) {
            size_t end = body.find('"', start + 1);
            if (end != std::string::npos) return body.substr(start + 1, end - start - 1);
        }
    }
    std::string out = body;
    if (out.size() > 300) out = out.substr(0, 300) + "...";
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

    // Truncate system prompt to prevent context bloat crashing a local LLM.
    // Hard cap: 12,000 chars of system context (~3,000 tokens) for 8B models.
    constexpr size_t kMaxSystemPromptChars = 12000;
    if (escapedSys.size() > kMaxSystemPromptChars) {
        escapedSys = escapedSys.substr(0, kMaxSystemPromptChars) + "...\\n[context truncated]";
    }

    std::string body = "{\"model\":\"" + config.modelName + "\",\"max_tokens\":2048,\"stream\":true,\"messages\":["
        "{\"role\":\"system\",\"content\":\"" + escapedSys + "\"},"
        "{\"role\":\"user\",\"content\":\"" + escapedUser + "\"}]}";

    std::string tmpFile = "/tmp/chronos_daemon_req.json";
    {
        std::ofstream out(tmpFile);
        out << body;
    }

    // -N: disable buffering for SSE streaming
    // --connect-timeout: fail fast if Ollama isn't running
    // --max-time: hard ceiling so a stalled model cannot hang the daemon.
    // 300s (not 120) because reasoning models over large contexts can take
    // a while before their first token, with the proxy streaming keepalive
    // events meanwhile.
    std::string cmd = "curl -N -s --connect-timeout 5 --max-time 300 " + config.apiUrl + " "
                      "-H \"Content-Type: application/json\" "
                      "-H \"Authorization: Bearer " + config.apiKey + "\" "
                      "-d @" + tmpFile;

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return;
    }

    char buffer[4096];
    std::string lineBuffer;
    std::string rawAll;
    bool streamDone = false;
    bool sawChunk = false;
    bool sawSseLine = false;
    while (!feof(pipe) && !streamDone) {
        if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            rawAll += buffer;
            lineBuffer += buffer;
            size_t newlinePos;
            while ((newlinePos = lineBuffer.find('\n')) != std::string::npos) {
                std::string line = lineBuffer.substr(0, newlinePos);
                lineBuffer = lineBuffer.substr(newlinePos + 1);

                // Trim trailing \r if present (SSE spec allows CRLF)
                if (!line.empty() && line.back() == '\r') line.pop_back();

                if (line.find("data: ") == 0) {
                    sawSseLine = true;
                    std::string data = line.substr(6);

                    // OpenAI SSE terminal sentinel — break the read loop immediately
                    if (data == "[DONE]") {
                        streamDone = true;
                        break;
                    }

                    // Ollama's /api/generate terminal: {"done":true}
                    if (data.find("\"done\":true") != std::string::npos ||
                        data.find("\"done\": true") != std::string::npos) {
                        // Still try to extract final content before breaking
                        std::string chunkContent = extractContent(data);
                        if (!chunkContent.empty()) { onChunk(chunkContent); sawChunk = true; }
                        streamDone = true;
                        break;
                    }

                    std::string chunkContent = extractContent(data);
                    if (!chunkContent.empty()) {
                        onChunk(chunkContent);
                        sawChunk = true;
                    }
                }
            }
        }
    }
    pclose(pipe);

    // The upstream returned no usable SSE content at all — most likely a
    // non-streaming error body (bad key, insufficient balance, 404...) or a
    // stream that was closed after only keepalive events (provider-side
    // stall/error). Relay the real reason so the CLI can report it instead
    // of a generic "no response" fallback.
    if (!sawChunk) {
        std::string errMsg = extractErrorMessage(rawAll);
        if (!errMsg.empty()) {
            onChunk("[API Error] " + errMsg);
        } else if (sawSseLine) {
            onChunk("[API Error] LLM stream closed before producing content "
                    "(provider-side error, timeout, or empty response). Retry the query.");
        }
    }
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

#include "chronos/infrastructure/llm/ipc_llm_client.hpp"
#include "chronos/cli/cli_util.hpp"
#include <nlohmann/json.hpp>
#include <thread>
#include <chrono>

namespace chronos {

IpcLLMClient::IpcLLMClient(const std::string& repoRoot)
    : repoRoot_(repoRoot), sockPath_(socketPathForRepo(repoRoot)) {}

bool IpcLLMClient::ensureDaemon() {
    if (daemonUp_) return true;
    IpcClient client;
    daemonUp_ = client.connect(sockPath_);
    if (!daemonUp_) {
        wakeDaemon(repoRoot_);
        for (int i = 0; i < 20 && !daemonUp_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            IpcClient retry;
            daemonUp_ = retry.connect(sockPath_);
        }
    }
    return daemonUp_;
}

bool IpcLLMClient::isAvailable() const {
    return !sockPath_.empty();
}

bool IpcLLMClient::sendComplete(const std::string& sysPrompt, const std::string& userQuery,
                                 std::string& result) {
    IpcClient client;
    if (!client.connect(sockPath_)) {
        if (!const_cast<IpcLLMClient*>(this)->ensureDaemon()) return false;
        if (!client.connect(sockPath_)) return false;
    }

    ChronosRequest req;
    req.command = "complete";
    req.traceId = "hyde-complete";
    req.systemPromptOverride = sysPrompt;
    req.userQuery = userQuery;

    bool gotText = false;
    client.sendAndStream(req, [&](const ChronosResponseChunk& chunk) {
        if (!chunk.textDelta.empty()) {
            result += chunk.textDelta;
            gotText = true;
        }
    });
    return gotText;
}

bool IpcLLMClient::sendEmbed(const std::string& text, std::vector<float>& vec) {
    IpcClient client;
    if (!client.connect(sockPath_)) {
        if (!const_cast<IpcLLMClient*>(this)->ensureDaemon()) return false;
        if (!client.connect(sockPath_)) return false;
    }

    ChronosRequest req;
    req.command = "embed";
    req.traceId = "hyde-embed";
    req.userQuery = text;

    bool gotResult = false;
    client.sendAndStream(req, [&](const ChronosResponseChunk& chunk) {
        if (!chunk.textDelta.empty()) {
            try {
                auto j = nlohmann::json::parse(chunk.textDelta);
                if (j.is_array()) {
                    for (const auto& v : j) {
                        vec.push_back(v.get<float>());
                    }
                }
                gotResult = true;
            } catch (...) {}
        }
    });
    return gotResult;
}

std::string IpcLLMClient::complete(const std::string& systemPrompt,
                                    const std::string& userQuery,
                                    int maxTokens) {
    (void)maxTokens;
    std::string result;
    sendComplete(systemPrompt, userQuery, result);
    return result;
}

bool IpcLLMClient::stream(const std::string& systemPrompt,
                          const std::string& userQuery,
                          int maxTokens,
                          const std::function<void(const std::string&)>& onChunk) {
    IpcClient client;
    if (!client.connect(sockPath_)) {
        if (!ensureDaemon()) return false;
        if (!client.connect(sockPath_)) return false;
    }

    ChronosRequest req;
    req.command = "ask";
    req.traceId = "ipc-stream";
    req.systemPromptOverride = systemPrompt;
    req.userQuery = userQuery;

    bool gotAny = false;
    client.sendAndStream(req, [&](const ChronosResponseChunk& chunk) {
        if (!chunk.textDelta.empty()) {
            gotAny = true;
            onChunk(chunk.textDelta);
        }
    });
    return gotAny;
}

std::vector<float> IpcLLMClient::embed(const std::string& text) {
    std::vector<float> vec;
    sendEmbed(text, vec);
    return vec;
}

} // namespace chronos
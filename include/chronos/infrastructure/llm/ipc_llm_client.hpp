#pragma once
// Phase 4 "Context-Aware HyDE RAG": an ILLMClient implementation that
// routes complete/stream/embed calls through the chronos-daemon IPC socket
// instead of directly to the LLM provider. This lets the CLI delegate all
// LLM operations to the daemon (which owns the ILLMClient lifecycle and
// handles provider fallback, timeouts, etc.) without duplicating config
// or provider logic.

#include "chronos/core/ILLMClient.hpp"
#include "chronos/ipc.hpp"
#include <string>

namespace chronos {

class IpcLLMClient : public ILLMClient {
public:
    explicit IpcLLMClient(const std::string& repoRoot);
    ~IpcLLMClient() override = default;

    std::string complete(const std::string& systemPrompt,
                         const std::string& userQuery,
                         int maxTokens = 2048) override;

    bool stream(const std::string& systemPrompt,
                const std::string& userQuery,
                int maxTokens,
                const std::function<void(const std::string&)>& onChunk) override;

    std::vector<float> embed(const std::string& text) override;

    bool isAvailable() const override;

    bool ensureDaemon();

private:
    std::string repoRoot_;
    std::string sockPath_;
    bool daemonUp_ = false;

    bool sendComplete(const std::string& sysPrompt, const std::string& userQuery,
                      std::string& result);
    bool sendEmbed(const std::string& text, std::vector<float>& vec);
};

} // namespace chronos
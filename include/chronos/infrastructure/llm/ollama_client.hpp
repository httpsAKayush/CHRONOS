#pragma once

#include "chronos/core/ILLMClient.hpp"
#include <string>

namespace chronos {

class OllamaClient : public ILLMClient {
public:
    OllamaClient(const std::string& baseUrl, const std::string& model, bool openAiCompat = false);
    ~OllamaClient() override;

    std::string complete(const std::string& systemPrompt,
                         const std::string& userQuery,
                         int maxTokens = 2048) override;

    bool stream(const std::string& systemPrompt,
                const std::string& userQuery,
                int maxTokens,
                const std::function<void(const std::string&)>& onChunk) override;

    std::vector<float> embed(const std::string& text) override;

    bool isAvailable() const override;

private:
    std::string baseUrl_;
    std::string model_;
    bool openAiCompat_;
    std::string host_;
    int port_ = 11434;
};

} // namespace chronos

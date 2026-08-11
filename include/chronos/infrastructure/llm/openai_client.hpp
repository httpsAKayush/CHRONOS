#pragma once

#include "chronos/core/ILLMClient.hpp"
#include <string>

namespace chronos {

class OpenAIClient : public ILLMClient {
public:
    OpenAIClient(const std::string& baseUrl, const std::string& apiKey, const std::string& model);
    ~OpenAIClient() override;

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
    std::string apiKey_;
    std::string model_;
    std::string host_;
    int port_ = 443;
    bool ssl_ = false;
    std::string basePath_;
};

} // namespace chronos

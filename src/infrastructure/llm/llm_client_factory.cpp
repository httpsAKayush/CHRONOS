#include "chronos/infrastructure/llm/llm_client_factory.hpp"
#include "chronos/infrastructure/llm/openai_client.hpp"
#include "chronos/infrastructure/llm/ollama_client.hpp"
#include "chronos/core/IConfig.hpp"
#include <httplib.h>
#include <string>

namespace chronos {

namespace {

class NullLLMClient : public ILLMClient {
public:
    std::string complete(const std::string&, const std::string&, int) override { return ""; }
    bool stream(const std::string&, const std::string&, int, const std::function<void(const std::string&)>&) override {
        return false;
    }
    std::vector<float> embed(const std::string&) override { return {}; }
    bool isAvailable() const override { return false; }
};

// Wraps a primary and a fallback provider. Used in "auto" mode: cloud is the
// primary, local the fallback. When the primary fails to produce any usable
// text (unreachable, API error, or an empty completion), the fallback is
// tried instead. This implements "try cloud first, fall back to local".
class FallbackLLMClient : public ILLMClient {
public:
    FallbackLLMClient(std::unique_ptr<ILLMClient> primary, std::unique_ptr<ILLMClient> fallback)
        : primary_(std::move(primary)), fallback_(std::move(fallback)) {}

    std::string complete(const std::string& systemPrompt, const std::string& userQuery, int maxTokens) override {
        std::string out = primary_->complete(systemPrompt, userQuery, maxTokens);
        if (!out.empty() && out.find("[API Error]") == std::string::npos) return out;
        return fallback_->complete(systemPrompt, userQuery, maxTokens);
    }

    bool stream(const std::string& systemPrompt, const std::string& userQuery, int maxTokens,
                const std::function<void(const std::string&)>& onChunk) override {
        bool gotAny = false;
        bool primaryOk = primary_->stream(systemPrompt, userQuery, maxTokens, [&](const std::string& c) {
            if (c.rfind("[API Error]", 0) == 0) return;
            gotAny = true;
            onChunk(c);
        });
        if (gotAny || primaryOk) return true;
        return fallback_->stream(systemPrompt, userQuery, maxTokens, onChunk);
    }

    std::vector<float> embed(const std::string& text) override {
        auto vec = primary_->embed(text);
        if (!vec.empty()) return vec;
        return fallback_->embed(text);
    }

    bool isAvailable() const override {
        return primary_->isAvailable() || fallback_->isAvailable();
    }

private:
    std::unique_ptr<ILLMClient> primary_;
    std::unique_ptr<ILLMClient> fallback_;
};

// Probes whether an Ollama-style endpoint is reachable (GET / on the base).
bool ollamaReachable(const std::string& baseUrl) {
    std::string host = baseUrl;
    int port = 80;
    bool ssl = false;

    if (host.find("http://") == 0) host = host.substr(7);
    else if (host.find("https://") == 0) { host = host.substr(8); ssl = true; port = 443; }

    size_t slash = host.find('/');
    if (slash != std::string::npos) host = host.substr(0, slash);

    size_t colon = host.find(':');
    if (colon != std::string::npos) {
        port = std::stoi(host.substr(colon + 1));
        host = host.substr(0, colon);
    } else if (ssl) {
        port = 443;
    }

    httplib::Client cli(host, port);
    cli.set_connection_timeout(1, 0);
    cli.set_read_timeout(2, 0);
    auto res = cli.Get("/");
    return res && (res->status == 200 || res->status == 404);
}

// Normalizes a base URL by stripping a trailing "/chat/completions".
std::string normalizeBaseUrl(std::string url) {
    if (url.find("/chat/completions") != std::string::npos) {
        url = url.substr(0, url.find("/chat/completions"));
    }
    return url;
}

} // namespace

std::unique_ptr<ILLMClient> createLLMClient(const IConfig& config) {
    std::string mode = config.get("llm.provider", "auto");
    if (mode != "local" && mode != "cloud") mode = "auto";

    std::string localUrl = config.get("llm.local.url", "");
    std::string localKey = config.get("llm.local.key", "ollama");
    std::string localModel = config.get("llm.local.model", "llama3.1:8b");

    std::string cloudUrl = config.get("llm.cloud.url", "");
    std::string cloudKey = config.get("llm.cloud.key", "");
    std::string cloudModel = config.get("llm.cloud.model", "");
    if (cloudModel.empty()) cloudModel = "gpt-4o";

    bool localConfigured = !localUrl.empty();
    bool cloudConfigured = !cloudUrl.empty();

    if (mode == "local" && localConfigured) {
        return std::make_unique<OllamaClient>(localUrl, localModel,
                                              localUrl.find("/v1") != std::string::npos);
    }

    if (mode == "cloud" && cloudConfigured && !cloudKey.empty() && cloudKey != "ollama") {
        return std::make_unique<OpenAIClient>(normalizeBaseUrl(cloudUrl), cloudKey, cloudModel);
    }

    if (mode == "auto") {
        // Cloud is the primary endpoint; local Ollama is the fallback when
        // cloud is unreachable or fails.  This matches the user expectation
        // that a configured cloud provider takes precedence over a local
        // model that may be limited or misconfigured.
        if (cloudConfigured && !cloudKey.empty() && cloudKey != "ollama") {
            auto cloudClient = std::make_unique<OpenAIClient>(
                normalizeBaseUrl(cloudUrl), cloudKey, cloudModel);
            if (localConfigured) {
                auto localClient = std::make_unique<OllamaClient>(
                    localUrl, localModel, localUrl.find("/v1") != std::string::npos);
                return std::make_unique<FallbackLLMClient>(
                    std::move(cloudClient), std::move(localClient));
            }
            return cloudClient;
        }
        if (localConfigured) {
            return std::make_unique<OllamaClient>(
                localUrl, localModel, localUrl.find("/v1") != std::string::npos);
        }
    }

    return std::make_unique<NullLLMClient>();
}

} // namespace chronos
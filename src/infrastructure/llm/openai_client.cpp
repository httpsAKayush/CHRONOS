#include "chronos/infrastructure/llm/openai_client.hpp"
#include <nlohmann/json.hpp>
#include <httplib.h>
#include <sstream>

namespace chronos {

OpenAIClient::OpenAIClient(const std::string& baseUrl, const std::string& apiKey, const std::string& model)
    : baseUrl_(baseUrl), apiKey_(apiKey), model_(model) {
    if (baseUrl_.find("http://") == 0) {
        host_ = baseUrl_.substr(7);
    } else if (baseUrl_.find("https://") == 0) {
        host_ = baseUrl_.substr(8);
        ssl_ = true;
    } else {
        host_ = baseUrl_;
    }

    size_t portPos = host_.find(':');
    if (portPos != std::string::npos) {
        port_ = std::stoi(host_.substr(portPos + 1));
        host_ = host_.substr(0, portPos);
    } else {
        port_ = ssl_ ? 443 : 80;
    }

    size_t pathPos = host_.find('/');
    if (pathPos != std::string::npos) {
        basePath_ = host_.substr(pathPos);
        host_ = host_.substr(0, pathPos);
    } else {
        basePath_ = "/v1";
    }
}

OpenAIClient::~OpenAIClient() = default;

std::string OpenAIClient::complete(const std::string& systemPrompt,
                                    const std::string& userQuery,
                                    int maxTokens) {
    httplib::Client cli(host_, port_);
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(30, 0);
    cli.set_write_timeout(30, 0);
    if (ssl_) cli.enable_server_certificate_verification(false);

    nlohmann::json body;
    body["model"] = model_;
    body["max_tokens"] = maxTokens;
    body["stream"] = false;
    body["messages"] = nlohmann::json::array();
    if (!systemPrompt.empty()) {
        body["messages"].push_back({{"role", "system"}, {"content", systemPrompt}});
    }
    body["messages"].push_back({{"role", "user"}, {"content", userQuery}});

    httplib::Headers headers;
    headers.emplace("Content-Type", "application/json");
    if (!apiKey_.empty() && apiKey_ != "ollama") {
        headers.emplace("Authorization", "Bearer " + apiKey_);
    }

    auto res = cli.Post(basePath_ + "/chat/completions", headers, body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), "application/json");
    if (!res) return "";

    try {
        auto j = nlohmann::json::parse(res->body);
        if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
            const auto& msg = j["choices"][0]["message"];
            if (msg.contains("content") && msg["content"].is_string()) {
                return msg["content"].get<std::string>();
            }
        }
    } catch (...) {}

    return "";
}

bool OpenAIClient::stream(const std::string& systemPrompt,
                          const std::string& userQuery,
                          int maxTokens,
                          const std::function<void(const std::string&)>& onChunk) {
    return streamChat({{"system", systemPrompt}, {"user", userQuery}}, maxTokens, onChunk);
}

bool OpenAIClient::streamChat(const std::vector<ChatMessage>& msg_history,
                              int maxTokens,
                              const std::function<void(const std::string&)>& onChunk) {
    httplib::Client cli(host_, port_);
    if (ssl_) {
        // Just rely on cpp-httplib's default cert loading if built with OpenSSL
        cli.enable_server_certificate_verification(false);
    }
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(30, 0);
    cli.set_write_timeout(30, 0);

    nlohmann::json body;
    body["model"] = model_;
    body["max_tokens"] = maxTokens;
    body["stream"] = true;
    body["messages"] = nlohmann::json::array();
    for (const auto& msg : msg_history) {
        if (!msg.content.empty()) {
            body["messages"].push_back({{"role", msg.role}, {"content", msg.content}});
        }
    }

    httplib::Request req;
    req.method = "POST";
    req.path = basePath_ + "/chat/completions";
    req.headers = {
        {"Content-Type", "application/json"},
        {"Authorization", "Bearer " + apiKey_}
    };
    req.body = body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);

    bool gotContent = false;
    bool cleanEnd = false;
    
    req.response_handler = [&](const httplib::Response& res) -> bool {
        if (res.status != 200) {
            if (!gotContent) {
                onChunk("[API Error] HTTP " + std::to_string(res.status) + ": " + res.body);
            }
            return false;
        }
        return true;
    };

    req.content_receiver = [&](const char* data, size_t data_length,
                               uint64_t, uint64_t) -> bool {
        std::string chunk(data, data_length);
        std::istringstream ss(chunk);
        std::string line;
        while (std::getline(ss, line)) {
            if (line.find("data: ") == 0) {
                std::string payload = line.substr(6);
                if (payload == "[DONE]") {
                    cleanEnd = true;
                    return true;
                }
                try {
                    auto j = nlohmann::json::parse(payload);
                    if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
                        const auto& delta = j["choices"][0]["delta"];
                        if (delta.contains("content") && !delta["content"].is_null()) {
                            std::string content = delta["content"].get<std::string>();
                            if (!content.empty()) {
                                onChunk(content);
                                gotContent = true;
                            }
                        }
                    }
                } catch (...) {}
            }
        }
        return true;
    };

    auto res = cli.send(req);
    if (!res || res->status != 200) {
        if (!gotContent) onChunk("[API Error] Connection failed to " + host_);
        return false;
    }

    return cleanEnd || gotContent;
}

std::vector<float> OpenAIClient::embed(const std::string& text) {
    httplib::Client cli(host_, port_);
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(30, 0);
    cli.set_write_timeout(30, 0);
    if (ssl_) cli.enable_server_certificate_verification(false);

    nlohmann::json body;
    body["input"] = text;
    body["model"] = model_;

    httplib::Headers headers;
    headers.emplace("Content-Type", "application/json");
    if (!apiKey_.empty() && apiKey_ != "ollama") {
        headers.emplace("Authorization", "Bearer " + apiKey_);
    }

    auto res = cli.Post(basePath_ + "/embeddings", headers, body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), "application/json");
    if (!res) return {};

    try {
        auto j = nlohmann::json::parse(res->body);
        if (j.contains("data") && j["data"].is_array() && !j["data"].empty()) {
            const auto& emb = j["data"][0]["embedding"];
            if (emb.is_array()) {
                std::vector<float> vec;
                for (const auto& v : emb) {
                    vec.push_back(v.get<float>());
                }
                return vec;
            }
        }
    } catch (...) {}

    return {};
}

bool OpenAIClient::isAvailable() const {
    return !baseUrl_.empty() && !model_.empty();
}

} // namespace chronos
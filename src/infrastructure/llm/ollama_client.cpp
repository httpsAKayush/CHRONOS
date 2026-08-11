#include "chronos/infrastructure/llm/ollama_client.hpp"
#include <nlohmann/json.hpp>
#include <httplib.h>
#include <sstream>
#include <iostream>

namespace chronos {

OllamaClient::OllamaClient(const std::string& baseUrl, const std::string& model, bool openAiCompat)
    : baseUrl_(baseUrl), model_(model), openAiCompat_(openAiCompat) {
    std::string url = baseUrl;
    if (url.find("http://") == 0) url = url.substr(7);
    else if (url.find("https://") == 0) url = url.substr(8);

    size_t portPos = url.find(':');
    if (portPos != std::string::npos) {
        host_ = url.substr(0, portPos);
        port_ = std::stoi(url.substr(portPos + 1));
    } else {
        host_ = url;
        port_ = 11434;
    }

    size_t slashPos = host_.find('/');
    if (slashPos != std::string::npos) {
        host_ = host_.substr(0, slashPos);
    }
}

OllamaClient::~OllamaClient() = default;

std::string OllamaClient::complete(const std::string& systemPrompt,
                                    const std::string& userQuery,
                                    int maxTokens) {
    httplib::Client cli(host_, port_);
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(120, 0);
    cli.set_write_timeout(30, 0);

    if (openAiCompat_) {
        nlohmann::json body;
        body["model"] = model_;
        body["max_tokens"] = maxTokens;
        body["stream"] = false;
        body["messages"] = nlohmann::json::array();
        if (!systemPrompt.empty()) {
            body["messages"].push_back({{"role", "system"}, {"content", systemPrompt}});
        }
        body["messages"].push_back({{"role", "user"}, {"content", userQuery}});

        auto res = cli.Post("/v1/chat/completions", body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), "application/json");
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

    nlohmann::json body;
    body["model"] = model_;
    body["stream"] = false;
    nlohmann::json messages = nlohmann::json::array();
    if (!systemPrompt.empty()) {
        messages.push_back({{"role", "system"}, {"content", systemPrompt}});
    }
    messages.push_back({{"role", "user"}, {"content", userQuery}});
    body["messages"] = messages;

    auto res = cli.Post("/api/chat", body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), "application/json");
    if (!res) {
        std::cerr << "[Ollama] Connection failed for model '" << model_ << "'\n";
        return "";
    }
    if (res->status != 200) {
        std::cerr << "[Ollama] HTTP " << res->status << " for model '" << model_ << "': "
                  << res->body.substr(0, std::min<size_t>(200, res->body.size())) << "\n";
        return "";
    }

    try {
        auto j = nlohmann::json::parse(res->body);
        if (j.contains("error") && j["error"].is_string()) {
            std::cerr << "[Ollama] " << j["error"].get<std::string>() << "\n";
            return "";
        }
        if (j.contains("message") && j["message"].contains("content")) {
            std::string content = j["message"]["content"].get<std::string>();
            if (!content.empty()) return content;
        }
        return "";
    } catch (...) {
        std::cerr << "[Ollama] Failed to parse response body\n";
        return "";
    }
}

bool OllamaClient::stream(const std::string& systemPrompt,
                          const std::string& userQuery,
                          int maxTokens,
                          const std::function<void(const std::string&)>& onChunk) {
    httplib::Client cli(host_, port_);
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(30, 0);
    cli.set_write_timeout(30, 0);

    if (openAiCompat_) {
        nlohmann::json body;
        body["model"] = model_;
        body["max_tokens"] = maxTokens;
        body["stream"] = true;
        body["messages"] = nlohmann::json::array();
        if (!systemPrompt.empty()) {
            body["messages"].push_back({{"role", "system"}, {"content", systemPrompt}});
        }
        body["messages"].push_back({{"role", "user"}, {"content", userQuery}});

        httplib::Request req;
        req.method = "POST";
        req.path = "/v1/chat/completions";
        req.headers = {{"Content-Type", "application/json"}};
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
            if (!gotContent) onChunk("[API Error] Connection failed");
            return false;
        }
        return cleanEnd || gotContent;
    }

    nlohmann::json body;
    body["model"] = model_;
    body["stream"] = true;
    nlohmann::json messages = nlohmann::json::array();
    if (!systemPrompt.empty()) {
        messages.push_back({{"role", "system"}, {"content", systemPrompt}});
    }
    messages.push_back({{"role", "user"}, {"content", userQuery}});
    body["messages"] = messages;

    httplib::Request req;
    req.method = "POST";
    req.path = "/api/chat";
    req.headers = {{"Content-Type", "application/json"}};
    req.body = body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);

    bool gotContent = false;
    bool sawDone = false;

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
            if (line.empty()) continue;
            try {
                auto j = nlohmann::json::parse(line);
                
                if (j.contains("done") && j["done"].is_boolean() && j["done"].get<bool>()) {
                    sawDone = true;
                    return true;
                }

                if (j.contains("message") && j["message"].contains("content")) {
                    std::string content = j["message"]["content"].get<std::string>();
                    if (!content.empty()) {
                        onChunk(content);
                        gotContent = true;
                    }
                }
            } catch (...) {}
        }
        return true;
    };

    auto res = cli.send(req);
    if (!res || res->status != 200) {
        if (!gotContent) onChunk("[API Error] Connection failed");
        return false;
    }

    return sawDone || gotContent;
}

std::vector<float> OllamaClient::embed(const std::string& text) {
    httplib::Client cli(host_, port_);
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(30, 0);
    cli.set_write_timeout(30, 0);

    if (openAiCompat_) {
        nlohmann::json body;
        body["input"] = text;
        body["model"] = model_;

        auto res = cli.Post("/v1/embeddings", body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), "application/json");
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

    nlohmann::json body;
    body["model"] = model_;
    body["prompt"] = text;

    auto res = cli.Post("/api/embeddings", body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), "application/json");
    if (!res) return {};

    try {
        auto j = nlohmann::json::parse(res->body);
        if (j.contains("embedding") && j["embedding"].is_array()) {
            std::vector<float> vec;
            for (const auto& v : j["embedding"]) {
                vec.push_back(v.get<float>());
            }
            return vec;
        }
    } catch (...) {}

    return {};
}

bool OllamaClient::isAvailable() const {
    return !baseUrl_.empty() && !model_.empty();
}

} // namespace chronos
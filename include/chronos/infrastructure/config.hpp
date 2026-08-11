#pragma once

#include "chronos/core/IConfig.hpp"
#include <string>
#include <unordered_map>

namespace chronos {

class Config : public IConfig {
public:
    Config();
    ~Config() = default;

    std::string get(const std::string& key, const std::string& defaultValue = "") const override;
    bool getBool(const std::string& key, bool defaultValue = false) const override;
    int getInt(const std::string& key, int defaultValue = 0) const override;
    bool has(const std::string& key) const override;
    std::unordered_map<std::string, std::string> getAll() const override;

    // Dual-profile accessors.
    //
    // providerMode() returns "auto", "local", or "cloud" — whatever the user
    // configured via `llm.provider`.  "auto" (default) means probe local
    // first, fall back to cloud.
    std::string providerMode() const;

    // ── Local profile (Ollama / localhost) ─────────────────────────────
    std::string localUrl() const;
    std::string localKey() const;
    std::string localModel() const;
    bool        localConfigured() const;

    // ── Cloud profile (NVIDIA / OpenRouter / OpenAI / …) ─────────────
    std::string cloudUrl() const;
    std::string cloudKey() const;
    std::string cloudModel() const;
    bool        cloudConfigured() const;

    // ── Resolved convenience (respects providerMode) ──────────────────
    // When mode == "auto", returns whichever profile is available (local
    // preferred, cloud fallback).  When mode == "local" or "cloud", returns
    // that profile's values exclusively.
    std::string apiKey() const;
    std::string apiUrl() const;
    std::string baseUrl() const;
    std::string model() const;
    std::string provider() const;     // "ollama" or "openai" (for adapter selection)
    bool        valid() const;

    void set(const std::string& key, const std::string& value);
    bool save();

private:
    void loadFromEnv();
    void loadFromGlobalConfig();
    void loadFromLocalConfig();
    void loadFromDotEnv();
    void migrateLegacyKeys();

    std::unordered_map<std::string, std::string> data_;
};

} // namespace chronos
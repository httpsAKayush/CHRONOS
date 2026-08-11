#include "chronos/infrastructure/config.hpp"
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace chronos {
namespace fs = std::filesystem;

Config::Config() {
    loadFromEnv();
    loadFromGlobalConfig();
    loadFromLocalConfig();
    loadFromDotEnv();
    migrateLegacyKeys();
}

void Config::loadFromEnv() {
    auto checkEnv = [this](const char* name, const std::string& key) {
        const char* val = std::getenv(name);
        if (val && val[0]) data_[key] = val;
    };

    checkEnv("NVI_API_KEY", "nvi.api_key");
    checkEnv("OPENROUTER_API_KEY", "openrouter.api_key");
    checkEnv("OPENAI_API_KEY", "openai.api_key");
    checkEnv("OR_MODEL", "openrouter.model");
    checkEnv("OR_API_URL", "openrouter.api_url");

    checkEnv("LLM_PROVIDER", "llm.provider");
    checkEnv("LLM_LOCAL_URL", "llm.local.url");
    checkEnv("LLM_LOCAL_KEY", "llm.local.key");
    checkEnv("LLM_LOCAL_MODEL", "llm.local.model");
    checkEnv("LLM_CLOUD_URL", "llm.cloud.url");
    checkEnv("LLM_CLOUD_KEY", "llm.cloud.key");
    checkEnv("LLM_CLOUD_MODEL", "llm.cloud.model");
}

void Config::loadFromGlobalConfig() {
    const char* homeDir = std::getenv("HOME");
    if (!homeDir) return;
    fs::path path = fs::path(homeDir) / ".chronos" / "config.json";
    if (!fs::exists(path)) return;

    std::ifstream in(path);
    if (!in) return;
    try {
        nlohmann::json j;
        in >> j;
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (it.value().is_string() && !data_.count(it.key())) {
                data_[it.key()] = it.value().get<std::string>();
            }
        }
    } catch (...) {}
}

void Config::loadFromLocalConfig() {
    fs::path path = fs::current_path() / ".chronos" / "config.json";
    if (!fs::exists(path)) return;

    std::ifstream in(path);
    if (!in) return;
    try {
        nlohmann::json j;
        in >> j;
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (it.value().is_string() && !data_.count(it.key())) {
                data_[it.key()] = it.value().get<std::string>();
            }
        }
    } catch (...) {}
}

void Config::loadFromDotEnv() {
    fs::path dotenv = fs::current_path() / ".env";
    if (!fs::exists(dotenv)) return;

    std::ifstream in(dotenv);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
            v = v.substr(1, v.size() - 2);
        }

        else if (k == "LLM_PROVIDER") k = "llm.provider";
        else if (k == "LLM_LOCAL_URL") k = "llm.local.url";
        else if (k == "LLM_LOCAL_KEY") k = "llm.local.key";
        else if (k == "LLM_LOCAL_MODEL") k = "llm.local.model";
        else if (k == "LLM_CLOUD_URL") k = "llm.cloud.url";
        else if (k == "LLM_CLOUD_KEY") k = "llm.cloud.key";
        else if (k == "LLM_CLOUD_MODEL") k = "llm.cloud.model";

        data_[k] = v;
    }
}

// ── Legacy key migration ────────────────────────────────────────────────
void Config::migrateLegacyKeys() {
    // Backward compatibility: flat legacy keys (llm.api_url / llm.api_key /
    // llm.model) are folded into the cloud profile when the cloud keys are
    // absent. They were the "old internet" config, so they always map to
    // cloud regardless of whether a local profile is configured.
    if (has("llm.api_url") && !has("llm.cloud.url")) {
        std::string legacyUrl = get("llm.api_url");
        std::string legacyKey = has("llm.api_key") ? get("llm.api_key") : "";
        std::string legacyModel = has("llm.model") ? get("llm.model") : "";

        data_["llm.cloud.url"] = legacyUrl;
        if (!legacyKey.empty()) data_["llm.cloud.key"] = legacyKey;
        if (!legacyModel.empty()) data_["llm.cloud.model"] = legacyModel;
    }

    // llm.url (separate legacy key) is deprecated — ignore it so it can no
    // longer shadow the local profile.
}

// ── Raw key access ──────────────────────────────────────────────────────
std::string Config::get(const std::string& key, const std::string& defaultValue) const {
    auto it = data_.find(key);
    return (it != data_.end()) ? it->second : defaultValue;
}

bool Config::getBool(const std::string& key, bool defaultValue) const {
    auto it = data_.find(key);
    if (it == data_.end()) return defaultValue;
    return it->second == "true" || it->second == "1" || it->second == "yes";
}

int Config::getInt(const std::string& key, int defaultValue) const {
    auto it = data_.find(key);
    if (it == data_.end()) return defaultValue;
    try { return std::stoi(it->second); } catch (...) { return defaultValue; }
}

bool Config::has(const std::string& key) const {
    return data_.count(key) > 0;
}

std::unordered_map<std::string, std::string> Config::getAll() const {
    return data_;
}

// ── Provider mode ───────────────────────────────────────────────────────
std::string Config::providerMode() const {
    std::string mode = get("llm.provider", "auto");
    if (mode != "local" && mode != "cloud") return "auto";
    return mode;
}

// ── Local profile ───────────────────────────────────────────────────────
std::string Config::localUrl() const {
    return get("llm.local.url", "");
}

std::string Config::localKey() const {
    return get("llm.local.key", "ollama");
}

std::string Config::localModel() const {
    return get("llm.local.model", "llama3.1:8b");
}

bool Config::localConfigured() const {
    return has("llm.local.url");
}

// ── Cloud profile ───────────────────────────────────────────────────────
std::string Config::cloudUrl() const {
    return get("llm.cloud.url", "");
}

std::string Config::cloudKey() const {
    return get("llm.cloud.key", "");
}

std::string Config::cloudModel() const {
    return get("llm.cloud.model", "");
}

bool Config::cloudConfigured() const {
    return has("llm.cloud.url") && !get("llm.cloud.url").empty();
}

// ── Resolved convenience (respects providerMode) ────────────────────────
// In "auto" mode the local profile is preferred; the cloud profile is only
// returned when the local one is absent (the factory probes availability
// separately).  In "local"/"cloud" modes only that profile is used.
std::string Config::provider() const {
    std::string mode = providerMode();
    if (mode == "local") return "ollama";
    if (mode == "cloud") return "openai";

    // auto
    if (localConfigured()) return "ollama";
    return "openai";
}

std::string Config::apiUrl() const {
    std::string mode = providerMode();
    std::string url;
    if (mode == "local") url = localUrl();
    else if (mode == "cloud") url = cloudUrl();
    else url = localConfigured() ? localUrl() : cloudUrl();

    if (url.find("completions") != std::string::npos) return url;
    if (url.back() == '/') url.pop_back();
    if (url.find("/v1") != std::string::npos) return url + "/chat/completions";
    return url + "/v1/chat/completions";
}

std::string Config::baseUrl() const {
    std::string url = apiUrl();
    size_t pos = url.find("/chat/completions");
    if (pos != std::string::npos) url = url.substr(0, pos);
    return url;
}

std::string Config::apiKey() const {
    std::string mode = providerMode();
    if (mode == "local") return localKey();
    if (mode == "cloud") return cloudKey();
    return localConfigured() ? localKey() : cloudKey();
}

std::string Config::model() const {
    std::string mode = providerMode();
    if (mode == "local") return localModel();
    if (mode == "cloud") return cloudModel();
    return localConfigured() ? localModel() : cloudModel();
}

bool Config::valid() const {
    return localConfigured() || cloudConfigured();
}

void Config::set(const std::string& key, const std::string& value) {
    data_[key] = value;
}

bool Config::save() {
    const char* homeDir = std::getenv("HOME");
    if (!homeDir) return false;
    fs::path path = fs::path(homeDir) / ".chronos" / "config.json";
    if (!fs::exists(path.parent_path())) {
        fs::create_directories(path.parent_path());
    }

    // Preserve existing entries that may have come from other sources.
    nlohmann::json j;
    for (const auto& [k, v] : data_) {
        j[k] = v;
    }

    std::ofstream out(path);
    if (!out) return false;
    out << j.dump(4);
    return true;
}

} // namespace chronos
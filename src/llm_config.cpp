#include "chronos/llm_config.hpp"
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace chronos {
namespace fs = std::filesystem;

// Reads .env from the current working directory and injects vars into the process environment.
// Skips vars that are already set so shell exports always win.
static void parseAndLoadDotEnv() {
    std::ifstream file(".env");
    if (!file.is_open()) return;

    std::string line;
    while (std::getline(file, line)) {
        // Trim leading/trailing whitespace
        auto ltrim = [](std::string& s) { s.erase(0, s.find_first_not_of(" \t\r\n")); };
        auto rtrim = [](std::string& s) {
            auto p = s.find_last_not_of(" \t\r\n");
            if (p != std::string::npos) s.erase(p + 1); else s.clear();
        };
        ltrim(line); rtrim(line);

        if (line.empty() || line[0] == '#') continue;

        size_t eqPos = line.find('=');
        if (eqPos == std::string::npos) continue;

        std::string key = line.substr(0, eqPos);
        std::string val = line.substr(eqPos + 1);

        ltrim(key); rtrim(key);
        // Strip surrounding quotes from the value
        val.erase(0, val.find_first_not_of(" \t\"'"));
        val.erase(val.find_last_not_of(" \t\"'") + 1);

        // Only set if not already in the environment (shell overrides .env)
        if (!val.empty() && !std::getenv(key.c_str())) {
            setenv(key.c_str(), val.c_str(), 0);
        }
    }
}

LlmConfig loadLlmConfig() {
    // Always load .env first so vars are available to getenv() calls below
    parseAndLoadDotEnv();

    LlmConfig config;
    
    // First priority: User's explicit config.json settings
    auto loadConfigJson = [](const fs::path& p) {
        nlohmann::json j;
        if (fs::exists(p)) {
            std::ifstream in(p);
            try { in >> j; } catch (...) {}
        }
        return j;
    };

    const char* homeDir = getenv("HOME");
    fs::path globalPath = fs::path(homeDir ? homeDir : "") / ".chronos" / "config.json";
    fs::path localPath = fs::current_path() / ".chronos" / "config.json";
    nlohmann::json globalConf = loadConfigJson(globalPath);
    nlohmann::json localConf = loadConfigJson(localPath);
    
    auto getVal = [&](const std::string& key) -> std::string {
        if (localConf.contains(key) && localConf[key].is_string()) return localConf[key].get<std::string>();
        if (globalConf.contains(key) && globalConf[key].is_string()) return globalConf[key].get<std::string>();
        return "";
    };
    
    std::string cfgModel = getVal("llm.model");
    std::string cfgUrl = getVal("llm.api_url");
    std::string cfgKey = getVal("llm.api_key");
    
    if (!cfgUrl.empty() && !cfgKey.empty()) {
        config.apiKey = cfgKey;
        // Append completions path if missing
        if (cfgUrl.find("completions") == std::string::npos) {
            std::string base = cfgUrl;
            if (base.back() == '/') base.pop_back();
            if (base.find("/v1") != std::string::npos) config.apiUrl = base + "/chat/completions";
            else config.apiUrl = base + "/v1/chat/completions"; // ollama fallback style
        } else {
            config.apiUrl = cfgUrl;
        }
        config.modelName = !cfgModel.empty() ? cfgModel : "gpt-4o";
        return config;
    }

    // Priority 1: Generic vars — matches the NVIDIA pattern in user's .env:
    //   API_KEY="nvapi-..."
    //   API_URL="https://integrate.api.nvidia.com/v1"
    //   MODEL="nvidia/nemotron-3-ultra-550b-a55b"
    const char* genKey   = std::getenv("API_KEY");
    const char* genUrl   = std::getenv("API_URL");
    const char* genModel = std::getenv("MODEL");

    if (genKey && genKey[0] && genUrl && genUrl[0]) {
        config.apiKey    = genKey;
        // API_URL is a base URL; append the completions path
        std::string base = genUrl;
        if (base.back() == '/') base.pop_back();
        config.apiUrl    = base + "/chat/completions";
        config.modelName = (genModel && genModel[0]) ? genModel : "nvidia/nemotron-3-nano-omni-30b-a3b-reasoning:free";
        return config;
    }

    // Priority 2: Provider-specific fallbacks
    const char* nviKey  = std::getenv("NVI_API_KEY");
    const char* orKey   = std::getenv("OPENROUTER_API_KEY");
    const char* oaKey   = std::getenv("OPENAI_API_KEY");
    const char* orModel = std::getenv("OR_MODEL");
    const char* orUrl   = std::getenv("OR_API_URL");

    if (nviKey && nviKey[0]) {
        config.apiKey    = nviKey;
        config.apiUrl    = "https://api.tokenrouter.com/v1/chat/completions";
        config.modelName = (genModel && genModel[0]) ? genModel : "nvidia/nemotron-3-nano-omni-30b-a3b-reasoning:free";
    } else if (orKey && orKey[0]) {
        config.apiKey    = orKey;
        config.apiUrl    = (orUrl && orUrl[0]) ? orUrl : "https://openrouter.ai/api/v1/chat/completions";
        config.modelName = (orModel && orModel[0]) ? orModel : "meta-llama/llama-3.1-8b-instruct:free";
    } else if (oaKey && oaKey[0]) {
        config.apiKey    = oaKey;
        config.apiUrl    = "https://api.openai.com/v1/chat/completions";
        config.modelName = (genModel && genModel[0]) ? genModel : "gpt-4o";
    }

    return config;
}

} // namespace chronos
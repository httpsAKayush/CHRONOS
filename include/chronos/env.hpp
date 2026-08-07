#pragma once
#include <string>
#include <fstream>
#include <unordered_map>
#include <filesystem>

namespace chronos {

inline std::unordered_map<std::string, std::string> loadEnv(const std::string& repoRoot) {
    std::unordered_map<std::string, std::string> env;
    std::ifstream in(std::filesystem::path(repoRoot) / ".env");
    if (!in) return env;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto pos = line.find('=');
        if (pos != std::string::npos) {
            std::string key = line.substr(0, pos);
            std::string val = line.substr(pos + 1);
            // Optional: trim quotes
            if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
                val = val.substr(1, val.size() - 2);
            }
            env[key] = val;
        }
    }
    return env;
}

}

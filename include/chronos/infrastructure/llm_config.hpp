#pragma once

#include <string>

namespace chronos {

struct LlmConfig {
    std::string apiKey;
    std::string apiUrl;
    std::string modelName;

    bool isValid() const {
        return !apiKey.empty() && !apiUrl.empty() && !modelName.empty();
    }
};

LlmConfig loadLlmConfig();

} // namespace chronos

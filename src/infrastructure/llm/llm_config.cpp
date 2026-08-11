#include "chronos/infrastructure/llm_config.hpp"
#include "chronos/infrastructure/config.hpp"

namespace chronos {

LlmConfig loadLlmConfig() {
    Config cfg;
    LlmConfig config;
    config.apiKey = cfg.apiKey();
    config.apiUrl = cfg.apiUrl();
    config.modelName = cfg.model();
    return config;
}

} // namespace chronos

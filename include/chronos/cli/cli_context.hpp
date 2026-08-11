#pragma once

#include "chronos/core/IConfig.hpp"
#include "chronos/core/IStorage.hpp"
#include "chronos/core/ILLMClient.hpp"
#include "chronos/infrastructure/codex.hpp"
#include "chronos/infrastructure/vector_index.hpp"
#include <memory>
#include <string>

namespace chronos {

struct CliContext {
    std::string repoRoot;
    std::shared_ptr<IConfig> config;
    std::shared_ptr<Codex> storage;
    std::shared_ptr<VectorIndex> vectors;
    std::unique_ptr<ILLMClient> llm;
};

} // namespace chronos
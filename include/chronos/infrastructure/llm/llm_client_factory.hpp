#pragma once

#include "chronos/core/ILLMClient.hpp"
#include "chronos/core/IConfig.hpp"
#include <memory>

namespace chronos {

std::unique_ptr<ILLMClient> createLLMClient(const IConfig& config);

} // namespace chronos

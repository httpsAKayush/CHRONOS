#pragma once

#include <string>

namespace chronos {

class ILLMClient {
public:
    virtual ~ILLMClient() = default;

    virtual std::string query(const std::string& prompt) = 0;
    virtual std::string generateSummary(const std::string& codeSnippet) = 0;
    virtual bool isAvailable() const = 0;
};

} // namespace chronos

#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <functional>

namespace chronos {

struct ChatMessage {
    std::string role;
    std::string content;
};

// The LLM port (Hexagonal Architecture). Pure interface — no HTTP, no config,
// no provider specifics. Domain/use-case code depends only on this abstraction
// so the OpenAIClient / OllamaClient adapters (and test mocks) stay swappable.
class ILLMClient {
public:
    virtual ~ILLMClient() = default;

    // Single-shot chat completion over the provider's primary chat endpoint.
    // Returns the assistant text (empty string on unrecoverable failure).
    virtual std::string complete(const std::string& systemPrompt,
                                 const std::string& userQuery,
                                 int maxTokens = 2048) = 0;

    // Streaming chat completion. Invokes onChunk for every non-empty text
    // delta. Returns true when the stream completed normally (including the
    // provider's explicit terminal sentinel), false on socket/parse failure.
    virtual bool stream(const std::string& systemPrompt,
                        const std::string& userQuery,
                        int maxTokens,
                        const std::function<void(const std::string&)>& onChunk) = 0;

    // Structured streaming chat completion. Takes a history of ChatMessages.
    virtual bool streamChat(const std::vector<ChatMessage>& messages,
                            int maxTokens,
                            const std::function<void(const std::string&)>& onChunk) = 0;

    // Dense text embedding used by the vector index and the HyDE RAG pipeline.
    virtual std::vector<float> embed(const std::string& text) = 0;

    // Legacy convenience helpers (kept for backwards compatibility with
    // existing callers and test mocks).
    virtual std::string query(const std::string& prompt) { return complete("", prompt, 2048); }
    virtual std::string generateSummary(const std::string& codeSnippet) {
        return complete(
            "You are a code summarization tool. Provide a concise 3-to-5 word label.",
            codeSnippet, 64);
    }

    // Whether the underlying provider/config is usable right now.
    virtual bool isAvailable() const = 0;
};

} // namespace chronos

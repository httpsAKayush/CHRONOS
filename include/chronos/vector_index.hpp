#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace chronos {

struct EmbeddingRecord {
    std::string nodeId;          // matches Codex Node.id
    std::vector<float> vector;   // fixed dimensionality (see VectorIndex::kDim)
    int64_t timestamp = 0;       // Temporal metadata for the decay algorithm
};

struct SeedMatch {
    std::string nodeId;
    float score = 0.f;           // similarity score
};

class VectorIndex {
public:
    static constexpr int kDim = 1536; // 768 for Nomic-Embed or Llama embeddings

    explicit VectorIndex(const std::string& repoRoot);
    ~VectorIndex();

    void upsert(const EmbeddingRecord& rec);
    void remove(const std::string& nodeId);

    // Hop 1 of Two-Hop Retrieval
    // Includes a temporal decay applied over `queryTimestamp` (or current time if 0)
    std::vector<SeedMatch> search(const std::vector<float>& queryVector, int topK, int64_t queryTimestamp = 0) const;

private:
    std::string path_;
    struct Impl;
    Impl* impl_;
};

// Generates true semantic embeddings by calling a local Ollama daemon
std::vector<float> embedText(const std::string& text);

} // namespace chronos

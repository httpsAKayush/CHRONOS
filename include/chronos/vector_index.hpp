#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <ctime>
#include <cmath>
#include <algorithm>

namespace chronos {

enum class MemoryTier {
    Hot,   // <= 30 days
    Warm,  // 30 days < age <= 365 days
    Cold   // > 365 days (31,536,000 seconds)
};

MemoryTier getMemoryTier(int64_t commitTimestamp, int64_t currentTimestamp = 0);

struct SQ8Vector {
    std::vector<int8_t> data;
    float minVal = 0.0f;
    float scale = 1.0f;

    static SQ8Vector quantize(const std::vector<float>& vec);
    std::vector<float> dequantize() const;
    size_t sizeBytes() const;
};

struct EmbeddingRecord {
    std::string nodeId;          // matches Codex Node.id
    std::vector<float> vector;   // fixed dimensionality (see VectorIndex::kDim)
    int64_t timestamp = 0;       // Temporal metadata for the decay algorithm
    MemoryTier tier = MemoryTier::Hot;
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

    size_t size() const;
    bool contains(const std::string& nodeId) const;

private:
    std::string path_;
    struct Impl;
    Impl* impl_;
};

// Generates true semantic embeddings by calling a local Ollama daemon
std::vector<float> embedText(const std::string& text);

} // namespace chronos


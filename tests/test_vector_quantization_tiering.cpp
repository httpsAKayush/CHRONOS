#include "test_framework.hpp"
#include "chronos/infrastructure/vector_index.hpp"
#include "chronos/infrastructure/codex.hpp"
#include "chronos/domain/ast_indexer.hpp"
#include <filesystem>
#include <fstream>
#include <vector>
#include <cmath>
#include <ctime>
#include <algorithm>
#include <iostream>

using namespace chronos;
namespace fs = std::filesystem;

namespace {

float computeCosineSimilarity(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return 0.0f;
    double dot = 0.0, normA = 0.0, normB = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += static_cast<double>(a[i]) * static_cast<double>(b[i]);
        normA += static_cast<double>(a[i]) * static_cast<double>(a[i]);
        normB += static_cast<double>(b[i]) * static_cast<double>(b[i]);
    }
    if (normA == 0.0 || normB == 0.0) return 0.0f;
    return static_cast<float>(dot / (std::sqrt(normA) * std::sqrt(normB)));
}

} // namespace

void test_sq8_quantization_precision() {
    // 1. Synthetic FP32 test vector (1536D) with varied float values
    std::vector<float> original(VectorIndex::kDim);
    for (int i = 0; i < VectorIndex::kDim; ++i) {
        original[i] = std::sin(static_cast<float>(i) * 0.1f) * std::exp(-static_cast<float>(i) * 0.001f);
    }

    // Quantize to SQ8
    SQ8Vector sq = SQ8Vector::quantize(original);
    
    // Verify quantized data size is 1536 bytes
    CHRONOS_CHECK(sq.data.size() == VectorIndex::kDim);

    // Dequantize back to FP32
    std::vector<float> restored = sq.dequantize();
    CHRONOS_CHECK(restored.size() == VectorIndex::kDim);

    // Compute Cosine Similarity between original and dequantized vector
    float sim = computeCosineSimilarity(original, restored);
    std::cout << "[Test SQ8 Precision] Cosine similarity: " << sim << "\n";
    CHRONOS_CHECK(sim > 0.98f);

    // Also test with embedText vector
    std::vector<float> textVec = embedText("void calculateTotal(int count, float price) { return count * price; }");
    SQ8Vector sqText = SQ8Vector::quantize(textVec);
    std::vector<float> restoredText = sqText.dequantize();
    float textSim = computeCosineSimilarity(textVec, restoredText);
    std::cout << "[Test SQ8 Precision] EmbedText Cosine similarity: " << textSim << "\n";
    CHRONOS_CHECK(textSim > 0.98f);
}

void test_sq8_memory_reduction() {
    const size_t kDim = VectorIndex::kDim; // 1536
    size_t fp32PerVectorBytes = kDim * sizeof(float); // 6,144 bytes
    
    SQ8Vector sq;
    sq.data.resize(kDim);
    size_t sq8PerVectorBytes = sq.sizeBytes(); // 1,536 + 4 + 4 = 1,544 bytes

    CHRONOS_CHECK(fp32PerVectorBytes == 6144);
    CHRONOS_CHECK(sq8PerVectorBytes == 1544);

    float reduction = 1.0f - (static_cast<float>(sq8PerVectorBytes) / static_cast<float>(fp32PerVectorBytes));
    std::cout << "[Test Memory Reduction] FP32 size: " << fp32PerVectorBytes 
              << " B, SQ8 size: " << sq8PerVectorBytes 
              << " B, Reduction: " << (reduction * 100.0f) << "%\n";

    // Expect reduction in range 74.87% ~ 75.0%
    CHRONOS_CHECK(reduction >= 0.748f && reduction <= 0.751f);

    // Scale test: 1,000 vectors
    size_t thousandFp32Bytes = 1000 * fp32PerVectorBytes;
    size_t thousandSq8Bytes = 1000 * sq8PerVectorBytes;
    size_t bytesSaved = thousandFp32Bytes - thousandSq8Bytes;
    
    CHRONOS_CHECK(thousandFp32Bytes == 6144000);
    CHRONOS_CHECK(thousandSq8Bytes == 1544000);
    CHRONOS_CHECK(bytesSaved == 4600000);
}

void test_memory_tier_classification() {
    int64_t refTime = 1700000000; // Fixed reference time (Unix timestamp)

    // Hot tier (<= 30 days = 2,592,000s)
    CHRONOS_CHECK(getMemoryTier(refTime - 100, refTime) == MemoryTier::Hot);
    CHRONOS_CHECK(getMemoryTier(refTime - (20 * 86400), refTime) == MemoryTier::Hot);
    CHRONOS_CHECK(getMemoryTier(refTime - (30 * 86400), refTime) == MemoryTier::Hot);

    // Warm tier (> 30 days and <= 365 days = 31,536,000s)
    CHRONOS_CHECK(getMemoryTier(refTime - (31 * 86400), refTime) == MemoryTier::Warm);
    CHRONOS_CHECK(getMemoryTier(refTime - (180 * 86400), refTime) == MemoryTier::Warm);
    CHRONOS_CHECK(getMemoryTier(refTime - (365 * 86400), refTime) == MemoryTier::Warm);

    // Cold tier (> 365 days)
    CHRONOS_CHECK(getMemoryTier(refTime - (366 * 86400), refTime) == MemoryTier::Cold);
    CHRONOS_CHECK(getMemoryTier(refTime - (500 * 86400), refTime) == MemoryTier::Cold);
    CHRONOS_CHECK(getMemoryTier(refTime - (1000 * 86400), refTime) == MemoryTier::Cold);

    // Edge cases (timestamp = 0 or future)
    CHRONOS_CHECK(getMemoryTier(0, refTime) == MemoryTier::Hot);
    CHRONOS_CHECK(getMemoryTier(refTime + 1000, refTime) == MemoryTier::Hot);
}

void test_cold_tier_vector_omission() {
    fs::path tmp = fs::temp_directory_path() / "chronos_test_cold_tier_omission";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    {
        Codex codex(tmp.string());
        VectorIndex vectorIndex(tmp.string());
        AstIndexer astIndexer(codex, vectorIndex, tmp.string());

        int64_t now = std::time(nullptr);
        int64_t coldTimestamp = now - (400 * 86400); // 400 days old (> 365d -> Cold Tier)
        int64_t hotTimestamp = now - (5 * 86400);    // 5 days old (<= 30d -> Hot Tier)

        std::string coldCode = "int legacyFunction() { return 42; }";
        std::string hotCode = "int activeFunction() { return 100; }";

        // Index Cold Tier code
        auto coldNodes = astIndexer.indexBuffer(coldCode, "legacy.cpp", "commit_cold_123", coldTimestamp);
        CHRONOS_CHECK(!coldNodes.empty());

        // Verify that Codex DB registered the nodes and history
        for (const auto& nid : coldNodes) {
            auto nodeOpt = codex.getNode(nid);
            CHRONOS_CHECK(nodeOpt.has_value());
            codex.recordHistory(nid, "commit_cold_123", coldTimestamp, "AI: Cold commit indexed");
        }
        auto coldHistory = codex.getHistory(coldNodes[0]);
        CHRONOS_CHECK(!coldHistory.empty());

        // Verify VectorIndex did NOT index Cold Tier vectors (vectorIndex.size() should be 0)
        CHRONOS_CHECK(vectorIndex.size() == 0);
        for (const auto& nid : coldNodes) {
            CHRONOS_CHECK(!vectorIndex.contains(nid));
        }

        // Now Index Hot Tier code
        auto hotNodes = astIndexer.indexBuffer(hotCode, "active.cpp", "commit_hot_456", hotTimestamp);
        CHRONOS_CHECK(!hotNodes.empty());

        // Verify VectorIndex DID index Hot Tier vector
        CHRONOS_CHECK(vectorIndex.size() == 1);
        for (const auto& nid : hotNodes) {
            CHRONOS_CHECK(vectorIndex.contains(nid));
        }
    }

    fs::remove_all(tmp);
}

void run_vector_quantization_tiering_tests() {
    test_sq8_quantization_precision();
    test_sq8_memory_reduction();
    test_memory_tier_classification();
    test_cold_tier_vector_omission();
}

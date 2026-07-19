#include "chronos/vector_index.hpp"
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <cctype>

namespace fs = std::filesystem;

namespace chronos {

struct VectorIndex::Impl {
    std::unordered_map<std::string, std::vector<float>> vectors;
};

namespace {
float cosine(const std::vector<float>& a, const std::vector<float>& b) {
    double dot = 0, na = 0, nb = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += a[i] * b[i];
        na += a[i] * a[i];
        nb += b[i] * b[i];
    }
    if (na == 0 || nb == 0) return 0.f;
    return static_cast<float>(dot / (std::sqrt(na) * std::sqrt(nb)));
}
}

VectorIndex::VectorIndex(const std::string& repoRoot) {
    fs::path chronosDir = fs::path(repoRoot) / ".chronos";
    fs::create_directories(chronosDir);
    path_ = (chronosDir / "vectors.bin").string();
    impl_ = new Impl();

    std::ifstream in(path_, std::ios::binary);
    if (in) {
        while (in.peek() != EOF) {
            uint32_t idLen = 0;
            in.read(reinterpret_cast<char*>(&idLen), sizeof(idLen));
            if (!in) break;
            std::string id(idLen, '\0');
            in.read(id.data(), idLen);
            std::vector<float> vec(kDim);
            in.read(reinterpret_cast<char*>(vec.data()), kDim * sizeof(float));
            impl_->vectors[id] = std::move(vec);
        }
    }
}

VectorIndex::~VectorIndex() {
    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    for (auto& [id, vec] : impl_->vectors) {
        uint32_t idLen = static_cast<uint32_t>(id.size());
        out.write(reinterpret_cast<const char*>(&idLen), sizeof(idLen));
        out.write(id.data(), idLen);
        out.write(reinterpret_cast<const char*>(vec.data()), kDim * sizeof(float));
    }
    delete impl_;
}

void VectorIndex::upsert(const EmbeddingRecord& rec) {
    impl_->vectors[rec.nodeId] = rec.vector;
}

void VectorIndex::remove(const std::string& nodeId) {
    impl_->vectors.erase(nodeId);
}

std::vector<SeedMatch> VectorIndex::search(const std::vector<float>& queryVector, int topK) const {
    std::vector<SeedMatch> results;
    results.reserve(impl_->vectors.size());
    for (auto& [id, vec] : impl_->vectors) {
        results.push_back({id, cosine(queryVector, vec)});
    }
    std::partial_sort(results.begin(),
                       results.begin() + std::min<size_t>(topK, results.size()),
                       results.end(),
                       [](const SeedMatch& a, const SeedMatch& b) { return a.score > b.score; });
    if (static_cast<int>(results.size()) > topK) results.resize(topK);
    return results;
}

std::vector<float> embedText(const std::string& text) {
    // Deterministic hashing embedder: tokenize on non-alnum, hash each token
    // into a bucket in [0, kDim), accumulate sign-weighted counts, then
    // L2-normalize. This is intentionally simple (see header rationale) —
    // it gives stable, repeatable nearest-neighbor behavior for identical or
    // near-identical code/query text without shipping model weights.
    std::vector<float> vec(VectorIndex::kDim, 0.f);
    std::string token;
    auto flush = [&]() {
        if (token.empty()) return;
        static const std::unordered_set<std::string> stopWords = {
            "the", "is", "in", "at", "which", "on", "for", "a", "an", "this", "that",
            "to", "and", "or", "what", "how", "why", "who", "when", "where", "are",
            "of", "it", "with", "as", "by", "from", "used", "project", "codebase", "repo"
        };
        if (stopWords.count(token)) {
            token.clear();
            return;
        }
        uint64_t h = 1469598103934665603ULL;
        for (unsigned char c : token) { h ^= c; h *= 1099511628211ULL; }
        int bucket = static_cast<int>(h % VectorIndex::kDim);
        float sign = ((h >> 63) & 1ULL) ? -1.f : 1.f;
        vec[bucket] += sign;
        token.clear();
    };
    for (char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c))) token += static_cast<char>(std::tolower(c));
        else flush();
    }
    flush();

    double norm = 0;
    for (float v : vec) norm += v * v;
    norm = std::sqrt(norm);
    if (norm > 0) for (float& v : vec) v = static_cast<float>(v / norm);
    return vec;
}

} // namespace chronos

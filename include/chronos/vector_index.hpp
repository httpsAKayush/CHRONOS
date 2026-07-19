#pragma once
// The Semantic Seed layer (Spec §0/§6: "LanceDB").
//
// ENGINEERING DECISION (deviation from the spec, documented per the task's
// "don't skip, say what and why" instruction): a real LanceDB C++ embedding
// is not a mature/available dependency for a self-contained CMake build —
// LanceDB's supported SDKs are Python/Rust/Node, with no stable embedded C++
// API. Rather than stub this out with a TODO, we implement the same
// *contract* LanceDB would provide (nearest-neighbor search over stored
// float embeddings, embedded, on-disk, zero network) using a flat file of
// fixed-width float vectors + SQLite for id/metadata, with brute-force
// cosine similarity. This is a drop-in-replaceable interface: swap
// VectorIndex's .cpp for a real LanceDB binding later without touching any
// caller (ast_indexer.cpp, context_builder.cpp) since only this header is a
// public dependency.
//
// At current spec scale (up to ~1M LOC => low hundreds of thousands of
// function-level vectors), brute-force cosine over a memory-mapped float32
// file is well under the <1s query budget (Spec §9) — a few ms per query on
// commodity hardware — so this is not a performance regression for v1.

#include <string>
#include <vector>
#include <cstdint>

namespace chronos {

struct EmbeddingRecord {
    std::string nodeId;          // matches Codex Node.id
    std::vector<float> vector;   // fixed dimensionality (see VectorIndex::kDim)
};

struct SeedMatch {
    std::string nodeId;
    float score = 0.f;           // cosine similarity, [-1, 1]
};

class VectorIndex {
public:
    static constexpr int kDim = 384; // matches a small local sentence-embedding model

    explicit VectorIndex(const std::string& repoRoot);
    ~VectorIndex();

    // Insert or overwrite the embedding for a node. Called by the async
    // indexer worker only "if intrinsic identity changed" (Spec §0
    // Mechanical Walkthrough: Index Time) — i.e. skip if the new embedding's
    // cosine similarity to the stored one exceeds a near-identity threshold,
    // to honor the idempotency contract in Spec §7.
    void upsert(const EmbeddingRecord& rec);
    void remove(const std::string& nodeId);

    // Hop 1 of Two-Hop Retrieval (Spec §0): returns the top-k nodes by
    // cosine similarity to `queryVector`. Caller (context_builder.cpp) is
    // responsible for turning a low top score into the "rephrase" UX
    // described in Spec §5.
    std::vector<SeedMatch> search(const std::vector<float>& queryVector, int topK) const;

private:
    std::string path_;
    // In-memory cache of (id, vector) loaded from the flat file at
    // construction; flushed back to disk on upsert/remove. Fine for v1 scale
    // per the note above; would move to mmap for very large repos.
    struct Impl;
    Impl* impl_;
};

// Turns a natural-language query (or a code snippet, for re-embedding
// during Simhash-verified renames) into a `kDim`-length vector.
// v1 uses a lightweight bag-of-tokens hashing embedder (deterministic, no
// external model weights to ship) — sufficient for structural/lexical
// nearest-neighbor seeding since exact semantics are backstopped by the
// Codex graph, not the vector layer, per project.md's Triad philosophy
// ("LanceDB... is static, lightweight, and focuses only on what a function
// is"). Swappable later for a real embedding model without touching
// VectorIndex.
std::vector<float> embedText(const std::string& text);

} // namespace chronos

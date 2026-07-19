#pragma once
// The async indexer worker (Spec §6 "The Indexer (Async)"). The Git hook
// itself (scripts/pre-commit) does NOT call into this directly on the
// blocking commit path — it fire-and-forgets a message onto a small local
// queue file and exits in well under the 500ms hard constraint (Spec §2).
// This class is what a detached background process (spawned by the hook,
// or by `chronos-indexer --drain` run from a systemd/launchd-style timer)
// actually runs.

#include <string>
#include <vector>
#include "chronos/codex.hpp"
#include "chronos/vector_index.hpp"

namespace chronos {

struct IndexStats {
    int filesProcessed = 0;
    int nodesUpserted = 0;
    int nodesTombstoned = 0;
    int nodesSkippedIdempotent = 0; // Spec §7 idempotency contract
    int parseFailures = 0;
};

class AstIndexer {
public:
    AstIndexer(Codex& codex, VectorIndex& vectors, const std::string& repoRoot);

    // Parses one file, computes Simhash per top-level function/method via
    // tree-sitter (if CHRONOS_HAVE_TREE_SITTER; else emits a single
    // whole-file node with parse_confidence=0.0 — Structural Uncertainty,
    // Spec Glossary — so retrieval still degrades gracefully instead of
    // silently losing the file), and upserts nodes/edges/aliases.
    //
    // Idempotency: for each discovered function span, if an existing node at
    // the same file_path+byte range already has the same simhash, this is a
    // no-op (zero DB writes), per Spec §7.
    //
    // Rename detection: if a function's simhash matches an existing node
    // that is NOT active at this byte range, we treat it as a rename/move
    // and call codex.recordAlias(oldId, newId, commitHash) instead of
    // creating a fresh disconnected node — this is what lets `login` queries
    // keep resolving to `signIn` after the rename (Spec §13 concrete test).
    void indexFile(const std::string& relativePath, const std::string& commitHash = "LIVE");

    // Parses a raw memory buffer (used by GitIndexer for historical commits)
    void indexBuffer(const std::string& source, const std::string& relativePath, const std::string& commitHash);

    // Marks all nodes previously recorded for `relativePath` inactive; used
    // when a file is deleted (Spec §8 edge case).
    void removeFile(const std::string& relativePath);

    const IndexStats& stats() const { return stats_; }

private:
    Codex& codex_;
    VectorIndex& vectors_;
    std::string repoRoot_;
    IndexStats stats_;

    // Returns true if tree-sitter-cpp is linked and usable.
    static bool hasTreeSitter();
};

} // namespace chronos

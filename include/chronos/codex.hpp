#pragma once
// Codex: the SQLite-backed structural graph described in Spec §8 (Data Model)
// and §6 (Storage & schema choice). This is the "Brain" — CALLS/INHERITS
// edges, alias-forwarding DAG, intent_summary history, parse_confidence.
//
// Design notes:
//  - Single-file SQLite DB at .chronos/codex.db, WAL mode for concurrent
//    reader (Querier) / writer (async Indexer worker) access.
//  - Idempotency (Spec §7): re-indexing an unchanged file must not write.
//    We enforce this by comparing simhash+byte range before any UPDATE/INSERT
//    (see AstIndexer::indexFile).
//  - Alias table uses union-find at insertion time (see unionFindRoot) so the
//    DAG can never contain a cycle, per the validation rule in §8.

#include <optional>
#include <string>
#include <vector>
#include <cstdint>
#include <sqlite3.h>

namespace chronos {

struct Node {
    std::string id;          // UUID
    std::string file_path;
    int64_t byte_start = 0;
    int64_t byte_end = 0;
    uint64_t simhash = 0;
    bool is_active = true;
    float parse_confidence = 1.0f;  // ADDED: Structural Uncertainty (Spec Glossary)
};

struct Edge {
    std::string source_id;
    std::string target_id;
    std::string type;               // "CALLS" | "INHERITS" | "PROBABLE_TARGET"
    float probable_target_weight = 1.0f;
};

struct HistoryEntry {
    std::string node_id;
    std::string commit_hash;
    std::string intent_summary;
};

// Result of a two-hop traversal (Hop 2 in Spec §0 Mechanical Walkthrough).
struct TraceResult {
    std::vector<Node> nodes;
    std::vector<Edge> edges;
    bool any_low_confidence = false;   // triggers uncertainty_warning (Spec §7)
};

class Codex {
public:
    // Opens (creating if necessary) the Codex at `.chronos/codex.db` under
    // `repoRoot`. Applies schema migrations via PRAGMA user_version (Spec §7
    // Versioning).
    explicit Codex(const std::string& repoRoot);
    ~Codex();

    Codex(const Codex&) = delete;
    Codex& operator=(const Codex&) = delete;

    // --- Node/Edge mutation (called only from the async indexer worker) ---
    void upsertNode(const Node& n);
    void tombstoneNode(const std::string& nodeId);            // is_active = false
    void upsertEdge(const Edge& e);
    void appendHistory(const HistoryEntry& h);

    // Alias forwarding: records old_id -> new_id at commit_hash, using
    // union-find so `resolveAlias` always returns the current DAG root in
    // O(alpha(n)) and no cycle can ever form (Spec §8 validation rule).
    void recordAlias(const std::string& oldId, const std::string& newId,
                      const std::string& commitHash);
    std::string resolveAlias(const std::string& id);           // -> root id

    // --- Lookups ---
    std::optional<Node> getNode(const std::string& id);
    std::optional<Node> findBySimhash(uint64_t simhash, const std::string& filePath);
    std::optional<Node> findBySimhashGlobal(uint64_t simhash);

    // --- Tracing ---
    void recordTrace(const std::string& traceId, const TraceResult& trace);
    std::optional<TraceResult> getTrace(const std::string& traceId);

    // Two-hop structural traversal: budgeted Personalized PageRank starting
    // from `seedNodeId`. `budget` bounds the number of pushed nodes so the
    // Git-hook/CLI latency targets in Spec §9 hold even on hot God Classes.
    // Implemented in ppr.cpp (kept separate from storage access).
    TraceResult localPushPPR(const std::string& seedNodeId, int budget,
                              double dampingFactor = 0.85);

    sqlite3* raw() { return db_; }

private:
    void migrate();
    sqlite3* db_ = nullptr;
    std::string dbPath_;
};

} // namespace chronos

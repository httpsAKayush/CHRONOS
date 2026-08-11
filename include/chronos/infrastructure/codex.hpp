#pragma once
// Codex: the SQLite-backed structural graph described in Spec §8 (Data Model)
// and §6 (Storage & schema choice). This is the "Brain" — CALLS/INHERITS
// edges, alias-forwarding DAG, intent_summary history, parse_confidence.
//
// Hexagonal Architecture (Ports & Adapters): this class is the *Storage
// Adapter*. It implements the pure IStorage port (chronos/core/IStorage.hpp);
// all domain/use-case code depends on the interface, never on this concrete
// type.
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
#include "chronos/core/IStorage.hpp"

namespace chronos {

class Codex : public IStorage {
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
    void updateAiSummary(const std::string& nodeId, const std::string& summary);
    void upsertContextNode(const std::string& id, const std::string& filePath, const std::string& summary);
    void tombstoneNode(const std::string& nodeId);            // is_active = false
    void upsertEdge(const Edge& e);
    void appendHistory(const HistoryEntry& h);

    // Alias forwarding: records old_id -> new_id at commit_hash, using
    // union-find so `resolveAlias` always returns the current DAG root in
    // O(alpha(n)) and no cycle can ever form (Spec §8 validation rule).
    void recordAlias(const std::string& oldId, const std::string& newId,
                      const std::string& commitHash);
    std::string resolveAlias(const std::string& id);           // -> root id

    // --- Imports (Hybrid Scoped Resolution) ---
    void insertFileImport(const std::string& filePath, const std::string& symbolName, const std::string& sourceModule);
    void clearFileImports(const std::string& filePath);

    // --- Temporal ---
    void recordHistory(const std::string& nodeId, const std::string& commitHash, int64_t timestamp, const std::string& msg);
    std::vector<HistoryRecord> getHistory(const std::string& nodeId);
    std::vector<HistoryRecord> getHistoryForFile(const std::string& filePath);

    // --- Transactions ---
    void beginTransaction();
    void commitTransaction();

    // --- Lookups ---
    std::optional<Node> getNode(const std::string& id);
    std::optional<Node> findBySimhash(uint64_t simhash, const std::string& filePath);
    std::optional<Node> findBySimhashGlobal(uint64_t simhash);
    
    std::vector<Edge> getEdges(const std::string& nodeId, bool outgoing);
    // Same as getEdges but strips external/stdlib noise so only user-project
    // nodes appear (site-packages, /usr/, zero-byte stubs excluded).
    std::vector<Edge> getEdgesFiltered(const std::string& nodeId, bool outgoing);
    std::vector<Node> queryNodesByPathPrefix(const std::string& prefix);

    // --- Tracing ---
    void recordTrace(const std::string& traceId, const TraceResult& trace);
    std::optional<TraceResult> getTrace(const std::string& traceId);

    // Two-hop structural traversal: budgeted Personalized PageRank starting
    // from `seedNodeId`. `budget` bounds the number of pushed nodes so the
    // Git-hook/CLI latency targets in Spec §9 hold even on hot God Classes.
    // Implemented in ppr.cpp (kept separate from storage access).
    TraceResult localPushPPR(const std::string& seedNodeId, int budget,
                              double dampingFactor = 0.85);
    TraceResult localPushPPR(const std::vector<std::string>& seeds, int budget,
                              double dampingFactor = 0.85);

    sqlite3* raw() { return db_; }

private:
    void migrate();
    sqlite3* db_ = nullptr;
    std::string dbPath_;
};

} // namespace chronos

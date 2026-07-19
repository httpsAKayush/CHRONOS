#include "chronos/codex.hpp"
#include "chronos/ppr.hpp"
#include <stdexcept>
#include <filesystem>
#include <sstream>
#include <algorithm>


namespace fs = std::filesystem;

namespace chronos {

namespace {

constexpr int kSchemaVersion = 1;

void execOrThrow(sqlite3* db, const std::string& sql) {
    char* errMsg = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errMsg) != SQLITE_OK) {
        std::string msg = errMsg ? errMsg : "unknown sqlite error";
        sqlite3_free(errMsg);
        throw std::runtime_error("Codex SQL error: " + msg + "\nSQL: " + sql);
    }
}

} // namespace

Codex::Codex(const std::string& repoRoot) {
    fs::path chronosDir = fs::path(repoRoot) / ".chronos";
    fs::create_directories(chronosDir);
    dbPath_ = (chronosDir / "codex.db").string();

    if (sqlite3_open(dbPath_.c_str(), &db_) != SQLITE_OK) {
        throw std::runtime_error("Failed to open Codex at " + dbPath_);
    }
    // WAL mode: async indexer worker writes while the CLI reads (Spec §3
    // Actor Table — Git Hook triggers async updates, Developer queries live).
    execOrThrow(db_, "PRAGMA journal_mode=WAL;");
    execOrThrow(db_, "PRAGMA foreign_keys=ON;");
    execOrThrow(db_, "PRAGMA busy_timeout=2000;");
    migrate();
}

Codex::~Codex() {
    if (db_) sqlite3_close(db_);
}

void Codex::migrate() {
    int userVersion = 0;
    {
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db_, "PRAGMA user_version;", -1, &stmt, nullptr);
        if (sqlite3_step(stmt) == SQLITE_ROW) userVersion = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    if (userVersion >= kSchemaVersion) return;

    execOrThrow(db_, R"SQL(
        BEGIN;

        CREATE TABLE IF NOT EXISTS nodes (
            id               TEXT PRIMARY KEY,
            file_path        TEXT NOT NULL,
            byte_start       INTEGER NOT NULL,
            byte_end         INTEGER NOT NULL,
            simhash          INTEGER NOT NULL,
            is_active        INTEGER NOT NULL,
            parse_confidence REAL NOT NULL
        );

        CREATE TABLE IF NOT EXISTS history (
            node_id       TEXT NOT NULL,
            commit_hash   TEXT NOT NULL,
            timestamp     INTEGER NOT NULL,
            synthetic_msg TEXT,
            PRIMARY KEY(node_id, commit_hash)
        );

        CREATE INDEX IF NOT EXISTS idx_nodes_file ON nodes(file_path);
        CREATE INDEX IF NOT EXISTS idx_nodes_simhash ON nodes(simhash);

        CREATE TABLE IF NOT EXISTS edges (
            source_id  TEXT NOT NULL REFERENCES nodes(id),
            target_id  TEXT NOT NULL REFERENCES nodes(id),
            type       TEXT NOT NULL,
            probable_target_weight REAL NOT NULL DEFAULT 1.0,
            PRIMARY KEY (source_id, target_id, type)
        );
        CREATE INDEX IF NOT EXISTS idx_edges_source ON edges(source_id);
        CREATE INDEX IF NOT EXISTS idx_edges_target ON edges(target_id);

        CREATE TABLE IF NOT EXISTS history (
            node_id        TEXT NOT NULL REFERENCES nodes(id),
            commit_hash    TEXT NOT NULL,
            intent_summary TEXT NOT NULL,
            PRIMARY KEY (node_id, commit_hash)
        );

        -- Path-compressed alias DAG (union-find). `root_id` is maintained
        -- eagerly on write so resolveAlias is a single indexed lookup.
        CREATE TABLE IF NOT EXISTS alias (
            old_id      TEXT PRIMARY KEY,
            new_id      TEXT NOT NULL,
            root_id     TEXT NOT NULL,
            commit_hash TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS trace_log (
            trace_id       TEXT PRIMARY KEY,
            node_ids_json  TEXT NOT NULL,
            created_at     INTEGER NOT NULL
        );

        COMMIT;
    )SQL");

    execOrThrow(db_, "PRAGMA user_version = " + std::to_string(kSchemaVersion) + ";");
}

void Codex::upsertNode(const Node& n) {
    static const char* sql = R"SQL(
        INSERT INTO nodes (id, file_path, byte_start, byte_end, simhash, is_active, parse_confidence)
        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)
        ON CONFLICT(id) DO UPDATE SET
            file_path = excluded.file_path,
            byte_start = excluded.byte_start,
            byte_end = excluded.byte_end,
            simhash = excluded.simhash,
            is_active = excluded.is_active,
            parse_confidence = excluded.parse_confidence;
    )SQL";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, n.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, n.file_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, n.byte_start);
    sqlite3_bind_int64(stmt, 4, n.byte_end);
    sqlite3_bind_int64(stmt, 5, static_cast<int64_t>(n.simhash));
    sqlite3_bind_int(stmt, 6, n.is_active ? 1 : 0);
    sqlite3_bind_double(stmt, 7, n.parse_confidence);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        throw std::runtime_error("upsertNode failed");
    }
    sqlite3_finalize(stmt);
}

void Codex::tombstoneNode(const std::string& nodeId) {
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_, "UPDATE nodes SET is_active = 0 WHERE id = ?1;", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, nodeId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    sqlite3_stmt* stmt2;
    sqlite3_prepare_v2(db_, "DELETE FROM edges WHERE source_id = ?1 OR target_id = ?1;", -1, &stmt2, nullptr);
    sqlite3_bind_text(stmt2, 1, nodeId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt2);
    sqlite3_finalize(stmt2);
}

void Codex::upsertEdge(const Edge& e) {
    static const char* sql = R"SQL(
        INSERT INTO edges (source_id, target_id, type, probable_target_weight)
        VALUES (?1, ?2, ?3, ?4)
        ON CONFLICT(source_id, target_id, type) DO UPDATE SET
            probable_target_weight = excluded.probable_target_weight;
    )SQL";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, e.source_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, e.target_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, e.type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 4, e.probable_target_weight);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void Codex::appendHistory(const HistoryEntry& h) {
    static const char* sql = R"SQL(
        INSERT INTO history (node_id, commit_hash, intent_summary)
        VALUES (?1, ?2, ?3)
        ON CONFLICT(node_id, commit_hash) DO UPDATE SET intent_summary = excluded.intent_summary;
    )SQL";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, h.node_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, h.commit_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, h.intent_summary.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void Codex::recordAlias(const std::string& oldId, const std::string& newId,
                         const std::string& commitHash) {
    // Union-find: root of newId becomes the shared root for oldId's whole
    // existing chain, giving O(1) amortized resolveAlias with no cycles,
    // per the "No circular dependencies in the alias DAG" validation rule.
    std::string newRoot = resolveAlias(newId);

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_,
        "INSERT INTO alias (old_id, new_id, root_id, commit_hash) VALUES (?1, ?2, ?3, ?4) "
        "ON CONFLICT(old_id) DO UPDATE SET new_id=excluded.new_id, root_id=excluded.root_id, commit_hash=excluded.commit_hash;",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, oldId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, newId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, newRoot.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, commitHash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // Path compression: repoint every alias that pointed at oldId's old root
    // directly at newRoot in one pass.
    sqlite3_stmt* upd;
    sqlite3_prepare_v2(db_, "UPDATE alias SET root_id = ?1 WHERE root_id = ?2;", -1, &upd, nullptr);
    sqlite3_bind_text(upd, 1, newRoot.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(upd, 2, oldId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(upd);
    sqlite3_finalize(upd);
}

std::string Codex::resolveAlias(const std::string& id) {
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_, "SELECT root_id FROM alias WHERE old_id = ?1;", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    std::string result = id;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        result = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return result;
}

void Codex::recordHistory(const std::string& nodeId, const std::string& commitHash, int64_t timestamp, const std::string& msg) {
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_,
        "INSERT OR REPLACE INTO history (node_id, commit_hash, timestamp, synthetic_msg) VALUES (?1, ?2, ?3, ?4);",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, nodeId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, commitHash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, timestamp);
    if (!msg.empty()) {
        sqlite3_bind_text(stmt, 4, msg.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 4);
    }
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::optional<Node> Codex::getNode(const std::string& id) {
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_,
        "SELECT id, file_path, byte_start, byte_end, simhash, is_active, parse_confidence "
        "FROM nodes WHERE id = ?1;", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<Node> out;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        Node n;
        n.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        n.file_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        n.byte_start = sqlite3_column_int64(stmt, 2);
        n.byte_end = sqlite3_column_int64(stmt, 3);
        n.simhash = static_cast<uint64_t>(sqlite3_column_int64(stmt, 4));
        n.is_active = sqlite3_column_int(stmt, 5) != 0;
        n.parse_confidence = static_cast<float>(sqlite3_column_double(stmt, 6));
        out = n;
    }
    sqlite3_finalize(stmt);
    return out;
}

std::optional<Node> Codex::findBySimhash(uint64_t simhash, const std::string& filePath) {
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_,
        "SELECT id, file_path, byte_start, byte_end, simhash, is_active, parse_confidence "
        "FROM nodes WHERE simhash = ?1 AND file_path = ?2 AND is_active = 1 LIMIT 1;",
        -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(simhash));
    sqlite3_bind_text(stmt, 2, filePath.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<Node> out;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        Node n;
        n.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        n.file_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        n.byte_start = sqlite3_column_int64(stmt, 2);
        n.byte_end = sqlite3_column_int64(stmt, 3);
        n.simhash = static_cast<uint64_t>(sqlite3_column_int64(stmt, 4));
        n.is_active = sqlite3_column_int(stmt, 5) != 0;
        n.parse_confidence = static_cast<float>(sqlite3_column_double(stmt, 6));
        out = n;
    }
    sqlite3_finalize(stmt);
    return out;
}

std::optional<Node> Codex::findBySimhashGlobal(uint64_t simhash) {
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_,
        "SELECT id, file_path, byte_start, byte_end, simhash, is_active, parse_confidence "
        "FROM nodes WHERE simhash = ?1 AND is_active = 1 LIMIT 1;",
        -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(simhash));
    std::optional<Node> out;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        Node n;
        n.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        n.file_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        n.byte_start = sqlite3_column_int64(stmt, 2);
        n.byte_end = sqlite3_column_int64(stmt, 3);
        n.simhash = static_cast<uint64_t>(sqlite3_column_int64(stmt, 4));
        n.is_active = sqlite3_column_int(stmt, 5) != 0;
        n.parse_confidence = static_cast<float>(sqlite3_column_double(stmt, 6));
        out = n;
    }
    sqlite3_finalize(stmt);
    return out;
}

TraceResult Codex::localPushPPR(const std::string& seedNodeId, int budget,
                                 double dampingFactor) {
    (void)dampingFactor; // dual-horizon push internally picks its own alphas
    PPREngine engine(db_);
    PPRScoreMap scores = engine.dualHorizonPush(seedNodeId, budget);

    TraceResult result;
    std::vector<std::pair<std::string, double>> ranked(scores.nodeIdToScore.begin(),
                                                         scores.nodeIdToScore.end());
    std::sort(ranked.begin(), ranked.end(), [](auto& a, auto& b) { return a.second > b.second; });
    if (static_cast<int>(ranked.size()) > budget) ranked.resize(budget);

    for (auto& [id, score] : ranked) {
        auto node = getNode(id);
        if (!node) continue;
        if (node->parse_confidence < 0.5f) result.any_low_confidence = true;
        result.nodes.push_back(*node);
    }

    for (auto& n : result.nodes) {
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db_,
            "SELECT source_id, target_id, type, probable_target_weight FROM edges "
            "WHERE source_id = ?1;", -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, n.id.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            Edge e;
            e.source_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            e.target_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            e.type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            e.probable_target_weight = sqlite3_column_double(stmt, 3);
            result.edges.push_back(e);
        }
        sqlite3_finalize(stmt);
    }

    return result;
}

void Codex::recordTrace(const std::string& traceId, const TraceResult& trace) {
    std::string json = "[";
    for (size_t i = 0; i < trace.nodes.size(); ++i) {
        if (i > 0) json += ",";
        json += "\"" + trace.nodes[i].id + "\"";
    }
    json += "]";

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_,
        "INSERT OR REPLACE INTO trace_log (trace_id, node_ids_json, created_at) "
        "VALUES (?1, ?2, strftime('%s', 'now'));", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, traceId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::optional<TraceResult> Codex::getTrace(const std::string& traceId) {
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_, "SELECT node_ids_json FROM trace_log WHERE trace_id = ?1;", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, traceId.c_str(), -1, SQLITE_TRANSIENT);
    
    std::string json;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        json = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    
    if (json.empty()) return std::nullopt;

    TraceResult result;
    size_t pos = 0;
    while ((pos = json.find('"', pos)) != std::string::npos) {
        size_t end = json.find('"', pos + 1);
        if (end == std::string::npos) break;
        std::string id = json.substr(pos + 1, end - pos - 1);
        auto node = getNode(id);
        if (node) {
            if (node->parse_confidence < 0.5f) result.any_low_confidence = true;
            result.nodes.push_back(*node);
        }
        pos = end + 1;
    }

    for (auto& n : result.nodes) {
        sqlite3_stmt* estmt;
        sqlite3_prepare_v2(db_,
            "SELECT source_id, target_id, type, probable_target_weight FROM edges "
            "WHERE source_id = ?1;", -1, &estmt, nullptr);
        sqlite3_bind_text(estmt, 1, n.id.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(estmt) == SQLITE_ROW) {
            Edge e;
            e.source_id = reinterpret_cast<const char*>(sqlite3_column_text(estmt, 0));
            e.target_id = reinterpret_cast<const char*>(sqlite3_column_text(estmt, 1));
            e.type = reinterpret_cast<const char*>(sqlite3_column_text(estmt, 2));
            e.probable_target_weight = sqlite3_column_double(estmt, 3);
            result.edges.push_back(e);
        }
        sqlite3_finalize(estmt);
    }
    
    return result;
}

} // namespace chronos

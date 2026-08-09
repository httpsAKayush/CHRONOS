#include "chronos/infrastructure/codex.hpp"
#include "chronos/domain/ppr.hpp"
#include <stdexcept>
#include <filesystem>
#include <sstream>
#include <algorithm>


namespace fs = std::filesystem;

namespace chronos {

namespace {

constexpr int kSchemaVersion = 3;

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

    if (userVersion == 0) {
        execOrThrow(db_, R"SQL(
            BEGIN;

            CREATE TABLE IF NOT EXISTS nodes (
                id               TEXT PRIMARY KEY,
                file_path        TEXT NOT NULL,
                byte_start       INTEGER NOT NULL,
                byte_end         INTEGER NOT NULL,
                simhash          INTEGER NOT NULL,
                is_active        INTEGER NOT NULL,
                parse_confidence REAL NOT NULL,
                ai_summary       TEXT
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
                start_line INTEGER DEFAULT 0,
                call_site_text TEXT,
                PRIMARY KEY (source_id, target_id, type)
            );
            CREATE INDEX IF NOT EXISTS idx_edges_source ON edges(source_id);
            CREATE INDEX IF NOT EXISTS idx_edges_target ON edges(target_id);

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

            CREATE TABLE IF NOT EXISTS file_imports (
                file_path TEXT NOT NULL,
                symbol_name TEXT NOT NULL,
                source_module TEXT NOT NULL,
                PRIMARY KEY(file_path, symbol_name)
            );

            COMMIT;
        )SQL");
    } else {
        if (userVersion == 1) {
            sqlite3_exec(db_, "ALTER TABLE nodes ADD COLUMN ai_summary TEXT;", nullptr, nullptr, nullptr);
            sqlite3_exec(db_, "ALTER TABLE edges ADD COLUMN start_line INTEGER DEFAULT 0;", nullptr, nullptr, nullptr);
        }
        if (userVersion < 3) {
            sqlite3_exec(db_, "ALTER TABLE edges ADD COLUMN call_site_text TEXT;", nullptr, nullptr, nullptr);
            // ai_summary was added in v2, but if userVersion < 3 (i.e. v2), it's already there. 
            // the previous implementation tried to add ai_summary in v2->v3 which caused errors.
        }
    }

    execOrThrow(db_, "PRAGMA user_version = " + std::to_string(kSchemaVersion) + ";");
}

void Codex::upsertNode(const Node& n) {
    if (n.byte_end <= n.byte_start) {
        throw std::invalid_argument("byte_end must be strictly greater than byte_start");
    }
    static const char* sql = R"SQL(
        INSERT INTO nodes (id, file_path, byte_start, byte_end, simhash, is_active, parse_confidence, ai_summary)
        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)
        ON CONFLICT(id) DO UPDATE SET
            file_path = excluded.file_path,
            byte_start = excluded.byte_start,
            byte_end = excluded.byte_end,
            simhash = excluded.simhash,
            is_active = excluded.is_active,
            parse_confidence = excluded.parse_confidence,
            ai_summary = excluded.ai_summary;
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
    if (!n.ai_summary.empty()) {
        sqlite3_bind_text(stmt, 8, n.ai_summary.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 8);
    }
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::string err = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        throw std::runtime_error("upsertNode failed: " + err);
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

void Codex::updateAiSummary(const std::string& nodeId, const std::string& summary) {
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_, "UPDATE nodes SET ai_summary = ?1 WHERE id = ?2;", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, summary.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, nodeId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void Codex::upsertEdge(const Edge& e) {
    static const char* sql = R"SQL(
        INSERT INTO edges (source_id, target_id, type, probable_target_weight, start_line, call_site_text)
        VALUES (?1, ?2, ?3, ?4, ?5, ?6)
        ON CONFLICT(source_id, target_id, type) DO UPDATE SET
            probable_target_weight = excluded.probable_target_weight,
            start_line = excluded.start_line,
            call_site_text = excluded.call_site_text;
    )SQL";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, e.source_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, e.target_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, e.type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 4, e.probable_target_weight);
    sqlite3_bind_int(stmt, 5, e.start_line);
    if (!e.call_site_text.empty()) {
        sqlite3_bind_text(stmt, 6, e.call_site_text.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 6);
    }
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void Codex::appendHistory(const HistoryEntry& h) {
    static const char* sql = R"SQL(
        INSERT INTO history (node_id, commit_hash, timestamp, synthetic_msg)
        VALUES (?1, ?2, 0, ?3)
        ON CONFLICT(node_id, commit_hash) DO UPDATE SET synthetic_msg = excluded.synthetic_msg;
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
    if (id.find("sym:") == 0) {
        std::string symbol = id.substr(4);
        
        std::string callingFilePath = "";
        sqlite3_stmt* fpStmt;
        sqlite3_prepare_v2(db_, "SELECT file_path FROM nodes WHERE id = ?1;", -1, &fpStmt, nullptr);
        sqlite3_bind_text(fpStmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(fpStmt) == SQLITE_ROW) {
            callingFilePath = reinterpret_cast<const char*>(sqlite3_column_text(fpStmt, 0));
        }
        sqlite3_finalize(fpStmt);
        
        if (!callingFilePath.empty()) {
            sqlite3_stmt* importStmt;
            sqlite3_prepare_v2(db_, "SELECT source_module FROM file_imports WHERE file_path = ?1 AND symbol_name = ?2;", -1, &importStmt, nullptr);
            sqlite3_bind_text(importStmt, 1, callingFilePath.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(importStmt, 2, symbol.c_str(), -1, SQLITE_TRANSIENT);
            
            std::string sourceModule;
            if (sqlite3_step(importStmt) == SQLITE_ROW) {
                sourceModule = reinterpret_cast<const char*>(sqlite3_column_text(importStmt, 0));
            }
            sqlite3_finalize(importStmt);
            
            if (!sourceModule.empty()) {
                std::string modPath = sourceModule;
                for (char& c : modPath) if (c == '.') c = '/';
                
                sqlite3_stmt* stmt;
                sqlite3_prepare_v2(db_, "SELECT root_id FROM alias WHERE old_id = ?1;", -1, &stmt, nullptr);
                sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
                
                std::string bestRoot = "";
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    std::string rootId = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                    
                    sqlite3_stmt* nodeStmt;
                    sqlite3_prepare_v2(db_, "SELECT file_path FROM nodes WHERE id = ?1;", -1, &nodeStmt, nullptr);
                    sqlite3_bind_text(nodeStmt, 1, rootId.c_str(), -1, SQLITE_TRANSIENT);
                    if (sqlite3_step(nodeStmt) == SQLITE_ROW) {
                        std::string nodePath = reinterpret_cast<const char*>(sqlite3_column_text(nodeStmt, 0));
                        if (nodePath.find(modPath + ".py") != std::string::npos || nodePath.find(modPath + "/__init__.py") != std::string::npos) {
                            bestRoot = rootId;
                        }
                    }
                    sqlite3_finalize(nodeStmt);
                    if (!bestRoot.empty()) break;
                }
                sqlite3_finalize(stmt);
                
                if (!bestRoot.empty()) {
                    return bestRoot;
                }
            }
        }
    }

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_, "SELECT root_id FROM alias WHERE old_id = ?1;", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    std::string result = id;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        result = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    
    if (id.find("sym:") == 0) {
        if (result == id) {
            throw std::runtime_error("Error: Function not found in Codex graph");
        }
        
        sqlite3_stmt* nStmt;
        sqlite3_prepare_v2(db_, "SELECT byte_start FROM nodes WHERE id = ?1;", -1, &nStmt, nullptr);
        sqlite3_bind_text(nStmt, 1, result.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(nStmt) == SQLITE_ROW) {
            if (sqlite3_column_int64(nStmt, 0) == 0) {
                sqlite3_finalize(nStmt);
                throw std::runtime_error("Error: Function not found in Codex graph");
            }
        }
        sqlite3_finalize(nStmt);
    }
    
    return result;
}

void Codex::insertFileImport(const std::string& filePath, const std::string& symbolName, const std::string& sourceModule) {
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_,
        "INSERT OR REPLACE INTO file_imports (file_path, symbol_name, source_module) VALUES (?1, ?2, ?3);",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, symbolName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, sourceModule.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void Codex::clearFileImports(const std::string& filePath) {
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_, "DELETE FROM file_imports WHERE file_path = ?1;", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
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

std::vector<Codex::HistoryRecord> Codex::getHistory(const std::string& nodeId) {
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_,
        "SELECT node_id, commit_hash, timestamp, synthetic_msg FROM history "
        "WHERE node_id = ?1 ORDER BY timestamp DESC;",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, nodeId.c_str(), -1, SQLITE_TRANSIENT);
    
    std::vector<Codex::HistoryRecord> records;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        HistoryRecord rec;
        rec.nodeId = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        rec.commitHash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        rec.timestamp = sqlite3_column_int64(stmt, 2);
        const char* msg = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        if (msg) rec.syntheticMsg = msg;
        records.push_back(rec);
    }
    sqlite3_finalize(stmt);
    return records;
}

std::vector<Codex::HistoryRecord> Codex::getHistoryForFile(const std::string& filePath) {
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_,
        "SELECT h.node_id, h.commit_hash, h.timestamp, h.synthetic_msg "
        "FROM history h JOIN nodes n ON h.node_id = n.id "
        "WHERE n.file_path = ?1 AND n.is_active = 1 ORDER BY h.timestamp DESC;",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
    
    std::vector<Codex::HistoryRecord> records;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        HistoryRecord rec;
        rec.nodeId = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        rec.commitHash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        rec.timestamp = sqlite3_column_int64(stmt, 2);
        const char* msg = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        if (msg) rec.syntheticMsg = msg;
        records.push_back(rec);
    }
    sqlite3_finalize(stmt);
    return records;
}

std::optional<Node> Codex::getNode(const std::string& id) {
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_,
        "SELECT id, file_path, byte_start, byte_end, simhash, is_active, parse_confidence, ai_summary "
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
        if (sqlite3_column_type(stmt, 7) != SQLITE_NULL) {
            n.ai_summary = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        }
        out = n;
    }
    sqlite3_finalize(stmt);
    return out;
}

std::optional<Node> Codex::findBySimhash(uint64_t simhash, const std::string& filePath) {
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_,
        "SELECT id, file_path, byte_start, byte_end, simhash, is_active, parse_confidence, ai_summary "
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
        if (sqlite3_column_type(stmt, 7) != SQLITE_NULL) {
            n.ai_summary = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        }
        out = n;
    }
    sqlite3_finalize(stmt);
    return out;
}

std::optional<Node> Codex::findBySimhashGlobal(uint64_t simhash) {
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_,
        "SELECT id, file_path, byte_start, byte_end, simhash, is_active, parse_confidence, ai_summary "
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
        if (sqlite3_column_type(stmt, 7) != SQLITE_NULL) {
            n.ai_summary = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        }
        out = n;
    }
    sqlite3_finalize(stmt);
    return out;
}

std::vector<Edge> Codex::getEdges(const std::string& nodeId, bool outgoing) {
    std::vector<Edge> edges;
    std::string sql = outgoing
        ? "SELECT target_id, type, probable_target_weight, start_line, call_site_text FROM edges WHERE source_id = ?1 ORDER BY start_line ASC;"
        : "SELECT source_id, type, probable_target_weight, start_line, call_site_text FROM edges WHERE target_id = ?1 ORDER BY start_line ASC;";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, nodeId.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Edge e;
        if (outgoing) {
            e.source_id = nodeId;
            e.target_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        } else {
            e.source_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            e.target_id = nodeId;
        }
        e.type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        e.probable_target_weight = sqlite3_column_double(stmt, 2);
        e.start_line = sqlite3_column_int(stmt, 3);
        if (sqlite3_column_text(stmt, 4)) {
            e.call_site_text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        }
        edges.push_back(e);
    }
    sqlite3_finalize(stmt);
    return edges;
}

// getEdgesFiltered: same traversal but with a SQL-level noise filter.
// Strict exclusion rules applied directly in the query:
//   - Skips any target whose file_path starts with nothing (external stubs)
//   - Skips any node living in site-packages, /usr/, or that is a bare
//     "external_symbol" type edge (stdlib builtins: print, len, etc.)
//   - Skips zero-byte stub nodes (byte_start == 0 AND byte_end <= 1)
std::vector<Edge> Codex::getEdgesFiltered(const std::string& nodeId, bool outgoing) {
    std::vector<Edge> edges;

    // We JOIN edges with nodes so we can apply file_path filters at query time.
    // For sym: stub targets, we JOIN through the alias table to the real resolved node
    // before applying byte-range and path filters. This ensures that edges pointing
    // to sym:foo stubs (which always have byte_end=1) still appear IF the real
    // implementing node passes the filter.
    std::string sql;
    if (outgoing) {
        sql = R"SQL(
            SELECT e.target_id, e.type, e.probable_target_weight, e.start_line, e.call_site_text
            FROM edges e
            LEFT JOIN alias al ON al.old_id = e.target_id
            JOIN nodes n ON n.id = COALESCE(al.root_id, e.target_id)
            WHERE e.source_id = ?1
              AND e.type != 'external_symbol'
              AND n.file_path NOT LIKE '%site-packages%'
              AND n.file_path NOT LIKE '/usr/%'
              AND n.file_path NOT LIKE '%/usr/%'
              AND NOT (n.byte_start = 0 AND n.byte_end <= 1)
            ORDER BY e.start_line ASC;
        )SQL";
    } else {
        sql = R"SQL(
            SELECT e.source_id, e.type, e.probable_target_weight, e.start_line, e.call_site_text
            FROM edges e
            LEFT JOIN alias al ON al.old_id = e.source_id
            JOIN nodes n ON n.id = COALESCE(al.root_id, e.source_id)
            WHERE (e.target_id = ?1 OR e.target_id IN (SELECT old_id FROM alias WHERE root_id = ?1))
              AND e.type != 'external_symbol'
              AND n.file_path NOT LIKE '%site-packages%'
              AND n.file_path NOT LIKE '/usr/%'
              AND n.file_path NOT LIKE '%/usr/%'
              AND NOT (n.byte_start = 0 AND n.byte_end <= 1)
            ORDER BY e.start_line ASC;
        )SQL";
    }

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, nodeId.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Edge e;
        if (outgoing) {
            e.source_id = nodeId;
            e.target_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        } else {
            e.source_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            e.target_id = nodeId;
        }
        e.type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        e.probable_target_weight = sqlite3_column_double(stmt, 2);
        e.start_line = sqlite3_column_int(stmt, 3);
        if (sqlite3_column_text(stmt, 4)) {
            e.call_site_text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        }
        edges.push_back(e);
    }
    sqlite3_finalize(stmt);
    return edges;
}

std::vector<Node> Codex::queryNodesByPathPrefix(const std::string& prefix) {
    std::vector<Node> nodes;
    sqlite3_stmt* stmt;
    std::string sql = "SELECT id, file_path, byte_start, byte_end, simhash, is_active, parse_confidence "
                      "FROM nodes WHERE file_path LIKE ?1 AND is_active = 1;";
    sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    std::string pattern = prefix + "%";
    sqlite3_bind_text(stmt, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Node n;
        n.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        n.file_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        n.byte_start = sqlite3_column_int64(stmt, 2);
        n.byte_end = sqlite3_column_int64(stmt, 3);
        n.simhash = static_cast<uint64_t>(sqlite3_column_int64(stmt, 4));
        n.is_active = sqlite3_column_int(stmt, 5) != 0;
        n.parse_confidence = static_cast<float>(sqlite3_column_double(stmt, 6));
        nodes.push_back(n);
    }
    sqlite3_finalize(stmt);
    return nodes;
}


TraceResult Codex::localPushPPR(const std::vector<std::string>& seeds, int budget,
                                 double dampingFactor) {
    (void)dampingFactor; // dual-horizon push internally picks its own alphas
    PPREngine engine(db_);
    PPRScoreMap scores = engine.dualHorizonPush(seeds, budget);

    TraceResult result;
    std::vector<std::pair<std::string, double>> ranked(scores.nodeIdToScore.begin(),
                                                         scores.nodeIdToScore.end());
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    if (static_cast<int>(ranked.size()) > budget) ranked.resize(budget);

    for (const auto& [id, score] : ranked) {
        auto node = getNode(id);
        if (!node) continue;
        if (node->parse_confidence < 0.5f) result.any_low_confidence = true;
        result.nodes.push_back(*node);
    }

    for (const auto& n : result.nodes) {
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
            e.probable_target_weight = static_cast<float>(sqlite3_column_double(stmt, 3));
            result.edges.push_back(e);
        }
        sqlite3_finalize(stmt);
    }

    return result;
}

TraceResult Codex::localPushPPR(const std::string& seedNodeId, int budget,
                                 double dampingFactor) {
    return localPushPPR(std::vector<std::string>{seedNodeId}, budget, dampingFactor);
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

void Codex::beginTransaction() {
    sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);
}

void Codex::commitTransaction() {
    sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
}

} // namespace chronos

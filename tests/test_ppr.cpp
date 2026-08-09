#include "test_framework.hpp"
#include "chronos/domain/ppr.hpp"
#include <sqlite3.h>
#include <filesystem>
#include <vector>

using namespace chronos;
namespace fs = std::filesystem;

namespace {

sqlite3* createTempPPRDatabase(const std::string& dbPath) {
    sqlite3* db = nullptr;
    if (sqlite3_open(dbPath.c_str(), &db) != SQLITE_OK) {
        return nullptr;
    }

    const char* schema = R"SQL(
        CREATE TABLE IF NOT EXISTS alias (
            old_id TEXT PRIMARY KEY,
            new_id TEXT NOT NULL,
            root_id TEXT NOT NULL,
            commit_hash TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS edges (
            source_id TEXT NOT NULL,
            target_id TEXT NOT NULL,
            type TEXT NOT NULL,
            probable_target_weight REAL NOT NULL DEFAULT 1.0,
            PRIMARY KEY (source_id, target_id, type)
        );
    )SQL";

    char* err = nullptr;
    if (sqlite3_exec(db, schema, nullptr, nullptr, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
        sqlite3_close(db);
        return nullptr;
    }
    return db;
}

void addEdge(sqlite3* db, const std::string& src, const std::string& tgt, const std::string& type = "CALLS", double weight = 1.0) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "INSERT INTO edges (source_id, target_id, type, probable_target_weight) VALUES (?1, ?2, ?3, ?4);", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, src.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, tgt.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 4, weight);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void addAlias(sqlite3* db, const std::string& oldId, const std::string& rootId) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "INSERT INTO alias (old_id, new_id, root_id, commit_hash) VALUES (?1, ?2, ?2, 'init');", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, oldId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, rootId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

} // namespace

void run_ppr_tests() {
    fs::path tmp = fs::temp_directory_path() / "chronos_test_ppr.db";
    fs::remove(tmp);

    sqlite3* db = createTempPPRDatabase(tmp.string());
    CHRONOS_CHECK(db != nullptr);

    // Graph structure:
    // A -> B (weight 1.0), A -> C (weight 1.0)
    // B -> D (weight 1.0)
    // X -> Y (weight 1.0)
    addEdge(db, "nodeA", "nodeB", "CALLS", 1.0);
    addEdge(db, "nodeA", "nodeC", "CALLS", 1.0);
    addEdge(db, "nodeB", "nodeD", "CALLS", 1.0);
    addEdge(db, "nodeX", "nodeY", "CALLS", 1.0);

    // Test 1: Single-seed Andersen local push
    {
        PPREngine engine(db);
        PPRScoreMap res = engine.localPush("nodeA", /*alpha=*/0.5, /*epsilon=*/1e-4, /*nodeBudget=*/10);
        CHRONOS_CHECK(res.nodeIdToScore.count("nodeA") > 0);
        CHRONOS_CHECK(res.nodeIdToScore.count("nodeB") > 0);
        CHRONOS_CHECK(res.nodeIdToScore.count("nodeC") > 0);
        CHRONOS_CHECK(res.nodeIdToScore["nodeA"] > res.nodeIdToScore["nodeB"]);
        CHRONOS_CHECK(res.nodeIdToScore["nodeA"] > res.nodeIdToScore["nodeC"]);
    }

    // Test 2: Multi-seed initialization
    {
        PPREngine engine(db);
        std::vector<std::string> seeds = {"nodeA", "nodeX"};
        PPRScoreMap res = engine.localPush(seeds, /*alpha=*/0.5, /*epsilon=*/1e-4, /*nodeBudget=*/10);
        CHRONOS_CHECK(res.nodeIdToScore.count("nodeA") > 0);
        CHRONOS_CHECK(res.nodeIdToScore.count("nodeX") > 0);
        CHRONOS_CHECK(res.nodeIdToScore.count("nodeB") > 0);
        CHRONOS_CHECK(res.nodeIdToScore.count("nodeY") > 0);
    }

    // Test 3: Dual-Horizon PPR (tight alpha=0.5 vs wide alpha=0.1 fused via RRF)
    {
        PPREngine engine(db);
        std::vector<std::string> seeds = {"nodeA"};
        PPRScoreMap res = engine.dualHorizonPush(seeds, /*nodeBudget=*/10);
        CHRONOS_CHECK(res.nodeIdToScore.count("nodeA") > 0);
        CHRONOS_CHECK(res.nodeIdToScore.count("nodeB") > 0);
        CHRONOS_CHECK(res.nodeIdToScore.count("nodeC") > 0);
        CHRONOS_CHECK(res.nodeIdToScore.count("nodeD") > 0);
    }

    // Test 4: Alias resolution during out-edge traversal
    {
        addAlias(db, "nodeTargetOld", "nodeD");
        addEdge(db, "nodeC", "nodeTargetOld", "CALLS", 1.0);

        PPREngine engine(db);
        engine.clearCache();
        PPRScoreMap res = engine.localPush("nodeC", /*alpha=*/0.5, /*epsilon=*/1e-4, /*nodeBudget=*/10);
        // Alias nodeTargetOld should resolve to nodeD
        CHRONOS_CHECK(res.nodeIdToScore.count("nodeD") > 0);
    }

    sqlite3_close(db);
    fs::remove(tmp);
}

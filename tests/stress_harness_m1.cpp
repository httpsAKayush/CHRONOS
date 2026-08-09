#include "chronos/domain/ppr.hpp"
#include "chronos/domain/rrf.hpp"
#include "chronos/domain/mmr.hpp"
#include "chronos/infrastructure/vector_index.hpp"
#include "chronos/infrastructure/codex.hpp"
#include "chronos/use_cases/context_builder.hpp"
#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <cmath>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <sqlite3.h>

namespace fs = std::filesystem;

void printHeader(const std::string& title) {
    std::cout << "\n======================================================\n";
    std::cout << " STRESS TEST: " << title << "\n";
    std::cout << "======================================================\n";
}

void printResult(const std::string& testName, bool passed, const std::string& details) {
    std::cout << "[" << (passed ? "PASS" : "FAIL") << "] " << testName << ": " << details << std::endl;
}

// -----------------------------------------------------------------------------
// 1. Andersen Local-Push PPR Stress & Bug Verification
// -----------------------------------------------------------------------------
void stressTestAndersenPPR() {
    printHeader("1. Andersen Local-Push PPR Stress & Bug Verification");

    fs::path tmpDir = fs::temp_directory_path() / "chronos_ppr_stress_db";
    fs::remove_all(tmpDir);
    fs::create_directories(tmpDir);

    sqlite3* db = nullptr;
    std::string dbPath = (tmpDir / "codex.db").string();
    if (sqlite3_open(dbPath.c_str(), &db) != SQLITE_OK) {
        std::cerr << "Failed to open sqlite db at " << dbPath << "\n";
        return;
    }

    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, R"SQL(
        CREATE TABLE nodes (
            id TEXT PRIMARY KEY,
            file_path TEXT NOT NULL,
            byte_start INTEGER NOT NULL,
            byte_end INTEGER NOT NULL,
            simhash INTEGER NOT NULL,
            is_active INTEGER NOT NULL,
            parse_confidence REAL NOT NULL
        );
        CREATE TABLE edges (
            source_id TEXT NOT NULL,
            target_id TEXT NOT NULL,
            type TEXT NOT NULL,
            probable_target_weight REAL NOT NULL DEFAULT 1.0,
            PRIMARY KEY (source_id, target_id, type)
        );
        CREATE TABLE alias (
            old_id TEXT PRIMARY KEY,
            new_id TEXT NOT NULL,
            root_id TEXT NOT NULL,
            commit_hash TEXT NOT NULL
        );
    )SQL", nullptr, nullptr, nullptr);

    // 1.1 Scale test: 50,000 nodes
    int nodeCount = 50000;
    std::cout << "Generating " << nodeCount << " node graph in SQLite...\n";
    sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

    sqlite3_stmt* nStmt;
    sqlite3_prepare_v2(db, "INSERT INTO nodes VALUES (?1, 'f.cpp', 0, 10, 0, 1, 1.0);", -1, &nStmt, nullptr);
    for (int i = 0; i < nodeCount; ++i) {
        std::string nid = "node_" + std::to_string(i);
        sqlite3_bind_text(nStmt, 1, nid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(nStmt);
        sqlite3_reset(nStmt);
    }
    sqlite3_finalize(nStmt);

    sqlite3_stmt* eStmt;
    sqlite3_prepare_v2(db, "INSERT INTO edges VALUES (?1, ?2, 'CALL', 1.0);", -1, &eStmt, nullptr);
    int edgeCount = 0;
    for (int i = 0; i < nodeCount - 1; ++i) {
        std::string src = "node_" + std::to_string(i);
        std::string tgt = "node_" + std::to_string(i + 1);
        sqlite3_bind_text(eStmt, 1, src.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(eStmt, 2, tgt.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(eStmt);
        sqlite3_reset(eStmt);
        edgeCount++;

        std::string tgt2 = "node_" + std::to_string((i * 7 + 13) % nodeCount);
        sqlite3_bind_text(eStmt, 1, src.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(eStmt, 2, tgt2.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(eStmt);
        sqlite3_reset(eStmt);
        edgeCount++;
    }
    sqlite3_finalize(eStmt);
    sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);

    chronos::PPREngine engine(db);

    auto start = std::chrono::high_resolution_clock::now();
    std::vector<std::string> seeds = {"node_0", "node_100"};
    int budget = 500;
    auto res = engine.localPush(seeds, 0.5, 1e-4, budget);
    auto end = std::chrono::high_resolution_clock::now();
    double elapsedMs = std::chrono::duration<double, std::milli>(end - start).count();

    bool budgetPassed = (static_cast<int>(res.nodeIdToScore.size()) <= budget);
    printResult("PPR 50k Graph Execution Time", elapsedMs < 1000.0,
                "Time = " + std::to_string(elapsedMs) + " ms (Budget < 1000ms)");
    printResult("PPR 50k Graph Budget Bounded", budgetPassed,
                "Returned nodes = " + std::to_string(res.nodeIdToScore.size()) + " <= budget " + std::to_string(budget));

    // 1.2 Non-Existent / Ghost Seed Leak Bug
    auto ghostRes = engine.localPush(std::string("ghost_non_existent_node"), 0.5, 1e-4, 100);
    bool ghostPassed = ghostRes.nodeIdToScore.empty();
    printResult("PPR Non-Existent Seed Isolation", ghostPassed,
                ghostPassed ? "PASS: Non-existent seed ignored" : "BUG: Ghost seed '" + ghostRes.nodeIdToScore.begin()->first + "' returned in PPR map with score " + std::to_string(ghostRes.nodeIdToScore.begin()->second));

    // 1.3 Push Queue Capacity Truncation Bug Demonstration
    // Construct a specific sub-graph:
    // S -> A (weight 1.0), S -> B (weight 1.0)
    // A -> HIGH_MASS (weight 100.0)
    // B -> B1, B2, B3, B4, B5, B6, B7, B8, B9, B10 (weight 1.0 each)
    sqlite3_exec(db, "INSERT INTO nodes VALUES ('seed_S', 'f.cpp', 0, 10, 0, 1, 1.0);", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "INSERT INTO nodes VALUES ('node_A', 'f.cpp', 0, 10, 0, 1, 1.0);", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "INSERT INTO nodes VALUES ('node_B', 'f.cpp', 0, 10, 0, 1, 1.0);", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "INSERT INTO nodes VALUES ('node_HIGH_MASS', 'f.cpp', 0, 10, 0, 1, 1.0);", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "INSERT INTO edges VALUES ('seed_S', 'node_A', 'CALL', 1.0);", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "INSERT INTO edges VALUES ('seed_S', 'node_B', 'CALL', 1.0);", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "INSERT INTO edges VALUES ('node_A', 'node_HIGH_MASS', 'CALL', 100.0);", nullptr, nullptr, nullptr);

    for (int i = 1; i <= 15; ++i) {
        std::string bname = "node_B" + std::to_string(i);
        sqlite3_exec(db, ("INSERT INTO nodes VALUES ('" + bname + "', 'f.cpp', 0, 10, 0, 1, 1.0);").c_str(), nullptr, nullptr, nullptr);
        sqlite3_exec(db, ("INSERT INTO edges VALUES ('node_B', '" + bname + "', 'CALL', 1.0);").c_str(), nullptr, nullptr, nullptr);
    }

    engine.clearCache();
    int smallCapBudget = 10;
    auto queueTruncRes = engine.localPush(std::string("seed_S"), 0.5, 1e-5, smallCapBudget);
    bool highMassFound = queueTruncRes.nodeIdToScore.count("node_HIGH_MASS") > 0;
    printResult("PPR Push Queue Capacity Fairness", highMassFound,
                highMassFound ? "PASS: High mass node reached" : "BUG: Queue size check premature truncation dropped node_HIGH_MASS despite high residual weight");

    sqlite3_close(db);
    fs::remove_all(tmpDir);
}

// -----------------------------------------------------------------------------
// 2. RRF Extremes & Stability Stress Test
// -----------------------------------------------------------------------------
void stressTestRRF() {
    printHeader("2. Extremes in RRF Weights and Rank List Sizes");

    // 2.1 Empty inputs
    std::vector<chronos::RankList> emptyVec;
    auto rrf1 = chronos::blendRRF(emptyVec);
    printResult("RRF Empty Vector", rrf1.empty(), "Returned empty list");

    // 2.2 Division by zero when k is negative
    chronos::RankList listA = {{"nodeA", 1.0}, {"nodeB", 0.8}};
    chronos::RankList listB = {{"nodeB", 0.9}, {"nodeA", 0.7}};

    auto rrfDivZero = chronos::blendRRF({listA, listB}, -1.0);
    bool hasInfOrNan = false;
    for (const auto& [id, score] : rrfDivZero) {
        if (std::isnan(score) || std::isinf(score)) {
            hasInfOrNan = true;
        }
    }
    printResult("RRF Negative k (-1.0) Stability", !hasInfOrNan,
                hasInfOrNan ? "BUG: Unvalidated k = -1.0 caused Division-by-Zero (Inf/NaN in scores)" : "PASS: No NaN/Inf");

    // 2.3 Equal RRF score tie-breaking dependency on Node ID string vs Semantic Rank
    chronos::RankList rank1 = {{"node-2", 1.0}, {"node-1", 0.9}};
    chronos::RankList rank2 = {{"node-1", 1.0}, {"node-2", 0.9}};
    auto rrfTie = chronos::blendRRF({rank1, rank2}, 60.0);
    // Scores for both are identical: 1/61 + 1/62.
    // Standard blendRRF breaks ties using a.first < b.first ("node-1" < "node-2").
    bool alphabeticalTieBreak = (rrfTie.size() == 2 && rrfTie[0].first == "node-1");
    printResult("RRF Arbitrary Alphabetical Tie-Breaker", alphabeticalTieBreak,
                "Equal-score nodes sorted alphabetically ('node-1' before 'node-2') overriding input list order");

    // 2.4 Scalability: 100k items
    chronos::RankList hugeList1, hugeList2;
    hugeList1.reserve(100000);
    hugeList2.reserve(100000);
    for (int i = 0; i < 100000; ++i) {
        hugeList1.emplace_back("node_" + std::to_string(i), 1.0 / (i + 1));
        hugeList2.emplace_back("node_" + std::to_string(99999 - i), 1.0 / (i + 1));
    }

    auto start = std::chrono::high_resolution_clock::now();
    auto rrfHuge = chronos::blendRRF({hugeList1, hugeList2}, 60.0);
    auto end = std::chrono::high_resolution_clock::now();
    double elapsedMs = std::chrono::duration<double, std::milli>(end - start).count();

    printResult("RRF 100k Items Execution Time", elapsedMs < 1000.0,
                "Time = " + std::to_string(elapsedMs) + " ms, Fused count = " + std::to_string(rrfHuge.size()));
}

// -----------------------------------------------------------------------------
// 3. Extreme Timestamps Stress Test & ContextBuilder Recency Inversion Bug
// -----------------------------------------------------------------------------
void stressTestTimestampsAndContextBuilder() {
    printHeader("3. Extreme Timestamps & ContextBuilder Integration");

    fs::path tmpDir = fs::temp_directory_path() / "chronos_cb_recency_stress";
    fs::remove_all(tmpDir);
    fs::create_directories(tmpDir);

    {
        chronos::Codex codex(tmpDir.string());
        chronos::VectorIndex index(tmpDir.string());

        chronos::Node n1; n1.id = "node-1"; n1.file_path = "f.cpp"; n1.byte_start = 0; n1.byte_end = 10; n1.simhash = 10; n1.parse_confidence = 1.0f;
        chronos::Node n2; n2.id = "node-2"; n2.file_path = "f.cpp"; n2.byte_start = 20; n2.byte_end = 30; n2.simhash = 20; n2.parse_confidence = 1.0f;
        codex.upsertNode(n1);
        codex.upsertNode(n2);

        std::ofstream out((tmpDir / "f.cpp").string());
        out << "0123456789012345678901234567890123456789";
        out.close();

        // Node 1 is old (ts=1000000), Node 2 is brand new (ts=2000000)
        chronos::EmbeddingRecord r1; r1.nodeId = "node-1"; r1.vector = chronos::embedText("sample query code"); r1.timestamp = 1000000;
        chronos::EmbeddingRecord r2; r2.nodeId = "node-2"; r2.vector = chronos::embedText("sample query code"); r2.timestamp = 2000000;
        index.upsert(r1);
        index.upsert(r2);

        // Test 3.1: Vector Index Hop 1 ordering standalone
        auto vecResults = index.search(chronos::embedText("sample query code"), 2, 2000000);
        bool vecOrderCorrect = (vecResults.size() == 2 && vecResults[0].nodeId == "node-2");
        printResult("VectorIndex Recency Decay Standalone", vecOrderCorrect,
                    vecOrderCorrect ? "PASS: node-2 ranked #1 in VectorIndex" : "FAIL: VectorIndex failed recency decay");

        // Test 3.2: ContextBuilder pipeline recency ordering
        chronos::ContextBuilder builder(codex, index, tmpDir.string());
        chronos::BuildResult res = builder.build("sample query code", 40, 10, 0.01f, 2000000);

        bool contextBuilderRecencyCorrect = (res.ok && !res.request.context.empty() && res.request.context[0].nodeId == "node-2");
        printResult("ContextBuilder End-to-End Recency Preservation", contextBuilderRecencyCorrect,
                    contextBuilderRecencyCorrect ? "PASS: Top context node is node-2" : "BUG: ContextBuilder inverted recency ranking! Top node returned was '" + (res.request.context.empty() ? "NONE" : res.request.context[0].nodeId) + "' instead of fresh 'node-2'");

        // Test 3.3: Negative Commit Timestamps (pre-1970 commits)
        chronos::EmbeddingRecord rNeg; rNeg.nodeId = "node-neg"; rNeg.vector = chronos::embedText("sample query code"); rNeg.timestamp = -315360000;
        index.upsert(rNeg);
        auto negSearch = index.search(chronos::embedText("sample query code"), 3, 2000000);
        float negScore = 0.0f;
        for (const auto& m : negSearch) {
            if (m.nodeId == "node-neg") negScore = m.score;
        }
        printResult("VectorIndex Negative Commit Timestamp Handling", negScore > 0.0f,
                    "Score for negative timestamp commit = " + std::to_string(negScore) + " (commit_ts <= 0 disables recency decay)");
    }

    fs::remove_all(tmpDir);
}

// -----------------------------------------------------------------------------
// 4. God Class Topology Handling under Connectivity-Based MMR
// -----------------------------------------------------------------------------
void stressTestGodClassMMR() {
    printHeader("4. God Class Topology Handling under Connectivity-Based MMR");

    std::vector<chronos::MMRCandidate> candidates;

    // 50 God Class method candidates with relevance 0.95
    for (int i = 1; i <= 50; ++i) {
        candidates.push_back({"God_M" + std::to_string(i), 0.95});
    }
    // 5 Feature candidates with relevance 0.70
    candidates.push_back({"Feature_A", 0.70});
    candidates.push_back({"Feature_B", 0.68});
    candidates.push_back({"Feature_C", 0.65});
    candidates.push_back({"Feature_D", 0.60});
    candidates.push_back({"Feature_E", 0.55});

    std::unordered_map<std::string, std::unordered_set<std::string>> adjacency;

    // God Class methods are all connected to GodClass_Core and to each other (Dense clique)
    for (int i = 1; i <= 50; ++i) {
        std::string nodeI = "God_M" + std::to_string(i);
        adjacency[nodeI].insert("GodClass_Core");
        adjacency["GodClass_Core"].insert(nodeI);

        for (int j = 1; j <= 50; ++j) {
            if (i != j) {
                adjacency[nodeI].insert("God_M" + std::to_string(j));
            }
        }
    }

    // Feature nodes are connected in separate independent components
    adjacency["Feature_A"].insert("Feature_A_Helper");
    adjacency["Feature_B"].insert("Feature_B_Helper");
    adjacency["Feature_C"].insert("Feature_C_Helper");

    int selectBudget = 5;
    auto selected = chronos::connectivityMMR(candidates, adjacency, selectBudget, 0.7);

    int godCount = 0;
    int featureCount = 0;
    for (const auto& cand : selected) {
        if (cand.nodeId.rfind("God_", 0) == 0) godCount++;
        if (cand.nodeId.rfind("Feature_", 0) == 0) featureCount++;
    }

    bool godClassStarvationPrevented = (featureCount > 0);
    printResult("God Class Starvation Prevention (lambda=0.7)", godClassStarvationPrevented,
                "Selected " + std::to_string(godCount) + " God Class methods and " +
                std::to_string(featureCount) + " diverse feature nodes.");

    // Edge case: All candidates in single God Class clique (100% interconnected)
    std::vector<chronos::MMRCandidate> pureGodCandidates;
    for (int i = 1; i <= 10; ++i) {
        pureGodCandidates.push_back({"God_M" + std::to_string(i), 0.95});
    }

    auto pureGodSelected = chronos::connectivityMMR(pureGodCandidates, adjacency, 5, 0.7);
    printResult("Pure God Class Clique Selection", pureGodSelected.size() == 5,
                "Selected " + std::to_string(pureGodSelected.size()) + " / 5 items without crashing");
}

int main() {
    std::cout << "======================================================\n";
    std::cout << " CHRONOS MILESTONE 1 EMPIRICAL STRESS TEST HARNESS   \n";
    std::cout << "======================================================\n";

    stressTestAndersenPPR();
    stressTestRRF();
    stressTestTimestampsAndContextBuilder();
    stressTestGodClassMMR();

    std::cout << "\n======================================================\n";
    std::cout << " STRESS TEST HARNESS COMPLETE                        \n";
    std::cout << "======================================================\n";
    return 0;
}

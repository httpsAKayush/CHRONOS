#include "chronos/infrastructure/vector_index.hpp"
#include "chronos/use_cases/context_builder.hpp"
#include "chronos/infrastructure/codex.hpp"
#include "chronos/domain/ppr.hpp"
#include "chronos/domain/mmr.hpp"

#include <iostream>
#include <vector>
#include <string>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <chrono>

namespace fs = std::filesystem;
using namespace chronos;

static int g_failures = 0;

#define ASSERT_TEST(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << " " << msg << std::endl; \
            g_failures++; \
        } \
    } while(0)

std::vector<float> makeDummyVector(float fillVal) {
    std::vector<float> vec(VectorIndex::kDim, 0.0f);
    vec[0] = fillVal;
    if (fillVal < 1.0f && fillVal > -1.0f) {
        vec[1] = static_cast<float>(std::sqrt(1.0 - static_cast<double>(fillVal) * fillVal));
    }
    return vec;
}

// -----------------------------------------------------------------------------
// Challenge Target 1: Andersen Local Push Residual Vector Bounds & Safety
// -----------------------------------------------------------------------------
void challenge_andersen_local_push_bounds() {
    std::cout << "[CHALLENGE 1] Stress testing Andersen Local Push Residual Vector Bounds..." << std::endl;
    fs::path tmp = fs::temp_directory_path() / "chronos_stress_local_push";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    {
        Codex codex(tmp.string());

        // 1. Create a dense star graph: node-hub connected to 200 leaf nodes
        Node hub;
        hub.id = "hub-0";
        hub.file_path = "hub.cpp";
        hub.byte_start = 0;
        hub.byte_end = 100;
        hub.simhash = 100;
        hub.parse_confidence = 1.0f;
        codex.upsertNode(hub);

        for (int i = 0; i < 200; ++i) {
            Node leaf;
            leaf.id = "leaf-" + std::to_string(i);
            leaf.file_path = "leaf.cpp";
            leaf.byte_start = i * 10;
            leaf.byte_end = (i + 1) * 10;
            leaf.simhash = 100 + i;
            leaf.parse_confidence = 1.0f;
            codex.upsertNode(leaf);

            Edge e;
            e.source_id = "hub-0";
            e.target_id = leaf.id;
            e.type = "CALLS";
            e.probable_target_weight = 1.0f;
            codex.upsertEdge(e);
        }

        // Test 1.1: Multi-seed with duplicates and non-existent nodes
        std::vector<std::string> seedsWithDupes = {"hub-0", "hub-0", "leaf-0", "leaf-0", "non_existent_node_xyz"};
        TraceResult res1 = codex.localPushPPR(seedsWithDupes, 50);
        ASSERT_TEST(!res1.nodes.empty(), "localPushPPR should return nodes for duplicate/mixed seeds");
        
        // Test 1.2: Check total probability mass conservation and score upper/lower bounds
        PPREngine engine(codex.raw());
        PPRScoreMap scoresMap = engine.localPush(seedsWithDupes, 0.5, 1e-4, 100);
        
        double totalSettledMass = 0.0;
        for (const auto& [nodeId, score] : scoresMap.nodeIdToScore) {
            ASSERT_TEST(!std::isnan(score) && !std::isinf(score), "PPR score must not be NaN or Inf");
            ASSERT_TEST(score >= 0.0, "PPR score must be non-negative");
            ASSERT_TEST(score <= 1.0 + 1e-7, "Individual PPR score must not exceed 1.0");
            totalSettledMass += score;
        }
        ASSERT_TEST(totalSettledMass <= 1.0 + 1e-6, "Total settled probability mass must not exceed 1.0");
        std::cout << "  -> Star graph settled mass: " << totalSettledMass << " (<= 1.0 constraint verified)" << std::endl;

        // Test 1.3: Extreme parameters
        std::vector<std::string> singleSeed = {"hub-0"};
        std::vector<std::string> emptySeeds = {};

        // nodeBudget = 0
        PPRScoreMap scoreBudgetZero = engine.localPush(singleSeed, 0.5, 1e-4, 0);
        ASSERT_TEST(scoreBudgetZero.nodeIdToScore.empty(), "localPush with budget 0 should settle 0 nodes");

        // nodeBudget < 0
        PPRScoreMap scoreBudgetNeg = engine.localPush(singleSeed, 0.5, 1e-4, -10);
        ASSERT_TEST(scoreBudgetNeg.nodeIdToScore.empty(), "localPush with negative budget should settle 0 nodes");

        // seeds empty
        PPRScoreMap scoreSeedsEmpty = engine.localPush(emptySeeds, 0.5, 1e-4, 50);
        ASSERT_TEST(scoreSeedsEmpty.nodeIdToScore.empty(), "localPush with empty seeds should return empty score map");

        // alpha = 1.0 (all mass settled on seed)
        PPRScoreMap scoreAlphaOne = engine.localPush(singleSeed, 1.0, 1e-4, 50);
        ASSERT_TEST(scoreAlphaOne.nodeIdToScore.size() == 1, "alpha=1.0 should only settle on seed node");
        ASSERT_TEST(std::abs(scoreAlphaOne.nodeIdToScore["hub-0"] - 1.0) < 1e-6, "alpha=1.0 seed score should be 1.0");

        // alpha = 0.0 (no mass settled)
        PPRScoreMap scoreAlphaZero = engine.localPush(singleSeed, 0.0, 1e-4, 50);
        double sumAlphaZero = 0.0;
        for (const auto& [id, sc] : scoreAlphaZero.nodeIdToScore) sumAlphaZero += sc;
        ASSERT_TEST(sumAlphaZero == 0.0, "alpha=0.0 should result in 0.0 settled mass");

        // epsilon = 0.0
        PPRScoreMap scoreEpsZero = engine.localPush(singleSeed, 0.5, 0.0, 20);
        ASSERT_TEST(!scoreEpsZero.nodeIdToScore.empty(), "localPush with epsilon=0 should complete bounded by budget");

        // epsilon = 1.0 (seed residual < epsilon * d_out, nothing pushed)
        PPRScoreMap scoreEpsOne = engine.localPush(singleSeed, 0.5, 1.0, 50);
        ASSERT_TEST(scoreEpsOne.nodeIdToScore.empty(), "localPush with epsilon=1.0 should settle 0 nodes");
    }

    fs::remove_all(tmp);
    std::cout << "[CHALLENGE 1] PASSED." << std::endl;
}

// -----------------------------------------------------------------------------
// Challenge Target 2: Env Var Parsing Robustness Under Invalid Strings & Symbols
// -----------------------------------------------------------------------------
void challenge_env_var_parsing_robustness() {
    std::cout << "[CHALLENGE 2] Stress testing Environment Variable Parsing Robustness..." << std::endl;
    fs::path tmp = fs::temp_directory_path() / "chronos_stress_env_parsing";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    {
        VectorIndex index(tmp.string());
        EmbeddingRecord r1;
        r1.nodeId = "node-alpha";
        r1.vector = makeDummyVector(1.0f);
        r1.timestamp = 1000000;
        index.upsert(r1);

        auto queryVec = makeDummyVector(1.0f);
        int64_t queryTs = 2000000;

        const std::vector<std::string> invalidEnvValues = {
            "invalid_text",
            "!@#$%^&*()_+-=[]{}|;':\",./<>?",
            "nan",
            "NaN",
            "inf",
            "-inf",
            "1e308",
            "1e999",
            "-1e999",
            "0.5_extra_symbols",
            "   0.7   ",
            "",
            "-1.0",
            "2.5",
            "-100.0"
        };

        for (const auto& envVal : invalidEnvValues) {
            setenv("CHRONOS_RECENCY_ALPHA", envVal.c_str(), 1);
            setenv("CHRONOS_RECENCY_LAMBDA", envVal.c_str(), 1);

            try {
                auto seeds = index.search(queryVec, 1, queryTs);
                ASSERT_TEST(!seeds.empty(), "VectorIndex::search must return seeds under invalid env vars");
                if (!seeds.empty()) {
                    float s = seeds[0].score;
                    ASSERT_TEST(!std::isnan(s) && !std::isinf(s), "Seed score must be finite number for env var value: " + envVal);
                }
            } catch (const std::exception& ex) {
                ASSERT_TEST(false, std::string("VectorIndex::search threw exception under env var value '") + envVal + "': " + ex.what());
            } catch (...) {
                ASSERT_TEST(false, std::string("VectorIndex::search threw unknown exception under env var value '") + envVal + "'");
            }
        }

        unsetenv("CHRONOS_RECENCY_ALPHA");
        unsetenv("CHRONOS_RECENCY_LAMBDA");
    }

    fs::remove_all(tmp);
    std::cout << "[CHALLENGE 2] PASSED." << std::endl;
}

// -----------------------------------------------------------------------------
// Challenge Target 3: Context Node Budget Enforcement
// -----------------------------------------------------------------------------
void challenge_context_node_budget_enforcement() {
    std::cout << "[CHALLENGE 3] Stress testing Context Node Budget Enforcement..." << std::endl;
    fs::path tmp = fs::temp_directory_path() / "chronos_stress_budget";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    {
        Codex codex(tmp.string());
        VectorIndex index(tmp.string());

        // Dummy source file
        std::ofstream out((tmp / "code.cpp").string());
        out << "int main() { return 0; }\n";
        out.close();

        // Populate 30 nodes and edges
        for (int i = 0; i < 30; ++i) {
            std::string id = "node-" + std::to_string(i);
            Node n;
            n.id = id;
            n.file_path = "code.cpp";
            n.byte_start = 0;
            n.byte_end = 20;
            n.simhash = i + 1;
            n.parse_confidence = 1.0f;
            codex.upsertNode(n);

            EmbeddingRecord r;
            r.nodeId = id;
            r.vector = makeDummyVector(1.0f);
            r.timestamp = 1000000 + i * 1000;
            index.upsert(r);
        }

        // Add edges between nodes
        for (int i = 0; i < 29; ++i) {
            Edge e;
            e.source_id = "node-" + std::to_string(i);
            e.target_id = "node-" + std::to_string(i + 1);
            e.type = "CALLS";
            e.probable_target_weight = 1.0f;
            codex.upsertEdge(e);
        }

        ContextBuilder builder(codex, index, tmp.string());

        const std::vector<int> testBudgets = { 0, 1, 3, 5, 10, 25, 30, 50, 100, -1, -10 };

        for (int budget : testBudgets) {
            BuildResult res = builder.build("main code", 40, budget, 0.01f, 2000000);
            if (budget <= 0) {
                ASSERT_TEST(res.request.context.empty(), "Context size must be 0 when contextNodeBudget <= 0 (budget=" + std::to_string(budget) + ")");
            } else {
                ASSERT_TEST(static_cast<int>(res.request.context.size()) <= budget, 
                    "Context size (" + std::to_string(res.request.context.size()) + ") exceeded contextNodeBudget (" + std::to_string(budget) + ")");
            }
        }
    }

    fs::remove_all(tmp);
    std::cout << "[CHALLENGE 3] PASSED." << std::endl;
}

// -----------------------------------------------------------------------------
// Challenge Target 4: Memory Safety & Quantization Stress
// -----------------------------------------------------------------------------
void challenge_memory_safety_and_quantization() {
    std::cout << "[CHALLENGE 4] Stress testing Memory Safety & Quantization..." << std::endl;
    fs::path tmp = fs::temp_directory_path() / "chronos_stress_memory";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    {
        VectorIndex index(tmp.string());
        int64_t now = 2000000000LL;

        // Upsert 500 records with Hot, Warm, Cold tiers
        for (int i = 0; i < 500; ++i) {
            EmbeddingRecord rec;
            rec.nodeId = "mem-node-" + std::to_string(i);
            rec.vector = makeDummyVector(static_cast<float>(i % 100) / 100.0f);
            
            if (i % 3 == 0) {
                rec.timestamp = now - 100; // Hot
                rec.tier = MemoryTier::Hot;
            } else if (i % 3 == 1) {
                rec.timestamp = now - 10000000; // Warm
                rec.tier = MemoryTier::Warm;
            } else {
                rec.timestamp = now - 40000000; // Cold (> 1 year)
                rec.tier = MemoryTier::Cold;
            }
            index.upsert(rec);
        }

        // Cold tier items should NOT be stored in index
        ASSERT_TEST(index.size() < 500, "Cold tier items should be excluded from VectorIndex");
        std::cout << "  -> Total items stored after cold tier filtering: " << index.size() << " / 500" << std::endl;

        // Perform 100 rapid searches
        auto q = makeDummyVector(0.5f);
        for (int k = 0; k < 100; ++k) {
            auto res = index.search(q, 10, now);
            ASSERT_TEST(!res.empty(), "Search should return results under memory stress");
        }
    }

    fs::remove_all(tmp);
    std::cout << "[CHALLENGE 4] PASSED." << std::endl;
}

int main() {
    std::cout << "========================================================" << std::endl;
    std::cout << " CHRONOS MILESTONE 1 EMPIRICAL CHALLENGER HARNESS" << std::endl;
    std::cout << "========================================================" << std::endl;

    challenge_andersen_local_push_bounds();
    challenge_env_var_parsing_robustness();
    challenge_context_node_budget_enforcement();
    challenge_memory_safety_and_quantization();

    std::cout << "========================================================" << std::endl;
    if (g_failures == 0) {
        std::cout << " ALL EMPIRICAL CHALLENGE TESTS PASSED SUCCESSFULLY!" << std::endl;
        std::cout << "========================================================" << std::endl;
        return 0;
    } else {
        std::cerr << " FAILURES DETECTED: " << g_failures << " test(s) failed." << std::endl;
        std::cout << "========================================================" << std::endl;
        return 1;
    }
}

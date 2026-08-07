#include "test_framework.hpp"
#include "chronos/vector_index.hpp"
#include "chronos/context_builder.hpp"
#include "chronos/codex.hpp"
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <cmath>
#include <vector>

using namespace chronos;
namespace fs = std::filesystem;

namespace {
std::vector<float> makeUnitVector(float firstVal) {
    std::vector<float> vec(VectorIndex::kDim, 0.0f);
    vec[0] = firstVal;
    if (firstVal < 1.0f && firstVal > -1.0f) {
        vec[1] = static_cast<float>(std::sqrt(1.0 - static_cast<double>(firstVal) * firstVal));
    }
    return vec;
}
} // namespace

void test_recency_decay_ordering() {
    fs::path tmp = fs::temp_directory_path() / "chronos_test_recency_ordering";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    {
        VectorIndex index(tmp.string());

        EmbeddingRecord r1; r1.nodeId = "node-old"; r1.vector = makeUnitVector(1.0f); r1.timestamp = 1000000;
        EmbeddingRecord r2; r2.nodeId = "node-mid"; r2.vector = makeUnitVector(1.0f); r2.timestamp = 1900000;
        EmbeddingRecord r3; r3.nodeId = "node-new"; r3.vector = makeUnitVector(1.0f); r3.timestamp = 2000000;

        index.upsert(r1);
        index.upsert(r2);
        index.upsert(r3);

        auto queryVec = makeUnitVector(1.0f);
        int64_t queryTs = 2000000;

        auto seeds = index.search(queryVec, 3, queryTs);
        CHRONOS_CHECK(seeds.size() == 3);
        if (seeds.size() == 3) {
            CHRONOS_CHECK(seeds[0].nodeId == "node-new");
            CHRONOS_CHECK(seeds[1].nodeId == "node-mid");
            CHRONOS_CHECK(seeds[2].nodeId == "node-old");
            CHRONOS_CHECK(seeds[0].score > seeds[1].score);
            CHRONOS_CHECK(seeds[1].score > seeds[2].score);
        }
    }

    fs::remove_all(tmp);
}

void test_recency_env_alpha_override() {
    fs::path tmp = fs::temp_directory_path() / "chronos_test_recency_alpha";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    {
        VectorIndex index(tmp.string());

        // Node A: Perfect semantic match (cos_sim = 1.0), old timestamp (deltaT = 1,000,000s)
        EmbeddingRecord rA; rA.nodeId = "node-A"; rA.vector = makeUnitVector(1.0f); rA.timestamp = 1000000;
        // Node B: Lower semantic match (cos_sim = 0.5), fresh timestamp (deltaT = 0s)
        EmbeddingRecord rB; rB.nodeId = "node-B"; rB.vector = makeUnitVector(0.5f); rB.timestamp = 2000000;

        index.upsert(rA);
        index.upsert(rB);

        auto queryVec = makeUnitVector(1.0f);
        int64_t queryTs = 2000000;

        // Test 1: CHRONOS_RECENCY_ALPHA=1.0 (pure semantic) -> Node A must rank top
        setenv("CHRONOS_RECENCY_ALPHA", "1.0", 1);
        auto seedsAlpha1 = index.search(queryVec, 2, queryTs);
        CHRONOS_CHECK(seedsAlpha1.size() == 2);
        if (seedsAlpha1.size() == 2) {
            CHRONOS_CHECK(seedsAlpha1[0].nodeId == "node-A");
            CHRONOS_CHECK(seedsAlpha1[1].nodeId == "node-B");
            CHRONOS_CHECK(std::abs(seedsAlpha1[0].score - 1.0f) < 2e-3f);
            CHRONOS_CHECK(std::abs(seedsAlpha1[1].score - 0.5f) < 2e-3f);
        }

        // Test 2: CHRONOS_RECENCY_ALPHA=0.0 (pure temporal) -> Node B must rank top
        setenv("CHRONOS_RECENCY_ALPHA", "0.0", 1);
        auto seedsAlpha0 = index.search(queryVec, 2, queryTs);
        CHRONOS_CHECK(seedsAlpha0.size() == 2);
        if (seedsAlpha0.size() == 2) {
            CHRONOS_CHECK(seedsAlpha0[0].nodeId == "node-B");
            CHRONOS_CHECK(seedsAlpha0[1].nodeId == "node-A");
            CHRONOS_CHECK(std::abs(seedsAlpha0[0].score - 1.0f) < 1e-4f); // e^0 = 1.0
            CHRONOS_CHECK(seedsAlpha0[1].score < 0.95f); // decayed e^(-1e-7 * 1e6) = e^(-0.1) ~ 0.9048
        }

        unsetenv("CHRONOS_RECENCY_ALPHA");
    }

    fs::remove_all(tmp);
}

void test_recency_env_lambda_override() {
    fs::path tmp = fs::temp_directory_path() / "chronos_test_recency_lambda";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    {
        VectorIndex index(tmp.string());

        EmbeddingRecord rX; rX.nodeId = "node-X"; rX.vector = makeUnitVector(1.0f); rX.timestamp = 1000000;
        EmbeddingRecord rY; rY.nodeId = "node-Y"; rY.vector = makeUnitVector(1.0f); rY.timestamp = 2000000;

        index.upsert(rX);
        index.upsert(rY);

        auto queryVec = makeUnitVector(1.0f);
        int64_t queryTs = 2000000;

        // Test 1: CHRONOS_RECENCY_LAMBDA=0.0 (no decay) -> both nodes must have identical scores = 1.0
        setenv("CHRONOS_RECENCY_LAMBDA", "0.0", 1);
        auto seedsNoDecay = index.search(queryVec, 2, queryTs);
        CHRONOS_CHECK(seedsNoDecay.size() == 2);
        if (seedsNoDecay.size() == 2) {
            CHRONOS_CHECK(std::abs(seedsNoDecay[0].score - seedsNoDecay[1].score) < 1e-4f);
            CHRONOS_CHECK(std::abs(seedsNoDecay[0].score - 1.0f) < 1e-4f);
        }

        // Test 2: CHRONOS_RECENCY_LAMBDA=1e-3 (aggressive decay)
        setenv("CHRONOS_RECENCY_LAMBDA", "1e-3", 1);
        auto seedsFastDecay = index.search(queryVec, 2, queryTs);
        CHRONOS_CHECK(seedsFastDecay.size() == 2);
        if (seedsFastDecay.size() == 2) {
            CHRONOS_CHECK(seedsFastDecay[0].nodeId == "node-Y");
            CHRONOS_CHECK(seedsFastDecay[1].nodeId == "node-X");
            // Node X score decays to alpha * 1.0 + 0 = 0.7
            CHRONOS_CHECK(std::abs(seedsFastDecay[1].score - 0.7f) < 1e-3f);
        }

        unsetenv("CHRONOS_RECENCY_LAMBDA");
    }

    fs::remove_all(tmp);
}

void test_context_builder_query_timestamp() {
    fs::path tmp = fs::temp_directory_path() / "chronos_test_cb_recency";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    {
        Codex codex(tmp.string());
        VectorIndex index(tmp.string());

        Node n1; n1.id = "node-1"; n1.file_path = "f.cpp"; n1.byte_start = 0; n1.byte_end = 10; n1.simhash = 10; n1.parse_confidence = 1.0f;
        Node n2; n2.id = "node-2"; n2.file_path = "f.cpp"; n2.byte_start = 20; n2.byte_end = 30; n2.simhash = 20; n2.parse_confidence = 1.0f;
        codex.upsertNode(n1);
        codex.upsertNode(n2);

        // Dummy source file for ContextBuilder snippet reading
        std::ofstream out((tmp / "f.cpp").string());
        out << "0123456789012345678901234567890123456789";
        out.close();

        // Node 1 is older, Node 2 is newer
        EmbeddingRecord r1; r1.nodeId = "node-1"; r1.vector = embedText("sample query code"); r1.timestamp = 1000000;
        EmbeddingRecord r2; r2.nodeId = "node-2"; r2.vector = embedText("sample query code"); r2.timestamp = 2000000;
        index.upsert(r1);
        index.upsert(r2);

        ContextBuilder builder(codex, index, tmp.string());
        
        // Passing explicit queryTimestamp = 2000000 -> node-2 has zero deltaT while node-1 has decay
        BuildResult res = builder.build("sample query code", 40, 10, 0.01f, 2000000);
        CHRONOS_CHECK(res.ok);
        CHRONOS_CHECK(!res.request.context.empty());
        if (!res.request.context.empty()) {
            CHRONOS_CHECK(res.request.context[0].nodeId == "node-2");
        }
    }

    fs::remove_all(tmp);
}

void test_recency_edge_cases() {
    fs::path tmp = fs::temp_directory_path() / "chronos_test_recency_edge";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    {
        VectorIndex index(tmp.string());

        // Zero timestamp node (un-timestamped)
        EmbeddingRecord rZero; rZero.nodeId = "node-zero"; rZero.vector = makeUnitVector(1.0f); rZero.timestamp = 0;
        // Future timestamp node
        EmbeddingRecord rFut; rFut.nodeId = "node-fut"; rFut.vector = makeUnitVector(1.0f); rFut.timestamp = 3000000;

        index.upsert(rZero);
        index.upsert(rFut);

        auto queryVec = makeUnitVector(1.0f);
        int64_t queryTs = 2000000;

        // Both un-timestamped (ts=0) and future (ts > queryTs) should have deltaT = 0 -> score = 1.0
        auto seeds = index.search(queryVec, 2, queryTs);
        CHRONOS_CHECK(seeds.size() == 2);
        if (seeds.size() == 2) {
            CHRONOS_CHECK(std::abs(seeds[0].score - 1.0f) < 1e-4f);
            CHRONOS_CHECK(std::abs(seeds[1].score - 1.0f) < 1e-4f);
        }

        // Test invalid/out-of-bounds env var parsing
        setenv("CHRONOS_RECENCY_ALPHA", "invalid_text", 1);
        auto seedsInvalidAlpha = index.search(queryVec, 2, queryTs);
        CHRONOS_CHECK(seedsInvalidAlpha.size() == 2); // should fallback to default alpha 0.7

        setenv("CHRONOS_RECENCY_ALPHA", "2.5", 1); // should clamp to 1.0
        auto seedsClampedAlphaMax = index.search(queryVec, 2, queryTs);
        CHRONOS_CHECK(seedsClampedAlphaMax.size() == 2);

        setenv("CHRONOS_RECENCY_ALPHA", "-1.5", 1); // should clamp to 0.0
        auto seedsClampedAlphaMin = index.search(queryVec, 2, queryTs);
        CHRONOS_CHECK(seedsClampedAlphaMin.size() == 2);

        setenv("CHRONOS_RECENCY_LAMBDA", "-0.01", 1); // should clamp to 0.0
        auto seedsClampedLambda = index.search(queryVec, 2, queryTs);
        CHRONOS_CHECK(seedsClampedLambda.size() == 2);

        unsetenv("CHRONOS_RECENCY_ALPHA");
        unsetenv("CHRONOS_RECENCY_LAMBDA");
    }

    fs::remove_all(tmp);
}

void run_recency_tests() {
    test_recency_decay_ordering();
    test_recency_env_alpha_override();
    test_recency_env_lambda_override();
    test_context_builder_query_timestamp();
    test_recency_edge_cases();
}

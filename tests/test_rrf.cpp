#include "test_framework.hpp"
#include "chronos/domain/rrf.hpp"
#include <cmath>

using namespace chronos;

void run_rrf_tests() {
    // Test 1: Standard 2-list RRF
    {
        RankList listA = {{"node1", 0.9}, {"node2", 0.8}, {"node3", 0.7}};
        RankList listB = {{"node2", 0.95}, {"node1", 0.85}, {"node4", 0.75}};

        auto fused = reciprocalRankFusion(listA, listB, /*k=*/60.0);
        CHRONOS_CHECK(fused.size() == 4);

        // Calculate expected RRF scores with k=60:
        // node1: listA rank 1 (1/61), listB rank 2 (1/62) => 1/61 + 1/62 = 0.0163934 + 0.0161290 = 0.0325224
        // node2: listA rank 2 (1/62), listB rank 1 (1/61) => 1/62 + 1/61 = 0.0325224
        // node3: listA rank 3 (1/63) => 1/63 = 0.015873
        // node4: listB rank 3 (1/63) => 1/63 = 0.015873
        CHRONOS_CHECK(std::abs(fused[0].second - fused[1].second) < 1e-6); // node1 and node2 tied in score
        CHRONOS_CHECK(fused[0].first == "node1"); // tie-breaker alphabetical node1 < node2
        CHRONOS_CHECK(fused[1].first == "node2");
    }

    // Test 2: N=3 Weighted blendRRF
    {
        RankList list1 = {{"A", 1.0}, {"B", 0.5}};
        RankList list2 = {{"B", 1.0}, {"C", 0.5}};
        RankList list3 = {{"C", 1.0}, {"A", 0.5}};

        std::vector<double> weights = {2.0, 1.0, 0.5};
        auto fused = blendRRF({list1, list2, list3}, /*k=*/60.0, weights);

        CHRONOS_CHECK(fused.size() == 3);
        // A score: w1/(60+1) + w3/(60+2) = 2.0/61 + 0.5/62 = 0.0327869 + 0.0080645 = 0.0408514
        // B score: w1/(60+2) + w2/(60+1) = 2.0/62 + 1.0/61 = 0.0322580 + 0.0163934 = 0.0486514
        // C score: w2/(60+2) + w3/(60+1) = 1.0/62 + 0.5/61 = 0.0161290 + 0.0081967 = 0.0243257
        CHRONOS_CHECK(fused[0].first == "B");
        CHRONOS_CHECK(fused[1].first == "A");
        CHRONOS_CHECK(fused[2].first == "C");
    }

    // Test 3: Parameter k sensitivity
    {
        RankList list1 = {{"X", 10.0}, {"Y", 5.0}};
        RankList list2 = {{"Y", 20.0}, {"X", 1.0}};

        auto fusedK10 = blendRRF({list1, list2}, /*k=*/10.0);
        CHRONOS_CHECK(fusedK10.size() == 2);
        // X score (k=10): 1/11 + 1/12 = 0.0909 + 0.0833 = 0.1742
        // Y score (k=10): 1/12 + 1/11 = 0.1742 -> tie, tie breaker X < Y
        CHRONOS_CHECK(fusedK10[0].first == "X");
        CHRONOS_CHECK(fusedK10[1].first == "Y");
    }
}

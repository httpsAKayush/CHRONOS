#include "test_framework.hpp"
#include "chronos/mmr.hpp"

using namespace chronos;

void run_mmr_tests() {
    // God Class scenario: "god1".."god5" are all mutually adjacent (same
    // class) with slightly higher relevance scores; "distinct1" is
    // relevant but topologically unrelated. Connectivity MMR should not let
    // the God Class swallow the whole budget -- "distinct1" must appear in
    // a budget-of-3 selection despite lower raw relevance.
    std::vector<MMRCandidate> candidates = {
        {"god1", 0.95}, {"god2", 0.94}, {"god3", 0.93}, {"god4", 0.92}, {"god5", 0.91},
        {"distinct1", 0.80},
    };
    std::unordered_map<std::string, std::unordered_set<std::string>> adjacency = {
        {"god1", {"god2", "god3", "god4", "god5"}},
        {"god2", {"god1", "god3", "god4", "god5"}},
        {"god3", {"god1", "god2", "god4", "god5"}},
        {"god4", {"god1", "god2", "god3", "god5"}},
        {"god5", {"god1", "god2", "god3", "god4"}},
        {"distinct1", {}},
    };

    auto selected = connectivityMMR(candidates, adjacency, /*selectCount=*/3, /*lambda=*/0.7);
    CHRONOS_CHECK(selected.size() == 3);

    bool distinctIncluded = false;
    for (auto& c : selected) if (c.nodeId == "distinct1") distinctIncluded = true;
    CHRONOS_CHECK(distinctIncluded);
}

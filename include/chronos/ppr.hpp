#pragma once
// Local-Push Personalized PageRank over the Codex graph (Spec §0 Two-Hop
// Retrieval, §10 Spike "Local-Push PPR").
//
// Why local-push instead of full PageRank: full PageRank is O(E) per query
// and would blow the <1s TUI budget (Spec §9) on a 1M-LOC graph. Local push
// (Andersen-Chung-Lang style) only touches nodes reachable within the
// residual-mass budget, giving an output whose size is bounded by `budget`
// regardless of total graph size.
//
// project.md III/"Ambiguity Handling" additionally asks for a *dual-horizon*
// variant: run the push twice with different reset probabilities (tight
// "diagnostic" horizon vs wide "architectural" horizon) and fuse via
// Reciprocal Rank Fusion (see rrf.hpp) rather than guessing query intent.

#include <string>
#include <unordered_map>
#include <string>
#include <vector>
#include <sqlite3.h>

namespace chronos {

struct PPRScoreMap {
    std::unordered_map<std::string, double> nodeIdToScore;
};

class PPREngine {
public:
    explicit PPREngine(sqlite3* db) : db_(db) {}

    // Single-horizon local push. `alpha` is the PPR jump probability (Spec
    // default: dampingFactor=0.85 => alpha=0.15). `epsilon` bounds residual
    // mass per node (push stops once residual/degree < epsilon), which is
    // what keeps this O(1/epsilon) instead of O(N).
    PPRScoreMap localPush(const std::string& seedId, double alpha,
                          double epsilon, int nodeBudget);

    // Dual-Horizon: runs a tight push (small alpha => stays close to seed,
    // good for "what does X call" diagnostic queries) and a wide push
    // (large alpha => spreads further, good for "how is X used across the
    // architecture") then fuses the two rankings with RRF.
    PPRScoreMap dualHorizonPush(const std::string& seedId, int nodeBudget);

private:
    sqlite3* db_;
    // Returns adjacent (target_id, weight) pairs for CALLS/INHERITS/
    // PROBABLE_TARGET edges out of `nodeId`.
    std::vector<std::pair<std::string, double>> outEdges(const std::string& nodeId);
};

} // namespace chronos

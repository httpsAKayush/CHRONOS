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

    // Multi-seed local push. `alpha` is the PPR jump probability (decay).
    // `epsilon` bounds residual mass per node (push condition: r(u) >= epsilon * d_out(u)).
    PPRScoreMap localPush(const std::vector<std::string>& seeds, double alpha,
                          double epsilon, int nodeBudget);

    // Single-seed overload for backward compatibility.
    PPRScoreMap localPush(const std::string& seedId, double alpha,
                          double epsilon, int nodeBudget) {
        return localPush(std::vector<std::string>{seedId}, alpha, epsilon, nodeBudget);
    }

    // Dual-Horizon multi-seed push: tight (alpha=0.5, epsilon=1e-4) and wide (alpha=0.1, epsilon=1e-5) fused via RRF.
    PPRScoreMap dualHorizonPush(const std::vector<std::string>& seeds, int nodeBudget);

    // Single-seed overload for backward compatibility.
    PPRScoreMap dualHorizonPush(const std::string& seedId, int nodeBudget) {
        return dualHorizonPush(std::vector<std::string>{seedId}, nodeBudget);
    }

    void clearCache() { adjacencyCache_.clear(); }

private:
    sqlite3* db_;

    struct CachedNode {
        std::vector<std::pair<std::string, double>> edges;
        double outWeightSum = 0.0;
    };
    std::unordered_map<std::string, CachedNode> adjacencyCache_;

    const CachedNode& getCachedNode(const std::string& nodeId);

    // Returns adjacent (target_id, weight) pairs for edges out of `nodeId`.
    std::vector<std::pair<std::string, double>> outEdges(const std::string& nodeId);
};

} // namespace chronos

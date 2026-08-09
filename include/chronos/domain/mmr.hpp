#pragma once
// Connectivity-Based MMR (project.md II.2 / Spec Glossary "MMR"): standard
// Maximal Marginal Relevance penalizes items that are textually similar to
// already-selected items. We instead penalize *topological redundancy* —
// two nodes that are graph-adjacent (or share many neighbors) in the Codex
// are redundant even if their code text looks different, and two unrelated
// nodes are diverse even if they look textually similar. This is what kills
// "God Class starvation": a God Class's methods are highly interconnected,
// so after the first one or two are selected, the rest get penalized hard
// and the budget goes to structurally distinct, relevant nodes instead.

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace chronos {

struct MMRCandidate {
    std::string nodeId;
    double relevance = 0.0; // from PPR/RRF score
};

// `adjacency` maps nodeId -> set of directly connected nodeIds (both
// directions folded together) as pulled from Codex edges. `lambda` trades
// off relevance vs diversity (Spec default 0.7 favors relevance).
std::vector<MMRCandidate> connectivityMMR(
    const std::vector<MMRCandidate>& candidates,
    const std::unordered_map<std::string, std::unordered_set<std::string>>& adjacency,
    int selectCount,
    double lambda = 0.7);

} // namespace chronos

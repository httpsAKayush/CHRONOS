#include "chronos/mmr.hpp"
#include <algorithm>
#include <limits>

namespace chronos {

namespace {
// Topological redundancy between two nodes: 1.0 if directly adjacent, else
// Jaccard similarity of their neighbor sets (shared-neighbor overlap), 0 if
// no relation at all. This is the "penalize topological redundancy, not
// files" rule from project.md.
double topoRedundancy(const std::string& a, const std::string& b,
                       const std::unordered_map<std::string, std::unordered_set<std::string>>& adj) {
    auto itA = adj.find(a);
    auto itB = adj.find(b);
    if (itA == adj.end() || itB == adj.end()) return 0.0;
    if (itA->second.count(b) || itB->second.count(a)) return 1.0;

    const auto& na = itA->second;
    const auto& nb = itB->second;
    if (na.empty() || nb.empty()) return 0.0;
    size_t shared = 0;
    for (auto& n : na) if (nb.count(n)) ++shared;
    size_t unionSize = na.size() + nb.size() - shared;
    return unionSize == 0 ? 0.0 : static_cast<double>(shared) / unionSize;
}
}

std::vector<MMRCandidate> connectivityMMR(
    const std::vector<MMRCandidate>& candidates,
    const std::unordered_map<std::string, std::unordered_set<std::string>>& adjacency,
    int selectCount,
    double lambda) {

    std::vector<MMRCandidate> pool = candidates;
    std::vector<MMRCandidate> selected;

    while (!pool.empty() && static_cast<int>(selected.size()) < selectCount) {
        double bestScore = -std::numeric_limits<double>::infinity();
        size_t bestIdx = 0;

        for (size_t i = 0; i < pool.size(); ++i) {
            double maxRedundancy = 0.0;
            for (auto& sel : selected) {
                maxRedundancy = std::max(maxRedundancy, topoRedundancy(pool[i].nodeId, sel.nodeId, adjacency));
            }
            double mmrScore = lambda * pool[i].relevance - (1.0 - lambda) * maxRedundancy;
            if (mmrScore > bestScore) {
                bestScore = mmrScore;
                bestIdx = i;
            }
        }

        selected.push_back(pool[bestIdx]);
        pool.erase(pool.begin() + bestIdx);
    }

    return selected;
}

} // namespace chronos

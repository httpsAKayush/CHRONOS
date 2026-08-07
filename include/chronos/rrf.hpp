#pragma once
// Reciprocal Rank Fusion: fuses two independently-ranked node lists (tight
// vs wide PPR horizon) into one ranking without needing to normalize their
// raw scores against each other (which live on different scales).
// score(node) = sum over lists containing node of 1 / (k + rank_in_list)
#include <string>
#include <unordered_map>
#include <vector>

namespace chronos {

using RankList = std::vector<std::pair<std::string, double>>;

// Blends arbitrary N >= 2 rank lists using Reciprocal Rank Fusion.
// Formula: RRF(d) = \sum_{m \in M} \frac{w_m}{k + r_m(d)}
std::vector<std::pair<std::string, double>> blendRRF(
    const std::vector<RankList>& rankLists,
    double k = 60.0,
    const std::vector<double>& weights = {});

// Backward-compatible 2-list wrapper
std::vector<std::pair<std::string, double>> reciprocalRankFusion(
    const RankList& rankedListA,
    const RankList& rankedListB,
    double k = 60.0);

} // namespace chronos

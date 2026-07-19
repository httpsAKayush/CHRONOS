#pragma once
// Reciprocal Rank Fusion: fuses two independently-ranked node lists (tight
// vs wide PPR horizon) into one ranking without needing to normalize their
// raw scores against each other (which live on different scales).
// score(node) = sum over lists containing node of 1 / (k + rank_in_list)
#include <string>
#include <unordered_map>
#include <vector>

namespace chronos {

std::vector<std::pair<std::string, double>> reciprocalRankFusion(
    const std::vector<std::pair<std::string, double>>& rankedListA,
    const std::vector<std::pair<std::string, double>>& rankedListB,
    double k = 60.0);

} // namespace chronos

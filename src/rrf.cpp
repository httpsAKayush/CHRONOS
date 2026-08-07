#include "chronos/rrf.hpp"
#include <algorithm>

namespace chronos {

std::vector<std::pair<std::string, double>> blendRRF(
    const std::vector<RankList>& rankLists,
    double k,
    const std::vector<double>& weights) {

    std::unordered_map<std::string, double> fused;

    for (size_t m = 0; m < rankLists.size(); ++m) {
        double w = (m < weights.size()) ? weights[m] : 1.0;
        const auto& list = rankLists[m];
        for (size_t i = 0; i < list.size(); ++i) {
            double r = static_cast<double>(i + 1); // 1-based rank
            fused[list[i].first] += w / (k + r);
        }
    }

    std::vector<std::pair<std::string, double>> out(fused.begin(), fused.end());
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) {
                  if (a.second != b.second) {
                      return a.second > b.second;
                  }
                  return a.first < b.first;
              });
    return out;
}

std::vector<std::pair<std::string, double>> reciprocalRankFusion(
    const RankList& rankedListA,
    const RankList& rankedListB,
    double k) {
    return blendRRF({rankedListA, rankedListB}, k);
}

} // namespace chronos

#include "chronos/rrf.hpp"
#include <algorithm>

namespace chronos {

std::vector<std::pair<std::string, double>> reciprocalRankFusion(
    const std::vector<std::pair<std::string, double>>& rankedListA,
    const std::vector<std::pair<std::string, double>>& rankedListB,
    double k) {

    std::unordered_map<std::string, double> fused;

    auto fold = [&](const std::vector<std::pair<std::string, double>>& list) {
        // list is expected pre-sorted descending by score; rank is 1-based.
        for (size_t i = 0; i < list.size(); ++i) {
            double contribution = 1.0 / (k + static_cast<double>(i + 1));
            fused[list[i].first] += contribution;
        }
    };
    fold(rankedListA);
    fold(rankedListB);

    std::vector<std::pair<std::string, double>> out(fused.begin(), fused.end());
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    return out;
}

} // namespace chronos

#include "chronos/ppr.hpp"
#include "chronos/rrf.hpp"
#include <algorithm>
#include <deque>
#include <unordered_map>

namespace chronos {

namespace {
std::string resolveAlias(sqlite3* db, const std::string& id) {
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, "SELECT root_id FROM alias WHERE old_id = ?1;", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    std::string result = id;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        result = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return result;
}
} // namespace

std::vector<std::pair<std::string, double>> PPREngine::outEdges(const std::string& nodeId) {
    std::vector<std::pair<std::string, double>> edges;
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_,
        "SELECT target_id, probable_target_weight FROM edges WHERE source_id = ?1;",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, nodeId.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string target = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        double weight = sqlite3_column_double(stmt, 1);
        target = resolveAlias(db_, target);
        edges.emplace_back(target, weight);
    }
    sqlite3_finalize(stmt);
    return edges;
}

// Andersen-Chung-Lang local push (approximate PPR). Maintains a probability
// mass `p` (settled score) and residual mass `r` per node. Repeatedly picks
// a node whose residual/out-degree exceeds epsilon and pushes: keeps alpha
// share for itself, distributes (1-alpha) share to neighbors weighted by
// PROBABLE_TARGET usage weight. Bounded by nodeBudget nodes touched, which
// keeps this safely under the Spec §9 <1s query budget even on dense graphs.
PPRScoreMap PPREngine::localPush(const std::string& seedId, double alpha,
                                  double epsilon, int nodeBudget) {
    std::unordered_map<std::string, double> p; // settled score
    std::unordered_map<std::string, double> r; // residual
    r[seedId] = 1.0;

    std::deque<std::string> queue{seedId};
    std::unordered_map<std::string, bool> queued{{seedId, true}};
    int touched = 0;

    while (!queue.empty() && touched < nodeBudget) {
        std::string u = queue.front();
        queue.pop_front();
        queued[u] = false;

        auto neighbors = outEdges(u);
        double outWeightSum = 0.0;
        for (auto& [t, w] : neighbors) outWeightSum += w;
        if (outWeightSum <= 0.0) outWeightSum = 1.0; // dangling node guard

        double residual = r[u];
        if (residual <= 0.0) continue;
        if (neighbors.empty() && residual / 1.0 < epsilon) continue;

        p[u] += alpha * residual;
        double pushMass = (1.0 - alpha) * residual;
        r[u] = 0.0;
        ++touched;

        for (auto& [target, weight] : neighbors) {
            double share = pushMass * (weight / outWeightSum);
            r[target] += share;
            double targetDegreeApprox = std::max<size_t>(1, outEdges(target).size());
            if (r[target] / targetDegreeApprox >= epsilon && !queued[target] &&
                touched + static_cast<int>(queue.size()) < nodeBudget) {
                queue.push_back(target);
                queued[target] = true;
            }
        }
    }

    PPRScoreMap result;
    result.nodeIdToScore = p;
    return result;
}

PPRScoreMap PPREngine::dualHorizonPush(const std::string& seedId, int nodeBudget) {
    // Tight horizon: high alpha => mass stays close to the seed (diagnostic:
    // "what does X directly call/get called by").
    PPRScoreMap tight = localPush(seedId, /*alpha=*/0.5, /*epsilon=*/1e-4, nodeBudget);
    // Wide horizon: low alpha => mass spreads further through the graph
    // (architectural: "how does X fit into the wider system").
    PPRScoreMap wide = localPush(seedId, /*alpha=*/0.1, /*epsilon=*/1e-5, nodeBudget);

    auto toSortedVec = [](const PPRScoreMap& m) {
        std::vector<std::pair<std::string, double>> v(m.nodeIdToScore.begin(), m.nodeIdToScore.end());
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
        return v;
    };

    auto fused = reciprocalRankFusion(toSortedVec(tight), toSortedVec(wide));

    PPRScoreMap out;
    for (auto& [id, score] : fused) out.nodeIdToScore[id] = score;
    return out;
}

} // namespace chronos

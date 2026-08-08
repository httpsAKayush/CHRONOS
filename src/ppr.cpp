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

const PPREngine::CachedNode& PPREngine::getCachedNode(const std::string& nodeId) {
    auto it = adjacencyCache_.find(nodeId);
    if (it != adjacencyCache_.end()) {
        return it->second;
    }

    CachedNode cached;
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT target_id, probable_target_weight, type FROM edges WHERE source_id = ?1;",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, nodeId.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string target = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        double weight = sqlite3_column_double(stmt, 1);
        std::string type = "";
        if (sqlite3_column_text(stmt, 2)) {
            type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        }
        
        target = resolveAlias(db_, target);

        // Step 1: Blacklist "God Nodes" (Graph Traversal Fix)
        if (type == "external_symbol" || target.rfind("sym:", 0) == 0) {
            continue; // Do not hop through this node!
        }

        cached.edges.emplace_back(target, weight);
        cached.outWeightSum += weight;
    }
    sqlite3_finalize(stmt);

    auto [insertedIt, success] = adjacencyCache_.emplace(nodeId, std::move(cached));
    return insertedIt->second;
}

std::vector<std::pair<std::string, double>> PPREngine::outEdges(const std::string& nodeId) {
    return getCachedNode(nodeId).edges;
}

// Andersen-Chung-Lang local push (approximate PPR). Maintains a probability
// mass `p` (settled score) and residual mass `r` per node.
// Strictly enforces push condition: r(u) >= epsilon * d_out(u).
// Multi-seed initialization splits initial residual 1.0 equally across seeds.
PPRScoreMap PPREngine::localPush(const std::vector<std::string>& seeds, double alpha,
                                  double epsilon, int nodeBudget) {
    std::unordered_map<std::string, double> p; // settled score
    std::unordered_map<std::string, double> r; // residual

    if (seeds.empty()) {
        return PPRScoreMap{};
    }

    double initialMass = 1.0 / static_cast<double>(seeds.size());
    std::deque<std::string> queue;
    std::unordered_map<std::string, bool> queued;

    for (const auto& seedId : seeds) {
        r[seedId] += initialMass;
        if (!queued[seedId]) {
            const auto& nodeInfo = getCachedNode(seedId);
            double d_out = nodeInfo.outWeightSum > 0.0 ? nodeInfo.outWeightSum : 1.0;
            if (r[seedId] >= epsilon * d_out) {
                queue.push_back(seedId);
                queued[seedId] = true;
            }
        }
    }

    int touched = 0;

    while (!queue.empty() && touched < nodeBudget) {
        std::string u = queue.front();
        queue.pop_front();
        queued[u] = false;

        const auto& nodeInfo = getCachedNode(u);
        double d_out_u = nodeInfo.outWeightSum > 0.0 ? nodeInfo.outWeightSum : 1.0;

        double residual = r[u];
        // Strictly enforce Andersen-style local-push condition r(u) >= epsilon * d_out(u)
        if (residual < epsilon * d_out_u) {
            continue;
        }

        p[u] += alpha * residual;
        double pushMass = (1.0 - alpha) * residual;
        r[u] = 0.0;
        ++touched;

        for (const auto& [target, weight] : nodeInfo.edges) {
            double share = pushMass * (weight / d_out_u);
            r[target] += share;

            const auto& targetInfo = getCachedNode(target);
            double d_out_target = targetInfo.outWeightSum > 0.0 ? targetInfo.outWeightSum : 1.0;

            if (r[target] >= epsilon * d_out_target && !queued[target] &&
                touched + static_cast<int>(queue.size()) < nodeBudget) {
                queue.push_back(target);
                queued[target] = true;
            }
        }
    }

    PPRScoreMap result;
    result.nodeIdToScore = std::move(p);
    return result;
}

PPRScoreMap PPREngine::dualHorizonPush(const std::vector<std::string>& seeds, int nodeBudget) {
    // Tight horizon: high alpha (0.5) => mass stays close to seeds
    PPRScoreMap tight = localPush(seeds, /*alpha=*/0.5, /*epsilon=*/1e-4, nodeBudget);
    // Wide horizon: low alpha (0.1) => mass spreads further through graph
    PPRScoreMap wide = localPush(seeds, /*alpha=*/0.1, /*epsilon=*/1e-5, nodeBudget);

    auto toSortedVec = [](const PPRScoreMap& m) {
        std::vector<std::pair<std::string, double>> v(m.nodeIdToScore.begin(), m.nodeIdToScore.end());
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        return v;
    };

    auto fused = reciprocalRankFusion(toSortedVec(tight), toSortedVec(wide));

    PPRScoreMap out;
    for (const auto& [id, score] : fused) {
        out.nodeIdToScore[id] = score;
    }
    return out;
}

} // namespace chronos

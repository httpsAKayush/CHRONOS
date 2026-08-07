#include "chronos/context_builder.hpp"
#include "chronos/mmr.hpp"
#include "chronos/rrf.hpp"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <random>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace chronos {

namespace {
std::string makeTraceId() {
    static std::mt19937_64 rng{std::random_device{}()};
    std::ostringstream out;
    out << std::hex << rng();
    return out.str();
}
}

ContextBuilder::ContextBuilder(Codex& codex, VectorIndex& vectors, std::string repoRoot)
    : codex_(codex), vectors_(vectors), repoRoot_(std::move(repoRoot)) {}

std::string ContextBuilder::readLiveSnippet(const Node& n) const {
    // Ground Truth (project.md I.3): never cache signatures — read live
    // bytes from disk at the exact moment of context assembly.
    std::string fullPath = (fs::path(repoRoot_) / n.file_path).string();
    std::ifstream in(fullPath, std::ios::binary);
    if (!in) return "";
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    int64_t start = std::max<int64_t>(0, n.byte_start);
    int64_t end = std::min<int64_t>(static_cast<int64_t>(content.size()), n.byte_end);
    if (end <= start) return "";
    return content.substr(start, end - start);
}

BuildResult ContextBuilder::build(const std::string& userQuery, int pprBudget,
                                   int contextNodeBudget, float seedConfidenceFloor,
                                   int64_t queryTimestamp) {
    BuildResult result;

    // --- Hop 1: LanceDB(VectorIndex) semantic seed ---
    auto queryVec = embedText(userQuery);
    auto seeds = vectors_.search(queryVec, /*topK=*/5, queryTimestamp);
    if (seeds.empty() || seeds.front().score < seedConfidenceFloor) {
        result.ok = false;
        result.reason =
            "I couldn't find matching concepts in the codebase. Are you referring to an external library?";
        return result;
    }

    // --- Hop 2: Codex local-push PPR from all seeds ---
    std::vector<std::string> seedIds;
    for (const auto& s : seeds) {
        seedIds.push_back(s.nodeId);
    }
    TraceResult trace = codex_.localPushPPR(seedIds, pprBudget);
    result.rawTrace = trace;

    if (trace.nodes.empty()) {
        // Fallback: If no structural edges exist, we should still use the raw semantic seed
        // rather than hallucinating or aborting (Graceful degradation).
        auto fallbackNode = codex_.getNode(seeds.front().nodeId);
        if (fallbackNode) {
            trace.nodes.push_back(*fallbackNode);
        } else {
            result.ok = false;
            result.reason = "Found a semantic match but the node was missing from the codex.";
            return result;
        }
    }

    // --- Blend Hop 1 semantic vector ranks and Hop 2 structural PPR ranks via RRF ---
    RankList hop1_ranks;
    for (const auto& s : seeds) {
        hop1_ranks.emplace_back(s.nodeId, s.score);
    }

    RankList hop2_ranks;
    for (const auto& n : trace.nodes) {
        hop2_ranks.emplace_back(n.id, 0.0);
    }

    RankList fusedRRF = blendRRF({hop1_ranks, hop2_ranks}, 60.0);
    std::unordered_map<std::string, double> fusedScoreMap;
    for (const auto& [id, score] : fusedRRF) {
        fusedScoreMap[id] = score;
    }

    // --- Connectivity-based MMR pruning to the context node budget ---
    std::vector<MMRCandidate> candidates;
    for (const auto& n : trace.nodes) {
        double relevance = fusedScoreMap.count(n.id) ? fusedScoreMap[n.id] : 0.0;
        candidates.push_back({n.id, relevance});
    }

    std::unordered_map<std::string, std::unordered_set<std::string>> adjacency;
    for (const auto& e : trace.edges) {
        adjacency[e.source_id].insert(e.target_id);
        adjacency[e.target_id].insert(e.source_id);
    }
    auto pruned = connectivityMMR(candidates, adjacency, contextNodeBudget);

    // --- Assemble live-byte context payload for the daemon ---
    ChronosRequest req;
    req.traceId = makeTraceId();
    req.userQuery = userQuery;
    req.requireCitations = true; // FR-8

    std::unordered_map<std::string, Node> nodeById;
    for (auto& n : trace.nodes) nodeById[n.id] = n;

    for (auto& cand : pruned) {
        auto it = nodeById.find(cand.nodeId);
        if (it == nodeById.end()) continue;
        ContextNode cn;
        cn.nodeId = it->second.id;
        cn.filePath = it->second.file_path;
        cn.codeSnippet = readLiveSnippet(it->second);
        cn.uncertain = it->second.parse_confidence < 0.5f; // Spec §7 Safety Contract trigger
        req.context.push_back(std::move(cn));
    }

    codex_.recordTrace(req.traceId, trace);

    result.ok = true;
    result.request = std::move(req);
    return result;
}

} // namespace chronos

#include "chronos/context_builder.hpp"
#include "chronos/mmr.hpp"
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
                                   int contextNodeBudget, float seedConfidenceFloor) {
    BuildResult result;

    // --- Hop 1: LanceDB(VectorIndex) semantic seed ---
    auto queryVec = embedText(userQuery);
    auto seeds = vectors_.search(queryVec, /*topK=*/5);
    if (seeds.empty() || seeds.front().score < seedConfidenceFloor) {
        result.ok = false;
        result.reason =
            "I couldn't find matching concepts in the codebase. Are you referring to an external library?";
        return result;
    }

    // --- Hop 2: Codex local-push PPR from the best seed ---
    TraceResult trace = codex_.localPushPPR(seeds.front().nodeId, pprBudget);
    result.rawTrace = trace;

    if (trace.nodes.empty()) {
        result.ok = false;
        result.reason = "Found a semantic match but it has no recorded structural edges yet.";
        return result;
    }

    // --- Connectivity-based MMR pruning to the context node budget ---
    std::vector<MMRCandidate> candidates;
    std::unordered_map<std::string, double> seedScoreById;
    for (auto& s : seeds) seedScoreById[s.nodeId] = s.score;

    for (auto& n : trace.nodes) {
        double relevance = seedScoreById.count(n.id) ? seedScoreById[n.id] : 0.5; // PPR already ranked order
        candidates.push_back({n.id, relevance});
    }
    std::unordered_map<std::string, std::unordered_set<std::string>> adjacency;
    for (auto& e : trace.edges) {
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

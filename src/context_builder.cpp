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
    // Path-resilient: if the stored path doesn't exist, walk the repo tree
    // to find a file whose basename matches (handles stale prefix in DB).
    auto tryRead = [&](const std::string& path) -> std::string {
        std::ifstream in(path, std::ios::binary);
        if (!in) return "";
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        int64_t start = std::max<int64_t>(0, n.byte_start);
        int64_t end = std::min<int64_t>(static_cast<int64_t>(content.size()), n.byte_end);
        if (end <= start) return "";
        return content.substr(start, end - start);
    };

    // Primary path: stored file_path joined with repoRoot
    std::string primary = (fs::path(repoRoot_) / n.file_path).string();
    std::string snippet = tryRead(primary);
    if (!snippet.empty()) return snippet;

    // Fallback: walk the repo looking for a file whose relative path *ends with*
    // the stored file_path basename. Handles "matching/matcher.py" → "ct_pipeline/matching/matcher.py"
    std::string storedFilename = fs::path(n.file_path).filename().string();
    std::string storedRelDir   = fs::path(n.file_path).parent_path().filename().string(); // e.g. "matching"
    try {
        for (auto& entry : fs::recursive_directory_iterator(repoRoot_)) {
            if (!entry.is_regular_file()) continue;
            auto p = entry.path();
            if (p.filename() != storedFilename) continue;
            // Extra guard: parent dir name must also match to avoid false positives
            if (p.parent_path().filename() != storedRelDir) continue;
            snippet = tryRead(p.string());
            if (!snippet.empty()) return snippet;
        }
    } catch (...) {}

    return "";
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

BuildResult ContextBuilder::buildExplain(std::string targetSymbol, const std::string& userQuery) {
    BuildResult result;

    if (targetSymbol.find("sym:") != 0) {
        targetSymbol = "sym:" + targetSymbol;
    }

    std::string rootId;
    try {
        rootId = codex_.resolveAlias(targetSymbol);
    } catch (const std::exception& e) {
        result.ok = false;
        result.reason = std::string("Lookup failed: ") + e.what();
        return result;
    }

    auto startNode = codex_.getNode(rootId);
    if (!startNode) {
        result.ok = false;
        result.reason = "Could not find node for target symbol in Codex.";
        return result;
    }

    // Traverse OUTGOING edges to collect direct callees (the function's dependencies).
    // Use filtered variant to exclude stdlib/external noise.
    std::vector<Edge> edges = codex_.getEdgesFiltered(rootId, /*outgoing=*/true);
    std::unordered_set<std::string> depIds;
    for (const auto& e : edges) {
        try {
            std::string resolved = codex_.resolveAlias(e.target_id);
            depIds.insert(resolved);
        } catch (...) {
            continue;
        }
    }

    ChronosRequest req;
    req.traceId = makeTraceId();
    req.requireCitations = true;

    if (userQuery.empty()) {
        req.userQuery = "Explain the architectural logic and data flow of this function and its direct dependencies.";
    } else {
        req.userQuery = userQuery;
    }

    ContextNode cnTarget;
    cnTarget.nodeId = startNode->id;
    cnTarget.filePath = startNode->file_path;
    cnTarget.codeSnippet = readLiveSnippet(*startNode);
    cnTarget.uncertain = startNode->parse_confidence < 0.5f;
    req.context.push_back(std::move(cnTarget));

    TraceResult trace;
    trace.nodes.push_back(*startNode);

    for (const auto& did : depIds) {
        if (did == startNode->id) continue;
        auto dNode = codex_.getNode(did);
        if (dNode && dNode->is_active) {
            ContextNode cn;
            cn.nodeId = dNode->id;
            cn.filePath = dNode->file_path;
            cn.codeSnippet = readLiveSnippet(*dNode);
            cn.uncertain = dNode->parse_confidence < 0.5f;
            req.context.push_back(std::move(cn));
            trace.nodes.push_back(*dNode);
        }
    }

    trace.edges = edges;
    codex_.recordTrace(req.traceId, trace);
    result.rawTrace = trace;
    result.ok = true;
    result.request = std::move(req);
    return result;
}

} // namespace chronos

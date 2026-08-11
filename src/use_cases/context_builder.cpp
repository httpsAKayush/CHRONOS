#include "chronos/use_cases/context_builder.hpp"
#include "chronos/domain/mmr.hpp"
#include "chronos/domain/rrf.hpp"
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

// Phase 2 "Strict Context Boundaries": hard upper token limit before any
// RAG payload is sent to the LLM adapter. We estimate tokens as chars/4 and
// prune context nodes in descending relevance order (drop lowest-MMR last,
// i.e. remove the least relevant first), truncating individual snippet
// bytes as needed. nodeIds are always kept so FR-8 citations still resolve.
constexpr size_t kMaxPayloadTokens = 4000;
constexpr size_t kMaxPayloadChars = kMaxPayloadTokens * 4;

void enforceTokenBudget(ChronosRequest& req) {
    size_t totalChars = 0;
    std::vector<ContextNode> kept;
    kept.reserve(req.context.size());
    for (auto& cn : req.context) {
        size_t available = kMaxPayloadChars - std::min(totalChars, kMaxPayloadChars);
        if (available == 0) break; // budget exhausted — drop the rest
        if (cn.codeSnippet.size() > available) {
            cn.codeSnippet = cn.codeSnippet.substr(0, available); // nodeId preserved for citations
        }
        totalChars += cn.codeSnippet.size();
        kept.push_back(std::move(cn));
    }
    req.context = std::move(kept);
}
} // namespace

ContextBuilder::ContextBuilder(Codex& codex, VectorIndex& vectors, std::string repoRoot)
    : codex_(codex), vectors_(vectors), repoRoot_(std::move(repoRoot)) {}

std::string ContextBuilder::readLiveSnippet(const Node& n) const {
    // Context nodes ([GLOBAL:REPO], [CONTEXT:DIR:...]) have no source bytes.
    if (n.byte_end <= n.byte_start) return "";
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
    // Build a fast lookup from seed nodeId -> score for temporal re-injection below
    std::unordered_map<std::string, double> seedScoreMap;
    for (const auto& s : seeds) {
        hop1_ranks.emplace_back(s.nodeId, s.score);
        seedScoreMap[s.nodeId] = s.score;
    }

    RankList hop2_ranks;
    for (const auto& n : trace.nodes) {
        // If PPR returned a node that was also a seed (typical when graph has no edges),
        // carry the temporally-weighted seed score forward so recency ordering is preserved.
        double pprScore = seedScoreMap.count(n.id) ? seedScoreMap[n.id] : 0.0;
        hop2_ranks.emplace_back(n.id, pprScore);
    }
    // RRF is position-based: sort hop2 by temporal score so recency ordering is honoured.
    std::sort(hop2_ranks.begin(), hop2_ranks.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

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
    // Sort by fused relevance descending so MMR starts from highest-scoring candidates.
    // Without this, candidates are in PPR traversal order which ignores temporal scoring.
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) { return a.relevance > b.relevance; });

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

    // Phase 2: hard token cap before the payload leaves this use case.
    enforceTokenBudget(req);

    codex_.recordTrace(req.traceId, trace);

    result.ok = true;
    result.request = std::move(req);
    return result;
}

BuildResult ContextBuilder::buildExplain(std::string targetSymbol, const std::string& userQuery) {
    BuildResult result;

    // Detect file paths (contains / or ends with a known extension) vs symbol names.
    bool isFilePath = (targetSymbol.find('/') != std::string::npos ||
                       targetSymbol.find('.') != std::string::npos);

    std::string rootId;
    std::vector<std::string> fileNodeIds;

    if (isFilePath) {
        // File path mode: find all function nodes in this file.
        std::string path = targetSymbol;
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(codex_.raw(),
            "SELECT id FROM nodes WHERE file_path = ?1 AND is_active = 1 "
            "AND parse_confidence > 0.0 ORDER BY byte_start;",
            -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            fileNodeIds.emplace_back(
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
        }
        sqlite3_finalize(stmt);

        if (fileNodeIds.empty()) {
            // Fallback: try the whole-file node (parse_confidence = 0.0)
            sqlite3_prepare_v2(codex_.raw(),
                "SELECT id FROM nodes WHERE file_path = ?1 AND is_active = 1 "
                "ORDER BY parse_confidence DESC LIMIT 1;",
                -1, &stmt, nullptr);
            sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                rootId = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            }
            sqlite3_finalize(stmt);
            if (rootId.empty()) {
                result.ok = false;
                result.reason = "No nodes found for file: " + path;
                return result;
            }
        } else {
            rootId = fileNodeIds[0];
        }
    } else {
        // Symbol mode: prepend sym: if needed and resolve.
        if (targetSymbol.find("sym:") != 0) {
            targetSymbol = "sym:" + targetSymbol;
        }
        try {
            rootId = codex_.resolveAlias(targetSymbol);
        } catch (const std::exception& e) {
            result.ok = false;
            result.reason = std::string("Lookup failed: ") + e.what();
            return result;
        }
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
        req.userQuery = isFilePath
            ? "Explain the architectural logic, data flow, and structure of all functions in this file."
            : "Explain the architectural logic and data flow of this function and its direct dependencies.";
    } else {
        req.userQuery = userQuery;
    }

    TraceResult trace;

    if (isFilePath && fileNodeIds.size() > 1) {
        // File-path mode: include all function nodes from the file.
        for (const auto& nid : fileNodeIds) {
            auto node = codex_.getNode(nid);
            if (!node || !node->is_active) continue;
            ContextNode cn;
            cn.nodeId = node->id;
            cn.filePath = node->file_path;
            cn.codeSnippet = readLiveSnippet(*node);
            cn.uncertain = node->parse_confidence < 0.5f;
            req.context.push_back(std::move(cn));
            trace.nodes.push_back(*node);
        }
    } else {
        ContextNode cnTarget;
        cnTarget.nodeId = startNode->id;
        cnTarget.filePath = startNode->file_path;
        cnTarget.codeSnippet = readLiveSnippet(*startNode);
        cnTarget.uncertain = startNode->parse_confidence < 0.5f;
        req.context.push_back(std::move(cnTarget));
        trace.nodes.push_back(*startNode);
    }

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

    // Phase 2: hard token cap before the payload leaves this use case.
    enforceTokenBudget(req);

    result.rawTrace = trace;
    result.ok = true;
    result.request = std::move(req);
    return result;
}

} // namespace chronos

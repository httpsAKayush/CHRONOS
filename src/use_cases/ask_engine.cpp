#include "chronos/use_cases/ask_engine.hpp"
#include "chronos/cli/cli_util.hpp"
#include <sstream>
#include <random>
#include <iostream>
#include <unordered_set>

namespace chronos {

namespace {
constexpr int kHyDETopK = 10;
constexpr int kFinalContextBudget = 8;

std::string makeTraceId() {
    static std::mt19937_64 rng{std::random_device{}()};
    std::ostringstream out;
    out << std::hex << rng();
    return out.str();
}
} // namespace

AskEngine::AskEngine(IStorage& storage, VectorIndex& vectors, IpcLLMClient& llm,
                     const std::string& repoRoot)
    : storage_(storage), vectors_(vectors), llm_(llm), repoRoot_(repoRoot) {}

// ── Step 1: Instant Fetch ────────────────────────────────────────────────
std::string AskEngine::fetchRepoSummary() {
    auto node = storage_.getNode("[GLOBAL:REPO]");
    if (node) return node->ai_summary;
    return "";
}

// ── Step 2: Subsystem HyDE Generation (zero-shot) ───────────────────────
std::string AskEngine::generateHypothetical(const std::string& repoSummary,
                                             const std::string& query) {
    std::string prompt =
        "You are a codebase architect writing a Hypothetical Document Embedding "
        "(HyDE) for a targeted code search.\n\n"
        "You have been given the master architectural summary of a repository "
        "and a user's query. Based ONLY on the architectural context provided, "
        "write a hypothetical, highly technical C++ code snippet or detailed "
        "technical explanation that would answer the query if it existed in "
        "this codebase. Be as specific and concrete as possible — mention "
        "file paths, class names, function signatures, data structures, or "
        "algorithmic details that are likely to match actual implementation "
        "nodes.\n\n"
        "REPOSITORY ARCHITECTURE:\n" + repoSummary + "\n\n"
        "USER QUERY:\n" + query + "\n\n"
        "Write the hypothetical technical response below:\n";

    std::string hypothetical = llm_.complete(prompt, "Write the hypothetical code/explanation.", 2048);
    return hypothetical;
}

// ── Step 3: Targeted Vector Search ──────────────────────────────────────
std::vector<SeedMatch> AskEngine::targetedSearch(const std::string& hypothetical, int topK) {
    // embedText() tries the configured LLM first and falls back to a
    // deterministic hash embedding when the model cannot embed (e.g.
    // llama3.1:8b on Ollama), so a query vector is always produced.
    std::vector<float> vec = embedText(hypothetical);
    if (vec.empty() || vec.size() < static_cast<size_t>(VectorIndex::kDim)) {
        return {};
    }
    std::vector<float> kDimVec(vec.begin(), vec.begin() + VectorIndex::kDim);
    return vectors_.search(kDimVec, topK);
}

// ── Step 4: Final Synthesis ─────────────────────────────────────────────
ChronosRequest AskEngine::synthesize(const std::vector<SeedMatch>& seeds,
                                      const std::string& query,
                                      const std::string& traceId) {
    ChronosRequest req;
    req.traceId = traceId;
    req.userQuery = query;
    req.requireCitations = true;

    TraceResult trace;
    std::unordered_set<std::string> addedIds;

    for (const auto& sm : seeds) {
        auto node = storage_.getNode(sm.nodeId);
        if (!node || !node->is_active) continue;
        if (addedIds.count(node->id)) continue;
        addedIds.insert(node->id);

        ContextNode cn;
        cn.nodeId = node->id;
        cn.filePath = node->file_path;
        cn.codeSnippet = readSnippetForNode(*node, repoRoot_);
        cn.uncertain = node->parse_confidence < 0.5f;
        req.context.push_back(std::move(cn));
        trace.nodes.push_back(*node);
    }

    std::unordered_set<std::string> addedDirs;
    for (const auto& sm : seeds) {
        auto node = storage_.getNode(sm.nodeId);
        if (!node) continue;

        auto dirEdges = storage_.getEdges(sm.nodeId, false);
        for (const auto& e : dirEdges) {
            if (e.type != "CONTAINS") continue;
            auto dirNode = storage_.getNode(e.source_id);
            if (!dirNode) continue;
            if (addedDirs.count(dirNode->id)) continue;
            addedDirs.insert(dirNode->id);

            ContextNode cn;
            cn.nodeId = dirNode->id;
            cn.filePath = dirNode->file_path;
            cn.codeSnippet = "[SUBSYSTEM SUMMARY]\n" + dirNode->ai_summary;
            cn.uncertain = false;
            req.context.push_back(std::move(cn));
            trace.nodes.push_back(*dirNode);
        }
    }

    auto repoNode = storage_.getNode("[GLOBAL:REPO]");
    if (repoNode) {
        ContextNode cn;
        cn.nodeId = repoNode->id;
        cn.filePath = repoNode->file_path;
        cn.codeSnippet = "[REPOSITORY SUMMARY]\n" + repoNode->ai_summary;
        cn.uncertain = false;
        req.context.push_back(std::move(cn));
        trace.nodes.push_back(*repoNode);
    }

    storage_.recordTrace(traceId, trace);
    return req;
}

// ── Public entry point ──────────────────────────────────────────────────
AskResult AskEngine::run(const std::string& query) {
    AskResult result;
    result.usedHyDE = false;

    std::string traceId = makeTraceId();

    // Step 1: Instant fetch
    std::string repoSummary = fetchRepoSummary();
    if (!repoSummary.empty()) {
        std::cout << "[HyDE] Fetched [GLOBAL:REPO] context (" << repoSummary.size() << " chars).\n";
    } else {
        std::cout << "[HyDE] No [GLOBAL:REPO] found — proceeding without architectural context.\n";
    }

    // Step 2: Generate hypothetical
    std::string hypothetical;
    if (llm_.isAvailable()) {
        std::cout << "[HyDE] Generating subsystem-aware hypothetical response...\n";
        hypothetical = generateHypothetical(repoSummary, query);
    }

    std::vector<SeedMatch> seeds;
    if (!hypothetical.empty()) {
        result.usedHyDE = true;
        std::cout << "[HyDE] Targeted vector search with " << hypothetical.size() << "-char hypothetical.\n";

        // Step 3: Targeted search
        seeds = targetedSearch(hypothetical, kHyDETopK);
        std::cout << "[HyDE] Found " << seeds.size() << " seed matches.\n";
    } else {
        std::cout << "[HyDE] LLM unavailable — falling back to plain embedText search.\n";
        auto queryVec = embedText(query);
        seeds = vectors_.search(queryVec, kHyDETopK);
        std::cout << "[Fallback] Found " << seeds.size() << " seed matches.\n";
    }

    if (seeds.empty()) {
        result.ok = false;
        result.reason = "No relevant code found in the codebase for this query.";
        return result;
    }

    // Step 4: Final synthesis payload
    auto req = synthesize(seeds, query, traceId);

    // Capture raw trace
    TraceResult trace;
    for (const auto& n : req.context) {
        auto node = storage_.getNode(n.nodeId);
        if (node) trace.nodes.push_back(*node);
    }

    result.ok = true;
    result.request = std::move(req);
    result.rawTrace = std::move(trace);
    return result;
}

} // namespace chronos
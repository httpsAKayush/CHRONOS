#include "chronos/use_cases/ask_engine.hpp"
#include "chronos/cli/cli_util.hpp"
#include "chronos/domain/rrf.hpp"
#include <sstream>
#include <random>
#include <iostream>
#include <unordered_set>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <regex>
#include <set>

namespace chronos {

namespace {
constexpr int kHyDETopK = 25;
constexpr int kFinalContextBudget = 8;
// Weight multiplier for structurally-expanded nodes (file/symbol direct hits)
constexpr double kStructuralBoost = 5.0;

std::string makeTraceId() {
    static std::mt19937_64 rng{std::random_device{}()};
    std::ostringstream out;
    out << std::hex << rng();
    return out.str();
}

// ─── Structural Signal Extraction ────────────────────────────────────────
// Parse the query for explicit file/symbol mentions. Returns a list of
// lowercase stems (without extension) and exact names.
std::vector<std::string> extractMentionedNames(const std::string& query) {
    std::vector<std::string> names;
    std::string q = query;
    // Lowercase the query for case-insensitive matching
    std::transform(q.begin(), q.end(), q.begin(),
                   [](unsigned char c){ return std::tolower(c); });

    // 1. Explicit file names: "decision.py", "agent.py", "e2e_learning_flow.py"
    static const std::regex filePattern(R"(\b([\w_]+)(?:\.(?:py|cpp|hpp|js|ts|go|rs|java|rb|cs))\b)");
    std::sregex_iterator it(q.begin(), q.end(), filePattern), end;
    for (; it != end; ++it) {
        names.push_back((*it)[1].str()); // stem only (without extension)
        names.push_back((*it)[0].str()); // full filename
    }

    // 2. Quoted identifiers: 'QLearningDecision', "phase_3", etc.
    static const std::regex quotedPattern(R"(['"]([^'"]+)['"])");
    std::sregex_iterator it2(q.begin(), q.end(), quotedPattern), end2;
    for (; it2 != end2; ++it2) {
        names.push_back((*it2)[1].str());
    }

    // 3. Common structural keywords paired with a name stem
    // e.g. "fxns inside decision", "functions in agent", "decision file"
    // Match patterns: "in(side)? <word>", "<word> file", "<word> class"
    static const std::regex contextualPattern(
        R"((?:inside|in|within|of|from)\s+([\w_]+)|(?:^|\s)([\w_]+)\s+(?:file|class|module|script|function|method|fxn))");
    std::sregex_iterator it3(q.begin(), q.end(), contextualPattern), end3;
    for (; it3 != end3; ++it3) {
        std::string m1 = (*it3)[1].str();
        std::string m2 = (*it3)[2].str();
        if (!m1.empty()) names.push_back(m1);
        if (!m2.empty()) names.push_back(m2);
    }

    // Deduplicate
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

// Check if a file_path matches any of our extracted name stems
bool fileMatchesAnyName(const std::string& filePath, const std::vector<std::string>& names) {
    // Get just the filename without directory
    std::string fname = filePath;
    size_t slash = fname.rfind('/');
    if (slash != std::string::npos) fname = fname.substr(slash + 1);

    // Lowercase
    std::string fnameLower = fname;
    std::transform(fnameLower.begin(), fnameLower.end(), fnameLower.begin(),
                   [](unsigned char c){ return std::tolower(c); });

    // Get stem (without extension)
    std::string stem = fnameLower;
    size_t dot = stem.rfind('.');
    if (dot != std::string::npos) stem = stem.substr(0, dot);

    for (const auto& name : names) {
        if (fnameLower == name || stem == name || fnameLower.find(name) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

AskEngine::AskEngine(IStorage& storage, VectorIndex& vectors, ILLMClient& llm,
                     const std::string& repoRoot, SessionManager* sessionManager)
    : storage_(storage), vectors_(vectors), llm_(llm), repoRoot_(repoRoot), sessionManager_(sessionManager) {}

// ── Step 1: Instant Fetch ────────────────────────────────────────────────
std::string AskEngine::fetchRepoSummary() {
    auto node = storage_.getNode("[GLOBAL:REPO]");
    if (!node) return "";
    // Explicit failure sentinel from the async global rollup: signal the
    // caller to retry rather than embedding garbage context into HyDE.
    if (node->ai_summary.rfind("[ROLLUP_FAILED:", 0) == 0) {
        return "";  // treated as "no context available, proceed without"
    }
    return node->ai_summary;
}

// ── Step 2: Subsystem HyDE Generation (zero-shot) ───────────────────────
std::string AskEngine::generateHypothetical(const std::string& repoSummary,
                                             const std::string& query,
                                             const std::string& language) {
    // 1. Fetch subsystem summaries for the HyDE to reference
    std::vector<std::string> subsystemSummaries;
    auto allNodes = storage_.queryNodesByPathPrefix("[CONTEXT:DIR:");
    
    std::string subsystems = "";
    for (const auto& n : allNodes) {
        if (n.id.rfind("[CONTEXT:DIR:", 0) == 0) {
            subsystems += n.ai_summary + "\n\n";
        }
    }

    // 2. Language-aware prompt (interpolated language string, never hardcoded)
    std::string langInstruction;
    if (language == "cpp") {
        langInstruction = "Write the explanation as a C++ code snippet and technical description using the same style as this repository (classes, methods, headers).";
    } else if (language == "python") {
        langInstruction = "Write the explanation as a Python code snippet and technical description using the same style as this repository (classes, methods, decorators).";
    } else if (language == "typescript" || language == "javascript") {
        langInstruction = "Write the explanation as a TypeScript/JavaScript code snippet and technical description using the same style as this repository (modules, classes, functions).";
    } else if (language == "rust") {
        langInstruction = "Write the explanation as a Rust code snippet and technical description using the same style as this repository (traits, structs, impl blocks).";
    } else if (language == "go") {
        langInstruction = "Write the explanation as a Go code snippet and technical description using the same style as this repository (structs, interfaces, packages).";
    } else {
        langInstruction = "Use the SAME PROGRAMMING LANGUAGE and terminology as this repository.";
    }

    std::string prompt =
        "You are a codebase architect writing a Hypothetical Document Embedding (HyDE) "
        "for a targeted code search.\n\n"
        "You have been given the master architectural summary AND subsystem summaries "
        "of a repository. Based ONLY on the architectural context provided, write a "
        "hypothetical, highly technical explanation that would answer the query if it "
        "existed in this codebase.\n"
        + langInstruction + "\n"
        "CRITICAL: Be as specific and concrete as possible — mention exact file paths, "
        "class names (from the SYMBOL MANIFEST), and function signatures.\n\n"
        "REPOSITORY LANGUAGE: " + language + "\n\n"
        "REPOSITORY ARCHITECTURE:\n" + repoSummary + "\n\n"
        "SUBSYSTEM DETAILS:\n" + subsystems + "\n\n"
        "USER QUERY:\n" + query + "\n\n"
        "Write the hypothetical technical response below (Keep it short and dense):\n";

    std::string hypothetical = llm_.complete(prompt, "Write the hypothetical explanation.", 200);
    return hypothetical;
}

// ── Step 3: Targeted Vector Search (Hybrid: HyDE + raw query + FTS5 + RRF) ─
std::vector<SeedMatch> AskEngine::targetedSearch(const std::string& hypothetical,
                                                 const std::string& query, int topK) {
    // Dense channel: anchor the dense vector back to the user's literal
    // tokens by concatenating the hypothetical with the raw query, correcting
    // for HyDE hallucination drift (see project audit "Silver Bullet" list).
    std::string combined = query + " " + hypothetical;
    std::vector<float> hypoVec = embedText(combined);

    std::vector<SeedMatch> denseHits;
    if (!hypoVec.empty() && hypoVec.size() >= static_cast<size_t>(VectorIndex::kDim)) {
        std::vector<float> kDimVec(hypoVec.begin(), hypoVec.begin() + VectorIndex::kDim);
        denseHits = vectors_.search(kDimVec, topK * 2);
    }
    for (size_t i = 0; i < denseHits.size() && i < 10; ++i) {
        auto n = storage_.getNode(denseHits[i].nodeId);
        std::cerr << "[HyDE] Dense hit #" << i << ": " << denseHits[i].nodeId
                  << " score=" << denseHits[i].score;
        if (n) std::cerr << " (" << n->file_path << " :: " << n->signature << ")";
        std::cerr << "\n";
    }

    // Sparse channel: exact-token FTS5 match on query symbols (Priority 4).
    auto ftsIds = storage_.ftsSearch(query, topK * 2);
    std::cerr << "[HyDE] FTS5 returned " << ftsIds.size() << " ids for query: " << query << "\n";
    std::vector<SeedMatch> sparseHits;
    for (const auto& id : ftsIds) {
        auto node = storage_.getNode(id);
        if (node && node->is_active && node->kind != "context") {
            sparseHits.push_back({id, 1.0f});
            std::cerr << "[HyDE] FTS5 hit: " << id << " (" << node->file_path << " :: " << node->signature << ")\n";
        }
    }
    std::cerr << "[HyDE] Dense hits: " << denseHits.size() << ", FTS5 sparse hits: " << sparseHits.size() << "\n";

    if (denseHits.empty() && sparseHits.empty()) return {};

    // RRF fusion: score(d) = Σ 1/(60 + rank_m(d)) across both ranked lists.
    RankList denseRank, sparseRank;
    for (size_t i = 0; i < denseHits.size(); ++i)
        denseRank.emplace_back(denseHits[i].nodeId, denseHits[i].score);
    for (size_t i = 0; i < sparseHits.size(); ++i)
        sparseRank.emplace_back(sparseHits[i].nodeId, sparseHits[i].score);

    auto fused = chronos::blendRRF({denseRank, sparseRank}, 60.0);
    if (static_cast<int>(fused.size()) > topK) fused.resize(topK);

    std::vector<SeedMatch> result;
    for (const auto& [id, score] : fused) {
        result.push_back({id, static_cast<float>(score)});
    }
    return result;
}

// ── Step 3b: Structural Graph Expansion ──────────────────────────────────────────────────────────
// When the query explicitly mentions a file or symbol by name, we bypass
// the semantic similarity penalty by directly traversing the Codex graph
// and injecting all child nodes (function/method/class definitions) as
// top-priority seeds. These are guaranteed to be directly relevant.
//
// Also implements "FTS5 Frequency Boost": if ANY file appears 3+ times
// across the FTS5 hit list it is a strong implicit anchor even when not
// named explicitly in the query (e.g. "phase 3 learning persistence" →
// e2e_learning_flow.py appears 3× in FTS5 → implicit anchor).
std::vector<SeedMatch> AskEngine::structuralExpand(const std::string& query,
                                                    const std::vector<SeedMatch>& rrfSeeds) {
    // 1. Extract the set of file/symbol names mentioned in the query
    auto mentionedNames = extractMentionedNames(query);

    std::unordered_set<std::string> anchorIds;

    // ── Explicit name matching ──────────────────────────────────────────
    if (!mentionedNames.empty()) {
        std::cerr << "[Structural] Detected " << mentionedNames.size() << " name mention(s) in query: ";
        for (const auto& n : mentionedNames) std::cerr << "'" << n << "' ";
        std::cerr << "\n";

        // 2a. Check existing RRF seeds — if a seed's file matches, it's an anchor
        for (const auto& sm : rrfSeeds) {
            auto node = storage_.getNode(sm.nodeId);
            if (!node || !node->is_active) continue;
            if (fileMatchesAnyName(node->file_path, mentionedNames)) {
                anchorIds.insert(sm.nodeId);
            }
        }

        // 2b. Scan all nodes in the graph for matching file paths
        auto allByPath = storage_.queryNodesByPathPrefix("");
        for (const auto& node : allByPath) {
            if (!node.is_active || node.kind == "context") continue;
            if (fileMatchesAnyName(node.file_path, mentionedNames)) {
                anchorIds.insert(node.id);
            }
        }
    }

    // ── FTS5 Frequency Boost ────────────────────────────────────────────
    // Count how many times each file_path appears in the RAW FTS5 hit list
    // (before RRF deduplication). RRF merging collapses multi-chunk files
    // into fewer entries, so we must re-run FTS5 directly here to get the
    // true per-file frequency. 3+ raw hits = strong implicit anchor.
    // (e.g. "phase 3 learning" → e2e_learning_flow.py has 3 raw FTS5 hits
    //  but only 1-2 entries survive in the 25-node RRF merged list)
    {
        auto rawFtsIds = storage_.ftsSearch(query, 200); // large limit, no RRF dedup
        std::unordered_map<std::string, int> fileHitCount;
        for (const auto& id : rawFtsIds) {
            auto node = storage_.getNode(id);
            if (!node || !node->is_active || node->kind == "context") continue;
            fileHitCount[node->file_path]++;
        }
        for (const auto& [filePath, count] : fileHitCount) {
            if (count >= 3) {
                std::cerr << "[Structural] FTS5 frequency anchor: '" << filePath
                          << "' appeared " << count << "x in raw FTS5.\n";
                // Promote ALL AST nodes from this file to anchor status
                auto allByPath = storage_.queryNodesByPathPrefix("");
                for (const auto& node : allByPath) {
                    if (!node.is_active || node.kind == "context") continue;
                    if (node.file_path == filePath) {
                        anchorIds.insert(node.id);
                    }
                }
            }
        }
    }

    if (anchorIds.empty()) {
        // No structural signals at all — return seeds unchanged
        return rrfSeeds;
    }

    std::cerr << "[Structural] Found " << anchorIds.size() << " anchor node(s). Expanding via PPR...\n";

    // 3. Run Local-Push PPR from all anchor nodes to find structurally related
    //    nodes (callers, callees, class members). Budget = 200.
    std::vector<std::string> anchorVec(anchorIds.begin(), anchorIds.end());
    auto pprResult = storage_.localPushPPR(anchorVec, 200, 0.85);

    // 4. Build a merged ranked list:
    //    - Anchor nodes get maximum boost (10×)
    //    - PPR-expanded nodes get rank-weighted boost (5×/rank)
    //    - Existing RRF seeds keep their original scores
    std::unordered_map<std::string, double> merged;

    for (size_t i = 0; i < pprResult.nodes.size(); ++i) {
        const auto& n = pprResult.nodes[i];
        if (!n.is_active || n.kind == "context") continue;
        double rankScore = kStructuralBoost * (1.0 / (1.0 + static_cast<double>(i)));
        auto it = merged.find(n.id);
        if (it == merged.end() || it->second < rankScore) {
            merged[n.id] = rankScore;
        }
    }

    for (const auto& id : anchorIds) {
        merged[id] = kStructuralBoost * 2.0; // highest priority
    }

    for (const auto& sm : rrfSeeds) {
        auto it = merged.find(sm.nodeId);
        if (it == merged.end()) {
            merged[sm.nodeId] = static_cast<double>(sm.score);
        }
    }

    // 5. Sort by score descending, cap at kHyDETopK
    std::vector<std::pair<std::string, double>> sorted(merged.begin(), merged.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    if (static_cast<int>(sorted.size()) > kHyDETopK) sorted.resize(kHyDETopK);

    std::cerr << "[Structural] Expanded seed list: " << sorted.size() << " nodes (structural nodes boosted).\n";
    for (size_t i = 0; i < sorted.size() && i < 5; ++i) {
        auto node = storage_.getNode(sorted[i].first);
        if (node) {
            std::cerr << "[Structural] Seed #" << i << ": " << node->file_path
                      << " :: " << node->signature << " (score=" << sorted[i].second << ")\n";
        }
    }

    std::vector<SeedMatch> result;
    result.reserve(sorted.size());
    for (const auto& [id, score] : sorted) {
        result.push_back({id, static_cast<float>(score)});
    }
    return result;
}

// ── Step 4: Final Synthesis ─────────────────────────────────────────────
ChronosRequest AskEngine::synthesize(const std::vector<SeedMatch>& seeds,
                                      const std::string& query,
                                      const std::string& traceId,
                                      const std::string& sessionId) {
    ChronosRequest req;
    req.traceId = traceId;
    req.userQuery = query;
    req.requireCitations = true;

    TraceResult trace;
    std::unordered_set<std::string> addedIds;
    std::unordered_set<std::string> addedDirs;

    // Large snippet budget — each node can show up to 8000 chars of actual code
    const size_t MAX_PER_SNIPPET = 8000;

    for (const auto& sm : seeds) {
        auto node = storage_.getNode(sm.nodeId);
        if (!node || !node->is_active) continue;
        if (addedIds.count(node->id)) continue;
        addedIds.insert(node->id);

        ContextNode cn;
        cn.nodeId = node->id;
        cn.filePath = node->file_path;

        if (sessionManager_ && !sessionId.empty() && sessionManager_->hasNode(sessionId, node->id)) {
            // Bucket B: Reused Nodes
            cn.codeSnippet = "[node:" + node->id + "] " + node->file_path + "::" + node->signature + " (Already in your memory. Refer to previous messages for its source code.)";
            sessionManager_->markNode(sessionId, node->id);
        } else {
            // Bucket A: New Nodes
            if (addedDirs.insert("hdr:" + node->file_path).second) {
                std::string path = repoRoot_ + "/" + node->file_path;
                std::ifstream in(path, std::ios::binary);
                if (in) {
                    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                    std::string headerStr = content.substr(0, std::min<size_t>(content.size(), 2000));
                    if (headerStr.size() == 2000) headerStr += "\n... (file body truncated)";
                    
                    ContextNode fHeader;
                    fHeader.nodeId = node->id;
                    fHeader.filePath = node->file_path;
                    fHeader.codeSnippet = "[FILE HEADER/DOCSTRING]\n" + headerStr;
                    fHeader.uncertain = false;
                    req.context.push_back(std::move(fHeader));
                }
            }

            cn.codeSnippet = readSnippetForNode(*node, repoRoot_);
            if (cn.codeSnippet.size() > MAX_PER_SNIPPET) {
                cn.codeSnippet = cn.codeSnippet.substr(0, MAX_PER_SNIPPET) + "\n... (truncated)";
            }
            if (sessionManager_ && !sessionId.empty()) sessionManager_->markNode(sessionId, node->id);
        }

        
        cn.uncertain = node->parse_confidence < 0.5f;
        req.context.push_back(std::move(cn));
        trace.nodes.push_back(*node);
    }

    // Re-use addedDirs for subsystem summaries
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

    // Enforce 32,000 char (approx 8,000 tokens) limit
    size_t totalChars = 0;
    std::vector<ContextNode> kept;
    for (auto& cn : req.context) {
        size_t available = 32000 - std::min<size_t>(totalChars, 32000);
        if (available == 0) break;
        if (cn.codeSnippet.size() > available) {
            cn.codeSnippet = cn.codeSnippet.substr(0, available);
        }
        totalChars += cn.codeSnippet.size();
        kept.push_back(std::move(cn));
    }
    req.context = std::move(kept);

    return req;
}

// ── Public entry point ──────────────────────────────────────────────────
AskResult AskEngine::run(const std::string& query, const std::string& sessionId) {
    AskResult result;
    result.usedHyDE = false;

    std::string traceId = makeTraceId();
    std::string activeQuery = query;

    if (sessionManager_ && !sessionId.empty()) {
        sessionManager_->incrementTurn(sessionId);
        sessionManager_->gc(sessionId); // Evict Bucket C (unused for 3 turns)

        auto history = sessionManager_->getHistory(sessionId);
        if (!history.empty()) {
            std::cout << "[Chat] Rewriting query using conversation history...\n";
            std::string historyStr = "--- CONVERSATION HISTORY ---\n";
            for (const auto& msg : history) {
                historyStr += msg.role + ": " + msg.content + "\n";
            }
            std::string prompt = "You are a query rewriting tool. Given the following conversation history and the user's latest follow-up question, rewrite the follow-up question so it is a standalone query that can be understood without the history. Resolve pronouns like 'it' or 'this' to their actual class or function names. DO NOT answer the query. ONLY output the rewritten query.\n\n"
                                 + historyStr + "\n--- LATEST QUESTION ---\n" + query + "\n\nREWRITTEN QUERY:\n";
            std::string rewritten = llm_.complete(prompt, "Rewrite the query.", 150);
            if (!rewritten.empty() && rewritten.find("[API Error]") == std::string::npos) {
                activeQuery = rewritten;
                std::cout << "[Chat] Rewrote query to: " << activeQuery << "\n";
            }
        }
    }

    // Step 1: Subsystem pre-fetch (BEFORE HyDE per architecture order)
    std::string repoSummary = fetchRepoSummary();
    if (!repoSummary.empty()) {
        std::cout << "[HyDE] Fetched [GLOBAL:REPO] context (" << repoSummary.size() << " chars).\n";
    } else {
        std::cout << "[HyDE] No [GLOBAL:REPO] found — proceeding without architectural context.\n";
    }

    // Detect dominant language from repo metadata (Priority 1)
    std::string language = "python";  // safe default
    auto langMeta = storage_.getRepoMetadata("dominant_language");
    if (langMeta) language = *langMeta;

    // Step 2: Generate hypothetical (language-aware, fed by pre-fetched context)
    std::string hypothetical;
    if (llm_.isAvailable()) {
        std::cout << "[HyDE] Generating " << language << "-aware hypothetical response...\n";
        hypothetical = generateHypothetical(repoSummary, query, language);
    }

    std::vector<SeedMatch> seeds;
    if (!hypothetical.empty()) {
        result.usedHyDE = true;
        std::cout << "[HyDE] Targeted vector search with " << hypothetical.size() << "-char hypothetical.\n";

        // Step 3: Targeted search (HyDE + raw query hybrid)
        seeds = targetedSearch(hypothetical, query, kHyDETopK);
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

    // Step 3b: Structural Graph Expansion — boost nodes from explicitly named
    // files/symbols so they always appear at the top of the context window,
    // overriding the semantic similarity penalty that causes documentation to
    // outrank actual code.
    seeds = structuralExpand(query, seeds);

    // Step 4: Final synthesis payload
    auto req = synthesize(seeds, activeQuery, traceId, sessionId);

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
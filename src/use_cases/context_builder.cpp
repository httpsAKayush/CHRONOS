#include "chronos/use_cases/context_builder.hpp"
#include "chronos/domain/mmr.hpp"
#include "chronos/domain/rrf.hpp"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <iostream>

namespace fs = std::filesystem;

namespace chronos {

namespace {

std::string makeTraceId() {
    static std::mt19937_64 rng{std::random_device{}()};
    std::ostringstream out;
    out << std::hex << rng();
    return out.str();
}

// For `chronos ask` (legacy): 4k tokens / 16k chars
constexpr size_t kMaxPayloadTokens = 4000;
constexpr size_t kMaxPayloadChars  = kMaxPayloadTokens * 4;

// For `chronos explain`: generous 8k tokens / 32k chars — we are reading
// specific known files, not doing open-ended RAG, so the context is always
// purposeful.
constexpr size_t kExplainMaxPayloadChars = 32000;

// Per-snippet cap for explain mode: one function can be at most 10k chars
constexpr size_t kExplainSnippetCap = 10000;

void enforceTokenBudget(ChronosRequest& req) {
    size_t totalChars = 0;
    std::vector<ContextNode> kept;
    kept.reserve(req.context.size());
    for (auto& cn : req.context) {
        size_t available = kMaxPayloadChars - std::min(totalChars, kMaxPayloadChars);
        if (available == 0) break;
        if (cn.codeSnippet.size() > available) {
            cn.codeSnippet = cn.codeSnippet.substr(0, available);
        }
        totalChars += cn.codeSnippet.size();
        kept.push_back(std::move(cn));
    }
    req.context = std::move(kept);
}

void enforceExplainBudget(ChronosRequest& req) {
    size_t totalChars = 0;
    std::vector<ContextNode> kept;
    kept.reserve(req.context.size());
    for (auto& cn : req.context) {
        size_t available = kExplainMaxPayloadChars - std::min(totalChars, kExplainMaxPayloadChars);
        if (available == 0) break;
        if (cn.codeSnippet.size() > available) {
            cn.codeSnippet = cn.codeSnippet.substr(0, available);
        }
        totalChars += cn.codeSnippet.size();
        kept.push_back(std::move(cn));
    }
    req.context = std::move(kept);
}

// ── Target Descriptor ────────────────────────────────────────────────────
// Parses the user-supplied target string into its components.
// Supported formats:
//   agent/decision.py               → file only
//   agent/decision.py::learn        → file + specific function/method
//   agent/decision.py::QLearningDecision  → file + class (includes all methods)
//   QLearningDecision               → bare symbol (class or function name)
//   learn                           → bare function name
struct ExplainTarget {
    std::string filePath;   // relative to repoRoot, e.g. "agent/decision.py"
    std::string symbolName; // empty = "whole file", otherwise a class or fn name
    bool isFilePath = false;
};

ExplainTarget parseTarget(const std::string& raw) {
    ExplainTarget t;
    // Check for file::symbol notation
    auto sep = raw.find("::");
    if (sep != std::string::npos) {
        t.filePath   = raw.substr(0, sep);
        t.symbolName = raw.substr(sep + 2);
        t.isFilePath = true;
        return t;
    }
    // Check if it looks like a file path (has / or ends with known extension)
    bool hasSlash = raw.find('/') != std::string::npos;
    bool hasExt   = (raw.size() > 3 &&
                     (raw.substr(raw.size()-3) == ".py"  ||
                      raw.substr(raw.size()-4) == ".cpp" ||
                      raw.substr(raw.size()-4) == ".hpp" ||
                      raw.substr(raw.size()-3) == ".js"  ||
                      raw.substr(raw.size()-3) == ".ts"  ||
                      raw.substr(raw.size()-3) == ".go"  ||
                      raw.substr(raw.size()-3) == ".rs"  ));
    if (hasSlash || hasExt) {
        t.filePath   = raw;
        t.symbolName = "";
        t.isFilePath = true;
    } else {
        // Bare symbol name
        t.symbolName = raw;
        t.isFilePath = false;
    }
    return t;
}

} // namespace

ContextBuilder::ContextBuilder(Codex& codex, VectorIndex& vectors, std::string repoRoot)
    : codex_(codex), vectors_(vectors), repoRoot_(std::move(repoRoot)) {}

std::string ContextBuilder::readLiveSnippet(const Node& n) const {
    if (n.byte_end <= n.byte_start) return "";
    auto tryRead = [&](const std::string& path) -> std::string {
        std::ifstream in(path, std::ios::binary);
        if (!in) return "";
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        int64_t start = std::max<int64_t>(0, n.byte_start);
        int64_t end   = std::min<int64_t>(static_cast<int64_t>(content.size()), n.byte_end);
        if (end <= start) return "";
        return content.substr(start, end - start);
    };

    std::string primary = (fs::path(repoRoot_) / n.file_path).string();
    std::string snippet = tryRead(primary);
    if (!snippet.empty()) return snippet;

    std::string storedFilename = fs::path(n.file_path).filename().string();
    std::string storedRelDir   = fs::path(n.file_path).parent_path().filename().string();
    try {
        for (auto& entry : fs::recursive_directory_iterator(repoRoot_)) {
            if (!entry.is_regular_file()) continue;
            auto p = entry.path();
            if (p.filename() != storedFilename) continue;
            if (p.parent_path().filename() != storedRelDir) continue;
            snippet = tryRead(p.string());
            if (!snippet.empty()) return snippet;
        }
    } catch (...) {}
    return "";
}

// Read the ENTIRE file, used for the mandatory file header in explain mode
static std::string readWholeFile(const std::string& repoRoot, const std::string& relPath) {
    std::string full = (fs::path(repoRoot) / relPath).string();
    std::ifstream in(full, std::ios::binary);
    if (!in) return "";
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

BuildResult ContextBuilder::build(const std::string& userQuery, int pprBudget,
                                    int contextNodeBudget, float seedConfidenceFloor,
                                    int64_t queryTimestamp, bool skeletonHops) {
    BuildResult result;

    auto queryVec = embedText(userQuery);
    auto seeds = vectors_.search(queryVec, /*topK=*/5, queryTimestamp);
    if (seeds.empty() || seeds.front().score < seedConfidenceFloor) {
        result.ok = false;
        result.reason =
            "I couldn't find matching concepts in the codebase. Are you referring to an external library?";
        return result;
    }

    std::vector<std::string> seedIds;
    for (const auto& s : seeds) seedIds.push_back(s.nodeId);
    auto contextNodes = codex_.queryNodesByPathPrefix("[CONTEXT:DIR:");
    for (const auto& n : contextNodes) {
        if (n.id.rfind("[CONTEXT:DIR:", 0) == 0 || n.id == "[GLOBAL:REPO]") {
            if (n.is_active) seedIds.push_back(n.id);
        }
    }
    TraceResult trace = codex_.localPushPPR(seedIds, pprBudget);
    result.rawTrace = trace;

    if (trace.nodes.empty()) {
        auto fallbackNode = codex_.getNode(seeds.front().nodeId);
        if (fallbackNode) {
            trace.nodes.push_back(*fallbackNode);
        } else {
            result.ok = false;
            result.reason = "Found a semantic match but the node was missing from the codex.";
            return result;
        }
    }

    RankList hop1_ranks;
    std::unordered_map<std::string, double> seedScoreMap;
    for (const auto& s : seeds) {
        hop1_ranks.emplace_back(s.nodeId, s.score);
        seedScoreMap[s.nodeId] = s.score;
    }

    RankList hop2_ranks;
    for (const auto& n : trace.nodes) {
        double pprScore = seedScoreMap.count(n.id) ? seedScoreMap[n.id] : 0.0;
        hop2_ranks.emplace_back(n.id, pprScore);
    }
    std::sort(hop2_ranks.begin(), hop2_ranks.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    RankList fusedRRF = blendRRF({hop1_ranks, hop2_ranks}, 60.0);
    std::unordered_map<std::string, double> fusedScoreMap;
    for (const auto& [id, score] : fusedRRF) fusedScoreMap[id] = score;

    std::vector<MMRCandidate> candidates;
    for (const auto& n : trace.nodes) {
        double relevance = fusedScoreMap.count(n.id) ? fusedScoreMap[n.id] : 0.0;
        candidates.push_back({n.id, relevance});
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) { return a.relevance > b.relevance; });

    std::unordered_map<std::string, std::unordered_set<std::string>> adjacency;
    for (const auto& e : trace.edges) {
        adjacency[e.source_id].insert(e.target_id);
        adjacency[e.target_id].insert(e.source_id);
    }
    auto pruned = connectivityMMR(candidates, adjacency, contextNodeBudget);

    ChronosRequest req;
    req.traceId = makeTraceId();
    req.userQuery = userQuery;
    req.requireCitations = true;

    std::unordered_map<std::string, Node> nodeById;
    for (auto& n : trace.nodes) nodeById[n.id] = n;

    std::unordered_set<std::string> seedIdSet(seedIds.begin(), seedIds.end());

    for (auto& cand : pruned) {
        auto it = nodeById.find(cand.nodeId);
        if (it == nodeById.end()) continue;
        ContextNode cn;
        cn.nodeId    = it->second.id;
        cn.filePath  = it->second.file_path;
        bool isHop   = skeletonHops && !seedIdSet.count(it->second.id);
        if (isHop) {
            cn.codeSnippet = "[SKELETON] " + it->second.ai_summary;
            if (it->second.ai_summary.empty())
                cn.codeSnippet = readLiveSnippet(it->second).substr(0, 200);
        } else {
            cn.codeSnippet = readLiveSnippet(it->second);
        }
        cn.uncertain = it->second.parse_confidence < 0.5f;
        req.context.push_back(std::move(cn));
    }

    enforceTokenBudget(req);
    codex_.recordTrace(req.traceId, trace);

    result.ok = true;
    result.request = std::move(req);
    return result;
}

// ── buildExplain — Full Cross-Linked Explain ──────────────────────────────
//
// Supports these target formats:
//   agent/decision.py                       → whole file
//   agent/decision.py::learn                → specific function
//   agent/decision.py::QLearningDecision    → class + all its methods
//   QLearningDecision                       → bare class name (sym: resolution)
//   learn                                   → bare function name
//
// Context assembly strategy (in priority order):
//   TIER 1 — Primary targets: full code of the requested nodes
//   TIER 2 — CONTAINS children: if target is a class, pull all its methods
//   TIER 3 — CALLS hop-1: for each primary node, follow outgoing CALLS edges
//            and include the callee's snippet ("cross-linked dependencies")
//   TIER 4 — IMPORTS hop-1: include module-level class/function headers
//            from imported files
//   TIER 5 — Repo & subsystem summary: always appended last for architectural
//            grounding without consuming primary budget
//
BuildResult ContextBuilder::buildExplain(std::string targetSymbol,
                                          const std::string& userQuery) {
    BuildResult result;

    ExplainTarget target = parseTarget(targetSymbol);

    // ── Resolve primary node IDs ───────────────────────────────────────────
    std::vector<std::string> primaryIds;    // TIER 1: the thing the user asked about
    std::vector<std::string> childIds;      // TIER 2: children (methods of a class)
    std::string primaryFilePath;            // for whole-file header injection

    if (target.isFilePath && target.symbolName.empty()) {
        // ── Mode A: Whole file ─────────────────────────────────────────────
        primaryFilePath = target.filePath;
        std::cerr << "[Explain] Mode: whole-file '" << target.filePath << "'\n";

        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(codex_.raw(),
            "SELECT id FROM nodes WHERE file_path = ?1 AND is_active = 1 "
            "ORDER BY byte_start;",
            -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, target.filePath.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            primaryIds.emplace_back(
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
        }
        sqlite3_finalize(stmt);

        if (primaryIds.empty()) {
            result.ok = false;
            result.reason = "[Explain] No indexed nodes found for file: " + target.filePath +
                            "\nTry running `chronos sync` first.";
            return result;
        }

    } else if (target.isFilePath && !target.symbolName.empty()) {
        // ── Mode B: file::Symbol ───────────────────────────────────────────
        primaryFilePath = target.filePath;
        std::cerr << "[Explain] Mode: file::symbol '"
                  << target.filePath << "::" << target.symbolName << "'\n";

        // Search by signature LIKE pattern to match both functions and classes
        sqlite3_stmt* stmt;
        // First pass: look for exact symbol name in the signature column
        sqlite3_prepare_v2(codex_.raw(),
            "SELECT id, kind FROM nodes WHERE file_path = ?1 AND is_active = 1 "
            "AND (signature LIKE ?2 OR id IN "
            "  (SELECT root_id FROM alias WHERE old_id = ?3));",
            -1, &stmt, nullptr);
        std::string likePattern = "%" + target.symbolName + "%";
        std::string symId       = "sym:" + target.symbolName;
        sqlite3_bind_text(stmt, 1, target.filePath.c_str(),   -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, likePattern.c_str(),        -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, symId.c_str(),              -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            primaryIds.push_back(id);
        }
        sqlite3_finalize(stmt);

        if (primaryIds.empty()) {
            // Fallback: FTS5 search scoped to the file
            auto allFts = codex_.ftsSearch(target.symbolName, 20);
            for (const auto& id : allFts) {
                auto node = codex_.getNode(id);
                if (node && node->is_active && node->file_path == target.filePath) {
                    primaryIds.push_back(id);
                }
            }
        }

        if (primaryIds.empty()) {
            result.ok = false;
            result.reason = "[Explain] Symbol '" + target.symbolName +
                            "' not found in file '" + target.filePath +
                            "'.\nTip: run `chronos map " + target.filePath +
                            "` to see what's indexed.";
            return result;
        }

        // TIER 2: For each found node, follow CONTAINS outgoing edges
        // → this pulls all methods of a class when the user specifies a class name
        std::cerr << "[Explain] Found " << primaryIds.size() << " primary node(s). "
                  << "Expanding CONTAINS children...\n";
        std::unordered_set<std::string> seen(primaryIds.begin(), primaryIds.end());
        for (const auto& pid : primaryIds) {
            auto edges = codex_.getEdges(pid, /*outgoing=*/true);
            for (const auto& e : edges) {
                if (e.type != "CONTAINS") continue;
                if (seen.count(e.target_id)) continue;
                auto child = codex_.getNode(e.target_id);
                if (!child || !child->is_active) continue;
                // Only include children from the same file
                if (child->file_path != target.filePath) continue;
                seen.insert(e.target_id);
                childIds.push_back(e.target_id);
            }
        }
        std::cerr << "[Explain] Found " << childIds.size() << " child node(s) via CONTAINS.\n";

    } else {
        // ── Mode C: Bare symbol name ───────────────────────────────────────
        std::cerr << "[Explain] Mode: bare symbol '" << target.symbolName << "'\n";
        std::string symKey = target.symbolName;
        if (symKey.find("sym:") != 0) symKey = "sym:" + symKey;

        std::string rootId;
        try {
            rootId = codex_.resolveAlias(symKey);
        } catch (...) {
            // Fallback: FTS5 across entire repo
            auto ftsIds = codex_.ftsSearch(target.symbolName, 10);
            if (!ftsIds.empty()) rootId = ftsIds.front();
        }

        if (rootId.empty()) {
            result.ok = false;
            result.reason = "[Explain] Could not resolve symbol '" + target.symbolName +
                            "'.\nTip: specify a file path like: " +
                            "chronos explain path/to/file.py::" + target.symbolName;
            return result;
        }
        primaryIds.push_back(rootId);

        // Pull CONTAINS children too (in case it's a class)
        auto edges = codex_.getEdges(rootId, true);
        std::unordered_set<std::string> seen{rootId};
        for (const auto& e : edges) {
            if (e.type != "CONTAINS") continue;
            if (seen.count(e.target_id)) continue;
            auto child = codex_.getNode(e.target_id);
            if (!child || !child->is_active) continue;
            seen.insert(e.target_id);
            childIds.push_back(e.target_id);
        }
    }

    // ── TIER 3: CALLS cross-link expansion ────────────────────────────────
    // For each primary + child node, walk both incoming and outgoing CALLS/IMPORTS
    // edges to discover what it depends on AND who depends on it in OTHER files.
    std::vector<std::string> incomingCallerIds;
    std::vector<std::string> outgoingCalleeIds;
    {
        std::unordered_set<std::string> allPrimary(primaryIds.begin(), primaryIds.end());
        for (const auto& id : childIds) allPrimary.insert(id);
        std::unordered_set<std::string> seenCrossLinks(allPrimary);

        for (const auto& pid : allPrimary) {
            // 1. Outgoing edges (what this node calls/imports)
            auto outEdges = codex_.getEdgesFiltered(pid, /*outgoing=*/true);
            for (const auto& e : outEdges) {
                if (e.type != "CALLS" && e.type != "IMPORTS") continue;
                std::string calleeId;
                try { calleeId = codex_.resolveAlias(e.target_id); } catch (...) { continue; }
                if (seenCrossLinks.count(calleeId)) continue;
                auto calleeNode = codex_.getNode(calleeId);
                if (!calleeNode || !calleeNode->is_active) continue;
                bool sameFile = (!primaryFilePath.empty() && calleeNode->file_path == primaryFilePath);
                if (sameFile) continue;
                seenCrossLinks.insert(calleeId);
                outgoingCalleeIds.push_back(calleeId);
            }
            // 2. Incoming edges (who calls/imports this node)
            auto inEdges = codex_.getEdgesFiltered(pid, /*outgoing=*/false);
            for (const auto& e : inEdges) {
                if (e.type != "CALLS" && e.type != "IMPORTS") continue;
                std::string callerId;
                try { callerId = codex_.resolveAlias(e.source_id); } catch (...) { callerId = e.source_id; }
                if (seenCrossLinks.count(callerId)) continue;
                auto callerNode = codex_.getNode(callerId);
                if (!callerNode || !callerNode->is_active) continue;
                bool sameFile = (!primaryFilePath.empty() && callerNode->file_path == primaryFilePath);
                if (sameFile) continue;
                seenCrossLinks.insert(callerId);
                incomingCallerIds.push_back(callerId);
            }
        }
        std::cerr << "[Explain] CALLS/IMPORTS expansion: " << incomingCallerIds.size() << " incoming, " 
                  << outgoingCalleeIds.size() << " outgoing cross-linked node(s).\n";
    }

    // ── TIER 4: IMPORTS — include top-level file headers of imported modules ──
    std::vector<std::string> importedFileHeaders; // file paths, not node IDs
    {
        std::unordered_set<std::string> seenFiles;
        if (!primaryFilePath.empty()) seenFiles.insert(primaryFilePath);

        for (const auto& cid : outgoingCalleeIds) {
            auto node = codex_.getNode(cid);
            if (!node) continue;
            if (seenFiles.count(node->file_path)) continue;
            seenFiles.insert(node->file_path);
            importedFileHeaders.push_back(node->file_path);
        }
        for (const auto& cid : incomingCallerIds) {
            auto node = codex_.getNode(cid);
            if (!node) continue;
            if (seenFiles.count(node->file_path)) continue;
            seenFiles.insert(node->file_path);
            importedFileHeaders.push_back(node->file_path);
        }
    }

    // ── Assemble ChronosRequest ────────────────────────────────────────────
    ChronosRequest req;
    req.traceId        = makeTraceId();
    req.requireCitations = true;

    // Auto-generate a rich query if none provided
    if (userQuery.empty()) {
        if (!target.symbolName.empty()) {
            req.userQuery = "Explain the complete logic, data flow, and cross-file dependencies of '"
                          + target.symbolName + "'. "
                          "Include who calls it, what it calls, what it returns, what state it modifies, "
                          "and how cross-linked functions (shown as [CROSS-LINK]) work together with it.";
        } else {
            req.userQuery = "Explain the architecture, data flow, and responsibilities of every "
                          "function and class in this file. For each function, describe: "
                          "(1) what it does, (2) who calls it, (3) what it calls, (4) what state it reads/writes. "
                          "Then show how it links with external cross-linked functions.";
        }
    } else {
        req.userQuery = "User Query: " + userQuery + "\n\n"
                        "Instructions: Answer the user's query using the provided file content. "
                        "Pay special attention to nodes marked [EXTERNAL CALLER] and [EXTERNAL DEPENDENCY] to understand how the file connects to the rest of the codebase. "
                        "If the user asks how this links to the main flow, find the [EXTERNAL CALLER] nodes that invoke it and trace their logic.";
    }

    TraceResult trace;
    std::unordered_set<std::string> addedNodes;

    // Helper: add a node to context with a section label
    auto addNode = [&](const std::string& nodeId, const std::string& section) {
        if (addedNodes.count(nodeId)) return;
        auto node = codex_.getNode(nodeId);
        if (!node || !node->is_active) return;
        addedNodes.insert(nodeId);

        std::string snippet = readLiveSnippet(*node);
        if (snippet.size() > kExplainSnippetCap)
            snippet = snippet.substr(0, kExplainSnippetCap) + "\n... (truncated)";

        ContextNode cn;
        cn.nodeId      = node->id;
        cn.filePath    = node->file_path;
        cn.codeSnippet = section + "\n" + snippet;
        cn.uncertain   = node->parse_confidence < 0.5f;
        req.context.push_back(std::move(cn));
        trace.nodes.push_back(*node);
    };

    // ── TIER 1: Whole-file header (first 3000 chars) ──────────────────────
    if (!primaryFilePath.empty()) {
        std::string fileContent = readWholeFile(repoRoot_, primaryFilePath);
        if (!fileContent.empty()) {
            std::string header = fileContent.substr(0, std::min<size_t>(fileContent.size(), 3000));
            if (fileContent.size() > 3000) header += "\n... (file continues below)";
            ContextNode hdr;
            hdr.nodeId      = "[FILE_HEADER:" + primaryFilePath + "]";
            hdr.filePath    = primaryFilePath;
            hdr.codeSnippet = "[FILE: " + primaryFilePath + "]\n" + header;
            hdr.uncertain   = false;
            req.context.push_back(std::move(hdr));
        }
    }

    // ── TIER 1: Primary nodes (the thing the user asked about) ────────────
    for (const auto& id : primaryIds) {
        addNode(id, "[PRIMARY]");
    }

    // ── TIER 2: CONTAINS children (class methods, nested functions) ───────
    for (const auto& id : childIds) {
        addNode(id, "[MEMBER]");
    }

    // ── TIER 3: Cross-links (who calls the target vs what the target calls) ───────────
    for (const auto& id : incomingCallerIds) {
        addNode(id, "[EXTERNAL CALLER: This node invokes the target] (" +
                    (codex_.getNode(id) ? codex_.getNode(id)->file_path : "?") + ")");
    }
    for (const auto& id : outgoingCalleeIds) {
        addNode(id, "[EXTERNAL DEPENDENCY: The target invokes this node] (" +
                    (codex_.getNode(id) ? codex_.getNode(id)->file_path : "?") + ")");
    }

    // ── TIER 4: Imported file headers (top-level docstring + class signatures) ─
    for (const auto& filePath : importedFileHeaders) {
        std::string fileContent = readWholeFile(repoRoot_, filePath);
        if (fileContent.empty()) continue;
        // Read only the first 1500 chars (module docstring + class definitions)
        std::string header = fileContent.substr(0, std::min<size_t>(fileContent.size(), 1500));
        ContextNode hdr;
        hdr.nodeId      = "[IMPORT_HDR:" + filePath + "]";
        hdr.filePath    = filePath;
        hdr.codeSnippet = "[IMPORTED MODULE: " + filePath + "]\n" + header;
        hdr.uncertain   = false;
        req.context.push_back(std::move(hdr));
    }

    // ── TIER 5: Repo summary for architectural grounding ──────────────────
    auto repoNode = codex_.getNode("[GLOBAL:REPO]");
    if (repoNode) {
        ContextNode cn;
        cn.nodeId      = repoNode->id;
        cn.filePath    = repoNode->file_path;
        cn.codeSnippet = "[REPOSITORY SUMMARY]\n" + repoNode->ai_summary;
        cn.uncertain   = false;
        req.context.push_back(std::move(cn));
    }

    if (req.context.empty()) {
        result.ok = false;
        result.reason = "[Explain] No code content could be assembled for the requested target.";
        return result;
    }

    enforceExplainBudget(req);
    trace.edges = codex_.getEdgesFiltered(primaryIds.empty() ? "" : primaryIds[0], true);
    codex_.recordTrace(req.traceId, trace);

    result.rawTrace = trace;
    result.ok       = true;
    result.request  = std::move(req);
    return result;
}

} // namespace chronos

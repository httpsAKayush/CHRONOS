#include "chronos/diagnose.hpp"
#include "chronos/ppr.hpp"
#include "chronos/rrf.hpp"
#include "chronos/mmr.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace chronos {

namespace {

// Deferred-work keywords from project_context.md §4.
static const std::vector<std::string> kDeferredWorkKeywords = {
    "temporary", "temp ", "hack", "kludge", "workaround", "will revert",
    "revert later", "will fix", "fix later", "for now", "short-term",
    "band-aid", "bandaid", "TODO", "FIXME", "next sprint", "next release",
    "technical debt", "quick fix", "hotfix"
};

// Common stack trace patterns for several common formats.
// Format 1 (GDB/addr2line): "  #N  0xADDR in FUNC (file:LINE)"
// Format 2 (Google-style):  "  FUNC(args) [file:LINE]"
// Format 3 (simple):        "  at FUNC in FILE:LINE"
// Format 4 (raw filename):  "FILE:LINE: ..."
static std::vector<std::string> extractCandidateNames(const std::string& line) {
    std::vector<std::string> names;

    // Regex: C/C++ function name — word chars, colons, angle brackets
    // We extract "words" that look like identifiers or file paths.
    std::regex identRe(R"((?:^|[\s(,#])([a-zA-Z_][a-zA-Z0-9_:]*(?:::[a-zA-Z_][a-zA-Z0-9_:]*)*))");
    std::regex fileRe(R"(([a-zA-Z0-9_/.\-]+\.(?:cpp|cc|cxx|h|hpp|c|py)))");

    {
        auto begin = std::sregex_iterator(line.begin(), line.end(), identRe);
        auto end   = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            std::string m = (*it)[1].str();
            if (m.size() >= 3) names.push_back(m);
        }
    }
    {
        auto begin = std::sregex_iterator(line.begin(), line.end(), fileRe);
        auto end   = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            names.push_back((*it)[1].str());
        }
    }
    return names;
}

} // namespace

DiagnoseEngine::DiagnoseEngine(Codex& codex, VectorIndex& vectors, const std::string& repoRoot)
    : codex_(codex), vectors_(vectors), repoRoot_(repoRoot) {}

// ---------------------------------------------------------------------------
// CrashLog Parsing
// ---------------------------------------------------------------------------

std::vector<DiagnoseEngine::CrashFrame>
DiagnoseEngine::parseCrashLog(const std::string& log) const {
    std::vector<CrashFrame> frames;
    std::istringstream ss(log);
    std::string line;

    // Patterns for common frame indicators.
    std::regex frameNumRe(R"(^\s*#\d+)");        // GDB: #0 #1 #2 ...
    std::regex atRe(R"(^\s*at\s)");              // Java/Python: "at ..."
    std::regex inRe(R"(\bin\b.*\.(?:cpp|cc|h|hpp|c|py))"); // C/C++ "in file.cpp"
    std::regex fileLineRe(R"(([a-zA-Z0-9_/.\-]+\.(?:cpp|cc|cxx|h|hpp|c|py)):(\d+))");

    while (std::getline(ss, line)) {
        std::string lowerLine = line;
        std::transform(lowerLine.begin(), lowerLine.end(), lowerLine.begin(), ::tolower);

        bool isFrame = std::regex_search(line, frameNumRe)
                    || std::regex_search(line, atRe)
                    || std::regex_search(line, inRe)
                    || lowerLine.find("file \"") != std::string::npos
                    || lowerLine.find("error") != std::string::npos
                    || lowerLine.find("exception") != std::string::npos
                    || lowerLine.find("traceback") != std::string::npos
                    || lowerLine.find("fail") != std::string::npos
                    || lowerLine.find("crash") != std::string::npos;

        if (!isFrame) continue;

        CrashFrame f;
        f.rawLine = line;

        // Extract file + line number.
        std::smatch m;
        if (std::regex_search(line, m, fileLineRe)) {
            f.fileName = m[1].str();
            try { f.lineNumber = std::stoi(m[2].str()); } catch (...) {}
        } else {
            // Check for python style: File "path", line X
            std::regex pyRe(R"re([Ff]ile\s+"([^"]+)",\s+line\s+(\d+))re");
            if (std::regex_search(line, m, pyRe)) {
                f.fileName = m[1].str();
                try { f.lineNumber = std::stoi(m[2].str()); } catch (...) {}
            }
        }

        // Extract function/identifier names.
        auto names = extractCandidateNames(line);
        if (!names.empty()) {
            f.functionName = names.front(); // primary guess
        }

        frames.push_back(std::move(f));
    }

    return frames;
}

// ---------------------------------------------------------------------------
// Frame → Codex Node Resolution
// ---------------------------------------------------------------------------

std::vector<std::string>
DiagnoseEngine::resolveFrame(const CrashFrame& frame) const {
    std::vector<std::string> nodeIds;
    std::unordered_set<std::string> seen;

    auto addIfNew = [&](const std::string& id) {
        if (!id.empty() && !seen.count(id)) {
            seen.insert(id);
            nodeIds.push_back(id);
        }
    };

    // Strategy 1: match by file path prefix in Codex nodes.
    if (!frame.fileName.empty()) {
        auto history = codex_.getHistoryForFile(frame.fileName);
        for (auto& rec : history) {
            addIfNew(rec.nodeId);
        }
        // Also try basename match.
        std::string basename = fs::path(frame.fileName).filename().string();
        if (basename != frame.fileName) {
            auto h2 = codex_.getHistoryForFile(basename);
            for (auto& rec : h2) addIfNew(rec.nodeId);
        }
    }

    // Strategy 2: vector semantic search on function name + file name.
    if (!frame.functionName.empty()) {
        std::string query = frame.functionName;
        if (!frame.fileName.empty()) query += " " + frame.fileName;
        auto vec = embedText(query);
        auto seeds = vectors_.search(vec, /*topK=*/5);
        for (auto& s : seeds) addIfNew(s.nodeId);
    }

    return nodeIds;
}

// ---------------------------------------------------------------------------
// Dependency Path (BFS)
// ---------------------------------------------------------------------------

std::string DiagnoseEngine::buildDependencyPath(
        const std::string& crashNodeId,
        const std::string& targetNodeId,
        const std::vector<Edge>& edges) const {

    if (crashNodeId == targetNodeId) return crashNodeId;

    // Build adjacency (outward + inward for richer path-finding).
    std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> adj;
    for (auto& e : edges) {
        if (e.type == "external_symbol" || e.target_id.rfind("sym:", 0) == 0) continue;
        adj[e.source_id].push_back({e.target_id, e.type});
        adj[e.target_id].push_back({e.source_id, e.type}); // bidirectional search
    }

    // BFS.
    std::unordered_map<std::string, std::string> parent;
    std::unordered_map<std::string, std::string> edgeLabel;
    std::vector<std::string> queue;
    queue.push_back(crashNodeId);
    parent[crashNodeId] = "";

    bool found = false;
    for (size_t qi = 0; qi < queue.size() && !found; ++qi) {
        std::string cur = queue[qi];
        if (adj.count(cur)) {
            for (auto& [nxt, label] : adj[cur]) {
                if (!parent.count(nxt)) {
                    parent[nxt] = cur;
                    edgeLabel[nxt] = label;
                    if (nxt == targetNodeId) { found = true; break; }
                    queue.push_back(nxt);
                }
            }
        }
    }

    if (!found) {
        // Return abbreviated path.
        return crashNodeId + " → [structural hop] → " + targetNodeId;
    }

    // Reconstruct path.
    std::vector<std::string> path;
    std::string cur = targetNodeId;
    while (!cur.empty()) {
        path.push_back(cur);
        cur = parent[cur];
    }
    std::reverse(path.begin(), path.end());

    std::ostringstream out;
    for (size_t i = 0; i < path.size(); ++i) {
        if (i > 0) {
            std::string lbl = edgeLabel.count(path[i]) ? edgeLabel[path[i]] : "→";
            out << " -[" << lbl << "]-> ";
        }
        // Show just a short id for readability.
        out << path[i].substr(0, 8) << "...";
    }
    return out.str();
}

// ---------------------------------------------------------------------------
// Deferred-Work Detection
// ---------------------------------------------------------------------------

bool DiagnoseEngine::detectDeferredWork(const std::string& commitMessage) {
    std::string lower = commitMessage;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    for (auto& kw : kDeferredWorkKeywords) {
        std::string kwLower = kw;
        std::transform(kwLower.begin(), kwLower.end(), kwLower.begin(), ::tolower);
        if (lower.find(kwLower) != std::string::npos) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Composite Score
// ---------------------------------------------------------------------------

double DiagnoseEngine::scoreCandidate(double semanticScore,
                                       int64_t commitTimestamp,
                                       int mutationScore,
                                       bool hasDeferredWork) const {
    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    double deltaT = (commitTimestamp > 0 && now > commitTimestamp)
                  ? static_cast<double>(now - commitTimestamp)
                  : 0.0;
    constexpr double kLambda = 3e-7; // ~3-month half-life
    double recency = std::exp(-kLambda * deltaT);

    // Mutation weight (capped at 1.0 for scoring purposes).
    double mutWeight = std::min(1.0, static_cast<double>(mutationScore) / 10.0);

    // Temporal Score is recency plus mutation magnitude, boosted by deferred-work
    double temporalScore = recency + mutWeight;
    if (hasDeferredWork) {
        temporalScore *= 1.5;
    }

    // Step 2: Implement the Semantic Floor
    if (semanticScore < 0.15) {
        temporalScore = 1.0; 
    }

    // Multiplicative Scoring
    double score = semanticScore * temporalScore;
    return std::min(1.0, score);
}

// ---------------------------------------------------------------------------
// Fetch verbatim commit message from git
// ---------------------------------------------------------------------------

std::string DiagnoseEngine::fetchCommitMessage(const std::string& commitHash) const {
    if (commitHash.empty()) return "";
    std::string cmd = "git -C \"" + repoRoot_ + "\" log --format=%B -n 1 " + commitHash + " 2>/dev/null";
    char buf[2048];
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    while (fgets(buf, sizeof(buf), pipe)) result += buf;
    pclose(pipe);
    // Trim trailing newlines.
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result;
}

// ---------------------------------------------------------------------------
// Main Diagnose Entry Point
// ---------------------------------------------------------------------------

DiagnoseResult DiagnoseEngine::diagnose(const std::string& crashLogText, int topN) const {
    DiagnoseResult result;

    // Step 1: Parse the crash log.
    auto frames = parseCrashLog(crashLogText);
    if (frames.empty()) {
        result.ok = false;
        result.reason = "Could not identify any stack frames in the provided log. "
                        "Try including a full crash trace (GDB, asan, or Python traceback format).";
        return result;
    }

    // Summarize which frames were resolved (for user display).
    std::ostringstream frameSummary;
    frameSummary << "Parsed " << frames.size() << " frame(s):";
    for (auto& f : frames) {
        frameSummary << "\n  [frame] " << f.rawLine.substr(0, 80);
        if (f.rawLine.size() > 80) frameSummary << "...";
    }
    result.crashSummary = frameSummary.str();

    // Step 2: Resolve frames to Codex node IDs.
    std::vector<std::string> crashNodeIds;
    std::unordered_set<std::string> seenIds;
    for (auto& frame : frames) {
        auto ids = resolveFrame(frame);
        for (auto& id : ids) {
            if (!seenIds.count(id)) {
                seenIds.insert(id);
                crashNodeIds.push_back(id);
            }
        }
    }

    if (crashNodeIds.empty()) {
        result.ok = false;
        result.reason = "Could not resolve any crash frames to indexed Codex nodes. "
                        "Run `chronos sync` on the repository first.";
        return result;
    }

    // Step 3: Dual-Horizon PPR from crash seeds — expand outward through
    // structural dependencies. The bugs that cause crashes are often NOT in the
    // crashing function itself but in what it calls (or what recently changed
    // in its call graph).
    PPREngine ppr(codex_.raw());
    auto pprScores = ppr.dualHorizonPush(crashNodeIds, /*nodeBudget=*/200);

    // Step 4: Collect candidates — for every node touched by PPR, look up its
    // commit history and rank each commit.
    std::unordered_map<std::string, DiagnosisCandidate> bestPerCommit;

    // Build edge list for path construction.
    // We need the full edge list from PPR-touched nodes. Query Codex for edges
    // among the touched set.
    std::vector<std::string> touchedNodeIds;
    for (auto& [id, _] : pprScores.nodeIdToScore) {
        touchedNodeIds.push_back(id);
    }

    // Collect all edges from Codex involving touched nodes (for path display).
    std::vector<Edge> allEdges;
    {
        sqlite3* db = codex_.raw();
        std::string inClause;
        for (size_t i = 0; i < touchedNodeIds.size(); ++i) {
            if (i) inClause += ",";
            inClause += "'" + touchedNodeIds[i] + "'";
        }
        if (!inClause.empty()) {
            std::string sql = "SELECT source_id, target_id, type, probable_target_weight "
                              "FROM edges WHERE source_id IN (" + inClause + ") "
                              "OR target_id IN (" + inClause + ");";
            sqlite3_stmt* stmt;
            if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    Edge e;
                    auto col = [&](int c) -> std::string {
                        const char* s = (const char*)sqlite3_column_text(stmt, c);
                        return s ? s : "";
                    };
                    e.source_id = col(0);
                    e.target_id = col(1);
                    e.type      = col(2);
                    e.probable_target_weight = static_cast<float>(sqlite3_column_double(stmt, 3));
                    allEdges.push_back(e);
                }
                sqlite3_finalize(stmt);
            }
        }
    }

    // For each PPR-touched node, get its history and score each commit.
    for (auto& [nodeId, pprScore] : pprScores.nodeIdToScore) {
        auto history = codex_.getHistory(nodeId);
        for (auto& rec : history) {
            if (rec.commitHash.empty()) continue;

            // Fetch the real verbatim commit message from git.
            std::string verbatimMsg = fetchCommitMessage(rec.commitHash);
            if (verbatimMsg.empty()) verbatimMsg = rec.syntheticMsg;

            bool deferred = detectDeferredWork(verbatimMsg)
                         || detectDeferredWork(rec.syntheticMsg);

            // The mutation score is embedded in the syntheticMsg.
            // Approximate from message length and presence of structural keywords.
            int mutScore = 1;
            if (rec.syntheticMsg.find("Structural mutation") != std::string::npos) mutScore = 5;
            if (rec.syntheticMsg.find("major") != std::string::npos)               mutScore = 8;

            double score = scoreCandidate(pprScore, rec.timestamp, mutScore, deferred);

            // Build dependency path from crash frame to this node.
            std::string depPath;
            if (!crashNodeIds.empty()) {
                depPath = buildDependencyPath(crashNodeIds[0], nodeId, allEdges);
            }

            // Resolve file path from node.
            std::string filePath;
            auto node = codex_.getNode(nodeId);
            if (node) filePath = node->file_path;

            // Step 3: Enforce Execution Boundaries
            if (filePath.length() >= 3 && filePath.substr(filePath.length() - 3) == ".md") continue;
            if (filePath.length() >= 4 && filePath.substr(filePath.length() - 4) == ".txt") continue;

            // Keep the best-scoring commit per (node, commit) pair.
            std::string key = nodeId + ":" + rec.commitHash;
            if (!bestPerCommit.count(key) || bestPerCommit[key].score < score) {
                DiagnosisCandidate cand;
                cand.nodeId        = nodeId;
                cand.filePath      = filePath;
                cand.commitHash    = rec.commitHash;
                cand.timestamp     = rec.timestamp;
                cand.commitMessage = verbatimMsg;
                cand.syntheticMsg  = rec.syntheticMsg;
                cand.score         = score;
                cand.dependencyPath = depPath;
                cand.hasDeferredWork = deferred;
                bestPerCommit[key] = std::move(cand);
            }
        }
    }

    // Step 5: Collect all candidates, sort by score descending.
    std::vector<DiagnosisCandidate> candidates;
    candidates.reserve(bestPerCommit.size());
    for (auto& [k, v] : bestPerCommit) {
        candidates.push_back(v);
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const DiagnosisCandidate& a, const DiagnosisCandidate& b) {
                  return a.score > b.score;
              });

    if (static_cast<int>(candidates.size()) > topN)
        candidates.resize(topN);

    if (candidates.empty()) {
        result.ok = false;
        result.reason = "No historical commits found in the structural neighborhood of the crash. "
                        "Run `chronos sync` to index the repository history.";
        return result;
    }

    result.ok = true;
    result.candidates = std::move(candidates);
    return result;
}

} // namespace chronos

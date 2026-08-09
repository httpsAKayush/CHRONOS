#pragma once
// DiagnoseEngine: The flagship "Ghost Bug" diagnosis capability.
//
// Ingests a raw stack trace / crash log, resolves frames to Codex nodes,
// walks OUTWARD through structural dependencies, and surfaces historical
// commits that most likely caused the failure — ranked by a composite score
// combining structural proximity, temporal recency, AST mutation weight, and
// "deferred-work" language detection in the original commit message.
//
// The project_context.md acceptance test: a crash in query.cpp (untouched for
// 3 months) caused by connection_pool.cpp (modified 6 weeks ago with
// "Temporarily reduced connection limits to patch memory leak, will revert
// next sprint.") MUST surface as the top-ranked candidate with verbatim commit
// message and full dependency path shown.

#include <string>
#include <vector>
#include <cstdint>
#include "chronos/infrastructure/codex.hpp"
#include "chronos/infrastructure/vector_index.hpp"

namespace chronos {

// A single ranked candidate commit that may have caused the crash.
struct DiagnosisCandidate {
    std::string nodeId;          // Codex node ID where the change occurred
    std::string filePath;        // File path of the node
    std::string commitHash;      // Commit that changed it
    int64_t     timestamp = 0;   // When the commit happened
    std::string commitMessage;   // Original (verbatim) commit message
    std::string syntheticMsg;    // AI-generated intent summary (if any)
    double      score = 0.0;     // Composite ranking score (higher = more suspicious)
    std::string dependencyPath;  // Human-readable path from crash frame to this commit
    bool        hasDeferredWork = false; // True if message contains "hack", "temporary", etc.
};

// Full result of a diagnose invocation.
struct DiagnoseResult {
    bool ok = false;
    std::string reason;           // set when !ok
    std::string crashSummary;     // which frames were resolved
    std::vector<DiagnosisCandidate> candidates; // ranked best-first
};

class DiagnoseEngine {
public:
    DiagnoseEngine(Codex& codex, VectorIndex& vectors, const std::string& repoRoot);

    // Main entry: ingest the crash log text and return ranked candidates.
    DiagnoseResult diagnose(const std::string& crashLogText, int topN = 10) const;

private:
    Codex& codex_;
    VectorIndex& vectors_;
    std::string repoRoot_;

    // Parse crash frames from a raw log. Returns file names and function names.
    struct CrashFrame {
        std::string rawLine;
        std::string functionName;
        std::string fileName;
        int lineNumber = -1;
    };
    std::vector<CrashFrame> parseCrashLog(const std::string& log) const;

    // Resolve a crash frame to Codex node IDs (by file path + semantic search).
    std::vector<std::string> resolveFrame(const CrashFrame& frame) const;

    // Build a human-readable dependency path from crashNodeId to targetNodeId
    // using BFS over Codex edges. Returns empty string if no path found.
    std::string buildDependencyPath(const std::string& crashNodeId,
                                    const std::string& targetNodeId,
                                    const std::vector<Edge>& edges) const;

    // Score a commit candidate using the multi-factor formula.
    double scoreCandidate(double semanticScore,
                          int64_t commitTimestamp,
                          int      mutationScore,
                          bool     hasDeferredWork) const;

    // Detect deferred-work language in a commit message.
    static bool detectDeferredWork(const std::string& commitMessage);

    // Fetch the real commit message from git for a given hash.
    std::string fetchCommitMessage(const std::string& commitHash) const;
};

} // namespace chronos

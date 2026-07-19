#pragma once
// Orchestrates Spec §0 "Query time" mechanical walkthrough end-to-end:
// LanceDB (VectorIndex) seed -> Codex PPR traversal -> connectivity MMR
// pruning to fit the context budget -> live file-byte read -> IpcClient
// payload. This is the "Querier" component from Spec §6.

#include <string>
#include <optional>
#include "chronos/codex.hpp"
#include "chronos/vector_index.hpp"
#include "chronos/ipc.hpp"

namespace chronos {

struct BuildResult {
    bool ok = false;
    std::string reason;              // set when !ok, e.g. low-confidence seed
    ChronosRequest request;          // populated when ok
    TraceResult rawTrace;            // kept around for Oracle-Only fallback / `chronos trace`
};

class ContextBuilder {
public:
    ContextBuilder(Codex& codex, VectorIndex& vectors, std::string repoRoot);

    // Full Two-Hop Retrieval + MMR pruning (Spec FR-2, FR-3 acceptance
    // checks). `seedConfidenceFloor` implements the unhappy path in Spec §0:
    // "LanceDB cannot find a high-confidence seed -> TUI informs the user to
    // rephrase rather than hallucinating."
    BuildResult build(const std::string& userQuery, int pprBudget = 40,
                       int contextNodeBudget = 10, float seedConfidenceFloor = 0.05f);

private:
    Codex& codex_;
    VectorIndex& vectors_;
    std::string repoRoot_;

    std::string readLiveSnippet(const Node& n) const;
};

} // namespace chronos

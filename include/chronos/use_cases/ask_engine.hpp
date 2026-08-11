#pragma once
// Phase 4 "Context-Aware HyDE RAG": a 4-step subsystem-aware pipeline
// replacing the naive vector search in the old ask flow:
//
//   1. Instant Fetch   — retrieve [GLOBAL:REPO] summary for instant
//                       architectural context
//   2. Subsystem HyDE  — zero-shot: ask the LLM to generate a hypothetical
//                       technical C++ code snippet / explanation based on
//                       the repo summary and the user's query
//   3. Targeted Search — embed the HyDE hypothetical and vector-search
//                       the dense index for exact core-logic matches
//   4. Final Synthesis — combine top structural AST nodes + their
//                       [CONTEXT:DIR:*] summaries and stream a grounded
//                       answer with FR-8 citations
//
// Fallback chain:
//   - No [GLOBAL:REPO] → skip instant fetch, proceed with HyDE
//   - HyDE fails       → fall back to plain embedText(query) (old behavior)
//   - LLM unavailable  → Oracle-only trace fallback

#include "chronos/core/IStorage.hpp"
#include "chronos/core/ILLMClient.hpp"
#include "chronos/infrastructure/vector_index.hpp"
#include "chronos/infrastructure/llm/ipc_llm_client.hpp"
#include "chronos/ipc.hpp"
#include <string>
#include <vector>

namespace chronos {

struct AskResult {
    bool ok = false;
    std::string reason;
    ChronosRequest request;   // populated when ok = true
    TraceResult rawTrace;     // kept for Oracle-Only fallback
    bool usedHyDE = false;
};

class AskEngine {
public:
    AskEngine(IStorage& storage, VectorIndex& vectors, IpcLLMClient& llm,
              const std::string& repoRoot);

    AskResult run(const std::string& query);

private:
    IStorage& storage_;
    VectorIndex& vectors_;
    IpcLLMClient& llm_;
    std::string repoRoot_;

    // Step 1
    std::string fetchRepoSummary();

    // Step 2 — zero-shot HyDE
    std::string generateHypothetical(const std::string& repoSummary,
                                     const std::string& query);

    // Step 3
    std::vector<SeedMatch> targetedSearch(const std::string& hypothetical, int topK);

    // Step 4 — assemble final payload
    ChronosRequest synthesize(const std::vector<SeedMatch>& seeds,
                              const std::string& query,
                              const std::string& traceId);
};

} // namespace chronos
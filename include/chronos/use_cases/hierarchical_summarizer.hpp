#pragma once
// Phase 3 "Bottom-Up Subsystem Summaries" + "Global Assembly": builds a
// 3-tier Architectural Knowledge Graph during `chronos sync`.
//
//   [GLOBAL:REPO] ──CONTAINS──> [CONTEXT:DIR:src/cli] ──CONTAINS──> AST nodes
//
// Tier 1 (per subsystem): shallow tree + file headers -> SUMMARY_MODULE
//   stored as node [CONTEXT:DIR:<relpath>] (kind='context').
// Tier 2 (global): module summaries + root docs -> SUMMARY_REPO stored as
//   node [GLOBAL:REPO] (kind='context').
//
// Gracefully degrades to a no-op (with structural fallback edges) when the
// ILLMClient is unavailable, so `chronos sync` never fails on a missing LLM.

#include "chronos/core/IStorage.hpp"
#include "chronos/core/ILLMClient.hpp"
#include "chronos/use_cases/repo_profiler.hpp"
#include <string>

namespace chronos {

class HierarchicalSummarizer {
public:
    HierarchicalSummarizer(IStorage& storage, ILLMClient& llm);

    // Runs the full pipeline. Returns the number of context nodes written.
    int run(const std::vector<SubsystemProfile>& profiles);

    // Convenience: profile() then run().
    int runOnRepo(const std::string& repoRoot);

    // Mid-tier summaries (per-subsystem) + structural edges only. The
    // expensive global rollup is intentionally NOT done here so it can be
    // offloaded to a background job (see runGlobalRollupAsync).
    int runMidTier(const std::vector<SubsystemProfile>& profiles);

    // Global rollup as a background job. Writes [GLOBAL:REPO] with
    // "[ROLLUP_FAILED:TIMEOUT]" on failure so fetchRepoSummary() can detect
    // and retry rather than serving silent garbage.
    void runGlobalRollup(const std::vector<SubsystemProfile>& profiles, int timeoutMs = 90000);

private:
    IStorage& storage_;
    ILLMClient& llm_;

    std::string summarizeModule(const SubsystemProfile& prof);
    std::string summarizeRepo(const std::vector<SubsystemProfile>& profiles,
                               const std::vector<std::string>& rootDocs);
    void linkDirToCodeNodes(const std::string& dirId, const std::string& dirPrefix);
    std::string buildSymbolManifest(const SubsystemProfile& prof);
};

} // namespace chronos
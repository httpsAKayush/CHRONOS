#pragma once
// The Oracle (Spec §6 Component Logic, FR-7, project.md "Oracle-Only Mode").
// Two responsibilities:
//   1. Render a raw deterministic structural trace with zero LLM synthesis
//      — used both as the FR-7 fallback and as the CI regression harness
//      described in project.md Phase 0 ("this is our regression harness").
//   2. Verify that every Node-ID citation an LLM response claims actually
//      exists in the Codex (Spec §13 "LLM Validation": citation-not-found
//      => test fails; Spec §5 Interaction Refinement: unverified answers
//      prompt the user to view the deterministic trace instead).

#include <string>
#include <vector>
#include "chronos/codex.hpp"

namespace chronos {

struct CitationCheckResult {
    bool allValid = true;
    std::vector<std::string> invalidNodeIds;
};

class Oracle {
public:
    Oracle(Codex& codex, std::string repoRoot) : codex_(codex), repoRoot_(std::move(repoRoot)) {}

    // Renders a human-readable structural call-chain trace for `traceRes`,
    // e.g.:
    //   Auth::login  [.chronos node 3f2a...]
    //     -> CALLS DB::query [confidence: high]
    //     -> CALLS Logger::write [confidence: LOW - unparsed macro region]
    // This is exactly what `chronos ask` prints when the daemon is
    // unreachable (FR-7) and what `chronos trace <TraceID>` prints (Spec §9
    // Observability).
    std::string renderTrace(const TraceResult& traceRes) const;

    // Scans `llmText` for `[node:<id>]`-style citation markers (the schema
    // the daemon's system prompt is required to emit per FR-8) and checks
    // each against the Codex. Used by the CI harness (Spec §13) and by the
    // CLI to decide whether to show the "Unverified" refinement prompt
    // (Spec §5).
    CitationCheckResult verifyCitations(const std::string& llmText);

private:
    Codex& codex_;
    std::string repoRoot_;
};

} // namespace chronos

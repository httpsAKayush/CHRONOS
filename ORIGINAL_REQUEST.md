# Original User Request

## 2026-08-07T08:33:52Z

Implement all remaining features and logic discussed in `project_context.md` into the Chronos C++ engine, ensuring it fully functions as a Temporal Codebase Engine that detects architectural drift and resolves "ghost bugs." 

Working directory: /home/zer0/CHRONO
Integrity mode: development

## Requirements

### R1. Implement Missing Core Logic (C++ Engine & CLI)
Audit the codebase against `project_context.md` and implement missing concepts into the C++ engine. Focus strictly on the C++ daemon and CLI tools (exclude IDE extensions). Key targets include:
- The Temporal Recency Algorithm (time-decay penalty in retrieval).
- Aggressive Git Noise Filtering (ignoring whitespace-only AST changes).
- The Staging Area Check (pre-commit hook for temporal collision warnings).
- Tiered Embedding Memory and SQ8 Vector Quantization.

### R2. Continuous Self-Evaluation
Loop through the project to debug, test, and reimplement features until they exactly match the scenarios described in the context document. You must evaluate your work objectively.

## Verification Resources
- Use the existing `test_chronos.sh` script to verify basic functionality.

## Acceptance Criteria

### Feature Completeness
- [ ] The engine correctly applies a time-decay penalty during retrieval, prioritizing recent commits while keeping old ones accessible.
- [ ] The engine explicitly ignores commits that only contain whitespace or comment changes via AST comparison.
- [ ] A functional pre-commit hook intercepts and warns the user of historical constraints before they commit.
- [ ] Running `test_chronos.sh` passes successfully after all modifications.

## Follow-up — 2026-08-07T16:25:00Z

Implement the flagship "Ghost Bug" diagnosis engine (`chronos diagnose`) for Project Chronos. This engine must ingest a raw stack trace, walk backward through the structural dependency graph, and surface historical commits that caused the failure, rather than just returning standard RAG search results.

Working directory: /home/zer0/CHRONO
Integrity mode: development

## Requirements

### R1. Implement `chronos diagnose` (The Diagnosis Engine)
Build the `chronos diagnose --trace <crash_log>` CLI command. It must parse the crash log, resolve frames to Codex nodes, and walk OUTWARD through structural dependencies to find related nodes. For each touched node, pull historical commits and rank them using a combination of structural proximity, temporal recency, AST mutation score, and deferred-work language detection ("temporary", "hack", "will revert").

### R2. Dual-Horizon PPR & Retrieval Enhancements
Ensure the retrieval pipeline uses Hop 2 (Structural expansion) with Local-push (Andersen-style) Personalized PageRank. Implement Reciprocal Rank Fusion (RRF) to combine semantic and structural ranks, and apply connectivity-based MMR for diversity pruning to prevent a single heavily-connected class from monopolizing the context budget.

### R3. Build the Acceptance Fixture
Create a synthetic Git repository with ~10 commits that exactly reproduces the "connection pooler" scenario: a crash in `query.cpp` (untouched for 3 months) caused by a commit in its dependency `connection_pool.cpp` (modified 6 weeks ago with a "temporary hack" message).

## Acceptance Criteria

### Acceptance Test (The Core Requirement)
- [ ] Running `chronos diagnose --trace <crash_log>` against the synthetic fixture repo MUST surface the `connection_pool.cpp` "temporary hack" commit as a top-ranked candidate.
- [ ] The output MUST explicitly show the verbatim commit message and the structural dependency path from the crash frame to that commit.

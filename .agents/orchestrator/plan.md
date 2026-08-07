# Project Chronos Implementation Plan

## Architecture Overview
Chronos is a 4D C++ Temporal Codebase Engine that combines Tree-sitter AST parsing, libgit2 commit DAG traversal, HNSW vector search with temporal recency decay, and pre-commit staging area collision checks.

## Milestones

| # | Name | Scope | Key Artifacts | Dependencies | Status |
|---|------|-------|---------------|--------------|--------|
| 1 | Temporal Recency Algorithm | Time-decay scoring ($S = \alpha \cdot cos\_sim + (1-\alpha) \cdot e^{-\lambda \Delta t}$) | `vector_index.hpp`, `vector_index.cpp` | None | DONE |
| 2 | Aggressive Git Noise Filtering | AST-based whitespace/comment diff scoring (Score 0) & chore filtering | `ast_mutation_scorer.cpp`, `git_indexer.cpp` | M1 | DONE |
| 3 | Staging Area Check Hook | Pre-commit hook for temporal collision warnings on staged diffs | `main_cli.cpp`, `scripts/pre-commit`, `context_builder.cpp` | M1, M2 | IN_PROGRESS |
| 4 | Tiered Memory & SQ8 Quantization | Hot/Warm/Cold tiering & INT8 scalar quantization for HNSW vector index | `vector_index.hpp`, `vector_index.cpp`, `codex.cpp` | M1, M2 | PLANNED |
| 5 | E2E Integration & Test Harness | `test_chronos.sh` script and full validation suite | `test_chronos.sh`, `tests/` | M1, M2, M3, M4 | PLANNED |

## Detailed Breakdown

### Milestone 1: Temporal Recency Algorithm
- **Goal**: Implement exact temporal recency exponential decay formula in vector retrieval.
- **Formula**: $S_{final} = \alpha \cdot \cos\_sim(q, v) + (1-\alpha) \cdot e^{-\lambda (t_{now} - t_{commit})}$.
- **Tasks**:
  1. Add configurable $\alpha$ and $\lambda$ parameters (via env vars / config with sensible defaults $\alpha=0.7, \lambda=1e-7$).
  2. Ensure `queryTimestamp` handling properly computes elapsed time $\Delta t = \max(0, t_{now} - t_{commit})$.
  3. Write test cases in unit test suite (`tests/test_recency.cpp` or updated `chronos_tests`) to verify score decay across timestamps.

### Milestone 2: Aggressive Git Noise Filtering
- **Goal**: Skip vector generation for whitespace-only, comment-only, lockfile, and chore commits.
- **Tasks**:
  1. Enhance `AstMutationScorer::scoreDiff` to strip comments and normalize whitespace before AST diffing, returning score `0` if no non-whitespace/non-comment code changes exist.
  2. Update `GitIndexer::indexHistory` to skip vector generation for files/commits scoring `0`.
  3. Add unit tests for AST diff scoring with whitespace and comment modifications.

### Milestone 3: Staging Area Check (Pre-Commit Hook)
- **Goal**: Intercept `git add` / staged changes in pre-commit hook and issue Temporal Collision Warnings if staged diffs conflict with historical guardrails.
- **Tasks**:
  1. Implement CLI subcommand `chronos check-staging` (or `--check-staging`) to diff staged files (`git diff --cached`) against Codex/VectorIndex notes.
  2. Cross-reference staged AST nodes with historical commit notes/warnings.
  3. Integrate into `scripts/pre-commit` to print clear warnings when historical rules are violated.

### Milestone 4: Tiered Embedding Memory & SQ8 Quantization
- **Goal**: Implement Hot/Warm/Cold memory tiers and INT8 (SQ8) quantization for HNSW vector storage.
- **Tasks**:
  1. Add INT8 (SQ8) scalar quantization in `VectorIndex` to compress FP32 vectors by 75%.
  2. Implement Cold tier archival for historical commits > 1 year old (storing metadata/commit messages only, omitting code embeddings).
  3. Verify memory efficiency and retrieval accuracy with unit tests.

### Milestone 5: E2E Integration & Verification Harness (`test_chronos.sh`)
- **Goal**: Create comprehensive E2E test script and verify all requirements.
- **Tasks**:
  1. Create `test_chronos.sh` script in project root.
  2. Ensure clean CMake build, unit test execution, indexing test, staging check test, and temporal decay test.
  3. Run all tests to 100% pass.

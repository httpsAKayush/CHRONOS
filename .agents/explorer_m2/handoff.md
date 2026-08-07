# Handoff Report: Milestone 2 — Aggressive Git Noise Filtering Investigation

## 1. Observation

- **Exact File Paths & Lines Examined**:
  - `src/ast_mutation_scorer.cpp` (lines 1-42):
    - Line 5-8: Supported extension check only handles 5 extensions (`.py`, `.cpp`, `.hpp`, `.c`, `.h`).
    - Line 12: Performs exact string comparison `if (oldContent == newContent) return 0;`.
    - Lines 20-38: Uses naive string counting (`countSubstrings`) for `def `, `class `, `public:`, returning score 10 if counts differ, and falling through to return 1 for any non-identical string.
  - `src/git_indexer.cpp` (lines 1-330):
    - Lines 63-66: `isBot` check in `indexHistory()` pre-pass:
      `bool isBot = (email.find("bot") != std::string::npos || email.find("noreply.github.com") != std::string::npos || msg.find("Auto-generated") == 0);`
    - Missing checks for `chore` commit prefixes (e.g. `chore:`, `chore(deps):`, `ci:`, `build(deps):`, `bump `).
    - Missing lockfile filtering (`package-lock.json`, `Cargo.lock`, `yarn.lock`, `pnpm-lock.yaml`, `composer.lock`, `Gemfile.lock`, `poetry.lock`, `go.sum`).
    - Lines 202-204: Uses `int threshold = std::max(1, avgLines / 10); if (commitMutationScore >= threshold)` to decide whether to process commits, which can drop valid internal logic changes (score 1) if `avgLines` is high.
  - `project_context.md` (lines 127-135, 508-523):
    - Defines the rule: "If a commit matches standard 'chore' prefixes, changes only whitespace, or modifies a lockfile (package-lock.json, Cargo.lock), the engine skips vector generation entirely."
    - Defines AST diff scoring: whitespace, line break, and comment-only changes MUST yield score 0.
  - `tests/test_main.cpp` and `tests/CMakeLists.txt`:
    - Clean test harness built with standard C++ `CHRONOS_CHECK` macro (`test_framework.hpp`). Currently passing test suite executed via `ctest --test-dir build --output-on-failure`.

- **Tool Commands Executed**:
  - `ctest --test-dir build --output-on-failure` (Passed 1/1 test).

---

## 2. Logic Chain

1. **Observation 1**: `AstMutationScorer::scoreDiff` returns `0` only when `oldContent == newContent` (exact string match).
2. **Reasoning 1**: Any modification consisting solely of spaces, tabs, newlines, or comment additions/edits causes `oldContent == newContent` to evaluate to `false`.
3. **Observation 2**: Non-identical content falls through to line 38 of `src/ast_mutation_scorer.cpp` returning `1` (or `10`).
4. **Reasoning 2**: Because whitespace/comment changes return score `1`, `GitIndexer` evaluates `commitMutationScore > 0`, causing garbage formatting/comment commits to trigger vector embedding generation, violating `project_context.md` (lines 127-135, 508-523).
5. **Observation 3**: `GitIndexer::indexHistory()` does not check for `chore` prefixes in commit messages or lockfiles in file paths.
6. **Reasoning 3**: Automated dependency updates, lockfile updates (`package-lock.json`, `Cargo.lock`), and chore commits are indexed into the Hot vector store, wasting vector memory and retrieval bandwidth.
7. **Deduction & Solution**: Implementing a language-aware comment stripper and token normalizer in `AstMutationScorer::scoreDiff` alongside explicit chore commit prefix and lockfile filtering in `GitIndexer::indexHistory()` will ensure all whitespace, formatting, comment-only, lockfile, and chore changes yield score `0` and skip vector generation.

---

## 3. Caveats

- **Tree-sitter linkage dependency**: While full tree-sitter AST parsing can be used when tree-sitter grammars are compiled and linked, the token-based comment and whitespace normalizer in `AstMutationScorer` acts as a robust, lightweight, standalone mechanism that operates identically whether tree-sitter grammars are present or absent.
- **Python Docstring Ambiguity**: Multi-line triple-quoted strings (`"""..."""`) in Python can act as docstrings or runtime multiline strings. Stripping `#` single-line comments while preserving string literals ensures syntax and string content remain strictly preserved while line comments are stripped cleanly.

---

## 4. Conclusion

The current implementation fails to filter whitespace/comment changes and chore/lockfile commits as required by `project_context.md`.

To complete Milestone 2:
1. **Enhance `AstMutationScorer::scoreDiff`**:
   - Implement language-aware comment stripping for C-style (`//`, `/* */`) and Python (`#`).
   - Implement token-based whitespace normalization. If token sequence of `oldContent` equals `newContent`, return `0`.
   - Support expanded file extensions (`.cpp`, `.hpp`, `.c`, `.h`, `.cc`, `.cxx`, `.hh`, `.hxx`, `.py`, `.pyi`, `.js`, `.ts`, `.jsx`, `.tsx`, `.java`, `.go`, `.rs`, `.cs`, `.mjs`, `.cjs`). Return 0 for unsupported extensions & lockfiles.
2. **Update `GitIndexer::indexHistory`**:
   - Filter chore commit prefixes (`chore:`, `chore(deps):`, `ci:`, `build(deps):`, `bump `) and bot authors (`[bot]`, `dependabot`, `renovate`).
   - Skip lockfiles (`package-lock.json`, `Cargo.lock`, `yarn.lock`, etc.).
   - Skip vector embedding generation when `commitMutationScore == 0`.
3. **Add Unit Tests in `tests/test_ast_mutation_scorer.cpp`**:
   - Test whitespace/indentation changes (score 0).
   - Test comment-only changes (score 0).
   - Test formatting/linebreaks (score 0).
   - Test lockfiles & unsupported extensions (score 0).
   - Test internal logic changes (score 1).
   - Test contract-breaking structural changes (score 10).

Detailed technical specification and proposed code diffs have been saved to `/home/zer0/CHRONO/.agents/explorer_m2/analysis.md`.

---

## 5. Verification Method

1. **Build Verification**:
   ```bash
   cmake -B build -S .
   cmake --build build --target chronos_tests
   ```
2. **Test Execution**:
   ```bash
   ctest --test-dir build --output-on-failure
   ```
3. **Invalidation Conditions**:
   - `AstMutationScorer::scoreDiff` returning `> 0` for comment/whitespace-only changes.
   - `AstMutationScorer::scoreDiff` returning `0` for real logic or structural changes.
   - `GitIndexer::indexHistory` indexing lockfiles or chore/bot commits into Codex/VectorIndex.

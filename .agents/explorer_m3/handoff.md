# Handoff Report — Milestone 3: Staging Area Check (Pre-Commit Hook)

## 1. Observation
- **`scripts/pre-commit`**: Currently runs `git diff --cached --name-only` and spawns `chronos-indexer` in background (`nohup setsid ... & disown`) (lines 19-44). It does **not** invoke `chronos check-staging` or print temporal collision warnings.
- **`src/main_cli.cpp`**: CLI subcommands currently supported are `init`, `sync`, `ask`, `trace`, `timeline` (lines 219-231). Subcommand `check-staging` is missing.
- **`src/codex.cpp` / `include/chronos/codex.hpp`**: The `history` SQLite table stores `(node_id, commit_hash, timestamp, synthetic_msg)` (lines 71-77, `codex.cpp`). `Codex::getHistory(nodeId)` fetches records for a specific node ID (lines 255-275), but there is no `getHistoryForFile(filePath)` method to fetch all historical notes for a file directly.
- **`src/ast_mutation_scorer.cpp`**: `AstMutationScorer::scoreDiff(oldContent, newContent, ext)` ranks code mutations (10 for structural changes, 1 for internal logic, 0 for whitespace/comments).
- **`project_context.md` (lines 541-548)**: Explicitly requires the Staging Area Check to intercept `git diff --cached`, parse uncommitted code, cross-reference with historical rules/notes (e.g. warning against adding a mutex or altering a timeout), and display a Temporal Collision Warning before/during commit.

## 2. Logic Chain
1. **Current System Deficit**: `scripts/pre-commit` triggers background indexing but does not inspect staged diffs for historical violations or output temporal collision warnings during `git commit`.
2. **CLI Requirement**: A subcommand `chronos check-staging [repo_root]` must be added to `src/main_cli.cpp`. It will execute `git diff --cached -U3`, extract modified/added staged lines and affected file paths, query historical notes from `codex.db`, and compare staged diff keywords/mutations against historical synthetic messages.
3. **Database Extension**: Adding `getHistoryForFile(const std::string& filePath)` to `Codex` enables efficient lookup of historical notes and warnings associated with nodes in staged files.
4. **Hook Integration**: Adding `chronos check-staging "$REPO_ROOT" || true` to `scripts/pre-commit` ensures developers receive real-time temporal collision warnings on `git commit` while preserving the <500ms fail-open contract.
5. **Test Harness**: Creating `tests/test_staging_check.cpp` (and registering it in `tests/test_main.cpp` and `tests/CMakeLists.txt`) verifies that staging check correctly detects conflicts between staged code (e.g., adding `std::mutex`) and historical notes (e.g., `"DO NOT MUTEX HERE"`).

## 3. Caveats
- Staged diff parsing relies on `git diff --cached` output. If `git` is not installed or not in a git repository, `chronos check-staging` must fail gracefully (return 0).
- `project_context.md` specifies fail-open operation in <500ms. High-speed string matching against SQLite historical notes ensures latency remains well within budget.

## 4. Conclusion
Milestone 3 requires adding `chronos check-staging` to `src/main_cli.cpp`, extending `Codex` with `getHistoryForFile`, integrating `chronos check-staging` into `scripts/pre-commit`, and adding a dedicated test harness in `tests/test_staging_check.cpp`. Detailed implementation steps and code snippets are documented in `/home/zer0/CHRONO/.agents/explorer_m3/analysis.md`.

## 5. Verification Method
1. Build test binary:
   ```bash
   cd /home/zer0/CHRONO/build && cmake .. && make chronos_tests chronos
   ```
2. Run test suite:
   ```bash
   /home/zer0/CHRONO/build/tests/chronos_tests
   ```
3. Manual verification of pre-commit hook execution:
   ```bash
   ./build/chronos check-staging /home/zer0/CHRONO
   ```
4. Verify files:
   - Check `/home/zer0/CHRONO/.agents/explorer_m3/analysis.md`
   - Check `/home/zer0/CHRONO/.agents/explorer_m3/handoff.md`

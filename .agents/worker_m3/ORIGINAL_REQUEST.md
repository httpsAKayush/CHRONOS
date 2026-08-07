## 2026-08-07T09:00:55Z
You are a Worker subagent for Milestone 3: Staging Area Check (Pre-Commit Hook).
Your working directory is /home/zer0/CHRONO/.agents/worker_m3.
Create your working directory /home/zer0/CHRONO/.agents/worker_m3 and progress.md immediately.

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A Forensic Auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

Your objective:
1. Read `/home/zer0/CHRONO/.agents/explorer_m3/analysis.md` and `/home/zer0/CHRONO/.agents/explorer_m3/handoff.md`.
2. Update `include/chronos/codex.hpp` & `src/codex.cpp`:
   - Implement `std::vector<HistoryRecord> Codex::getHistoryForFile(const std::string& filePath)` to fetch all historical notes and synthetic messages for active nodes in a file.
3. Update `src/main_cli.cpp`:
   - Add `chronos check-staging [repo_root]` subcommand.
   - Run `git diff --cached` to parse staged changes/files.
   - Cross-reference staged diffs with historical notes from `Codex::getHistoryForFile` and print structured Temporal Collision Warnings if staged changes conflict with historical constraints/notes.
4. Update `scripts/pre-commit`:
   - Add `chronos check-staging "$REPO_ROOT" || true` to display real-time Temporal Collision Warnings during `git commit`.
5. Create `tests/test_staging_check.cpp` and update `tests/CMakeLists.txt` & `tests/test_main.cpp`:
   - Write unit tests verifying `Codex::getHistoryForFile` and staging collision detection.
6. Build (`cmake -B build -S . && cmake --build build`) and run `./build/tests/chronos_tests` & `ctest`.
7. Write your handoff report to `/home/zer0/CHRONO/.agents/worker_m3/handoff.md` with build and test logs, and send a message back to parent.

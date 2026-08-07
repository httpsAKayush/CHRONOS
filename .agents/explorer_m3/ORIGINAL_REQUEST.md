## 2026-08-07T08:57:32Z
You are an Explorer subagent for Milestone 3: Staging Area Check (Pre-Commit Hook).
Your working directory is /home/zer0/CHRONO/.agents/explorer_m3.
Create your working directory /home/zer0/CHRONO/.agents/explorer_m3 and progress.md immediately.

Your objective:
1. Examine `scripts/pre-commit`, `src/main_cli.cpp`, `src/context_builder.cpp`, `include/chronos/context_builder.hpp`, `src/codex.cpp`, and `src/oracle.cpp`.
2. Compare against `project_context.md` requirements (lines 541-548, Requirement R1 & R2 acceptance criteria):
   - The Staging Area Check intercepts staged files (`git diff --cached`) during pre-commit.
   - It performs Temporal Collision Warnings by cross-referencing staged changes/AST nodes against historical notes, commit messages, and warnings stored in Codex/VectorIndex.
   - If a staged change conflicts with a historical constraint (e.g. historical note warning "DO NOT MUTEX HERE" or "TEMPORARILY REDUCED TIMEOUT"), Chronos displays a temporal collision warning.
3. Design:
   - CLI command `chronos check-staging` (or `chronos check-staged`) in `src/main_cli.cpp` that checks staged diffs against Codex notes and prints structured temporal warnings.
   - Integration in `scripts/pre-commit` so developers receive temporal collision warnings during `git commit`.
   - Dedicated unit test harness in `tests/test_staging_check.cpp` verifying warning output when staged code matches historical notes/warnings.
4. Document technical findings and proposed implementation plan in `/home/zer0/CHRONO/.agents/explorer_m3/analysis.md` and write handoff report to `/home/zer0/CHRONO/.agents/explorer_m3/handoff.md`. Send a message back to parent.

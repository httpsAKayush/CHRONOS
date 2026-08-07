# Orchestrator Task Context

- Project Root: `/home/zer0/CHRONO`
- Working Directory for Orchestrator: `/home/zer0/CHRONO/.agents/orchestrator`
- Original Request File: `/home/zer0/CHRONO/.agents/ORIGINAL_REQUEST.md`
- Context Document: `/home/zer0/CHRONO/project_context.md`
- Verification Script: `/home/zer0/CHRONO/test_chronos.sh` (or `tests/` directory)

## Objective
Fully implement all missing core logic described in `project_context.md` for the Chronos C++ engine and CLI:
1. Temporal Recency Algorithm (time-decay penalty in retrieval).
2. Aggressive Git Noise Filtering (ignoring whitespace/comment-only AST changes).
3. Staging Area Check (pre-commit hook for temporal collision warnings).
4. Tiered Embedding Memory and SQ8 Vector Quantization.
5. Continuous self-evaluation and verification using tests/scripts until all scenarios pass.

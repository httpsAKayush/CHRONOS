# Project: Chronos "Ghost Bug" Diagnosis Engine & Retrieval Enhancements

## Architecture
- C++ Engine with CLI interface (`chronos`).
- Graph Index / Codex nodes for code structural relationships.
- Retrieval Pipeline: Local-push (Andersen-style) PPR for Hop 2 structural expansion, Reciprocal Rank Fusion (RRF) combining semantic + PPR ranks, Connectivity-based MMR for diversity pruning.
- Diagnosis Engine: Crash log parser, outward graph traversal, commit ranking (proximity, recency, AST mutation, deferred-work detection), dependency path renderer.

## Code Layout
- `include/` and `src/`: Core engine headers and implementation files.
- `src/cli/` or `src/commands/`: CLI command definitions (including `diagnose`).
- `src/retrieval/` or `include/chronos/`: PPR, RRF, MMR, and ranking modules.
- `tests/` or `scripts/`: Fixtures, unit tests, and acceptance test scripts (`test_chronos.sh`).

## Milestones
| # | Name | Scope | Dependencies | Status |
|---|------|-------|-------------|--------|
| 1 | Dual-Horizon PPR & Retrieval Enhancements | Hop 2 Andersen Local-push PPR, RRF, connectivity-based MMR | none | IN_PROGRESS |
| 2 | Ghost Bug Diagnosis Engine (`chronos diagnose`) | CLI command, trace parser, outward walk, multi-feature commit ranker | M1 | PLANNED |
| 3 | Synthetic Acceptance Fixture | Git repo with ~10 commits reproducing connection pooler crash | none | PLANNED |
| 4 | End-to-End Acceptance Test & Audit | Verify `chronos diagnose` surfaces candidate commit + path | M1, M2, M3 | PLANNED |

## Interface Contracts
### Crash Trace Parser ↔ Codex Graph Resolution
- `CrashLogParser::parse(const std::string& log_path) -> StackTrace`
- `CodexGraph::resolve_frame(const StackFrame& frame) -> std::vector<NodeId>`

### Outward Traversal & PPR 
- `PersonalizedPageRank::compute_local_push(const Graph& g, const std::vector<NodeId>& seeds, double alpha, double epsilon) -> PPRMap`
- `RRF::blend_ranks(const std::vector<RankList>& rank_lists, double k) -> ScoredList`
- `MMR::prune_diversity(const ScoredList& list, double lambda, double similarity_threshold) -> FilteredList`

### Commit Scoring & Ranking
- `CommitRanker::score_commits(const std::vector<NodeId>& touched_nodes, const Graph& graph, const GitRepo& repo) -> std::vector<RankedCommit>`
- Feature weights: structural proximity, temporal recency time-decay, AST mutation score, deferred-work language detection ("temporary", "hack", "will revert").

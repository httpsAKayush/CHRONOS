# Architecture: The Triad of Truth

Chronos structurally isolates three concerns that standard "blob" RAG mixes
together, per `project.md` §I:

| Layer | Owns | Component | Storage |
|---|---|---|---|
| Semantic identity | "what a function *is*" | `VectorIndex` | `.chronos/vectors.bin` |
| Structural/temporal relationships | "what a function *touches* and *why*" | `Codex` | `.chronos/codex.db` (SQLite) |
| Ground truth | live code bytes | `ContextBuilder::readLiveSnippet` | the file system, read at query time |

No layer caches another layer's data. In particular, `ContextBuilder` never
stores a function's outbound call signatures inside the vector store — that
was explicitly ruled out (Spec §15) to avoid cascading update costs when
downstream signatures change.

## Two-Hop Retrieval

1. **Hop 1 (semantic seed):** `VectorIndex::search` finds the node(s) whose
   embedding is nearest the query. If the top score is below
   `seedConfidenceFloor`, `ContextBuilder::build` refuses rather than
   guessing (Spec §0 unhappy path).
2. **Hop 2 (structural expansion):** `Codex::localPushPPR` runs a
   budgeted, local-push Personalized PageRank from the seed
   (`ppr.cpp`), using a **dual-horizon** strategy (`PPREngine::dualHorizonPush`)
   that runs a tight push and a wide push in parallel and fuses them with
   Reciprocal Rank Fusion (`rrf.cpp`) instead of trying to classify the
   query as "diagnostic" vs "architectural" up front.
3. **Connectivity-based MMR** (`mmr.cpp`) prunes the PPR output down to the
   context-node budget, penalizing *topological* redundancy (adjacent/
   highly-overlapping neighbor sets) rather than textual similarity — this
   is what prevents a God Class's 40 interconnected methods from consuming
   the entire context window.
4. **Ground truth read:** for each surviving node, `ContextBuilder` reads
   the live byte range straight from disk — never a cached copy.
5. The assembled `ChronosRequest` (with per-node `uncertain` flags derived
   from `parse_confidence`) is sent to `chronos-daemon` over `AF_UNIX`.

## Identity tracking (Simhash + alias DAG)

`Simhash::compute` hashes only structural/control-flow tokens supplied by
the AST walk in `ast_indexer.cpp`, which explicitly excludes
`identifier`/`type_identifier`/`field_identifier` node kinds. A pure rename
therefore produces a bit-identical fingerprint. When `AstIndexer::indexFile`
finds a matching fingerprint at a new location, it calls
`Codex::recordAlias`, which performs a union-find-style write (root
repointing + path compression) so that:

- the alias DAG can never contain a cycle,
- `resolveAlias` is a single indexed lookup regardless of chain length,
- old queries ("How does `login` work?") keep resolving to the renamed
  node (`signIn`) without needing a fresh LLM embedding.

## Oracle-Only Mode & citation enforcement

`Oracle` (`oracle.cpp`) has two jobs that both stem from the same idea —
"the graph is the source of truth, the LLM is optional synthesis on top of
it":

- `renderTrace` turns a `TraceResult` into a plain call-chain listing with
  zero LLM involvement. This is what `chronos ask` prints if the daemon is
  down (FR-7) and what CI runs as the regression harness described in
  `project.md` Phase 0.
- `verifyCitations` scans an LLM's answer for `[node:<id>]` markers and
  checks each against the Codex. `main_cli.cpp::cmdAsk` uses this to flag
  "Unverified" answers and offer the deterministic trace instead (Spec §5
  Interaction Refinement), and the same function is what the CI suite in
  `tests/test_oracle_harness.cpp` exercises for FR-8/§13.

## Execution tier

`chronos-daemon` is a thin policy/relay layer, not a model runtime.
It never links `llama.cpp` (Spec §2 hard constraint); it lazy-loads by
shelling out to a local Ollama instance on first request and self-exits
after 15 idle minutes (`FR-5`). The system prompt it sends is the
enforcement mechanism for FR-8's citation contract and the §7 Safety
Contract's `uncertainty_warning` prefacing; because a model can still
ignore its system prompt, `Oracle::verifyCitations` is the actual
guarantee, not the prompt text.

## Known incomplete pieces (see also `docs/DECISIONS.md`)

- `chronos trace <id>` (Spec §9 Observability) needs a `trace_log` Codex
  table (`traceId -> node set`) that `ContextBuilder::build` should populate
  on every call. Not wired up yet — flagged with a `TODO` in
  `main_cli.cpp::cmdTrace` rather than silently omitted.
- `IpcServer`'s request handler in `main_daemon.cpp` doesn't re-decode the
  full `context[]` array from the wire (see the note in `ipc.cpp
  decodeRequest`); v1 keeps the daemon stateless. See `docs/DECISIONS.md`.

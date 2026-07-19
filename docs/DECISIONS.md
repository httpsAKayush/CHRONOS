# Deviations from the literal spec, and open TODOs

Per the build instructions: nothing below is silently skipped — each item
states what's missing and exactly what to do to finish it.

## 1. LanceDB → embedded flat-file cosine store (`vector_index.cpp`)

**Why:** LanceDB does not ship a stable, embeddable C++ SDK (its supported
surfaces are Python/Rust/Node). A CMake-only C++20 project cannot depend on
it without vendoring a large, actively-changing Rust core and writing FFI
bindings ourselves — that FFI layer would be the single largest, riskiest
piece of this project and isn't what the spec is actually about.

**What we did instead:** `VectorIndex` implements the exact same contract
(`upsert`, `remove`, `search` by cosine similarity, embedded, on-disk,
zero-network) as a flat file of `float32[384]` vectors + brute-force cosine.
At the spec's stated scale (≤1M LOC ⇒ low hundreds of thousands of
function-level vectors) this comfortably meets the <1s query budget on
commodity hardware.

**To swap in real LanceDB later:** only `src/vector_index.cpp` needs to
change; `include/chronos/vector_index.hpp`'s public interface is the
contract every other file (`ast_indexer.cpp`, `context_builder.cpp`) already
codes against.

## 2. Embedding model → deterministic hashing embedder (`embedText`)

**Why:** shipping/loading a real sentence-embedding model means picking a
runtime (ONNX/GGML/etc.), bundling weights, and adding a large new
dependency surface — out of scope for "one pass, don't skip structure but
don't gold-plate a sub-component."

**What we did instead:** a token-hash bag-of-words embedder. It's
deterministic and stable for near-identical text, which is enough for
Hop-1 seeding since Hop-2 (the Codex graph) is the actual source of
structural truth per `project.md`'s Triad philosophy.

**To upgrade:** replace the body of `embedText` in `vector_index.cpp` with
a call into a real local embedding model; `VectorIndex::kDim` may need to
change to match the model's output width, and any persisted `.chronos/vectors.bin`
would need a one-time re-embed pass (`chronos sync` already walks every file,
so extending it to force a re-embed is a small change there).

## 3. `chronos trace <traceId>` not implemented

**What's missing:** persistence of `traceId -> TraceResult` so a past
answer's provenance can be replayed later (Spec §9 Observability).

**Exactly what to do:** add a `trace_log(trace_id TEXT PRIMARY KEY, node_ids_json TEXT, created_at INTEGER)`
table in `Codex::migrate`, have `ContextBuilder::build` insert a row keyed by
`req.traceId` right before returning, and implement `cmdTrace` in
`main_cli.cpp` as: look up the row, split the node id list, call
`Codex::getNode` for each, reconstruct a `TraceResult`, and call
`Oracle::renderTrace` on it.

## 4. Daemon doesn't re-decode the full `context[]` array (`ipc.cpp`)

**Why:** keeping v1's daemon fully stateless and the wire format simple.

**What's missing:** `IpcServer`'s handler in `main_daemon.cpp` builds the
system prompt from a template but doesn't inline every context node's code
snippet into it yet.

**Exactly what to do:** implement array parsing in `decodeRequest`
(`ipc.cpp`) — walk the `"context":[...]` substring, split on top-level
`},{` boundaries, and reuse `extractStringField`/a new `extractBoolField`
call per object — then in `main_daemon.cpp` append each `ContextNode`'s
`filePath`/`codeSnippet`/`uncertain` flag into `systemPrompt` before calling
`ollamaChatBlocking`.

## 5. `tree-sitter`/`tree-sitter-cpp` are optional link-time dependencies

**Why:** these are typically vendored as git submodules or built from
source (they're not always available as a distro package), and pulling the
grammar source into this listing would be pure boilerplate, not project
logic.

**What happens without them:** `CMakeLists.txt` detects their absence via
`find_library` and compiles without `CHRONOS_HAVE_TREE_SITTER`;
`AstIndexer::indexFile` then takes the whole-file/`parse_confidence=0.0`
path documented inline in `ast_indexer.cpp`, so the system stays fully
functional (degraded precision, not degraded reliability).

**To enable full precision:** `git submodule add` tree-sitter and
tree-sitter-cpp, build them (they're small, dependency-free C libraries),
and point `CMAKE_PREFIX_PATH`/`find_library` at the build output — no source
changes required, `CMakeLists.txt` already wires the define through.

## 6. Rebase-time hook behavior (Spec §18 Open Question)

Left as an explicit open question, matching the spec. `scripts/pre-commit`
runs on every commit including ones created by `git rebase -i`'s replay,
which is safe (idempotent, fail-open, async) but potentially wasteful
(many small indexer spawns during a long rebase). Not fixed here because
the spec itself lists this as unresolved; the safe default (index every
commit) was kept rather than guessing at a debounce policy.

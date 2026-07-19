# Chronos

A zero-idle, 4D native C++ RAG engine for codebases: semantic vector search
(Hop 1) fused with deterministic AST graph traversal (Hop 2) and temporal
Git history, served through a lazy-loading local LLM daemon over `AF_UNIX`.

## Architecture

```
                    ┌────────────────────┐
  git commit  ───▶  │  pre-commit hook   │  <500ms, fail-open, fire-and-forget
                    └─────────┬──────────┘
                              │ detached spawn
                              ▼
                    ┌────────────────────┐
                    │  chronos-indexer    │  async worker
                    │  (tree-sitter AST)  │
                    └──────┬───────┬──────┘
                           │       │
                 writes    │       │  writes (only if identity changed)
                           ▼       ▼
                 ┌──────────────┐ ┌──────────────┐
                 │ SQLite Codex │ │ VectorIndex   │   (semantic seed layer;
                 │ (structural  │ │ (LanceDB-     │    see docs/DECISIONS.md
                 │  graph, Hop2)│ │  contract, Hop1)│  for why this isn't the
                 └──────┬───────┘ └──────┬────────┘   LanceDB SDK directly)
                        │                │
                        └───────┬────────┘
                                ▼
                     ┌────────────────────┐
   chronos ask "..." │  ContextBuilder     │  Two-Hop Retrieval + MMR pruning
   (CLI/TUI)   ────▶ │  (the "Querier")    │  + live file-byte read
                     └─────────┬──────────┘
                               │ AF_UNIX JSON-RPC (0600, /tmp/chronos-*.sock)
                               ▼
                     ┌────────────────────┐
                     │   chronos-daemon    │  lazy-load, 15min idle sleep
                     │  (system-prompt +   │
                     │   citation policy)  │
                     └─────────┬──────────┘
                               │ localhost:11434 HTTP
                               ▼
                        ┌─────────────┐
                        │   Ollama    │  actual LLM weights/runtime
                        └─────────────┘

   If daemon/model unavailable at any point → Oracle-Only Mode:
   the CLI renders TraceResult directly via Oracle::renderTrace(),
   no LLM synthesis involved (FR-7).
```

## Build

See [SETUP.md](SETUP.md).

## Design

See [ARCHITECTURE.md](ARCHITECTURE.md) for the separation of the Vector /
Graph / File System triad, and [docs/DECISIONS.md](docs/DECISIONS.md) for
places this implementation deliberately deviates from the literal spec text
(and why).

## CLI

```
chronos init                 # create .chronos/, install git hook, .gitignore
chronos ask "<question>"     # two-hop retrieval + LLM synthesis (or Oracle-Only fallback)
chronos sync                 # repair Codex state if a commit bypassed the hook
chronos trace <traceId>      # (not yet implemented -- see TODO in main_cli.cpp)
```
# CHRONOS

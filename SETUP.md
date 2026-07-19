# Setup

## Prerequisites

- CMake 3.20+
- A C++20 compiler (GCC 11+/Clang 14+)
- `libsqlite3-dev` (Debian/Ubuntu) or `sqlite3` (Homebrew)
- Optional but recommended: `tree-sitter` + `tree-sitter-cpp` grammar
  libraries on your linker path. **Without these, Chronos still builds and
  runs** — it falls back to whole-file nodes with `parse_confidence = 0.0`
  instead of function-level AST nodes (see `ast_indexer.cpp`). Function-level
  precision requires the real grammar.
- [Ollama](https://ollama.com) running locally (`ollama serve`) with a model
  pulled (e.g. `ollama pull llama3`) for LLM synthesis. Chronos works fully
  in Oracle-Only Mode without this.

## Build

```bash
git clone <this-repo>
cd chronos
mkdir build && cd build
cmake ..
make -j
ctest --output-on-failure   # runs tests/ (Simhash, alias DAG, MMR, Oracle harness)
```

This produces three binaries: `chronos` (CLI), `chronos-indexer` (async
worker), `chronos-daemon` (LLM execution tier). Put all three on your `PATH`.

## Initialize a repo

```bash
cd /path/to/your/cpp/project
chronos init
```

This creates `.chronos/`, adds it to `.gitignore`, and installs
`scripts/pre-commit` as your Git hook (copy `scripts/pre-commit` from this
repo into your project root first, or point `chronos init` at a checkout
that has it alongside the binary — see the `hookSrc` lookup in
`main_cli.cpp::cmdInit`).

## First index

The hook only indexes files touched by future commits. To build the initial
graph for the whole repo:

```bash
chronos sync
```

## Ask a question

```bash
chronos ask "How does the auth payload move through the system?"
```

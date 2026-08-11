# Chronos — The Temporal Codebase Engine

Understand any codebase. Ask questions. Get precise, cited answers.

Chronos is a static-analysis + LLM engine that builds a **temporal codebase graph**: it parses your code into a structural graph (ASTs, functions, classes, calls), indexes it semantically with embeddings, and tracks structural mutations over git history. It answers conceptual questions, explains symbols, maps call graphs, and diagnoses crashes — all with citations into your actual source.

> **Note:** the complete, finished project lives on the **`res` branch** of the repository. Use `git clone -b res` (see below).

---

## Features

- **Discovery Mode (`ask`)** — conceptual questions with cited answers
- **Surgery Mode (`explain`)** — symbol-level detail for any function/class
- **Architectural X-Ray (`map`)** — upstream/downstream call graphs with depth control
- **Ghost Bug Diagnosis (`diagnose`)** — walk backward from a stack trace
- **Temporal Indexing** — structural mutations tracked across git history (`timeline`)
- **Temporal Collision detection** (`check-staging`) — catch stale references before you commit
- **AI commit messages** (`commit`) — generate a Conventional Commit message from your staged diff and commit with one confirmation
- **Dual-profile LLM** — auto-switching between local (Ollama) and cloud endpoints
- **Trace inspection** (`trace`) — see exactly what context the LLM was given
- **Graph export** (`export`) — JSON or Mermaid for visualization

---

## Requirements

- CMake ≥ 3.16, a C++17 compiler (g++/clang)
- SQLite3, tree-sitter (C library + dev headers)
- Python 3 + tree-sitter language grammars (`tree-sitter-cpp`, `tree-sitter-python`, etc.)
- (Optional) Ollama for the local LLM profile

---

## Install

### 1. Install dependencies

```bash
# Ubuntu/Debian
sudo apt update && sudo apt install -y \
    cmake g++ git sqlite3 libsqlite3-dev \
    libtree-sitter-dev tree-sitter \
    python3 python3-pip

# Arch/Manjaro
sudo pacman -S cmake gcc git sqlite tree-sitter python python-pip

# Fedora
sudo dnf install cmake gcc-c++ git sqlite-devel tree-sitter python3 python3-pip

# macOS (Homebrew)
brew install cmake git sqlite tree-sitter python

# tree-sitter grammars for your languages (C++, Python, etc.)
pip install --break-system-packages tree-sitter-cpp tree-sitter-python \
    tree-sitter-javascript tree-sitter-css
```

### 2. Build & install

```bash
git clone -b res https://github.com/httpsAKayush/CHRONOS.git
cd CHRONOS
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
sudo make install
```

This installs:

- `/usr/local/bin/chronos`
- `/usr/local/lib/libchronos_core.so`

> No sudo / no install? Run directly from the build directory:
> `alias chronos=/path/to/CHRONOS/build/chronos`

---

## Quick start

```bash
# 1. Initialize in your project
cd /path/to/your/project
chronos init

# 2. Configure LLM (choose ONE profile)
chronos config set llm.provider auto
chronos config set llm.local.url http://localhost:11434
chronos config set llm.local.key ollama
chronos config set llm.local.model llama3.1:8b

# 3. Index your codebase
chronos sync

# 4. Start using
chronos ask "how does authentication work?"
chronos explain login_handler --query "what validation does it do?"
chronos map login_handler --downstream
chronos status
```

---

## LLM configuration

Chronos uses a dual-profile, auto-switching setup:

| `llm.provider` | Behavior                              |
|----------------|---------------------------------------|
| `auto` (default) | Cloud first, fall back to local    |
| `local`        | Always use local Ollama               |
| `cloud`        | Always use the cloud endpoint         |

```bash
# Local profile (Ollama / llama.cpp / OpenAI-compatible local)
chronos config set llm.local.url http://localhost:11434
chronos config set llm.local.key ollama
chronos config set llm.local.model llama3.1:8b

# Cloud profile (NVIDIA / OpenRouter / OpenAI / custom)
chronos config set llm.cloud.url https://api.openai.com/v1
chronos config set llm.cloud.key sk-...
chronos config set llm.cloud.model gpt-4o

# View current config
chronos config list
```

---

## Common workflows

```bash
# After pulling changes, repair index if hook was bypassed
chronos sync

# Debug a crash from CI logs
chronos diagnose --trace build.log --top 10

# See what a function calls (downstream) or what calls it (upstream)
chronos map my_function --both --depth 2

# Export graph for visualization
chronos export --format mermaid > graph.mmd

# Inspect what context the LLM was given for a trace
chronos trace crash-a116c6e0

# Prevent temporal collisions before committing
chronos check-staging --strict

# Generate an AI commit message for your staged changes
git add -A
chronos commit          # review + confirm, or:
chronos commit --yes    # auto-accept (non-interactive)
```

---

## Commands overview

| Command | Purpose |
|---------|---------|
| `init` | Initialize `.chronos/`, install git hook |
| `sync` | Index codebase → parse ASTs → build graph & vector index |
| `ask "..."` | Conceptual questions (Discovery Mode) |
| `explain <symbol>` | Symbol-level detail (Surgery Mode) |
| `map <symbol>` | Upstream/downstream call graph |
| `diagnose --trace <log>` | Ghost Bug Diagnosis from stack traces |
| `trace <traceId>` | Inspect the context graph for a trace ID |
| `timeline <path>` | Structural mutation history for a file/symbol |
| `check-staging` | Temporal Collision prevention |
| `commit` | AI-generated Conventional Commit message from staged changes |
| `status` | Daemon health, graph size, system state |
| `config` | Manage LLM keys, providers, environment |
| `export` | Dump structural graph to JSON or Mermaid |
| `clean` | Destroy local index, free disk space |

Full help is available in [help.md](help.md) or with `chronos --help`. Per-command help: `chronos <command> --help`.

---

## Project structure

```
src/          C++ sources (core engine + CLI)
include/      Public headers (chronos/)
docs/         Design and architecture docs
scripts/      Helper scripts (indexer, daemon, hooks)
tests/        Test suite
external/     Vendored dependencies (tree-sitter grammars)
install.sh    Install helper
```

---

## License

See the repository for license details.

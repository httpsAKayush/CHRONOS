```text
┌──────────────────────────────────────────────────────────────────────────────┐
│  Chronos — The Temporal Codebase Engine                                      │
│  Understand any codebase. Ask questions. Get precise, cited answers.         │
└──────────────────────────────────────────────────────────────────────────────┘

USAGE
    chronos <command> [options] [arguments]

CORE COMMANDS
    init                 Initialize .chronos/ in current repo, install git hook
                         Dot-folders (.agent, .vscode) are auto-skipped.
    sync                 Index codebase → parse ASTs → build graph & vector index
    ask "question"       Conceptual questions with Structural Graph Expansion
                         chronos ask "where is the connection pooler configured?"
    chat                 Start an interactive stateful Code-Aware REPL session
                         chronos chat --list                 # View past sessions
                         chronos chat --session <id>         # Resume a session
                         chronos chat --session <id> --no-history # Start fresh screen
    explain <target>     Cross-linked deep-dive into a file, class, or function
                         chronos explain serve/tcp_server.py
                         chronos explain agent/decision.py::QLearningDecision
                         chronos explain initialize_multicast
    map <symbol>         Architectural X-Ray — upstream/downstream call graph
                         chronos map initialize_multicast --both --depth 3
    diagnose             Ghost Bug Diagnosis — walk backward from stack trace
                         chronos diagnose --trace crash.log --top 5
    commit               AI commit message for staged changes (Conventional Commits)
                         chronos commit --all --amend

SYSTEM MANAGEMENT
    status               Daemon health, graph size, system state
    config               Manage LLM keys, providers, environment
    export               Dump structural graph to JSON or Mermaid
    clean                Destroy local index, free disk space

──────────────────────────────────────────────────────────────────────────────
FIRST-TIME SETUP (Recommended Sequence)
──────────────────────────────────────────────────────────────────────────────

# 1. BUILD & INSTALL
    # Clone the repository and compile using CMake:
    git clone -b res https://github.com/httpsAKayush/CHRONOS.git
    cd CHRONOS && mkdir build && cd build
    cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc)

    # Best practice: Symlink to your local bin instead of global install
    mkdir -p ~/.local/bin
    ln -s $(pwd)/chronos ~/.local/bin/chronos
    # (Ensure ~/.local/bin is in your PATH)

# 2. CONFIGURE LLM (Local or Cloud)
    # The default provider is 'auto' (Cloud first, local fallback).

    # ── LOCAL (Ollama, offline, fast) ────────────────────────────────
    chronos config set llm.local.url http://localhost:11434
    chronos config set llm.local.model llama3.1:8b

    # ── CLOUD (OpenAI / OpenRouter / NVIDIA) ─────────────────────────
    chronos config set llm.cloud.url https://api.openai.com/v1
    chronos config set llm.cloud.key sk-...
    chronos config set llm.cloud.model gpt-4o

# 3. INITIALIZE IN YOUR PROJECT
    cd /path/to/your/project
    chronos init
    # Creates .chronos/, adds to .gitignore, installs pre-commit hook

# 4. INDEX YOUR CODEBASE
    chronos sync
    # First run builds the structural graph and semantic index.

# 5. START USING
    chronos chat

──────────────────────────────────────────────────────────────────────────────
HOW RETRIEVAL WORKS (chronos ask & chronos chat)
──────────────────────────────────────────────────────────────────────────────

    chronos uses a 4-step HyDE RAG pipeline:
    Step 0 — Query Rewriting (Stateful Sessions)
             Resolves pronouns via Delta Context Assembly on local LLMs.
    Step 1 — Repo Summary Fetch
             Grabs the architectural [GLOBAL:REPO] summary.
    Step 2 — HyDE Generation
             LLM predicts code based on summary. Corrects hallucination.
    Step 3 — Hybrid Search (Dense + Sparse + Structural)
             Fuses Vector hits and FTS5 keyword hits.
             Applies Structural Graph Expansion (PPR walk from anchors).
    Step 4 — Synthesis
             Assembles 32k chars of context. Cites every claim with [node:id].

──────────────────────────────────────────────────────────────────────────────
AI COMMIT MESSAGES (chronos commit)
──────────────────────────────────────────────────────────────────────────────

    Writes a Conventional Commit message for staged changes.
    git add -A && chronos commit
    
    Options:
      --all     Stage everything first (tracked + untracked)
      --amend   Amend the last commit instead of creating a new one

──────────────────────────────────────────────────────────────────────────────
Run `chronos <command> --help` for specific command options.
```

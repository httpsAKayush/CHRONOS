
┌──────────────────────────────────────────────────────────────────────────────┐
│  Chronos — The Temporal Codebase Engine                                      │
│  Understand any codebase. Ask questions. Get precise, cited answers.         │
└──────────────────────────────────────────────────────────────────────────────┘

USAGE
    chronos <command> [options] [arguments]

CORE COMMANDS
    init                 Initialize .chronos/ in current repo, install git hook
    sync                 Index codebase → parse ASTs → build graph & vector index
    ask "question"       Conceptual questions (Discovery Mode)
                         chronos ask "where is the connection pooler configured?"
                         chronos ask "why is the UDP multicast failing?" --crash crash.log
    explain <symbol>     Symbol-level detail (Surgery Mode)
                         chronos explain initialize_multicast
                         chronos explain serve/tcp_server.py --query "what port does this bind to?"
    map <symbol>         Architectural X-Ray — upstream/downstream call graph
                         chronos map initialize_multicast --downstream
                         chronos map initialize_multicast --upstream
                         chronos map initialize_multicast --both --depth 3
    diagnose --trace <log>  Ghost Bug Diagnosis — walk backward from stack trace
                         chronos diagnose --trace crash.log --top 5
    trace <traceId>      Inspect exact context graph LLM was given for a trace ID
                         chronos trace crash-a116c6e0
    timeline <path>      History of structural mutations for file/symbol
                         chronos timeline serve/tcp_server.py
    check-staging        Prevent "Temporal Collisions" before committing
                         chronos check-staging --strict
    commit               AI commit message for staged changes (Conventional Commits)
                         chronos commit
                         chronos commit --all --amend

SYSTEM MANAGEMENT
    status               Daemon health, graph size, system state
    config               Manage LLM keys, providers, environment
    export               Dump structural graph to JSON or Mermaid
    clean                Destroy local index, free disk space

──────────────────────────────────────────────────────────────────────────────
FIRST-TIME SETUP (copy-paste this sequence)
──────────────────────────────────────────────────────────────────────────────

# 1. INSTALL DEPENDENCIES
    # Ubuntu/Debian:
    sudo apt update && sudo apt install -y \
        cmake g++ git sqlite3 libsqlite3-dev \
        libtree-sitter-dev tree-sitter \
        python3 python3-pip

    # Arch/Manjaro:
    sudo pacman -S cmake gcc git sqlite tree-sitter python python-pip

    # Fedora:
    sudo dnf install cmake gcc-c++ git sqlite-devel tree-sitter python3 python3-pip

    # macOS (Homebrew):
    brew install cmake git sqlite tree-sitter python

    # tree-sitter grammars for your languages (C++, Python, etc.):
    pip install --break-system-packages tree-sitter-cpp tree-sitter-python \
        tree-sitter-javascript tree-sitter-css

# 2. BUILD & INSTALL
    git clone -b res https://github.com/httpsAKayush/CHRONOS.git
    cd CHRONOS
    mkdir -p build && cd build
    cmake .. -DCMAKE_BUILD_TYPE=Release
    make -j$(nproc)
    sudo make install
    # This installs: /usr/local/bin/chronos  /usr/local/lib/libchronos_core.so

# 3. INITIALIZE IN YOUR PROJECT
    cd /path/to/your/project
    chronos init
    # Creates .chronos/, adds to .gitignore, installs pre-commit hook

# 4. CONFIGURE LLM (choose ONE profile)
    # ── LOCAL (Ollama, free, offline, fast) ───────────────────────────────
    # Install Ollama first: https://ollama.com
    curl -fsSL https://ollama.com/install.sh | sh
    ollama pull llama3.1:8b   # or mistral, codellama, etc.

    chronos config set llm.provider auto
    chronos config set llm.local.url http://localhost:11434
    chronos config set llm.local.key ollama
    chronos config set llm.local.model llama3.1:8b

    # ── CLOUD (NVIDIA / OpenRouter / OpenAI) ──────────────────────────────
    # Get API key from: https://platform.openai.com  or  https://openrouter.ai  or  https://build.nvidia.com
    chronos config set llm.provider auto
    chronos config set llm.cloud.url https://api.openai.com/v1
    chronos config set llm.cloud.key sk-...
    chronos config set llm.cloud.model gpt-4o

    # provider modes: auto (cloud first, local fallback) | local | cloud

# 5. INDEX YOUR CODEBASE
    chronos sync
    # First run takes 10–60s depending on repo size. Subsequent runs are instant.

# 6. START USING
    chronos ask "how does authentication work?"
    chronos explain login_handler --query "what validation does it do?"
    chronos map login_handler --downstream
    chronos status

──────────────────────────────────────────────────────────────────────────────
LLM CONFIGURATION (dual-profile, auto-switching)
──────────────────────────────────────────────────────────────────────────────

    llm.provider = auto     Cloud first, fall back to local (default)
    llm.provider = local    Always use local Ollama
    llm.provider = cloud    Always use cloud endpoint

    # Local profile (Ollama / llama.cpp / OpenAI-compatible local):
    chronos config set llm.local.url http://localhost:11434
    chronos config set llm.local.key ollama
    chronos config set llm.local.model llama3.1:8b

    # Cloud profile (NVIDIA / OpenRouter / OpenAI / custom):
    chronos config set llm.cloud.url https://api.openai.com/v1
    chronos config set llm.cloud.key sk-...
    chronos config set llm.cloud.model gpt-4o

    # View current config:
    chronos config list

──────────────────────────────────────────────────────────────────────────────
COMMON WORKFLOWS
──────────────────────────────────────────────────────────────────────────────

    # After pulling changes, repair index if hook was bypassed:
    chronos sync

    # Debug a crash from CI logs:
    chronos diagnose --trace build.log --top 10

    # See what a function calls (downstream) or what calls it (upstream):
    chronos map my_function --both --depth 2

    # Export graph for visualization:
    chronos export --format mermaid > graph.mmd

    # Let AI write your commit message:
    git add -A && chronos commit

──────────────────────────────────────────────────────────────────────────────
AI COMMIT MESSAGES (chronos commit)
──────────────────────────────────────────────────────────────────────────────

    Writes a Conventional Commit message for your staged changes and commits
    them for you — no more staring at a blank terminal after coding.

    # 1. Stage the changes you want to commit:
    git add -A
    #    (or `git add <file>...` for only some changes)

    # 2. Generate the message and commit:
    chronos commit

    What happens under the hood:
      [1/3] Collecting staged changes...   → runs `git diff --cached`
      [2/3] Waking LLM daemon...           → starts chronos-daemon on demand
      [3/3] Generating commit message...   → your active LLM (local or cloud)
                                            reads the diff and writes the message
      --- Staged files ---
      --- Proposed commit message ---
      Accept this commit message? [Y/n/e]  → your review step

    Confirmation prompt:
      Y or Enter   Accept the message and run `git commit` right away
      n            Abort — nothing is committed, your staged changes stay intact
      e            Open $EDITOR (default vi) to rewrite the message, then commit

    Options:
      chronos commit --all     Stage everything first (tracked + untracked)
      chronos commit --amend   Amend the last commit instead of creating a new one
      chronos commit --yes     Accept without confirmation (for scripts/CI)

    Generated messages follow Conventional Commits:
      <type>(<scope>): <subject>
      e.g. feat(calc): add divide function
      types: feat, fix, refactor, docs, chore, test, perf, build, ci, style, revert
      subject: imperative mood, lowercase, under 50 chars, no trailing period

    No staged changes? chronos commit explains what to do and exits cleanly.
    LLM message looks wrong? Press n to abort or e to edit — you always see
    the message before anything is committed.

──────────────────────────────────────────────────────────────────────────────
GLOBAL INSTALLATION (make chronos available everywhere)
──────────────────────────────────────────────────────────────────────────────

    # Already covered in step 2 above:
    cd CHRONO/build && sudo make install

    # Or run directly from build without install:
    alias chronos=/path/to/CHRONO/build/chronos

──────────────────────────────────────────────────────────────────────────────
GET HELP FOR ANY COMMAND
──────────────────────────────────────────────────────────────────────────────

    chronos <command> --help
    # e.g. chronos sync --help, chronos ask --help

──────────────────────────────────────────────────────────────────────────────

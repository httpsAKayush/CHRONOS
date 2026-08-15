// chronos: the Querier CLI/TUI (Spec §5 Interaction Model). Deliberately a
// CLI, not an IDE plugin (Spec §2 non-goal), to keep zero coupling with
// heavy IDE environments.
//
// Hexagonal Architecture: this file is the composition root. It performs
// Dependency Injection — building the adapters (Config, Codex storage,
// VectorIndex, ILLMClient) once and wiring them into the Command objects.
// No business logic lives here; each subcommand is dispatched to a
// dedicated ICommand implementation.
//
// Subcommands:
//   chronos init                 -- create .chronos/, add to .gitignore, install git hook
//   chronos ask "<query>"        -- HyDE + Structural Graph Expansion + FTS5 Frequency Boost
//   chronos explain <target>     -- 5-tier cross-linked explain: file / file::fn / file::Class / symbol
//   chronos trace <traceId>      -- Spec §9 Observability: replay which nodes backed an answer
//   chronos sync                 -- Spec §11 mitigation: repair Codex state if hooks were bypassed

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "chronos/cli/commands.hpp"
#include "chronos/cli/cli_context.hpp"
#include "chronos/env.hpp"
#include "chronos/infrastructure/config.hpp"
#include "chronos/infrastructure/codex.hpp"
#include "chronos/infrastructure/vector_index.hpp"
#include "chronos/infrastructure/llm/llm_client_factory.hpp"

namespace fs = std::filesystem;
using namespace chronos;

namespace {

void printHelp() {
    std::cout << R"(
┌──────────────────────────────────────────────────────────────────────────────┐
│  Chronos — The Temporal Codebase Engine                                      │
│  Understand any codebase. Ask questions. Get precise, cited answers.         │
└──────────────────────────────────────────────────────────────────────────────┘

USAGE
    chronos <command> [options] [arguments]

CORE COMMANDS
    init                 Initialize .chronos/ in current repo, install git hook
                         Dot-folders (.agent, .planning, .vscode, etc.) are auto-skipped
    sync                 Index codebase → parse ASTs → build graph & vector index
    ask "question"       Conceptual questions with Structural Graph Expansion (Discovery Mode)
                         chronos ask "where is the connection pooler configured?"
                         chronos ask "how does learn() persist state?"
                         chronos ask "what is phase 3 and who calls it?"
    chat                 Start an interactive stateful Code-Aware REPL session
                         chronos chat --list                 # View past sessions
                         chronos chat --session <id>         # Resume a session
    explain <target>     Cross-linked deep-dive into a file, class, or function (Surgery Mode)
                         -- Whole file:
                         chronos explain agent/decision.py
                         chronos explain serve/tcp_server.py --query "what port does this bind to?"
                         -- Specific function (follows CALLS edges into other files):
                         chronos explain agent/decision.py::learn
                         chronos explain agent/decision.py::learn --query "how does it persist state?"
                         -- Specific class (pulls all methods + cross-links):
                         chronos explain agent/decision.py::QLearningDecision
                         -- Bare symbol name (resolves across the whole repo):
                         chronos explain QLearningDecision
                         chronos explain initialize_multicast
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
    # Note: dot-folders (.agent, .planning, .vscode, .git, etc.) are
    # automatically skipped during indexing.

# 4. CONFIGURE LLM (choose ONE profile)
    # ── LOCAL (Ollama, free, offline, fast) ──────────────────────────────────────────
    # Install Ollama first: https://ollama.com
    curl -fsSL https://ollama.com/install.sh | sh
    ollama pull llama3.1:8b   # or mistral, codellama, etc.

    chronos config set llm.provider auto
    chronos config set llm.local.url http://localhost:11434
    chronos config set llm.local.key ollama
    chronos config set llm.local.model llama3.1:8b

    # ── CLOUD (NVIDIA / OpenRouter / OpenAI) ──────────────────────────────────
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
    chronos chat   # for stateful conversations
    chronos explain agent/login_handler.py --query "what validation does it do?"
    chronos explain agent/login_handler.py::LoginHandler   # class + all methods
    chronos map login_handler --downstream
    chronos status

──────────────────────────────────────────────────────────────────────────────
──────────────────────────────────────────────────────────────────────────────
HOW RETRIEVAL WORKS (chronos ask & chronos chat)
──────────────────────────────────────────────────────────────────────────────

    chronos uses a 4-step HyDE RAG pipeline, available in two modes:
    1. chronos ask: Deterministic, stateless, one-shot searches.
    2. chronos chat: Stateful, interactive REPL that remembers context.

    Step 0 — Query Rewriting (Stateful Sessions Only - chronos chat)
             Uses conversation history to intelligently resolve pronouns in
             follow-up questions (e.g., "what does it do?"). Leverages Delta
             Context Assembly to reuse KV caches and save tokens on local LLMs.

    Step 1 — Repo Summary Fetch
             Grabs the architectural [GLOBAL:REPO] summary for context.

    Step 2 — HyDE Hypothetical Generation
             Asks the LLM to generate a hypothetical code answer based on
             the repo summary. Corrects for semantic hallucination drift.

    Step 3 — Hybrid Search (Dense + Sparse + Structural)
             a) Dense vector search (HyDE-embedded query)
             b) FTS5 keyword search on code signatures
             c) RRF fusion of both channels
             d) Structural Graph Expansion (NEW):
                • Explicit mentions: "decision file" → boosts all decision.py nodes
                • FTS5 frequency: files appearing 3+ times in keyword hits become
                  implicit anchors (catches "phase 3 learning" → e2e_learning_flow.py)
                • Local-Push PPR walk from anchors to surface callers/callees

    Step 4 — Synthesis
             Assembles up to 32,000 chars of grounded code context.
             Each node snippet capped at 8,000 chars (full function bodies).
             Cites every claim with [node:id] references.

──────────────────────────────────────────────────────────────────────────────
HOW EXPLAIN WORKS (chronos explain)
──────────────────────────────────────────────────────────────────────────────

    explain uses a 5-tier context assembly (NOT semantic search):

    TIER 1 — File header (first 3,000 chars: imports, module docstring)
    TIER 2 — Primary nodes (full code of exactly what was requested)
    TIER 3 — CONTAINS children (all methods when a class is requested)
    TIER 4 — CALLS cross-links (callees in other files, e.g. dao::set_q)
    TIER 5 — IMPORTS file headers + repo architectural summary

    Target format table:
      file.py                  → all AST nodes in the file
      file.py::function_name   → function + cross-linked callees
      file.py::ClassName       → class + all its methods + cross-links
      SymbolName               → resolved by alias or FTS5 fallback

    Context budget: 32,000 chars / 8k tokens (10k per snippet).
    ask vs explain:
      chronos ask  — semantic search across the whole repo, best for discovery
      chronos explain — scope-locked to a target, best for deep dives

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

    # Ask a stateless conceptual question across the whole codebase:
    chronos ask "how does the Q-learning agent persist its state?"
    chronos ask "what is phase 3 and who calls it?"
    chronos ask "where is the connection pooler configured?"

    # Start an interactive stateful code-aware REPL:
    chronos chat
    chronos chat --list                 # list past sessions
    chronos chat --session session-123  # resume a past session

    # Explain a specific file / class / function:
    chronos explain agent/decision.py
    chronos explain agent/decision.py::QLearningDecision
    chronos explain agent/decision.py::learn --query "how does it persist state?"

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
)";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]) == "help" || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
        printHelp();
        return (argc < 2) ? 2 : 0;
    }
    std::string cmd = argv[1];
    std::string repoRoot = fs::current_path().string();

    auto env = loadEnv(repoRoot);
    for (const auto& [k, v] : env) {
        setenv(k.c_str(), v.c_str(), 1);
    }

    Config globalConfig;

    auto llm = createLLMClient(globalConfig);

    CliContext ctx{
        .repoRoot = repoRoot,
        .config = std::make_shared<Config>(globalConfig),
        .storage = std::make_shared<Codex>(repoRoot),
        .vectors = std::make_shared<VectorIndex>(repoRoot),
        .llm = std::move(llm)
    };

    std::unordered_map<std::string, std::unique_ptr<ICommand>> commands;
    commands["init"] = std::make_unique<CmdInit>(ctx);
    commands["sync"] = std::make_unique<CmdSync>(ctx);
    commands["ask"] = std::make_unique<CmdAsk>(ctx);
    commands["chat"] = std::make_unique<CmdChat>(ctx);
    commands["explain"] = std::make_unique<CmdExplain>(ctx);
    commands["map"] = std::make_unique<CmdMap>(ctx);
    commands["diagnose"] = std::make_unique<CmdDiagnose>(ctx);
    commands["trace"] = std::make_unique<CmdTrace>(ctx);
    commands["timeline"] = std::make_unique<CmdTimeline>(ctx);
    commands["check-staging"] = std::make_unique<CmdCheckStaging>(ctx);
    commands["status"] = std::make_unique<CmdStatus>(ctx);
    commands["config"] = std::make_unique<CmdConfig>(ctx);
    commands["export"] = std::make_unique<CmdExport>(ctx);
    commands["clean"] = std::make_unique<CmdClean>(ctx);
    commands["commit"] = std::make_unique<CmdCommit>(ctx);

    auto it = commands.find(cmd);
    if (it == commands.end()) {
        std::cerr << "Unknown command: " << cmd << "\n";
        printHelp();
        return 1;
    }

    return it->second->execute(argc, argv);
}
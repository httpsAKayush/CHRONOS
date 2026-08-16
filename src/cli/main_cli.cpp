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

void printColoredHelp(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '@' && i + 3 < text.size() && text[i+3] == '@') {
            std::string tag = text.substr(i+1, 2);
            if (tag == "CY") { out += "[1;36m"; i+=3; }
            else if (tag == "MG") { out += "[1;35m"; i+=3; }
            else if (tag == "YL") { out += "[1;33m"; i+=3; }
            else if (tag == "GR") { out += "[1;32m"; i+=3; }
            else if (tag == "BL") { out += "[1;34m"; i+=3; }
            else if (tag == "RE") { out += "[0m"; i+=3; }
            else if (tag == "BO") { out += "[1;37m"; i+=3; }
            else if (tag == "DI") { out += "[3;90m"; i+=3; }
            else out += text[i];
        } else {
            out += text[i];
        }
    }
    std::cout << out << "[0m";
}

void printHelp() {
    printColoredHelp(R"(
@BL@┌──────────────────────────────────────────────────────────────────────────────┐@RE@
@BL@│@RE@  @CY@Chronos@RE@ — The Temporal Codebase Engine                                      @BL@│@RE@
@BL@│@RE@  Understand any codebase. Ask questions. Get precise, cited answers.         @BL@│@RE@
@BL@└──────────────────────────────────────────────────────────────────────────────┘@RE@

@MG@USAGE@RE@
    @GR@chronos@RE@ <command> [options] [arguments]

@MG@CORE COMMANDS@RE@
    @CY@init@RE@                 Initialize .chronos/ in current repo, install git hook
                         @DI@Dot-folders (.agent, .vscode) are auto-skipped.@RE@
    @CY@sync@RE@                 Index codebase → parse ASTs → build graph & vector index
    @CY@ask@RE@ "question"       Conceptual questions with Structural Graph Expansion
                         @DI@chronos ask "where is the connection pooler configured?"@RE@
    @CY@chat@RE@                 Start an interactive stateful Code-Aware REPL session
                         @DI@chronos chat --list                 # View past sessions@RE@
                         @DI@chronos chat --session <id>         # Resume a session@RE@
                         @DI@chronos chat --session <id> --no-history # Start fresh screen@RE@
    @CY@explain@RE@ <target>     Cross-linked deep-dive into a file, class, or function
                         @DI@chronos explain serve/tcp_server.py@RE@
                         @DI@chronos explain agent/decision.py::QLearningDecision@RE@
                         @DI@chronos explain initialize_multicast@RE@
    @CY@map@RE@ <symbol>         Architectural X-Ray — upstream/downstream call graph
                         @DI@chronos map initialize_multicast --both --depth 3@RE@
    @CY@diagnose@RE@             Ghost Bug Diagnosis — walk backward from stack trace
                         @DI@chronos diagnose --trace crash.log --top 5@RE@
    @CY@commit@RE@               AI commit message for staged changes (Conventional Commits)
                         @DI@chronos commit --all --amend@RE@

@MG@SYSTEM MANAGEMENT@RE@
    @CY@status@RE@               Daemon health, graph size, system state
    @CY@config@RE@               Manage LLM keys, providers, environment
    @CY@export@RE@               Dump structural graph to JSON or Mermaid
    @CY@clean@RE@                Destroy local index, free disk space

@BL@──────────────────────────────────────────────────────────────────────────────@RE@
@BO@FIRST-TIME SETUP@RE@ @DI@(Recommended Sequence)@RE@
@BL@──────────────────────────────────────────────────────────────────────────────@RE@

@YL@# 1. BUILD & INSTALL@RE@
    @DI@# Clone the repository and install locally (no sudo required!):@RE@
    @GR@git clone -b res https://github.com/httpsAKayush/CHRONOS.git@RE@
    @GR@cd CHRONOS && mkdir build && cd build@RE@
    @GR@cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=~/.local@RE@
    @GR@make -j$(nproc) && make install@RE@
    @DI@# This safely installs chronos to ~/.local/bin, meaning you can@RE@
    @DI@# safely rename or move the CHRONOS source folder later!@RE@

@YL@# 2. CONFIGURE LLM (Local or Cloud)@RE@
    @DI@# The default provider is 'auto' (Cloud first, local fallback).@RE@

    @DI@# ── LOCAL (Ollama, offline, fast) ────────────────────────────────@RE@
    @GR@chronos config set llm.local.url http://localhost:11434@RE@
    @GR@chronos config set llm.local.model llama3.1:8b@RE@

    @DI@# ── CLOUD (OpenAI / OpenRouter / NVIDIA) ─────────────────────────@RE@
    @GR@chronos config set llm.cloud.url https://api.openai.com/v1@RE@
    @GR@chronos config set llm.cloud.key sk-...@RE@
    @GR@chronos config set llm.cloud.model gpt-4o@RE@

@YL@# 3. INITIALIZE IN YOUR PROJECT@RE@
    @GR@cd /path/to/your/project@RE@
    @GR@chronos init@RE@
    @DI@# Creates .chronos/, adds to .gitignore, installs pre-commit hook@RE@

@YL@# 4. INDEX YOUR CODEBASE@RE@
    @GR@chronos sync@RE@
    @DI@# First run builds the structural graph and semantic index.@RE@

@YL@# 5. START USING@RE@
    @GR@chronos chat@RE@

@BL@──────────────────────────────────────────────────────────────────────────────@RE@
@BO@HOW RETRIEVAL WORKS (chronos ask & chronos chat)@RE@
@BL@──────────────────────────────────────────────────────────────────────────────@RE@

    @CY@chronos@RE@ uses a 4-step HyDE RAG pipeline:
    @CY@Step 0@RE@ — @MG@Query Rewriting@RE@ (Stateful Sessions)
             Resolves pronouns via Delta Context Assembly on local LLMs.
    @CY@Step 1@RE@ — @MG@Repo Summary Fetch@RE@
             Grabs the architectural [GLOBAL:REPO] summary.
    @CY@Step 2@RE@ — @MG@HyDE Generation@RE@
             LLM predicts code based on summary. Corrects hallucination.
    @CY@Step 3@RE@ — @MG@Hybrid Search (Dense + Sparse + Structural)@RE@
             Fuses Vector hits and FTS5 keyword hits.
             Applies Structural Graph Expansion (PPR walk from anchors).
    @CY@Step 4@RE@ — @MG@Synthesis@RE@
             Assembles 32k chars of context. Cites every claim with [node:id].

@BL@──────────────────────────────────────────────────────────────────────────────@RE@
@BO@AI COMMIT MESSAGES (chronos commit)@RE@
@BL@──────────────────────────────────────────────────────────────────────────────@RE@

    Writes a Conventional Commit message for staged changes.
    @GR@git add -A && chronos commit@RE@
    
    Options:
      @GR@--all@RE@     Stage everything first (tracked + untracked)
      @GR@--amend@RE@   Amend the last commit instead of creating a new one

@BL@──────────────────────────────────────────────────────────────────────────────@RE@
@DI@Run `chronos <command> --help` for specific command options.@RE@
)");
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
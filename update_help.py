import re

with open('src/cli/main_cli.cpp', 'r') as f:
    content = f.read()

new_help = r'''void printColoredHelp(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '@' && i + 3 < text.size() && text[i+3] == '@') {
            std::string tag = text.substr(i+1, 2);
            if (tag == "CY") { out += "\033[1;36m"; i+=3; }
            else if (tag == "MG") { out += "\033[1;35m"; i+=3; }
            else if (tag == "YL") { out += "\033[1;33m"; i+=3; }
            else if (tag == "GR") { out += "\033[1;32m"; i+=3; }
            else if (tag == "BL") { out += "\033[1;34m"; i+=3; }
            else if (tag == "RE") { out += "\033[0m"; i+=3; }
            else if (tag == "BO") { out += "\033[1;37m"; i+=3; }
            else if (tag == "DI") { out += "\033[3;90m"; i+=3; }
            else out += text[i];
        } else {
            out += text[i];
        }
    }
    std::cout << out << "\033[0m";
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
    @DI@# Clone the repository and compile using CMake:@RE@
    @GR@git clone https://github.com/httpsAKayush/CHRONOS.git@RE@
    @GR@cd CHRONOS && mkdir build && cd build@RE@
    @GR@cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc)@RE@

    @DI@# Best practice: Symlink to your local bin instead of global install@RE@
    @GR@mkdir -p ~/.local/bin@RE@
    @GR@ln -s $(pwd)/chronos ~/.local/bin/chronos@RE@
    @DI@# (Ensure ~/.local/bin is in your PATH)@RE@

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
}'''

# Replace the existing printHelp() implementation
# Find the start of void printHelp() {
# and the end of the namespace anon block
pattern = re.compile(r'void printHelp\(\) \{.*?\n\}', re.DOTALL)
content = pattern.sub(new_help, content)

with open('src/cli/main_cli.cpp', 'w') as f:
    f.write(content)

print("Help text updated.")

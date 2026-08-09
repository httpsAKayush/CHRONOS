# Original User Request

## 2026-08-07T08:33:52Z

Implement all remaining features and logic discussed in `project_context.md` into the Chronos C++ engine, ensuring it fully functions as a Temporal Codebase Engine that detects architectural drift and resolves "ghost bugs." 

Working directory: /home/zer0/CHRONO
Integrity mode: development

## Requirements

### R1. Implement Missing Core Logic (C++ Engine & CLI)
Audit the codebase against `project_context.md` and implement missing concepts into the C++ engine. Focus strictly on the C++ daemon and CLI tools (exclude IDE extensions). Key targets include:
- The Temporal Recency Algorithm (time-decay penalty in retrieval).
- Aggressive Git Noise Filtering (ignoring whitespace-only AST changes).
- The Staging Area Check (pre-commit hook for temporal collision warnings).
- Tiered Embedding Memory and SQ8 Vector Quantization.

### R2. Continuous Self-Evaluation
Loop through the project to debug, test, and reimplement features until they exactly match the scenarios described in the context document. You must evaluate your work objectively.

## Verification Resources
- Use the existing `test_chronos.sh` script to verify basic functionality.

## Acceptance Criteria

### Feature Completeness
- [ ] The engine correctly applies a time-decay penalty during retrieval, prioritizing recent commits while keeping old ones accessible.
- [ ] The engine explicitly ignores commits that only contain whitespace or comment changes via AST comparison.
- [ ] A functional pre-commit hook intercepts and warns the user of historical constraints before they commit.
- [ ] Running `test_chronos.sh` passes successfully after all modifications.

## Follow-up — 2026-08-07T16:25:00Z

Implement the flagship "Ghost Bug" diagnosis engine (`chronos diagnose`) for Project Chronos. This engine must ingest a raw stack trace, walk backward through the structural dependency graph, and surface historical commits that caused the failure, rather than just returning standard RAG search results.

Working directory: /home/zer0/CHRONO
Integrity mode: development

## Requirements

### R1. Implement `chronos diagnose` (The Diagnosis Engine)
Build the `chronos diagnose --trace <crash_log>` CLI command. It must parse the crash log, resolve frames to Codex nodes, and walk OUTWARD through structural dependencies to find related nodes. For each touched node, pull historical commits and rank them using a combination of structural proximity, temporal recency, AST mutation score, and deferred-work language detection ("temporary", "hack", "will revert").

### R2. Dual-Horizon PPR & Retrieval Enhancements
Ensure the retrieval pipeline uses Hop 2 (Structural expansion) with Local-push (Andersen-style) Personalized PageRank. Implement Reciprocal Rank Fusion (RRF) to combine semantic and structural ranks, and apply connectivity-based MMR for diversity pruning to prevent a single heavily-connected class from monopolizing the context budget.

### R3. Build the Acceptance Fixture
Create a synthetic Git repository with ~10 commits that exactly reproduces the "connection pooler" scenario: a crash in `query.cpp` (untouched for 3 months) caused by a commit in its dependency `connection_pool.cpp` (modified 6 weeks ago with a "temporary hack" message).

## Acceptance Criteria

### Acceptance Test (The Core Requirement)
- [ ] Running `chronos diagnose --trace <crash_log>` against the synthetic fixture repo MUST surface the `connection_pool.cpp` "temporary hack" commit as a top-ranked candidate.
- [ ] The output MUST explicitly show the verbatim commit message and the structural dependency path from the crash frame to that commit.

## Follow-up — 2026-08-09T04:11:47Z

You are acting as a Staff/Principal C++ Engineer. We are now executing the 'Enterprise Refactor Phase' for Chronos. Our goal is to transform the current working codebase into a highly modular, decoupled, and scalable C++ system following Domain-Driven Design (DDD), Hexagonal Architecture (Ports and Adapters), and SOLID principles.

I have assessed the current state of the repository. We have a 1600+ line main_cli.cpp, stray Python scripts, and tightly coupled database/LLM logic. We will systematically dismantle this monolith without breaking existing functionality.

## Phase 1: The Great Cleanup & Verification Setup

Before gutting the core logic, we must establish a safety net to objectively verify that this architectural refactor does not break core functionality.

- Clean the Root: Move test.json, test.mermaid, patch_cli.py, recover.py, recover2.py, recovered.txt, cmdDiagnose_recovery.txt, test.py, and gdb_commands.txt to a .dev_trash/ folder.
- Establish Snapshot Parity: Before changing C++ code, we will write a quick Bash integration test (tests/e2e_parity.sh) that captures the exact stdout of chronos status and chronos sync on a dummy repository. This snapshot will be our objective source of truth. If the refactored engine's output deviates from the snapshot, the build fails.

## Phase 2: The Target Enterprise C++ Directory Structure

We will reorganize the monolithic src/ and include/ directories into a strict, domain-driven layout:

CHRONOS/
├── CMakeLists.txt           # Modern target-based CMake linking
├── include/chronos/
│   ├── core/                # Interfaces/Ports (ILLMClient, IStorage, IASTParser, IGitScanner)
│   ├── domain/              # Pure business logic (MMR, ASTMutationScorer, ContextBuilder)
│   ├── use_cases/           # Orchestrators (DiagnoseEngine, SyncManager)
│   └── infrastructure/      # Adapter declarations (SQLiteStorage, OllamaClient)
├── src/
│   ├── cli/                 # The CLI Layer
│   │   ├── main_cli.cpp     # Thin entry point (parses argv/argc)
│   │   ├── router.cpp       # Routes to Command implementations
│   │   └── commands/        # CmdSync, CmdDiagnose, CmdStatus, CmdConfig
│   ├── daemon/              # Background IPC and Daemon management
│   ├── infrastructure/      # Concrete Adapters
│   │   ├── storage/         # codex.cpp (SQLite implementation)
│   │   ├── llm/             # llm_config.cpp, ollama_client.cpp
│   │   └── vcs/             # git_indexer.cpp (LibGit2 implementation)
│   └── indexing/            # AST parsers and Tree-sitter routers
└── tests/                   # Unit Tests (Mocking interfaces using Catch2/GTest)

## Phase 3: Core Design Patterns & Constraints

You must strictly enforce the following patterns during the rewrite:
- Hexagonal Architecture (Ports and Adapters): Domain logic (diagnose.cpp, ast_mutation_scorer.cpp) must NEVER execute raw SQL, touch the filesystem, or make cURL requests. They will communicate strictly through pure virtual interfaces (IStorage, ILLMClient).
- Dismantling the God Class (Command Pattern): The 1,689-line main_cli.cpp will be gutted. Each command (e.g., chronos sync) will get its own dedicated class inheriting from an ICommand interface.
- Dependency Injection (DI): main_cli.cpp will instantiate the concrete adapters (e.g., SQLiteStorage, ConfigManager) and inject them via constructor into the Use Cases. No global singletons for stateful services.
- Observer Pattern for UI: The core indexing engine must contain ZERO std::cout statements. It will emit SyncProgressEvent objects. The CLI layer will observe these events to render the dynamic TTY progress bar.
- Graceful Exception Handling: Replace all raw exit(1) and segmentation faults with standard C++ exceptions (std::runtime_error, chronos::DatabaseException). Catch these at the CLI boundary to display user-friendly error messages.

## Phase 4: Objective Verification & Testing (Crucial)

To prove the refactor is successful, we will utilize Interface Mocking:
- We will create a MockStorage class that implements IStorage in-memory.
- We will write unit tests for the DiagnoseEngine and ASTMutationScorer passing them the MockStorage. This objectively verifies the algorithms work flawlessly without touching a real SQLite database.

## Phase 5: Sequential Execution Strategy

We will refactor incrementally. Do not proceed to the next step until the current step compiles and passes the parity tests.
- Step 1: The Core Interfaces (Ports). Define IStorage.hpp, ILLMClient.hpp, IGitScanner.hpp, and IConfig.hpp with pure virtual methods.
- Step 2: The Mocks & Snapshot Setup. Implement MockStorage and write the initial parity bash script.
- Step 3: The Adapter Implementations. Refactor codex.cpp and llm_config.cpp to inherit from and fulfill the new interfaces.
- Step 4: The Domain Injection. Rewrite diagnose.cpp and ast_indexer.cpp to accept interfaces via constructor dependency injection.
- Step 5: Dismantling main_cli.cpp. Implement the Command Router, Observer UI, and split the CLI into isolated files.

Execute Step 1 now. Provide the complete, production-grade C++ header files for IStorage.hpp, ILLMClient.hpp, and IGitScanner.hpp.

## Follow-up — 2026-08-09T04:25:00Z

Execute the "Enterprise Refactor Phase" for the Chronos Temporal Codebase Engine. The goal is to transform the current working monolithic codebase into a highly modular, decoupled, and scalable C++ system following Domain-Driven Design (DDD), Hexagonal Architecture (Ports and Adapters), and SOLID principles — without breaking any existing functionality.

Working directory: /home/zer0/CHRONO
Integrity mode: development

## Current Codebase State

The engine is a working C++ 20 project with:
- **17 source files** in a flat `src/` directory (~7,164 total lines)
- **15 headers** in a flat `include/chronos/` directory
- **4 embryonic interfaces** already in `include/chronos/core/` (IStorage.hpp, ILLMClient.hpp, IGitScanner.hpp, IConfig.hpp)
- **A 1,689-line monolithic `main_cli.cpp`** that contains all CLI commands inline
- **16 unit test files** in `tests/`
- **Development artifacts** littered in root: test.json (6MB), test.mermaid, patch_cli.py, recover.py, recover2.py, recovered.txt, cmdDiagnose_recovery.txt, gdb_commands.txt, test.py, test.js, test.css
- Build system: CMake with FetchContent (libgit2, hnswlib, nlohmann_json, cpp-httplib), tree-sitter grammars via submodules
- All existing `chronos sync`, `chronos status`, `chronos ask`, `chronos map`, `chronos diagnose`, `chronos config`, `chronos export`, `chronos clean`, `chronos staging-check` commands are functional

## Requirements

### R1. Establish a Safety Net Before Refactoring
Before any source code restructuring begins, create a comprehensive integration test that captures the exact behavior of the current engine. This test must verify that `chronos sync` and `chronos status` produce consistent, correct output on a controlled dummy repository. All subsequent refactoring steps must pass this test before proceeding.

### R2. Clean the Repository Root
Move all development artifacts (test.json, test.mermaid, patch_cli.py, recover.py, recover2.py, recovered.txt, cmdDiagnose_recovery.txt, gdb_commands.txt, test.py, test.js, test.css) to a `.dev_trash/` folder so the root is clean and professional. Update `.gitignore` accordingly.

### R3. Restructure into Domain-Driven Directory Layout
Reorganize the existing source and header files into a strict domain-driven layout with clear separation:

```
include/chronos/
├── core/          # Ports (Pure Virtual Interfaces): IStorage, ILLMClient, IGitScanner, IConfig, ICommand
├── domain/        # Pure Business Logic headers: ast_mutation_scorer, mmr, ppr, rrf, simhash
├── use_cases/     # Orchestrator headers: diagnose, context_builder, oracle
└── infrastructure/# Concrete Adapter declarations: codex, vector_index, git_indexer, llm_config

src/
├── cli/           # main_cli.cpp (gutted to argv parsing only), router.cpp, commands/
├── daemon/        # main_daemon.cpp, main_indexer.cpp, ipc.cpp
├── infrastructure/
│   ├── storage/   # codex.cpp, vector_index.cpp
│   ├── llm/       # llm_config.cpp, openai_client.cpp, ollama_client.cpp
│   └── vcs/       # git_indexer.cpp
├── use_cases/     # diagnose.cpp, context_builder.cpp, oracle.cpp
└── domain/        # ast_indexer.cpp, ast_mutation_scorer.cpp, mmr.cpp, ppr.cpp, rrf.cpp, simhash.cpp
```

CMakeLists.txt must be updated to reflect the new file locations. The project must compile and all existing tests must pass after reorganization.

### R4. Enforce Hexagonal Architecture Boundaries
- **Domain logic** (diagnose.cpp, ast_mutation_scorer.cpp, mmr.cpp, ppr.cpp, rrf.cpp, simhash.cpp) must NEVER execute raw SQL queries or cURL/HTTP requests directly. They communicate strictly through the interface ports (IStorage, ILLMClient).
- **Use cases** (diagnose.cpp, context_builder.cpp, oracle.cpp) accept interfaces via constructor dependency injection.
- **Infrastructure adapters** (codex.cpp, vector_index.cpp, git_indexer.cpp, LLM clients) implement the interfaces.
- **CLI layer** (main_cli.cpp) instantiates adapters and injects them — no global singletons for stateful services.

### R5. Dismantle the God Class (main_cli.cpp)
Split the 1,689-line main_cli.cpp into the Command Pattern:
- A thin `main_cli.cpp` that only parses argc/argv
- A `router.cpp` that maps command strings to ICommand implementations
- Individual command files (CmdSync, CmdDiagnose, CmdStatus, CmdAsk, CmdMap, CmdConfig, CmdExport, CmdClean, CmdStagingCheck) each in their own file under `src/cli/commands/`
- Each command class implements an `ICommand` interface with an `execute()` method

### R6. Implement the LLM Factory Pattern
Create an LLMFactory that reads the llm_config and dynamically injects either an OpenAIClient (for cloud APIs with Bearer Token auth) or an OllamaClient (for local network calls without auth) into components that need LLM access. The factory decision should be based on the configured API URL (localhost → Ollama, otherwise → OpenAI-compatible).

## Verification Resources

- The existing `test_chronos.sh` script at `/home/zer0/CHRONO/test_chronos.sh` contains integration tests
- The existing unit tests in `tests/` cover: simhash, PPR, MMR, RRF, codex aliases, AST mutation scoring, recency, staging checks, oracle harness, vector quantization/tiering, and core interfaces
- Run existing tests with: `cd build && ctest --output-on-failure`

## Acceptance Criteria

### Build Integrity
- [ ] The project compiles cleanly with `cmake .. && make -j$(nproc)` from the build directory after all changes
- [ ] All existing unit tests pass (`ctest --output-on-failure` returns 0)
- [ ] No new compiler warnings introduced by the refactoring (beyond existing third-party warnings)

### Functional Parity
- [ ] `chronos sync` on the CHRONO repo itself produces the same structural graph (node count within ±5%) as before the refactor
- [ ] `chronos status` displays the same sections (Structural Graph, Semantic Index, Temporal Index, System Health) with correct values
- [ ] `chronos ask "what algorithm used for matching?"` returns a non-empty response
- [ ] `chronos config get llm.model` returns the configured model name
- [ ] A new integration test script (`tests/e2e_parity.sh`) exists and passes, verifying the above commands work correctly

### Architectural Compliance
- [ ] No source file in `src/domain/` or `src/use_cases/` contains `#include <sqlite3.h>`, `sqlite3_*` function calls, or raw HTTP/cURL calls
- [ ] `src/cli/main_cli.cpp` is under 100 lines (argument parsing and adapter wiring only)
- [ ] At least 8 separate command files exist under `src/cli/commands/`
- [ ] An `ICommand` interface exists with at least an `execute()` pure virtual method
- [ ] The LLM factory pattern is implemented and selects the correct client based on config

### Directory Structure
- [ ] Development artifacts are moved to `.dev_trash/` and the repo root contains only project-essential files
- [ ] Source files are organized into the subdirectory layout specified in R3
- [ ] `.gitignore` is updated to exclude `.dev_trash/`


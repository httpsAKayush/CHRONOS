# Technical Analysis & Architectural Design: Milestone 3 — Staging Area Check (Pre-Commit Hook)

## Executive Summary
Milestone 3 implements the **Staging Area Check** and **Temporal Collision Warning** system for CHRONO, fulfilling requirements R1 and R2 (`project_context.md`, lines 541–548). When a developer stages changes (`git add`) and initiates a commit (`git commit`), Chronos intercepts staged files via the pre-commit hook, extracts staged diffs, cross-references modified code/AST nodes against historical notes, commit messages, and synthetic warnings stored in the Codex database (`codex.db`) and VectorIndex, and outputs structured temporal collision warnings if staged changes violate historical constraints.

---

## 1. Existing System & File Examination

### 1.1 `scripts/pre-commit`
- **Current Behavior**: Intercepts staged C/C++/Python files via `git diff --cached --name-only` and spawns `chronos-indexer` in background detached mode (`nohup setsid ... & disown`).
- **Limitation**: It does **not** invoke any staged diff check CLI command. Developers do not receive real-time temporal collision warnings during `git commit`.

### 1.2 `src/main_cli.cpp`
- **Current Subcommands**: `init`, `sync`, `ask`, `trace`, `timeline`.
- **Limitation**: Lacks a `check-staging` (or `check-staged`) subcommand for evaluating staged code against historical notes.

### 1.3 `src/codex.cpp` & `include/chronos/codex.hpp`
- **Storage**: Table `history` (`node_id`, `commit_hash`, `timestamp`, `synthetic_msg`) stores AI synthetic commit messages and historical notes (e.g. `"DO NOT MUTEX HERE"`, `"TEMPORARILY REDUCED TIMEOUT"`).
- **Existing Methods**: `getHistory(nodeId)` fetches records for a specific node ID. `getNode(id)` retrieves node metadata.
- **Needed Feature**: A lookup method to retrieve all historical records for a file path or list of active nodes in a given file (`getHistoryForFile(filePath)` or querying `history` joined with `nodes`).

### 1.4 `src/context_builder.cpp`, `src/oracle.cpp`, `src/ast_mutation_scorer.cpp`
- **`AstMutationScorer`**: Categorizes diffs into structural scores (10: contract-breaking structural change, 1: internal logic modification, 0: formatting/comments).
- **`Oracle`**: Renders deterministic traces and citation checks.
- **`VectorIndex`**: Performs semantic search against node embeddings.

---

## 2. Requirement Gap Analysis

| Requirement (`project_context.md` lines 541-548) | Current State | Target State (Milestone 3) |
|---|---|---|
| Intercept staged files (`git diff --cached`) during pre-commit | `scripts/pre-commit` detects changed files but only triggers background indexer | `scripts/pre-commit` runs `chronos check-staging` synchronously (<500ms) before indexer |
| Cross-reference staged diffs/AST nodes against Codex historical notes & commit messages | No comparison logic exists between staged diffs and `codex.db` history | `chronos check-staging` inspects staged diffs, matches affected file nodes, and checks `history.synthetic_msg` |
| Display structured Temporal Collision Warnings on constraint violation | No warning output mechanism | Formatted, high-visibility output warning developers about specific historical conflicts |
| Test Coverage | No unit test for staging area check | `tests/test_staging_check.cpp` added to test suite verifying warning generation |

---

## 3. Proposed Technical Architecture & Design

```
+-------------------------------------------------------------------------------+
|                             git commit execution                              |
+-------------------------------------------------------------------------------+
                                       |
                                       v
+-------------------------------------------------------------------------------+
|                           scripts/pre-commit hook                             |
|  1. Runs: `chronos check-staging "$REPO_ROOT"`                                |
|  2. Detaches `chronos-indexer` in background (existing fail-open behavior)    |
+-------------------------------------------------------------------------------+
                                       |
                                       v
+-------------------------------------------------------------------------------+
|                       CLI Subcommand: `chronos check-staging`                 |
|  - Queries `git diff --cached` for staged file patches & file list             |
|  - Parses staged added/modified lines & AST nodes                             |
|  - Retrieves historical notes from Codex (`getHistoryForFile` / `getHistory`) |
|  - Evaluates AST Mutation score (`AstMutationScorer::scoreDiff`)              |
|  - Matches staged keywords against historical constraints ("MUTEX", "TIMEOUT")|
+-------------------------------------------------------------------------------+
                                       |
                                       +-----------------------+
                                       |                       |
                             Collision Detected           No Collision
                                       |                       |
                                       v                       v
                    +-----------------------+      +-----------------------+
                    | Output Structured     |      | Output: Clean staging |
                    | Temporal Warning      |      | Area clean (Exit 0)   |
                    +-----------------------+      +-----------------------+
```

### 3.1 CLI Command (`chronos check-staging`)
- Syntax: `chronos check-staging [repo_root] [--strict]`
- Reads staged diffs using `git diff --cached -U3`.
- For each modified/added staged file:
  1. Identifies staged file path and modified diff hunks.
  2. Queries Codex for historical notes associated with the file (`getHistoryForFile(filePath)` or node-level `getHistory`).
  3. Checks if staged diff lines add/modify code that conflicts with constraint keywords found in `synthetic_msg` (e.g. `MUTEX`, `LOCK`, `TIMEOUT`, `SLEEP`, `THREAD`, `DEADLOCK`, `CRITICAL`, `HAZARD`).
  4. Computes AST mutation score via `AstMutationScorer::scoreDiff`.
  5. If a collision is found, formats and prints a **Temporal Collision Warning**:
     ```
     ================================================================================
     [TEMPORAL COLLISION WARNING]
     File: src/main.cpp
     Staged Modification:
       + std::mutex mtx;
     Historical Constraint Violation:
       - Commit 4b2f1a9: "DO NOT MUTEX HERE - causes deadlock with AsyncLogger worker thread"
     Action: Please review historical constraint before committing.
     ================================================================================
     ```
- Exit codes: Returns `0` by default (fail-open mode unless `--strict` flag is passed).

### 3.2 Helper Method in `Codex` (`src/codex.cpp`)
Add `getHistoryForFile`:
```cpp
std::vector<HistoryRecord> Codex::getHistoryForFile(const std::string& filePath) {
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_,
        "SELECT h.node_id, h.commit_hash, h.timestamp, h.synthetic_msg "
        "FROM history h JOIN nodes n ON h.node_id = n.id "
        "WHERE n.file_path = ?1 ORDER BY h.timestamp DESC;",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
    std::vector<HistoryRecord> records;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        HistoryRecord rec;
        rec.nodeId = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        rec.commitHash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        rec.timestamp = sqlite3_column_int64(stmt, 2);
        const char* msg = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        if (msg) rec.syntheticMsg = msg;
        records.push_back(rec);
    }
    sqlite3_finalize(stmt);
    return records;
}
```

### 3.3 Integration in `scripts/pre-commit`
Update `scripts/pre-commit`:
```bash
# Execute Staging Area Check for Temporal Collision Warnings
if command -v chronos >/dev/null 2>&1; then
  chronos check-staging "$REPO_ROOT" || true
fi
```

### 3.4 Unit Test Harness (`tests/test_staging_check.cpp`)
Add dedicated test file `tests/test_staging_check.cpp`:
- Tests `cmdCheckStaging` / staging collision detector with mock repo & mock Codex database.
- Inserts historical note containing `"DO NOT MUTEX HERE"` for node in `src/test_sample.cpp`.
- Simulates staged diff adding `std::mutex m_mutex;`.
- Asserts collision warning output is generated and matches expected historical warning text.
- Connects test to `tests/test_main.cpp` and `tests/CMakeLists.txt`.

---

## 4. Implementation Plan for Implementer

1. **Update `include/chronos/codex.hpp` & `src/codex.cpp`**:
   - Add `std::vector<HistoryRecord> getHistoryForFile(const std::string& filePath);`
2. **Create / Update `src/main_cli.cpp`**:
   - Implement `int cmdCheckStaging(const std::string& repoRoot, bool strictMode)`
   - Add `check-staging` subcommand to CLI dispatcher in `main()`.
3. **Update `scripts/pre-commit`**:
   - Add call to `chronos check-staging "$REPO_ROOT" || true` before disowning the background indexer.
4. **Create `tests/test_staging_check.cpp`**:
   - Implement `void run_staging_check_tests()` using `CHRONOS_CHECK`.
   - Update `tests/test_main.cpp` and `tests/CMakeLists.txt`.

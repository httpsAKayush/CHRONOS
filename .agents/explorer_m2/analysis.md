# Analysis: Milestone 2 — Aggressive Git Noise Filtering

## Executive Summary
This document provides a comprehensive technical investigation of `AstMutationScorer::scoreDiff` (`src/ast_mutation_scorer.cpp`) and `GitIndexer::indexHistory` (`src/git_indexer.cpp`) against the specification in `project_context.md` (lines 127-135, 508-523). It details current shortcomings, outlines the design for language-aware comment/whitespace normalization and noise filtering, and provides proposed implementation diffs and unit test specifications for `tests/test_ast_mutation_scorer.cpp`.

---

## 1. Specification Requirements (project_context.md)

From `project_context.md` (lines 127-135, 508-523):
1. **Garbage & Noise Filtering**:
   - Commits matching standard `chore` prefixes (e.g. `chore:`, `chore(deps):`, `build(deps):`, `ci:`), changing only whitespace, or modifying lockfiles (`package-lock.json`, `Cargo.lock`, `yarn.lock`) MUST skip vector embedding generation entirely.
   - Bot commits (e.g. `[bot]`, `dependabot`, `renovate`, `github-actions`, `noreply.github.com`) MUST be filtered out during repository scanning.
2. **AST Mutation Scoring**:
   - The diff between "Before" and "After" versions of a file is fed into structural AST analysis.
   - If the structural AST is identical (meaning only whitespace, indentation, line breaks, or comments were modified), `AstMutationScorer::scoreDiff` MUST return `0`.
   - Score classification:
     - **Score 10**: Contract-breaking structural changes (function/class added or deleted, visibility changed, signature modified).
     - **Score 1**: Internal logic changes (loop bounds, assignment, statement changes inside functions).
     - **Score 0**: Noise / No structural AST change (whitespace, comments, formatting, lockfiles, unsupported non-code files).

---

## 2. Current Implementation Audit & Gap Analysis

### 2.1 `AstMutationScorer::scoreDiff` (`src/ast_mutation_scorer.cpp`)
- **Current Behavior**:
  ```cpp
  if (extension != ".py" && extension != ".cpp" && extension != ".hpp" && extension != ".c" && extension != ".h") {
      return 0; // Not a supported language
  }
  if (oldContent == newContent) return 0;
  ```
- **Identified Deficiencies**:
  1. **Exact String Match Only**: Line 12 checks `oldContent == newContent`. If a single space, newline, tab, or comment is added/removed, `oldContent == newContent` evaluates to `false`.
  2. **False Positive Logic Scoring**: Because comment/whitespace changes fail the `oldContent == newContent` check, they fall through to line 38 and return `1` (or line 30 returning `10` if string counting is affected). This causes formatting/comment commits to be indexed into the Hot vector index as structural mutations.
  3. **Limited Language Scope**: Only checks `.py`, `.cpp`, `.hpp`, `.c`, `.h`. Missing common languages like `.js`, `.ts`, `.jsx`, `.tsx`, `.java`, `.go`, `.rs`, `.cs`, `.cc`, `.cxx`, `.hh`, `.hxx`, `.mjs`, `.cjs`.
  4. **No String-Aware Comment Removal**: No scanner exists to strip `//` or `/* */` in C-style languages or `#` in Python while preserving string literals (`"..."`, `'...'`).

### 2.2 `GitIndexer::indexHistory` (`src/git_indexer.cpp`)
- **Current Behavior**:
  - Pre-pass scan only checks:
    ```cpp
    bool isBot = (email.find("bot") != std::string::npos || 
                  email.find("noreply.github.com") != std::string::npos ||
                  msg.find("Auto-generated") == 0);
    ```
  - Filtering threshold logic:
    ```cpp
    int threshold = std::max(1, avgLines / 10);
    if (commitMutationScore >= threshold) { ... }
    ```
- **Identified Deficiencies**:
  1. **Missing Chore Prefix Filtering**: Commits with messages starting with `chore`, `chore:`, `chore(...)`, `build(deps)`, `ci:`, `bump ` are not skipped in the pre-pass.
  2. **Missing Bot Identifiers**: Common bot author names/emails like `dependabot[bot]`, `renovate[bot]`, `github-actions[bot]` are partially missed if `email` lacks `"bot"`.
  3. **No Lockfile Exclusion**: Lockfiles like `package-lock.json`, `Cargo.lock`, `yarn.lock`, `pnpm-lock.yaml`, `composer.lock`, `Gemfile.lock`, `poetry.lock`, `go.sum` are not explicitly skipped in `score_cb` or `index_cb`.
  4. **Flawed Threshold Heuristic**: Using `threshold = std::max(1, avgLines / 10)` means a single structural logic change (score 1) in a high-density commit repository (e.g. `avgLines = 200`, `threshold = 20`) will be skipped even though `commitMutationScore == 1` (>0)! Conversely, if `commitMutationScore == 0`, vector generation must be skipped unconditionally.

---

## 3. Detailed Proposed Design

### 3.1 Robust Multi-Language Comment & Token Normalization (`src/ast_mutation_scorer.cpp`)

To determine if `oldContent` and `newContent` differ only by comments and whitespace, we implement a 2-stage pipeline in `AstMutationScorer::scoreDiff`:

#### Stage A: Language-Aware Comment Removal
- **C-Style Languages** (`.cpp`, `.hpp`, `.c`, `.h`, `.cc`, `.cxx`, `.hh`, `.hxx`, `.js`, `.ts`, `.jsx`, `.tsx`, `.java`, `.go`, `.rs`, `.cs`, etc.):
  - Scan state machine: `NORMAL`, `IN_SINGLE_QUOTE`, `IN_DOUBLE_QUOTE`, `IN_RAW_STRING`, `IN_LINE_COMMENT`, `IN_BLOCK_COMMENT`.
  - While in string states (`IN_SINGLE_QUOTE`, `IN_DOUBLE_QUOTE`), characters (including `//` or `/*`) are copied verbatim. Escape sequences (`\'`, `\"`) are respected.
  - When encountering `//` outside strings: enter `IN_LINE_COMMENT` and skip until `\n` or EOF.
  - When encountering `/*` outside strings: enter `IN_BLOCK_COMMENT` and skip until `*/` or EOF.
- **Python / Hash Comment Languages** (`.py`, `.pyi`, `.sh`):
  - Scan state machine: `NORMAL`, `IN_SINGLE_QUOTE`, `IN_DOUBLE_QUOTE`, `IN_TRIPLE_SINGLE`, `IN_TRIPLE_DOUBLE`, `IN_LINE_COMMENT`.
  - While in string states (including triple-quoted docstrings), characters are preserved.
  - When encountering `#` outside strings: enter `IN_LINE_COMMENT` and skip until `\n` or EOF.

#### Stage B: Tokenization & Whitespace Collapse
- Extract tokens from the comment-stripped source string:
  1. Identifiers / Keywords: contiguous `[a-zA-Z0-9_]`.
  2. Multi-character operators: `==`, `!=`, `<=`, `>=`, `&&`, `||`, `++`, `--`, `->`, `::`, `+=`, `-=`, `*=`, `/=`, `%=`, `<<`, `>>`.
  3. Single-character operators/punctuation: `+`, `-`, `*`, `/`, `%`, `=`, `<`, `>`, `(`, `)`, `{`, `}`, `[`, `]`, `;`, `,`, `.`, `:`, `!`, `&`, `|`, `^`, `~`, `?`.
  4. String/Char literals: preserved as single tokens.
- Compare `vector<string> oldTokens` vs `vector<string> newTokens`.
- **If `oldTokens == newTokens`**: Return `0` (identical structural tokens; only formatting, indentation, line breaks, or comments were changed).

#### Stage C: Contract-Breaking vs Internal Logic Scoring
If `oldTokens != newTokens`:
- If one content was empty and the other non-empty (and non-empty contains tokens): return `10`.
- Count key structural markers (`def `, `class `, `struct `, `public:`, `protected:`, `private:`). If counts differ: return `10`.
- Otherwise: return `1`.

### 3.2 Chore & Lockfile Filtering (`src/git_indexer.cpp`)

1. **Lockfile Helper Function**:
   ```cpp
   bool isLockfile(const std::string& path) {
       std::string filename = fs::path(path).filename().string();
       return (filename == "package-lock.json" || filename == "Cargo.lock" ||
               filename == "yarn.lock" || filename == "pnpm-lock.yaml" ||
               filename == "composer.lock" || filename == "Gemfile.lock" ||
               filename == "poetry.lock" || filename == "go.sum");
   }
   ```
2. **Chore & Bot Commit Helper Function**:
   ```cpp
   bool isChoreOrBotCommit(const std::string& msg, const std::string& author, const std::string& email) {
       std::string lowerMsg = msg;
       for (char &c : lowerMsg) c = std::tolower(c);
       std::string lowerAuthor = author;
       for (char &c : lowerAuthor) c = std::tolower(c);
       std::string lowerEmail = email;
       for (char &c : lowerEmail) c = std::tolower(c);

       // Bot checks
       if (lowerEmail.find("bot") != std::string::npos ||
           lowerEmail.find("noreply.github.com") != std::string::npos ||
           lowerAuthor.find("[bot]") != std::string::npos ||
           lowerAuthor.find("dependabot") != std::string::npos ||
           lowerAuthor.find("renovate") != std::string::npos ||
           lowerMsg.find("auto-generated") == 0) {
           return true;
       }

       // Chore prefix checks
       if (lowerMsg.find("chore:") == 0 || lowerMsg.find("chore(") == 0 ||
           lowerMsg.find("chore ") == 0 || lowerMsg.find("build(deps)") == 0 ||
           lowerMsg.find("ci:") == 0 || lowerMsg.find("bump ") == 0) {
           return true;
       }

       return false;
   }
   ```
3. **Commit Vector Generation Condition**:
   - In `GitIndexer::indexHistory()`:
     - Pre-pass scan uses `isChoreOrBotCommit(msg, author, email)` to skip bot/chore commits upfront.
     - `score_cb` skips lockfiles (`isLockfile(path)` returns score 0).
     - Commit indexing condition: `if (commitMutationScore > 0)` instead of arbitrary `commitMutationScore >= threshold`. If `commitMutationScore == 0` (e.g. only whitespace/comments/lockfiles changed), vector embedding generation is skipped entirely.

---

## 4. Proposed Source Diffs

### 4.1 Proposed Change for `src/ast_mutation_scorer.cpp`
```cpp
// Snippet of proposed AstMutationScorer::scoreDiff implementation:
bool isSupportedCodeExtension(const std::string& ext) {
    static const std::unordered_set<std::string> exts = {
        ".cpp", ".hpp", ".c", ".h", ".cc", ".cxx", ".hh", ".hxx",
        ".py", ".pyi", ".js", ".ts", ".jsx", ".tsx", ".java", ".go", ".rs", ".cs", ".mjs", ".cjs"
    };
    return exts.count(ext) > 0;
}

std::string stripComments(const std::string& src, const std::string& ext) {
    std::string out;
    out.reserve(src.size());
    size_t n = src.size();
    bool isPython = (ext == ".py" || ext == ".pyi");

    size_t i = 0;
    while (i < n) {
        // String literal handling
        if (src[i] == '"' || src[i] == '\'') {
            char quote = src[i];
            out += src[i++];
            while (i < n) {
                if (src[i] == '\\' && i + 1 < n) {
                    out += src[i++];
                    out += src[i++];
                } else if (src[i] == quote) {
                    out += src[i++];
                    break;
                } else {
                    out += src[i++];
                }
            }
            continue;
        }

        // C-style single-line comment //
        if (!isPython && i + 1 < n && src[i] == '/' && src[i+1] == '/') {
            i += 2;
            while (i < n && src[i] != '\n') i++;
            out += ' ';
            continue;
        }

        // C-style block comment /* */
        if (!isPython && i + 1 < n && src[i] == '/' && src[i+1] == '*') {
            i += 2;
            while (i + 1 < n && !(src[i] == '*' && src[i+1] == '/')) i++;
            if (i + 1 < n) i += 2;
            out += ' ';
            continue;
        }

        // Python single-line comment #
        if (isPython && src[i] == '#') {
            while (i < n && src[i] != '\n') i++;
            out += ' ';
            continue;
        }

        out += src[i++];
    }
    return out;
}

std::vector<std::string> tokenize(const std::string& code) {
    std::vector<std::string> tokens;
    size_t i = 0, n = code.size();
    while (i < n) {
        if (std::isspace(static_cast<unsigned char>(code[i]))) {
            i++;
            continue;
        }
        if (std::isalnum(static_cast<unsigned char>(code[i])) || code[i] == '_') {
            size_t start = i;
            while (i < n && (std::isalnum(static_cast<unsigned char>(code[i])) || code[i] == '_')) i++;
            tokens.push_back(code.substr(start, i - start));
            continue;
        }
        // Multi-char operators
        if (i + 1 < n) {
            std::string op2 = code.substr(i, 2);
            if (op2 == "==" || op2 == "!=" || op2 == "<=" || op2 == ">=" ||
                op2 == "&&" || op2 == "||" || op2 == "++" || op2 == "--" ||
                op2 == "->" || op2 == "::" || op2 == "+=" || op2 == "-=") {
                tokens.push_back(op2);
                i += 2;
                continue;
            }
        }
        // Single char token
        tokens.push_back(std::string(1, code[i++]));
    }
    return tokens;
}
```

---

## 5. Unit Test Suite Design (`tests/test_ast_mutation_scorer.cpp`)

We propose adding `tests/test_ast_mutation_scorer.cpp` and registering `run_ast_mutation_scorer_tests()` in `tests/test_main.cpp` and `tests/CMakeLists.txt`.

### Test Cases Matrix:
1. `test_whitespace_and_indentation_changes()`:
   - Verifies C++ and Python code with spaces/tabs/newlines removed/added returns `scoreDiff == 0`.
2. `test_comment_only_changes()`:
   - Verifies adding/modifying `//`, `/* */`, and `#` comments returns `scoreDiff == 0`.
   - Verifies `//` inside string literals (e.g. `"http://test.com"`) is preserved as code tokens.
3. `test_formatting_and_linebreaks()`:
   - Verifies line break insertion around braces/parentheses yields `scoreDiff == 0`.
4. `test_lockfile_and_unsupported_extensions()`:
   - Verifies lockfiles (`package-lock.json`, `Cargo.lock`, `yarn.lock`) and `.md`/`.txt` yield `scoreDiff == 0`.
5. `test_real_logic_changes()`:
   - Verifies changing variable values, expressions, loop conditions returns `scoreDiff == 1`.
6. `test_contract_breaking_changes()`:
   - Verifies adding/deleting functions or classes or changing visibility (`public:`) returns `scoreDiff == 10`.

---

## 6. Verification Method

1. **Build Verification**:
   ```bash
   cmake -B build -S .
   cmake --build build --target chronos_tests
   ```
2. **Execution & Test Verification**:
   ```bash
   ctest --test-dir build --output-on-failure
   ```
3. **Invalidation Conditions**:
   - If any whitespace or comment change yields score > 0, the test fails.
   - If real logic changes yield score 0, the test fails.

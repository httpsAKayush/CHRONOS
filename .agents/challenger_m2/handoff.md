# Adversarial Stress Testing Report — Milestone 2: Aggressive Git Noise Filtering

## 1. Observation
We conducted empirical adversarial stress testing on `AstMutationScorer::scoreDiff` (`src/ast_mutation_scorer.cpp`) and `GitIndexer` (`src/git_indexer.cpp`).

### Tool Commands & Results:
1. **Existing Unit Test Suite Execution**:
   - Command: `./build/tests/chronos_tests`
   - Output: `All Chronos tests passed.` (100% PASS).

2. **Empirical Stress Test Harness Execution**:
   - Source: `/home/zer0/CHRONO/.agents/challenger_m2/stress_test_m2.cpp`
   - Compilation: `g++ -std=c++20 /home/zer0/CHRONO/.agents/challenger_m2/stress_test_m2.cpp -I/home/zer0/CHRONO/include -I/home/zer0/CHRONO/build/_deps/libgit2-src/include -I/home/zer0/CHRONO/build/_deps/json-src/include -I/home/zer0/CHRONO/build/_deps/hnswlib-src -I/home/zer0/CHRONO/build/_deps/httplib-src -L/home/zer0/CHRONO/build -L/home/zer0/CHRONO/build/_deps/libgit2-build -Wl,-rpath,/home/zer0/CHRONO/build -Wl,-rpath,/home/zer0/CHRONO/build/_deps/libgit2-build -lchronos_core -lgit2 -lsqlite3 -lpthread -o /home/zer0/CHRONO/.agents/challenger_m2/stress_test_m2`
   - Output: `STRESS TEST SUMMARY: 93/93 PASSED`.

3. **Key Measurements & Observed Behavior**:
   - **Malformed Comments / String Literals**:
     - C++ unclosed block comment (`/* unclosed...`): Completed in 39 μs. Score: 0.
     - C++ unclosed string literal (`"unclosed...`): Completed in 7 μs. Score: 1.
     - Python unclosed triple quote (`"""` / `'''`): Completed in 3–5 μs. Score: 1.
     - All malformed string/comment edge cases terminated cleanly without infinite loop, out-of-bounds read, or crash.
   - **Massive Whitespace & Indentation Diffs**:
     - 100,000 lines of added spaces/tabs/newlines: Completed in **1.87 ms**. Score: 0.
     - 5,000 functions re-indented (100,000 lines diff): Completed in **8.31 ms**. Score: 0.
     - Execution time scales linearly O(N) and memory consumption remains bounded.
   - **Lockfiles & Unsupported File Types**:
     - Lockfiles (`package-lock.json`, `Cargo.lock`, `yarn.lock`, `pnpm-lock.yaml`, `composer.lock`, `gemfile.lock`, `poetry.lock`, `go.sum`, `PACKAGE-LOCK.JSON`, `sub/dir/Cargo.lock`, `.lock`): All 14 test cases evaluated to score 0.
     - Unsupported files (`.md`, `.txt`, `.json`, `.yaml`, `.yml`, `.xml`, `.csv`, `.png`, `.jpg`, `.pdf`, `.zip`, `.MD`, `Makefile`, `Dockerfile`): All 18 test cases evaluated to score 0.
     - Supported code extensions (`.cpp`, `.hpp`, `.py`, `.js`, `.ts`, `.java`, `.go`, `.rs`, `.cs`, `.sh` and uppercase variants `.CPP`, `.Py`): All 27 test cases correctly matched supported extensions case-insensitively.
   - **Chore Commits & Bot Authors**:
     - Chore prefixes (`chore:`, `chore(deps):`, `chore `, `build(deps)`, `ci:`, `ci(`, `ci `, `bump `, `auto-generated`): All 11 test cases detected and filtered.
     - Bot authors/emails (`dependabot[bot]`, `renovate[bot]`, `github-actions[bot]`, `dependabot`, `renovate`, email containing `bot` or `noreply.github.com`): All 7 test cases detected and filtered.
     - Normal developer commits (`feat:`, `fix:`, `refactor:`, `docs:`, `fix chore tracking...`, `choreography engine...`): None of the 6 test cases were falsely flagged as chore commits.
   - **Vulnerability / Edge Case Discovery**:
     - In `src/git_indexer.cpp` line 46:
       `if (lowerEmail.find("bot") != std::string::npos)`
       Empirical observation: Developer email `abbott@company.com` with author `Arthur Abbott` and commit message `feat: feature` was flagged as a bot commit (`isChoreOrBotCommit` returned `true`).

---

## 2. Logic Chain
1. **Observation**: In `src/ast_mutation_scorer.cpp:47-127` (`stripComments`), loop counters `i` for `/*` and `"` check boundary conditions (`while (i + 1 < n && !(src[i] == '*' && src[i+1] == '/')) i++; if (i + 1 < n) i += 2; else i = n;`).
   - **Inference**: Unclosed block comments and multiline string literals safely advance `i` to `n` and return without infinite loops or invalid memory access. This is confirmed by empirical execution in 39 μs.

2. **Observation**: In `src/ast_mutation_scorer.cpp:248-250`, `tokenize(oldStripped) == tokenize(newStripped)` returns score 0 before running signature extraction.
   - **Inference**: Tokenization strips all whitespace tokens, resulting in identical token vectors for whitespace/indentation diffs. For 100,000 lines, token comparison finishes in under 2 ms, achieving linear time performance.

3. **Observation**: In `src/ast_mutation_scorer.cpp:15-45` and `src/git_indexer.cpp:22-35`, `isLockfile` and `isSupportedCodeExtension` convert inputs to lowercase (`for (char &c : p) c = std::tolower(...)`) and extract `fs::path(p).filename().string()` / `fs::path(p).extension().string()`.
   - **Inference**: Path prefixes, directory structures, and uppercase extensions (`PACKAGE-LOCK.JSON`, `.CPP`) are handled correctly without path-traversal or case-sensitivity vulnerabilities.

4. **Observation**: In `src/git_indexer.cpp:45-53`:
   ```cpp
   if (lowerEmail.find("bot") != std::string::npos ||
       lowerEmail.find("noreply.github.com") != std::string::npos ||
       ...) {
       return true;
   }
   ```
   - **Inference**: `lowerEmail.find("bot")` performs a substring search rather than matching word boundaries or checking bot prefixes/domains. Human email addresses containing `"bot"` (e.g. `abbott@...`, `talbot@...`, `bott@...`, `robotics@...`) will match `"bot"` inside the username/domain, causing `GitIndexer` to skip history indexing for valid developer commits.

5. **Observation**: End-to-end integration test with libgit2 repository confirmed that `GitIndexer::indexHistory()` successfully pre-passes and indexes structural commits while skipping chore/bot commits and zero-mutation keyframes.

---

## 3. Caveats
- The OpenRouter / OpenAI API call inside `GitIndexer::indexHistory()` (`src/git_indexer.cpp:318`) uses external `curl` calls when `OPENROUTER_API_KEY` is present. In CODE_ONLY network mode without API keys set, `GitIndexer` falls back to `syntheticMsg = "AI: Structural mutation detected"`, which was verified in our test run.
- Very large binary files (e.g. >100MB) were not tested in the git repository, but `git_blob_is_binary` in `libgit2` handles binary blob checks at the C level.

---

## 4. Conclusion
Milestone 2 implementation of Aggressive Git Noise Filtering is **highly robust, safe against malformed inputs, and exceptionally fast** (processing 100,000 line whitespace diffs in under 2 milliseconds).
The existing test suite passes 100% (`./build/tests/chronos_tests`).

### Challenge & Risk Summary:
- **Overall Risk Assessment**: LOW to MEDIUM.
- **Identified Risk (Medium)**: Substring matching on email `"bot"` in `src/git_indexer.cpp:46` causes false positives for human developers named Abbott, Talbot, Bott, etc.
- **Recommended Mitigation**: Modify line 46 in `src/git_indexer.cpp` to check for bot identifiers as separate tokens, domains, or prefixes (e.g. `[bot]`, `.bot@`, `bot@`, `-bot@`), or check `lowerAuthor` / `lowerEmail` against specific bot user patterns rather than bare substring `"bot"` in `lowerEmail`.

---

## 5. Verification Method
To independently verify all claims and findings in this report:

1. **Run existing project test suite**:
   ```bash
   ./build/tests/chronos_tests
   ```
   Expected output: `All Chronos tests passed.`

2. **Run empirical challenger stress test suite**:
   ```bash
   g++ -std=c++20 /home/zer0/CHRONO/.agents/challenger_m2/stress_test_m2.cpp \
     -I/home/zer0/CHRONO/include \
     -I/home/zer0/CHRONO/build/_deps/libgit2-src/include \
     -I/home/zer0/CHRONO/build/_deps/json-src/include \
     -I/home/zer0/CHRONO/build/_deps/hnswlib-src \
     -I/home/zer0/CHRONO/build/_deps/httplib-src \
     -L/home/zer0/CHRONO/build \
     -L/home/zer0/CHRONO/build/_deps/libgit2-build \
     -Wl,-rpath,/home/zer0/CHRONO/build \
     -Wl,-rpath,/home/zer0/CHRONO/build/_deps/libgit2-build \
     -lchronos_core -lgit2 -lsqlite3 -lpthread \
     -o /home/zer0/CHRONO/.agents/challenger_m2/stress_test_m2 && \
   /home/zer0/CHRONO/.agents/challenger_m2/stress_test_m2
   ```
   Expected output: `STRESS TEST SUMMARY: 93/93 PASSED` including confirmation of the email substring false-positive check.

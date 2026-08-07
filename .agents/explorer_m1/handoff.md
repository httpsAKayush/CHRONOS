# Handoff Report — Milestone 1: Temporal Recency Algorithm

## 1. Observation

### 1.1 Vector Index Hardcoded Constants & Formula
In `src/vector_index.cpp` (lines 146–165):
```cpp
146:     const double alpha = 0.7; // Weight between semantic and temporal
147:     const double lambda = 0.0000001; // Decay constant (~3 months half-life)
...
156:         float dist = top.first;
157:         // Convert L2 squared distance roughly to cosine similarity (if normalized vectors)
158:         float cos_sim = 1.0f - (dist / 2.0f);
159:         
160:         int64_t commit_ts = impl_->timestamps[label];
161:         double deltaT = std::max<double>(0, queryTimestamp - commit_ts);
162:         
163:         // Exponential temporal decay formula
164:         float final_score = (alpha * cos_sim) + ((1.0 - alpha) * std::exp(-lambda * deltaT));
```

### 1.2 `ContextBuilder` API Disconnect
In `include/chronos/context_builder.hpp` (lines 30–31):
```cpp
30:     BuildResult build(const std::string& userQuery, int pprBudget = 40,
31:                        int contextNodeBudget = 10, float seedConfidenceFloor = 0.05f);
```
In `src/context_builder.cpp` (line 45):
```cpp
45:     auto seeds = vectors_.search(queryVec, /*topK=*/5);
```
`ContextBuilder::build` does not declare or accept `queryTimestamp`, passing `0` by default to `VectorIndex::search`.

### 1.3 Test Suite Missing Recency Suite
In `tests/CMakeLists.txt` (lines 1–7):
```cmake
1: add_executable(chronos_tests
2:   test_main.cpp
3:   test_simhash.cpp
4:   test_codex_alias.cpp
5:   test_mmr.cpp
6:   test_oracle_harness.cpp
7: )
```
`tests/test_recency.cpp` does not yet exist.

---

## 2. Logic Chain

1. **Observation 1.1** proves that `VectorIndex::search()` computes the exponential temporal decay score using the formula $S_{final} = \alpha \cdot \text{cos\_sim}(q,v) + (1-\alpha) \cdot e^{-\lambda \Delta t}$. However, $\alpha$ and $\lambda$ are hardcoded to `0.7` and `0.0000001` (`1e-7`), ignoring `CHRONOS_RECENCY_ALPHA` and `CHRONOS_RECENCY_LAMBDA` environment variables.
2. **Observation 1.1** also shows that if `commit_ts == 0` (un-timestamped node), `queryTimestamp - commit_ts` results in a huge $\Delta t$, unfairly decaying un-timestamped code blocks to $\alpha \cdot \text{cos\_sim}$.
3. **Observation 1.2** demonstrates that `ContextBuilder::build` invokes `vectors_.search(queryVec, 5)` without specifying `queryTimestamp`. As a result, temporal decay in `ContextBuilder` queries is locked to `std::chrono::system_clock::now()`, preventing historical queries or timestamped diagnostic sessions.
4. **Observation 1.3** demonstrates that there is no test coverage for temporal recency ordering, configurable environment variables ($\alpha$, $\lambda$), timestamp propagation, or edge cases.

---

## 3. Caveats

- **Existing Test Failure**: Running `./build/tests/chronos_tests` currently fails on `test_codex_alias.cpp:36` because an exception check failed. This is an existing pre-milestone test failure unrelated to the Temporal Recency Algorithm changes.
- **Scope Limit**: As an Explorer agent operating under read-only guidelines, no direct modifications were applied to `src/vector_index.cpp`, `src/context_builder.cpp`, or `tests/`. All changes are detailed as an actionable implementation plan in `/home/zer0/CHRONO/.agents/explorer_m1/analysis.md`.

---

## 4. Conclusion

The Temporal Recency Algorithm mathematical formula is currently correctly written in `src/vector_index.cpp`, but requires three key enhancements to reach full specification compliance:
1. Make $\alpha$ and $\lambda$ configurable via `CHRONOS_RECENCY_ALPHA` (default `0.7`) and `CHRONOS_RECENCY_LAMBDA` (default `1e-7`) with safe fallback/clamping parsing.
2. Extend `ContextBuilder::build` to accept `int64_t queryTimestamp = 0` and pass it to `VectorIndex::search`, while adding a guard for `commit_ts == 0`.
3. Create a dedicated unit test suite `tests/test_recency.cpp` covering decay ordering, environment variable overrides, timestamp propagation, and edge cases, integrated into `tests/CMakeLists.txt` and `tests/test_main.cpp`.

---

## 5. Verification Method

To verify the proposed implementation once written by an Implementer agent:

1. **Files to Inspect**:
   - `/home/zer0/CHRONO/.agents/explorer_m1/analysis.md`
   - `src/vector_index.cpp` (env var parsing, zero timestamp handling)
   - `include/chronos/context_builder.hpp` & `src/context_builder.cpp` (`queryTimestamp` parameter)
   - `tests/test_recency.cpp` (new unit test file)
   - `tests/CMakeLists.txt` & `tests/test_main.cpp` (integration)

2. **Build and Test Commands**:
   ```bash
   cmake -B build -S .
   cmake --build build
   ./build/tests/chronos_tests
   ```

3. **Invalidation Conditions**:
   - `CHRONOS_RECENCY_ALPHA=1.0` fails to suppress temporal decay penalty.
   - `CHRONOS_RECENCY_LAMBDA=0.0` fails to produce uniform decay factors across different commit timestamps.
   - `ContextBuilder::build` fails to propagate custom `queryTimestamp` down to `VectorIndex::search`.

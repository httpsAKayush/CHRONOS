# Milestone 1: Temporal Recency Algorithm — Technical Analysis & Implementation Plan

## Executive Summary
This document provides a comprehensive technical examination of the Temporal Recency Algorithm in Project CHRONO (`include/chronos/vector_index.hpp`, `src/vector_index.cpp`, and `src/context_builder.cpp`). It compares the current codebase against the specification defined in `project_context.md` and details the necessary modifications to make parameter weighting configurable, ensure correct time-decay computation across query layers, and establish a dedicated unit test suite.

---

## 1. Existing Implementation Breakdown

### 1.1 `include/chronos/vector_index.hpp`
- **Structures**:
  - `EmbeddingRecord`: Contains `std::string nodeId`, `std::vector<float> vector`, and `int64_t timestamp` (Unix epoch timestamp of the commit/node).
  - `SeedMatch`: Contains `std::string nodeId` and `float score`.
- **`VectorIndex::search` Signature**:
  ```cpp
  std::vector<SeedMatch> search(const std::vector<float>& queryVector, int topK, int64_t queryTimestamp = 0) const;
  ```
  `queryTimestamp` defaults to `0`, signaling that the search should use the current system time.

### 1.2 `src/vector_index.cpp`
- **Storage**: `Impl` struct maintains `unordered_map<hnswlib::labeltype, int64_t> timestamps` mapping index labels to commit Unix timestamps.
- **Distance & Similarity**:
  - Uses `hnswlib::L2Space`.
  - Converts L2 squared distance $d^2$ to Cosine Similarity assuming unit-normalized embedding vectors:
    ```cpp
    float dist = top.first;
    float cos_sim = 1.0f - (dist / 2.0f);
    ```
- **Decay & Scoring Logic** (lines 146-165):
  ```cpp
  const double alpha = 0.7; // Weight between semantic and temporal
  const double lambda = 0.0000001; // Decay constant (~3 months half-life)
  ...
  int64_t commit_ts = impl_->timestamps[label];
  double deltaT = std::max<double>(0, queryTimestamp - commit_ts);
  float final_score = (alpha * cos_sim) + ((1.0 - alpha) * std::exp(-lambda * deltaT));
  ```
- **Issues Identified**:
  1. **Hardcoded Parameters**: `alpha` (0.7) and `lambda` (1e-7) are hardcoded constants inside `VectorIndex::search()`. Environment variables `CHRONOS_RECENCY_ALPHA` and `CHRONOS_RECENCY_LAMBDA` are ignored.
  2. **Un-timestamped / Zero-Timestamp Handling**: If `commit_ts == 0` (e.g. for synthetic nodes or legacy nodes without Git metadata), `queryTimestamp - commit_ts` yields a massive $\Delta t$ ($\approx 1.7\times 10^9$ seconds), resulting in $e^{-\lambda \Delta t} \approx 0.0$, heavily penalizing un-timestamped code nodes.

### 1.3 `include/chronos/context_builder.hpp` & `src/context_builder.cpp`
- **`ContextBuilder::build` Signature**:
  ```cpp
  BuildResult build(const std::string& userQuery, int pprBudget = 40,
                     int contextNodeBudget = 10, float seedConfidenceFloor = 0.05f);
  ```
- **Invocation in `src/context_builder.cpp`** (line 45):
  ```cpp
  auto seeds = vectors_.search(queryVec, /*topK=*/5);
  ```
- **Issues Identified**:
  - `ContextBuilder::build` does **not** accept or forward `queryTimestamp`.
  - When callers execute high-level queries (e.g., CLI `chronos ask` or temporal diagnosis for historical crashes), `queryTimestamp` is always `0`, forcing the system to evaluate temporal decay against system runtime `t_now` rather than a historical query time.

---

## 2. Requirement Comparison (`project_context.md`)

| Aspect | `project_context.md` Spec | Current Codebase | Gap / Assessment |
|---|---|---|---|
| **Recency Formula** | $S_{final} = \alpha \cdot \text{cos\_sim}(q, v) + (1 - \alpha) \cdot e^{-\lambda(t_{now} - t_{commit})}$ | `(alpha * cos_sim) + ((1.0 - alpha) * std::exp(-lambda * deltaT))` | Math matches mathematically; implementation is correct. |
| **Semantic Weight ($\alpha$)** | Tunable weight (default 0.7) | Hardcoded `const double alpha = 0.7;` | Must read from `CHRONOS_RECENCY_ALPHA` env var with default 0.7. |
| **Decay Constant ($\lambda$)** | Tunable decay constant (default 1e-7) | Hardcoded `const double lambda = 0.0000001;` | Must read from `CHRONOS_RECENCY_LAMBDA` env var with default 1e-7. |
| **Elapsed Time ($\Delta t$)** | $t_{now} - t_{commit}$ | `std::max<double>(0, queryTimestamp - commit_ts)` | Correctly caps $\Delta t \ge 0$, but needs explicit zero-timestamp guard (`commit_ts == 0`). |
| **Query Timestamp Flow** | Passable query time $t_{now}$ | Hardcoded to default `0` in `ContextBuilder::build()` | `ContextBuilder::build` must take optional `int64_t queryTimestamp = 0` parameter and forward it to `VectorIndex::search`. |
| **Unit Test Coverage** | Dedicated unit test suite required | Absent for temporal recency | Need `tests/test_recency.cpp` covering decay, env vars, propagation, and edge cases. |

---

## 3. Detailed Proposed Modifications

### 3.1 Configurable Alpha and Lambda via Environment Variables

#### Design
Create a robust environment variable helper function in `src/vector_index.cpp`:

```cpp
namespace {
double getEnvDouble(const char* name, double defaultValue, double minVal, double maxVal) {
    const char* envVal = std::getenv(name);
    if (!envVal || *envVal == '\0') {
        return defaultValue;
    }
    try {
        double val = std::stod(envVal);
        if (val < minVal) return minVal;
        if (val > maxVal) return maxVal;
        return val;
    } catch (...) {
        return defaultValue;
    }
}
} // namespace
```

#### Update `VectorIndex::search()` in `src/vector_index.cpp`:
Replace hardcoded constants:
```cpp
// Before:
// const double alpha = 0.7;
// const double lambda = 0.0000001;

// After:
const double alpha = getEnvDouble("CHRONOS_RECENCY_ALPHA", 0.7, 0.0, 1.0);
const double lambda = getEnvDouble("CHRONOS_RECENCY_LAMBDA", 1e-7, 0.0, 1e2);
```

### 3.2 Enhanced `queryTimestamp` & Zero-Timestamp Handling

#### Update `VectorIndex::search()`:
```cpp
int64_t commit_ts = impl_->timestamps[label];
// If commit_ts is 0 (un-timestamped node) or in the future relative to query, deltaT = 0
double deltaT = (commit_ts > 0 && queryTimestamp > commit_ts)
                    ? static_cast<double>(queryTimestamp - commit_ts)
                    : 0.0;
```

#### Update `ContextBuilder` API:
In `include/chronos/context_builder.hpp`:
```cpp
BuildResult build(const std::string& userQuery, int pprBudget = 40,
                   int contextNodeBudget = 10, float seedConfidenceFloor = 0.05f,
                   int64_t queryTimestamp = 0);
```

In `src/context_builder.cpp`:
```cpp
BuildResult ContextBuilder::build(const std::string& userQuery, int pprBudget,
                                   int contextNodeBudget, float seedConfidenceFloor,
                                   int64_t queryTimestamp) {
    BuildResult result;

    // --- Hop 1: LanceDB(VectorIndex) semantic seed ---
    auto queryVec = embedText(userQuery);
    auto seeds = vectors_.search(queryVec, /*topK=*/5, queryTimestamp);
    ...
```

---

## 4. Test Suite Architecture (`tests/test_recency.cpp`)

### 4.1 Test Cases to Implement

1. **`test_recency_decay_ordering()`**:
   - Upsert 3 nodes ($N_1, N_2, N_3$) with identical unit vectors $[1, 0, 0, \dots]$.
   - $N_1.timestamp = 1,000,000$ (1,000s ago relative to query).
   - $N_2.timestamp = 1,900,000$ (100s ago relative to query).
   - $N_3.timestamp = 2,000,000$ (0s ago relative to query).
   - Execute `search` with `queryTimestamp = 2,000,000`.
   - Assert `seeds[0].nodeId == N_3`, `seeds[1].nodeId == N_2`, `seeds[2].nodeId == N_1`.
   - Assert `seeds[0].score > seeds[1].score > seeds[2].score`.

2. **`test_recency_env_alpha_override()`**:
   - Create two nodes:
     - Node A: High cosine similarity ($\text{cos\_sim} \approx 1.0$), old timestamp ($t = 1,000,000$).
     - Node B: Lower cosine similarity ($\text{cos\_sim} \approx 0.7$), fresh timestamp ($t = 2,000,000$).
   - Set `CHRONOS_RECENCY_ALPHA=1.0` (pure semantic): Node A must rank first.
   - Set `CHRONOS_RECENCY_ALPHA=0.0` (pure temporal): Node B must rank first.
   - Set `CHRONOS_RECENCY_ALPHA=0.7` (default hybrid): Compare score output against expected formula.

3. **`test_recency_env_lambda_override()`**:
   - Set `CHRONOS_RECENCY_LAMBDA=0.0` (zero decay): Nodes with identical similarity should get equal decay score ($e^0 = 1.0$) regardless of timestamp difference.
   - Set `CHRONOS_RECENCY_LAMBDA=1e-3` (aggressive decay): Older node score decays rapidly to $\alpha \cdot \text{cos\_sim}$.

4. **`test_context_builder_query_timestamp()`**:
   - Construct a `ContextBuilder` mock environment.
   - Pass explicit `queryTimestamp` to `ContextBuilder::build`.
   - Verify that seeds retrieved by `ContextBuilder` reflect temporal decay relative to `queryTimestamp`.

5. **`test_recency_edge_cases()`**:
   - Future commit timestamp ($t_{commit} > t_{now}$): verify $\Delta t = 0$ caps decay factor at $1.0$.
   - Un-timestamped node ($t_{commit} = 0$): verify $\Delta t = 0$.
   - Invalid env var strings (`CHRONOS_RECENCY_ALPHA="abc"`): verify fallback to default $0.7$.

### 4.2 Integration into CMake and Main Test Harness

In `tests/CMakeLists.txt`:
```cmake
add_executable(chronos_tests
  test_main.cpp
  test_simhash.cpp
  test_codex_alias.cpp
  test_mmr.cpp
  test_oracle_harness.cpp
  test_recency.cpp
)
```

In `tests/test_main.cpp`:
```cpp
void run_simhash_tests();
void run_codex_alias_tests();
void run_mmr_tests();
void run_oracle_harness_tests();
void run_recency_tests();

int main() {
    run_simhash_tests();
    run_codex_alias_tests();
    run_mmr_tests();
    run_oracle_harness_tests();
    run_recency_tests();
    ...
```

---

## 5. Risk Assessment & Verification Plan

### 5.1 Risks
- **Env Var Side Effects**: Mutating environment variables with `setenv` / `unsetenv` during unit tests could affect parallel tests if tests are run in parallel threads.
  - *Mitigation*: Reset environment variables to original states or `unsetenv` in test tear-down blocks.
- **HNSW L2 Distance Precision**: Precision in float conversion `1.0f - (dist / 2.0f)` requires normalized vectors. `embedText()` output is normalized, but artificial test vectors must also be unit normalized to ensure `cos_sim` remains bounded in $[0.0, 1.0]$.

### 5.2 Verification Commands
1. Build test binary:
   ```bash
   cmake -B build -S . && cmake --build build
   ```
2. Run test harness:
   ```bash
   ./build/tests/chronos_tests
   ```
3. Run CTest:
   ```bash
   ctest --test-dir build --output-on-failure
   ```

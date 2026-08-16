The Exact Detailed Flow of chronos ask (After All Changes)
Here's the complete, step-by-step execution path from the moment you type chronos ask "..." to the final response.

Stage 0: Binary Dispatch

Your shell → ~/.local/bin/chronos (CLI binary)
           → ~/.local/bin/chronos-daemon (background LLM server)
The CLI process connects to the daemon via a Unix socket at /tmp/chronos-<hash>.sock. The daemon owns the Ollama connection and streams tokens back. The CLI renders them to your terminal.

Stage 1: Repo Summary Fetch (fetchRepoSummary)
Reads the [GLOBAL:REPO] node from the SQLite Codex DB.
This is a special "context node" (no source bytes) that holds an AI-generated architectural summary of your entire repo — created during chronos sync.
If absent or failed ([ROLLUP_FAILED:...]), it's skipped silently.
Output: ~2,000 char architectural summary string used in Step 2.
Stage 2: HyDE Hypothetical Generation (generateHypothetical)
Sends a prompt to the local LLM (via IPC to the daemon) containing:
The repo architectural summary
All [CONTEXT:DIR:...] subsystem summaries (one per folder)
Your query
A language-specific instruction ("write as Python code snippets…")
The LLM generates a short (~200 token) hypothetical answer — a dense, technical "fake" response that would answer your question if such code existed.
Why: Raw user queries are too short and vague to embed well. The hypothetical is a rich, code-specific document that embeds much better in the vector space.
Stage 3: Hybrid Search — RRF Fusion (targetedSearch)
Two parallel retrieval channels run simultaneously:

Channel A — Dense (Semantic) Search
Concatenates query + hypothetical into one string.
Embeds it using the local embedding model → 384-dim float vector.
Searches the HNSW vector index (VectorIndex) for the top-50 nearest neighbours by cosine similarity.
Returns: [(nodeId, score), ...]
Channel B — Sparse (Lexical) FTS5 Search
Tokenizes the raw query.
Runs a full-text search against SQLite's FTS5 index (which indexes all node signatures + code summaries).
Returns up to 50 matching node IDs.
Fusion — Reciprocal Rank Fusion (RRF)
Both lists are merged using the formula: score(node) = Σ 1/(60 + rank_in_list)
A node that appears in both lists gets contributions from both.
The merged, deduplicated list is sorted and trimmed to top 25 seeds.
Problem this creates: Nodes from documentation files (like scripts_documentation.md) naturally score very high on both channels because they contain human language describing the code. Actual code nodes (decision.py::learn()) contain raw Python and score lower on semantic similarity. This is why old chronos ask gave vague doc-based answers.

Stage 3b: Structural Graph Expansion — THE CORE NEW STEP (structuralExpand)
This is the key fix. It runs after RRF and before the LLM synthesis:

Sub-step A — Explicit Name Detection
A regex scanner parses your query for:
File names: "decision.py", "agent.py" → extracts stem "decision"
Quoted identifiers: 'QLearningDecision', "phase_3"
Contextual patterns: "inside decision", "fxns in agent", "decision file" → extracts "decision"
If names found → scans all nodes in the Codex graph (queryNodesByPathPrefix("")) looking for nodes whose file_path basename matches any extracted name.
All matching nodes become anchor nodes.
Sub-step B — FTS5 Raw Frequency Boost
Re-runs storage_.ftsSearch(query, 200) — a fresh, high-limit FTS5 query, independent of RRF, no deduplication.
Counts how many times each file_path appears in these 200 raw hits.
Any file with ≥ 3 raw FTS5 hits becomes an implicit anchor even if its name wasn't mentioned explicitly.
Example: Query "phase 3 learning persistence" → e2e_learning_flow.py appears 3× → implicit anchor → score boosted to 10.0
Sub-step C — PPR Graph Walk
Takes all anchor node IDs collected in A and B.
Runs Local-Push Personalized PageRank (localPushPPR) with budget=200, damping=0.85.
This walks the dependency graph outward from anchors: pulling in callers, callees, class siblings, imported modules.
Returns nodes sorted by PPR importance score.
Sub-step D — Re-rank and Merge
All PPR-expanded nodes get rank-weighted scores: 5.0 × (1 / (1 + rank))
Direct anchor nodes get the maximum score: 10.0
Original RRF seeds keep their original (much lower) scores (~0.016 after normalization)
Merged map is sorted descending and trimmed to top 25 seeds.
Effect: decision.py functions that had score ~0.43 now have score 10.0. scripts_documentation.md stays at ~0.016. The LLM context window is now dominated by actual code, not docs.

Stage 4: Context Assembly (synthesize)
For each of the 25 prioritized seeds (in order):

File Header — reads the first 2,000 chars of the actual source file from disk (gets the module docstring + imports). Added once per unique file.
Node Snippet — reads node.byte_start → node.byte_end bytes from the source file using readSnippetForNode(). This is the exact AST span (function body, class definition, etc.). Capped at 8,000 chars per node (was 1,200 — ~7× larger).
Subsystem Summary — for each node, traverses CONTAINS edges backwards to find the [CONTEXT:DIR:...] parent node and includes its AI summary once.
Repo Summary — appended at the end.
Total context budget: 32,000 chars (~8,000 tokens). Was 16,000.

Everything is assembled into a ChronosRequest struct with:

userQuery: your question
context[]: ordered list of {nodeId, filePath, codeSnippet, uncertain}
requireCitations: true
Stage 5: LLM Streaming (IpcLLMClient::stream)
The ChronosRequest is serialized to JSON and sent over the Unix socket to chronos-daemon.
The daemon forwards it to Ollama (local) as a structured chat request.
Ollama streams tokens back → daemon forwards as ChronosResponseChunk JSON lines → CLI renders them to your terminal character by character.
When chunk.done == true, the stream ends cleanly.
If you ^C, IpcClient::interrupt() half-closes the socket (SHUT_WR) → daemon's next write gets EPIPE → daemon aborts generation → frees VRAM immediately.
Stage 6: Oracle Verification + Trace
After full response is received, citations like [node:abc123...] are verified against the Codex graph.
If any cited node ID doesn't exist → [Unverified] warning printed.
The full trace (which nodes were used) is recorded with a traceId for later chronos trace <id>.
Before vs After: What Actually Changed
Component	Before	After
Snippet size	1,200 chars (~30 lines)	8,000 chars (~200 lines)
Context budget	16,000 chars	32,000 chars
File header	1,200 chars	2,000 chars
Retrieval	Dense + FTS5 + RRF only	Dense + FTS5 + RRF + Structural Expansion
Named file queries	❌ docs outranked code	✅ file nodes boosted to score=10
Conceptual queries	❌ relevant file skipped after dedup	✅ raw FTS5 count ≥3 → implicit anchor
Graph traversal	❌ none	✅ PPR walks callers/callees from anchors
Crash on exit	❌ free(): invalid pointer SIGABRT	✅ Rule-of-Five move semantics on IpcClient + VectorIndex
Dot-folder indexing	❌ .agent, .planning indexed	✅ skipped at chronos init via vendor_filter.hpp
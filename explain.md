Here is the exact, step-by-step flow of how chronos explain operates internally inside the C++ engine for any target. Unlike chronos ask, which relies on fuzzy vector embeddings and semantic search, chronos explain is a deterministic, graph-walking engine designed to force absolute structural context into the LLM.

1. Target Resolution
When you run chronos explain <target> --query "...", the CLI sends the request to the daemon. The ContextBuilder queries the local codex.db (SQLite) to resolve your target into concrete AST nodes.

If you provide a file path (e.g., agent/decision.py), it grabs every active node that belongs to that file.
If you provide a symbol (e.g., QLearningDecision), it resolves the alias to its specific node ID. This initial set of nodes becomes the Primary Set (Tier 1).
2. The 5-Tier Graph Assembly
To give the LLM a perfect understanding of how the code works and fits into the architecture, the engine walks the AST structural graph (edges table) outward in 5 distinct tiers:

Tier 1: Primary Nodes The engine extracts the exact byte ranges for the primary nodes and reads their raw text directly from the disk.
Tier 2: Member Expansion (CONTAINS Edges) The engine walks outgoing CONTAINS edges to gather all nested children (e.g., all the methods inside a class, or inner functions).
Tier 3: Directional Cross-Link Expansion (CALLS & IMPORTS Edges) This is the most critical step for tracing flows. For every node gathered in Tiers 1 & 2, the engine walks outward:
Incoming Edges (Who calls me?): The engine finds all nodes across the entire repository that have a CALLS or IMPORTS edge pointing to your target. It extracts their raw code and prefixes them with [EXTERNAL CALLER: This node invokes the target]. Because of the AST design, if a specific method (like __init__) calls your target, its parent class (like QLearningDecision) is bubbled up and included so the LLM has full context of the caller.
Outgoing Edges (What do I depend on?): The engine finds all nodes your target CALLS or IMPORTS. It extracts their code and prefixes them with [EXTERNAL DEPENDENCY: The target invokes this node].
Tier 4: Import Headers For any external file touched in Tier 3, the engine grabs the first ~1,500 characters of that file (usually capturing the module docstring and top-level class signatures). This gives the LLM high-level context of external dependencies without reading the whole file.
Tier 5: Architectural Grounding Finally, the [GLOBAL:REPO] node—a hierarchical summary of the entire codebase generated during chronos sync—is injected to give the LLM a 10,000-foot view.
3. Budget Enforcement & Truncation
Before sending this massive wall of text to the LLM, the ContextBuilder enforces a strict context budget (e.g., capping snippets at 10,000 characters). If a cross-linked node (like a massive God Class) is too large, it is safely truncated with \n... (truncated) so it doesn't push the primary target out of the LLM's context window.

4. Prompt Injection
The assembled blocks are wrapped in a strict system prompt that leverages the explicit directionality we just added:

"Instructions: Answer the user's query using the provided file content. Pay special attention to nodes marked [EXTERNAL CALLER] and [EXTERNAL DEPENDENCY] to understand how the file connects to the rest of the codebase. If the user asks how this links to the main flow, find the [EXTERNAL CALLER] nodes that invoke it and trace their logic."

5. LLM Inference & Traceability Logging
The final payload is routed through the LLMFactory to your configured provider (Ollama, OpenAI, Anthropic). As the LLM streams the answer to your terminal, the daemon takes the array of every single node ID it assembled and performs an INSERT INTO trace_log in the SQLite database. It generates a unique hexadecimal Trace ID, allowing you to run chronos trace <id> later to audit exactly what lines of code the LLM was allowed to read to formulate that specific answer.
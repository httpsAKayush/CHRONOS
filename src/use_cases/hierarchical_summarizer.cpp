#include "chronos/use_cases/hierarchical_summarizer.hpp"
#include "chronos/use_cases/repo_profiler.hpp"
#include <iostream>
#include <regex>
#include <algorithm>
#include <thread>
#include <chrono>
#include <atomic>
#include <exception>

namespace chronos {

namespace {
constexpr const char* kGlobalNodeId = "[GLOBAL:REPO]";
}

HierarchicalSummarizer::HierarchicalSummarizer(IStorage& storage, ILLMClient& llm)
    : storage_(storage), llm_(llm) {}

std::string HierarchicalSummarizer::summarizeModule(const SubsystemProfile& prof) {
    std::string prompt =
        "You are an architect summarizing a subsystem. "
        "Identify the primary purpose and extract any layered pipelines "
        "(e.g., 'Layer 1: PatternMatcher', 'Layer 2: ASTAnalyzer', 'Layer 3: LocalLLM'). "
        "Format: 'Uses a 3-layer pipeline: Layer 1 (Class) does X, Layer 2 (Class) does Y, ...'. "
        "Keep to 5 sentences max.\n\n"
        "SUBSYSTEM: " + prof.displayName + "\n"
        "FILE TREE:\n" + prof.shallowTree + "\n"
        "FILE HEADERS:\n" + prof.fileHeaders + "\n";

    std::string summary = llm_.complete(prompt, "Summarize this subsystem architecture.", 1024);
    if (summary.empty()) {
        // Structural fallback when the LLM is unavailable or failed.
        summary = "Subsystem " + prof.displayName + " (" +
                  std::to_string(prof.fileCount) + " files, " +
                  std::to_string(prof.totalBytes) + " bytes).\n" + prof.shallowTree;
    }

    // APPEND SYMBOL MANIFEST (Guarantees HyDE has exact Python class names to anchor to)
    std::string manifest = buildSymbolManifest(prof);
    if (!manifest.empty()) {
        summary += "\n\nSYMBOL MANIFEST: " + manifest;
    }

    return summary;
}

std::string HierarchicalSummarizer::summarizeRepo(const std::vector<SubsystemProfile>& profiles,
                                                   const std::vector<std::string>& rootDocs) {
    std::string prompt =
        "You are a codebase architect performing a global architectural assembly.\n"
        "Below are per-subsystem summaries and root build/documentation files of a\n"
        "repository. Write a master repository summary (8-12 sentences) covering:\n"
        "(1) the overall purpose of the system, (2) the role of each major subsystem\n"
        "and how they fit together, (3) the primary data/control flow between them.\n\n"
        "ROOT DOCUMENTATION:\n";
    for (const auto& doc : rootDocs) {
        prompt += doc + "\n";
    }
    prompt += "\nSUBSYSTEM SUMMARIES:\n";
    for (const auto& prof : profiles) {
        auto node = storage_.getNode("[CONTEXT:DIR:" + prof.relPath + "]");
        prompt += "### " + prof.displayName + "\n";
        prompt += (node ? node->ai_summary : "  (no summary)") + "\n";
    }

    std::string summary = llm_.complete(prompt, "Write the master repository summary.", 1536);
    if (summary.empty()) {
        summary = "[ROLLUP_FAILED:TIMEOUT] Repository with " + std::to_string(profiles.size()) +
                  " top-level subsystems. Global rollup timed out or LLM unavailable.";
    }
    return summary;
}

void HierarchicalSummarizer::linkDirToCodeNodes(const std::string& dirId, const std::string& dirPrefix) {
    auto codeNodes = storage_.queryNodesByPathPrefix(dirPrefix);
    for (const auto& n : codeNodes) {
        Edge e;
        e.source_id = dirId;
        e.target_id = n.id;
        e.type = "CONTAINS";
        e.probable_target_weight = 0.3f;
        storage_.upsertEdge(e);
    }
}

int HierarchicalSummarizer::run(const std::vector<SubsystemProfile>& profiles) {
    int written = runMidTier(profiles);
    runGlobalRollup(profiles);
    return written + 1;
}

int HierarchicalSummarizer::runMidTier(const std::vector<SubsystemProfile>& profiles) {
    if (profiles.empty()) return 0;

    int written = 0;
    for (const auto& prof : profiles) {
        std::string nodeId = "[CONTEXT:DIR:" + prof.relPath + "]";
        std::string dirPath = prof.relPath + "/";
        std::string summary = summarizeModule(prof);
        storage_.upsertContextNode(nodeId, dirPath, summary);
        ++written;

        linkDirToCodeNodes(nodeId, prof.relPath + "/");
    }

    for (const auto& prof : profiles) {
        Edge e;
        e.source_id = kGlobalNodeId;
        e.target_id = "[CONTEXT:DIR:" + prof.relPath + "]";
        e.type = "CONTAINS";
        e.probable_target_weight = 0.3f;
        storage_.upsertEdge(e);
    }

    return written;
}

int HierarchicalSummarizer::runOnRepo(const std::string& repoRoot) {
    RepoProfiler profiler(repoRoot);
    auto profiles = profiler.profile();
    return run(profiles);
}

void HierarchicalSummarizer::runGlobalRollup(const std::vector<SubsystemProfile>& profiles, int timeoutMs) {
    // Run with a bounded budget so a slow LLM can't block `chronos sync`
    // indefinitely. On any failure we persist an explicit sentinel string
    // rather than a silent placeholder, so fetchRepoSummary() / HyDE can
    // detect and retry instead of embedding garbage context.
    std::string repoSummary;
    try {
        // Best-effort time guard: if the LLM is pathologically slow we still
        // want to bound total wait. We rely on the LLM client's own timeout,
        // but we also wrap in a watchdog thread so sync never hangs forever.
        std::atomic<bool> done{false};
        std::string result;
        std::exception_ptr err;

        std::thread worker([&]() {
            try {
                result = summarizeRepo(profiles, profiles.empty() ? std::vector<std::string>{} : profiles.front().rootDocs);
            } catch (...) {
                err = std::current_exception();
            }
            done = true;
        });

        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (!done && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        if (done) {
            worker.join();
            if (err) {
                repoSummary = "[ROLLUP_FAILED:TIMEOUT]";
            } else {
                repoSummary = result;
            }
        } else {
            // Timed out — detach the worker so it can finish later but don't
            // block sync. Mark failure explicitly.
            worker.detach();
            repoSummary = "[ROLLUP_FAILED:TIMEOUT]";
        }
    } catch (...) {
        repoSummary = "[ROLLUP_FAILED:TIMEOUT]";
    }

    storage_.upsertContextNode(kGlobalNodeId, "/", repoSummary);
}

// Build a comma-separated manifest of key classes/functions in this directory
// by extracting from file headers (already captured during profiling)
std::string HierarchicalSummarizer::buildSymbolManifest(const SubsystemProfile& prof) {
    std::vector<std::string> symbols;
    symbols.reserve(30);
    
    // Extract class and function names from file headers using regex
    static const std::regex classPattern(R"(class\s+(\w+))");
    static const std::regex functionPattern(R"(def\s+(\w+))");
    
    std::smatch match;
    std::string headers = prof.fileHeaders;
    
    // Helper lambda to check and add unique symbols
    auto tryAdd = [&](const std::string& s) {
        if (s.size() > 2 && s.size() < 60 && s != "self" && s != "cls") {
            if (std::find(symbols.begin(), symbols.end(), s) == symbols.end()) {
                symbols.push_back(s);
                return symbols.size() >= 30;
            }
        }
        return false;
    };
    
    // Extract class names (Python and C++)
    std::string::const_iterator searchStart(headers.cbegin());
    while (std::regex_search(searchStart, headers.cend(), match, classPattern)) {
        if (tryAdd(match[1].str())) break;
        searchStart = match.suffix().first;
    }
    
    // Extract function names (Python)
    searchStart = headers.cbegin();
    while (std::regex_search(searchStart, headers.cend(), match, functionPattern)) {
        if (tryAdd(match[1].str())) break;
        searchStart = match.suffix().first;
    }
    
    if (symbols.empty()) return "";
    
    std::string manifest;
    for (size_t i = 0; i < symbols.size(); ++i) {
        if (i > 0) manifest += ", ";
        manifest += symbols[i];
    }
    return manifest;
}

} // namespace chronos
#include "chronos/use_cases/hierarchical_summarizer.hpp"
#include "chronos/use_cases/repo_profiler.hpp"
#include <iostream>

namespace chronos {

namespace {
constexpr const char* kGlobalNodeId = "[GLOBAL:REPO]";
}

HierarchicalSummarizer::HierarchicalSummarizer(IStorage& storage, ILLMClient& llm)
    : storage_(storage), llm_(llm) {}

std::string HierarchicalSummarizer::summarizeModule(const SubsystemProfile& prof) {
    std::string prompt =
        "You are a codebase architect building a hierarchical knowledge graph.\n"
        "Given a subsystem's file tree and source file headers, write a concise\n"
        "technical summary (5-10 sentences) describing: (1) what this subsystem does,\n"
        "(2) its key components and their responsibilities, (3) how they interact.\n"
        "Be precise and specific to the actual code shown.\n\n"
        "SUBSYSTEM: " + prof.displayName + "\n\n"
        "FILE TREE:\n" + prof.shallowTree + "\n"
        "FILE HEADERS:\n" + prof.fileHeaders + "\n";

    std::string summary = llm_.complete(prompt, "Summarize this subsystem.", 1024);
    if (summary.empty()) {
        // Structural fallback when the LLM is unavailable or failed.
        summary = "Subsystem " + prof.displayName + " (" +
                  std::to_string(prof.fileCount) + " files, " +
                  std::to_string(prof.totalBytes) + " bytes).\n" + prof.shallowTree;
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
        summary = "Repository with " + std::to_string(profiles.size()) +
                  " top-level subsystems. Run `chronos sync` with an available LLM "
                  "for a generated SUMMARY_REPO.";
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
        e.probable_target_weight = 1.0f;
        storage_.upsertEdge(e);
    }
}

int HierarchicalSummarizer::run(const std::vector<SubsystemProfile>& profiles) {
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

    std::string repoSummary = summarizeRepo(profiles, profiles.empty() ? std::vector<std::string>{} : profiles.front().rootDocs);
    storage_.upsertContextNode(kGlobalNodeId, "/", repoSummary);
    ++written;

    for (const auto& prof : profiles) {
        Edge e;
        e.source_id = kGlobalNodeId;
        e.target_id = "[CONTEXT:DIR:" + prof.relPath + "]";
        e.type = "CONTAINS";
        e.probable_target_weight = 1.0f;
        storage_.upsertEdge(e);
    }

    return written;
}

int HierarchicalSummarizer::runOnRepo(const std::string& repoRoot) {
    RepoProfiler profiler(repoRoot);
    auto profiles = profiler.profile();
    return run(profiles);
}

} // namespace chronos
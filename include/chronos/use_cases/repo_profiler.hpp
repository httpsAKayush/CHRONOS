#pragma once
// Phase 3 "Deterministic Profiling": a C++ directory scanner that
// categorizes top-level source directories (e.g. src/cli, src/storage) and
// gathers root documentation (README.md, CMakeLists.txt, package.json).
// The output feeds the HierarchicalSummarizer's bottom-up subsystem
// summaries. The scan is fully deterministic (sorted, stable order).

#include <string>
#include <vector>

namespace chronos {

struct SubsystemProfile {
    std::string relPath;          // e.g. "src/cli" (top-level dir, no trailing slash)
    std::string displayName;      // e.g. "src/cli"
    std::string shallowTree;      // capped, deterministic file listing
    std::string fileHeaders;      // first N lines of each source file (capped)
    std::vector<std::string> rootDocs; // README.md / CMakeLists.txt / package.json text
    int fileCount = 0;
    int64_t totalBytes = 0;
};

class RepoProfiler {
public:
    explicit RepoProfiler(std::string repoRoot);

    // Depth-1 scan of <repoRoot>/<topDir>. Every top-level directory that
    // is not a vendor directory becomes a SubsystemProfile (sorted by name).
    std::vector<SubsystemProfile> profile() const;

    // Root documentation gathered at repo root (README.md, CMakeLists.txt,
    // package.json, pyproject.toml, etc.), capped in size.
    std::vector<std::string> rootDocs() const;

private:
    std::string repoRoot_;
};

} // namespace chronos
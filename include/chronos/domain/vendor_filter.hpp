#pragma once
// Phase 3 "Vendor Filtering": a single shared predicate for aggressively
// skipping irrelevant directories (vendor/, build/, external/, tests/,
// node_modules/, .chronos/, .git/) everywhere the indexer walks the tree —
// ast_indexer, git_indexer, and the sync directory scans.

#include <string>
#include <filesystem>

namespace chronos {

inline bool isVendorPath(const std::string& path) {
    static const char* kSkipDirs[] = {
        "vendor/",   "build/",    "external/",
        "tests/",    "node_modules/", ".chronos/",
        ".git/",     ".dev_trash/", "dist/",
        ".agents/"
    };
    for (const auto* seg : kSkipDirs) {
        if (path.find(seg) != std::string::npos) return true;
    }
    return false;
}

inline bool isVendorPath(const std::filesystem::path& p) {
    std::string s = p.generic_string();
    return isVendorPath(s);
}

} // namespace chronos
#include "chronos/use_cases/repo_profiler.hpp"
#include "chronos/domain/vendor_filter.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace chronos {

namespace {
constexpr size_t kMaxShallowTreeChars = 4000;
constexpr size_t kMaxHeadersChars = 6000;
constexpr size_t kMaxHeaderLinesPerFile = 12;
constexpr size_t kMaxTreeEntries = 200;
constexpr size_t kMaxRootDocChars = 8000;

std::string detectDominantLanguageImpl(const fs::path& repoRoot) {
    std::unordered_map<std::string, int> extCounts;
    std::vector<std::string> codeExts = {".py", ".cpp", ".cc", ".cxx", ".c", ".h", ".hpp", ".ts", ".tsx", ".js", ".jsx", ".rs", ".go", ".java", ".kt", ".swift"};
    
    try {
        for (auto& entry : fs::recursive_directory_iterator(repoRoot, fs::directory_options::skip_permission_denied)) {
            if (!entry.is_regular_file()) continue;
            if (isVendorPath(entry.path().generic_string())) continue;
            std::string ext = entry.path().extension().string();
            if (std::find(codeExts.begin(), codeExts.end(), ext) != codeExts.end()) {
                extCounts[ext]++;
            }
        }
    } catch (...) {}
    
    if (extCounts.empty()) return "python";
    
    auto maxIt = std::max_element(extCounts.begin(), extCounts.end(),
        [](const auto& a, const auto& b) { return a.second < b.second; });
    
    std::string ext = maxIt->first;
    if (ext == ".py") return "python";
    if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".c" || ext == ".h" || ext == ".hpp") return "cpp";
    if (ext == ".ts" || ext == ".tsx" || ext == ".js" || ext == ".jsx") return "typescript";
    if (ext == ".rs") return "rust";
    if (ext == ".go") return "go";
    if (ext == ".java") return "java";
    if (ext == ".kt") return "kotlin";
    if (ext == ".swift") return "swift";
    return "python";
}

} // namespace anonymous

RepoProfiler::RepoProfiler(std::string repoRoot) : repoRoot_(std::move(repoRoot)) {}

static std::string readFileCapped(const std::string& path, size_t cap) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (content.size() > cap) content.resize(cap);
    return content;
}

static bool isIndexableSource(const std::string& path) {
    static const char* kExts[] = {
        ".cpp", ".h", ".hpp", ".cc", ".cxx", ".c", ".py", ".ts", ".tsx",
        ".js", ".jsx", ".css", ".rs", ".go", ".java", ".kt", ".swift",
        ".sh", ".cmake", ".txt", ".md", ".json", ".yaml", ".yml", ".toml",
        ".proto", ".sql", ".vue", ".rb", ".php"
    };
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot);
    for (const auto* e : kExts) {
        if (ext == e) return true;
    }
    return false;
}

std::vector<SubsystemProfile> RepoProfiler::profile() const {
    std::vector<SubsystemProfile> profiles;

    std::vector<std::string> topDirs;
    try {
        for (auto& entry : fs::directory_iterator(repoRoot_)) {
            if (!entry.is_directory()) continue;
            std::string name = entry.path().filename().string();
            std::string rel = entry.path().generic_string().substr(repoRoot_.size());
            if (rel.size() > 1 && rel.front() == '/') rel = rel.substr(1);
            if (isVendorPath(name + "/") || isVendorPath(rel)) continue;
            if (name == ".git") continue;
            topDirs.push_back(rel);
        }
    } catch (...) {}

    std::sort(topDirs.begin(), topDirs.end());

    for (const auto& rel : topDirs) {
        SubsystemProfile prof;
        prof.relPath = rel;
        prof.displayName = rel;

        fs::path dirPath = fs::path(repoRoot_) / rel;
        std::vector<std::string> treeEntries;
        int64_t totalBytes = 0;

        try {
            for (auto& entry : fs::recursive_directory_iterator(dirPath,
                        fs::directory_options::skip_permission_denied)) {
                std::string ep = entry.path().generic_string();
                std::string relFull = ep.substr(repoRoot_.size() + 1);
                if (isVendorPath(relFull)) continue;
                if (entry.is_directory()) continue;
                if (!entry.is_regular_file()) continue;
                std::string relEntry = ep.substr(dirPath.string().size() + 1);
                if (treeEntries.size() < kMaxTreeEntries) {
                    treeEntries.push_back(relEntry + " (" + std::to_string(entry.file_size()) + "B)");
                }
                totalBytes += static_cast<int64_t>(entry.file_size());
                ++prof.fileCount;
            }
        } catch (...) {}

        std::string tree;
        for (const auto& e : treeEntries) {
            tree += e + "\n";
            if (tree.size() >= kMaxShallowTreeChars) break;
        }
        if (tree.size() > kMaxShallowTreeChars) tree.resize(kMaxShallowTreeChars);
        prof.shallowTree = tree;
        prof.totalBytes = totalBytes;

        std::string headers;
        int headerFiles = 0;
        try {
            for (auto& entry : fs::recursive_directory_iterator(dirPath,
                        fs::directory_options::skip_permission_denied)) {
                std::string ep = entry.path().generic_string();
                std::string relFull = ep.substr(repoRoot_.size() + 1);
                if (isVendorPath(relFull)) continue;
                if (!entry.is_regular_file()) continue;
                if (!isIndexableSource(entry.path().string())) continue;

                std::ifstream in(entry.path(), std::ios::binary);
                if (!in) continue;
                std::string line;
                int lines = 0;
                headers += "===== " + ep.substr(repoRoot_.size() + 1) + " =====\n";
                while (std::getline(in, line) && lines < kMaxHeaderLinesPerFile) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    headers += line + "\n";
                    ++lines;
                }
                headers += "\n";
                ++headerFiles;
                if (headers.size() >= kMaxHeadersChars) break;
            }
        } catch (...) {}
        if (headers.size() > kMaxHeadersChars) headers.resize(kMaxHeadersChars);
        prof.fileHeaders = headers;

        prof.rootDocs = rootDocs();
        profiles.push_back(std::move(prof));
    }

    return profiles;
}

std::vector<std::string> RepoProfiler::rootDocs() const {
    static const char* kDocNames[] = {
        "README.md", "README.rst", "README", "CMakeLists.txt",
        "package.json", "pyproject.toml", "Cargo.toml", "go.mod",
        "Makefile", "Makefile.am", "meson.build", "BUILD", "WORKSPACE",
        "Dockerfile", ".env.example", "docs/ARCHITECTURE.md"
    };

    std::vector<std::string> docs;
    for (const auto* name : kDocNames) {
        fs::path p = fs::path(repoRoot_) / name;
        if (!fs::exists(p) || !fs::is_regular_file(p)) continue;
        std::string content = readFileCapped(p.string(), kMaxRootDocChars);
        if (content.empty()) continue;
        docs.push_back("===== " + std::string(name) + " =====\n" + content);
    }
    return docs;
}

std::string RepoProfiler::detectDominantLanguage() const {
    return detectDominantLanguageImpl(fs::path(repoRoot_));
}

} // namespace chronos
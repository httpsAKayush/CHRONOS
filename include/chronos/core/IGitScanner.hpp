#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace chronos {

struct GitCommit {
    std::string oid;
    std::string parent_oid;
    std::string message;
    std::string author;
    int64_t timestamp = 0;
};

class IGitScanner {
public:
    virtual ~IGitScanner() = default;

    virtual std::vector<GitCommit> getCommits(int maxDepth = 100) = 0;
    virtual std::optional<GitCommit> getCommit(const std::string& hash) = 0;
    virtual std::vector<std::string> getModifiedFiles(const std::string& commitHash) = 0;
    virtual std::string getFileContentAtCommit(const std::string& filePath, const std::string& commitHash) = 0;
    virtual std::string getCurrentCommitHash() const = 0;
    virtual void indexHistory(int syncDepthChoice = 3) = 0;
};

} // namespace chronos

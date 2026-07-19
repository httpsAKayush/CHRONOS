#pragma once
#include <string>
#include <vector>
#include "chronos/codex.hpp"
#include "chronos/ast_indexer.hpp"

namespace chronos {

struct GitCommit {
    std::string oid;
    std::string parent_oid;
    std::string message;
    std::string author;
    int64_t timestamp;
};

class GitIndexer {
public:
    GitIndexer(const std::string& repoRoot, Codex& codex, AstIndexer& astIndexer);
    ~GitIndexer();

    void indexHistory();

    AstIndexer& getAstIndexer() { return astIndexer_; }
    void setCurrentCommitHash(const std::string& h) { currentCommitHash_ = h; }
    const std::string& getCurrentCommitHash() const { return currentCommitHash_; }
    void* getRepo() { return repo_; }

private:
    std::string repoRoot_;
    Codex& codex_;
    AstIndexer& astIndexer_;
    void* repo_; 
    std::string currentCommitHash_;
};

} // namespace chronos

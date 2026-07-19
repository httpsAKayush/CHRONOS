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
    Codex& getCodex() { return codex_; }
    void setCurrentCommitHash(const std::string& h) { currentCommitHash_ = h; }
    const std::string& getCurrentCommitHash() const { return currentCommitHash_; }
    void setCurrentTimestamp(int64_t t) { currentTimestamp_ = t; }
    int64_t getCurrentTimestamp() const { return currentTimestamp_; }
    void setCurrentMessage(const std::string& m) { currentMessage_ = m; }
    const std::string& getCurrentMessage() const { return currentMessage_; }
    void* getRepo() { return repo_; }

private:
    std::string repoRoot_;
    Codex& codex_;
    AstIndexer& astIndexer_;
    void* repo_; 
    std::string currentCommitHash_;
    int64_t currentTimestamp_;
    std::string currentMessage_;
};

} // namespace chronos

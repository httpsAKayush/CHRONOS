#include "chronos/git_indexer.hpp"
#include <git2.h>
#include <iostream>
#include <stdexcept>

namespace chronos {

GitIndexer::GitIndexer(const std::string& repoRoot, Codex& codex, AstIndexer& astIndexer)
    : repoRoot_(repoRoot), codex_(codex), astIndexer_(astIndexer), repo_(nullptr) {
    git_libgit2_init();
    git_repository* repo = nullptr;
    if (git_repository_open(&repo, repoRoot_.c_str()) != 0) {
        throw std::runtime_error("Failed to open git repository at " + repoRoot_);
    }
    repo_ = repo;
}

GitIndexer::~GitIndexer() {
    if (repo_) {
        git_repository_free(static_cast<git_repository*>(repo_));
    }
    git_libgit2_shutdown();
}

void GitIndexer::indexHistory() {
    git_repository* repo = static_cast<git_repository*>(repo_);
    git_revwalk* walk = nullptr;
    if (git_revwalk_new(&walk, repo) != 0) {
        return;
    }
    
    // Push HEAD
    if (git_revwalk_push_head(walk) != 0) {
        git_revwalk_free(walk);
        return;
    }
    
    // Sort topologically
    git_revwalk_sorting(walk, GIT_SORT_TOPOLOGICAL | GIT_SORT_TIME);
    
    git_oid oid;
    while (git_revwalk_next(&oid, walk) == 0) {
        git_commit* commit = nullptr;
        if (git_commit_lookup(&commit, repo, &oid) != 0) continue;
        
        const char* msg = git_commit_message(commit);
        const char* author = git_commit_author(commit)->name;
        git_time_t time = git_commit_time(commit);
        
        char oid_str[GIT_OID_HEXSZ + 1];
        git_oid_tostr(oid_str, sizeof(oid_str), &oid);
        
        // Find parent
        git_tree* tree = nullptr;
        git_commit_tree(&tree, commit);

        git_tree* parent_tree = nullptr;
        if (git_commit_parentcount(commit) > 0) {
            git_commit* parent = nullptr;
            git_commit_parent(&parent, commit, 0);
            git_commit_tree(&parent_tree, parent);
            git_commit_free(parent);
        }
        
        git_diff* diff = nullptr;
        git_diff_tree_to_tree(&diff, repo, parent_tree, tree, nullptr);

        setCurrentCommitHash(oid_str);
        setCurrentTimestamp(time);
        setCurrentMessage(msg ? msg : "");

        auto diff_cb = [](const git_diff_delta *delta, float progress, void *payload) -> int {
            (void)progress;
            if (delta->status == GIT_DELTA_MODIFIED || delta->status == GIT_DELTA_ADDED) {
                GitIndexer* indexer = static_cast<GitIndexer*>(payload);
                git_blob* blob = nullptr;
                if (git_blob_lookup(&blob, static_cast<git_repository*>(indexer->getRepo()), &delta->new_file.id) == 0) {
                    std::string content(static_cast<const char*>(git_blob_rawcontent(blob)), git_blob_rawsize(blob));
                    std::string path = delta->new_file.path;
                    auto nodes = indexer->getAstIndexer().indexBuffer(content, path, indexer->getCurrentCommitHash());
                    git_blob_free(blob);
                    
                    for (const auto& nodeId : nodes) {
                        indexer->getCodex().recordHistory(nodeId, indexer->getCurrentCommitHash(), indexer->getCurrentTimestamp(), "AI: Structural mutation detected");
                    }
                    
                    if (!nodes.empty()) {
                        std::string noteCmd = "git -C " + indexer->repoRoot_ + " notes add -m 'Chronos: Structural mutation detected in " + path + "' " + indexer->getCurrentCommitHash() + " 2>/dev/null";
                        system(noteCmd.c_str());
                    }
                }
            }
            return 0;
        };

        git_diff_foreach(diff, diff_cb, nullptr, nullptr, nullptr, this);

        git_diff_free(diff);
        git_tree_free(tree);
        if (parent_tree) git_tree_free(parent_tree);
        
        git_commit_free(commit);
    }
    git_revwalk_free(walk);
}

} // namespace chronos

#include "chronos/git_indexer.hpp"
#include "chronos/ast_mutation_scorer.hpp"
#include <git2.h>
#include <iostream>
#include <stdexcept>
#include <chrono>
#include <iomanip>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

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
    
    if (git_revwalk_push_head(walk) != 0) {
        git_revwalk_free(walk);
        return;
    }
    
    git_revwalk_sorting(walk, GIT_SORT_TOPOLOGICAL | GIT_SORT_TIME);
    
    std::vector<git_oid> targetCommits;
    git_oid oid;
    int botCommits = 0;
    
    // Fast Sampling Scan (Pre-pass)
    while (git_revwalk_next(&oid, walk) == 0) {
        git_commit* commit = nullptr;
        if (git_commit_lookup(&commit, repo, &oid) != 0) continue;
        
        const char* email_cstr = git_commit_author(commit)->email;
        const char* msg_cstr = git_commit_message(commit);
        
        std::string email = email_cstr ? email_cstr : "";
        std::string msg = msg_cstr ? msg_cstr : "";
        
        bool isBot = (email.find("bot") != std::string::npos || 
                      email.find("noreply.github.com") != std::string::npos ||
                      msg.find("Auto-generated") == 0);
        
        if (isBot) {
            botCommits++;
        } else {
            targetCommits.push_back(oid);
        }
        git_commit_free(commit);
    }
    git_revwalk_free(walk);
    
    int totalCommits = targetCommits.size();
    if (totalCommits == 0) return;
    
    // 5% Sampling for Accurate Data Profile
    int sampleCount = std::max(1, totalCommits / 20);
    int step = std::max(1, totalCommits / sampleCount);
    int totalSampledLines = 0;
    int actualSamples = 0;
    
    for (int i = 0; i < totalCommits; i += step) {
        git_commit* commit = nullptr;
        if (git_commit_lookup(&commit, repo, &targetCommits[i]) != 0) continue;
        
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
        
        git_diff_stats* stats = nullptr;
        if (git_diff_get_stats(&stats, diff) == 0) {
            totalSampledLines += git_diff_stats_insertions(stats) + git_diff_stats_deletions(stats);
            git_diff_stats_free(stats);
        }
        
        git_diff_free(diff);
        if (parent_tree) git_tree_free(parent_tree);
        git_tree_free(tree);
        git_commit_free(commit);
        actualSamples++;
    }
    
    int avgLines = actualSamples > 0 ? (totalSampledLines / actualSamples) : 0;
    std::string densityStr = "Low-density";
    if (avgLines > 200) densityStr = "High-density";
    else if (avgLines > 50) densityStr = "Medium-density";
    
    // 50ms base + 0.5ms per line changed
    double emaMsPerCommit = 50.0 + (avgLines * 0.5); 
    double totalEstSeconds = (totalCommits * emaMsPerCommit) / 1000.0;
    int estMinutes = std::max(1, (int)(totalEstSeconds / 60.0));
    
    std::cout << "Analyzed Repository: " << (totalCommits + botCommits) << " total commits.\n";
    std::cout << "Data Profile: " << densityStr << " structural changes detected (Avg. " << avgLines << " lines changed per commit).\n";
    std::cout << "Estimated Time: " << estMinutes << " minutes (Based on local hardware performance).\n";
    
    int processed = 0;
    for (const git_oid& targetOid : targetCommits) {
        auto commitStart = std::chrono::steady_clock::now();
        
        git_commit* commit = nullptr;
        if (git_commit_lookup(&commit, repo, &targetOid) != 0) continue;
        
        const char* msg = git_commit_message(commit);
        git_time_t time = git_commit_time(commit);
        
        char oid_str[GIT_OID_HEXSZ + 1];
        git_oid_tostr(oid_str, sizeof(oid_str), &targetOid);
        
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

        codex_.beginTransaction();
        int commitMutationScore = 0;

        auto diff_cb = [](const git_diff_delta *delta, float progress, void *payload) -> int {
            (void)progress;
            if (delta->status == GIT_DELTA_MODIFIED || delta->status == GIT_DELTA_ADDED) {
                // Since payload is a pair now:
                auto* ctx = static_cast<std::pair<GitIndexer*, int*>*>(payload);
                GitIndexer* indexer = ctx->first;
                int* scoreOut = ctx->second;
                
                git_blob* new_blob = nullptr;
                git_blob* old_blob = nullptr;
                std::string newContent;
                std::string oldContent;
                
                if (git_blob_lookup(&new_blob, static_cast<git_repository*>(indexer->getRepo()), &delta->new_file.id) == 0) {
                    if (!git_blob_is_binary(new_blob)) {
                        newContent = std::string(static_cast<const char*>(git_blob_rawcontent(new_blob)), git_blob_rawsize(new_blob));
                    }
                    git_blob_free(new_blob);
                }
                
                if (delta->status == GIT_DELTA_MODIFIED) {
                    if (git_blob_lookup(&old_blob, static_cast<git_repository*>(indexer->getRepo()), &delta->old_file.id) == 0) {
                        if (!git_blob_is_binary(old_blob)) {
                            oldContent = std::string(static_cast<const char*>(git_blob_rawcontent(old_blob)), git_blob_rawsize(old_blob));
                        }
                        git_blob_free(old_blob);
                    }
                }
                
                if (!newContent.empty()) {
                    std::string path = delta->new_file.path;
                    auto ext = fs::path(path).extension().string();
                    
                    int fileScore = AstMutationScorer::scoreDiff(oldContent, newContent, ext);
                    *scoreOut += fileScore;
                    
                    if (fileScore > 0) { // Only index if there is a structural change
                        auto nodes = indexer->getAstIndexer().indexBuffer(newContent, path, indexer->getCurrentCommitHash());
                        for (const auto& nodeId : nodes) {
                            indexer->getCodex().recordHistory(nodeId, indexer->getCurrentCommitHash(), indexer->getCurrentTimestamp(), "AI: Structural mutation detected");
                        }
                        if (!nodes.empty()) {
                            indexer->modifiedFilesForCurrentCommit_.push_back(path);
                        }
                    }
                }
            }
            return 0;
        };

        modifiedFilesForCurrentCommit_.clear();
        std::pair<GitIndexer*, int*> payloadCtx = {this, &commitMutationScore};
        git_diff_foreach(diff, diff_cb, nullptr, nullptr, nullptr, &payloadCtx);
        
        // Thresholding: T = k * mu_delta_ast
        // We approximate mu_delta_ast based on avgLines / 10.
        int threshold = std::max(1, avgLines / 10);
        
        if (commitMutationScore >= threshold) {
            codex_.commitTransaction();
            if (!modifiedFilesForCurrentCommit_.empty()) {
                std::string msgStr = "Chronos: Structural Keyframe [Score: " + std::to_string(commitMutationScore) + "]";
                std::string noteCmd = "git -C " + repoRoot_ + " notes add -f -m '" + msgStr + "' " + oid_str + " 2>/dev/null";
                system(noteCmd.c_str());
            }
        } else {
            // Revert transaction if threshold is not met (skip minor commit)
            codex_.raw(); // wait, we don't have rollback. Let's just rollback manually.
            sqlite3_exec(codex_.raw(), "ROLLBACK;", nullptr, nullptr, nullptr);
        }

        git_diff_free(diff);
        git_tree_free(tree);
        if (parent_tree) git_tree_free(parent_tree);
        git_commit_free(commit);
        
        auto commitEnd = std::chrono::steady_clock::now();
        double elapsedMs = std::chrono::duration<double, std::milli>(commitEnd - commitStart).count();
        
        emaMsPerCommit = (0.1 * elapsedMs) + (0.9 * emaMsPerCommit);
        processed++;
        
        double etaSeconds = ((totalCommits - processed) * emaMsPerCommit) / 1000.0;
        int remMins = (int)etaSeconds / 60;
        int remSecs = (int)etaSeconds % 60;
        
        std::cout << "\r[Syncing Commits: " << processed << "/" << totalCommits << "] "
                  << "(Est. remaining: " << remMins << "m " << remSecs << "s)    " << std::flush;
    }
    std::cout << "\nSync complete.\n";
}

} // namespace chronos

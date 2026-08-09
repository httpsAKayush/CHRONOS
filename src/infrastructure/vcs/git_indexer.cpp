#include "chronos/infrastructure/git_indexer.hpp"
#include "chronos/domain/ast_mutation_scorer.hpp"
#include <git2.h>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <chrono>
#include <iomanip>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <httplib.h>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace chronos {

namespace {

bool isLockfile(const std::string& path) {
    std::string filename = fs::path(path).filename().string();
    std::string lowerFilename = filename;
    for (char &c : lowerFilename) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lowerFilename == "package-lock.json" || lowerFilename == "cargo.lock" ||
        lowerFilename == "yarn.lock" || lowerFilename == "pnpm-lock.yaml" ||
        lowerFilename == "composer.lock" || lowerFilename == "gemfile.lock" ||
        lowerFilename == "poetry.lock" || lowerFilename == "go.sum") {
        return true;
    }
    std::string ext = fs::path(lowerFilename).extension().string();
    if (ext == ".lock") return true;
    return false;
}

bool isChoreOrBotCommit(const std::string& msg, const std::string& author, const std::string& email) {
    std::string lowerMsg = msg;
    for (char &c : lowerMsg) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string lowerAuthor = author;
    for (char &c : lowerAuthor) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string lowerEmail = email;
    for (char &c : lowerEmail) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // Bot checks
    if (lowerEmail.find("bot") != std::string::npos ||
        lowerEmail.find("noreply.github.com") != std::string::npos ||
        lowerAuthor.find("[bot]") != std::string::npos ||
        lowerAuthor.find("dependabot") != std::string::npos ||
        lowerAuthor.find("renovate") != std::string::npos ||
        lowerMsg.rfind("auto-generated", 0) == 0) {
        return true;
    }

    // Chore prefix checks
    if (lowerMsg.rfind("chore:", 0) == 0 ||
        lowerMsg.rfind("chore(", 0) == 0 ||
        lowerMsg.rfind("chore ", 0) == 0 ||
        lowerMsg.rfind("build(deps)", 0) == 0 ||
        lowerMsg.rfind("ci:", 0) == 0 ||
        lowerMsg.rfind("ci(", 0) == 0 ||
        lowerMsg.rfind("ci ", 0) == 0 ||
        lowerMsg.rfind("bump ", 0) == 0) {
        return true;
    }

    return false;
}

} // namespace

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

void GitIndexer::indexHistory(int syncDepthChoice) {
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
        
        const git_signature* authorSig = git_commit_author(commit);
        const char* name_cstr = authorSig ? authorSig->name : "";
        const char* email_cstr = authorSig ? authorSig->email : "";
        const char* msg_cstr = git_commit_message(commit);
        
        std::string author = name_cstr ? name_cstr : "";
        std::string email = email_cstr ? email_cstr : "";
        std::string msg = msg_cstr ? msg_cstr : "";
        
        if (isChoreOrBotCommit(msg, author, email)) {
            botCommits++;
        } else {
            targetCommits.push_back(oid);
        }
        git_commit_free(commit);
    }
    git_revwalk_free(walk);
    
    int totalCommits = targetCommits.size();
    if (totalCommits == 0) return;
    
    int processed = 0;
    int threshold = (syncDepthChoice == 2) ? 14 : 0; // Smart keyframing requires score >= 15
    double emaMsPerCommit = 50.0;

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

        int commitMutationScore = 0;

        auto score_cb = [](const git_diff_delta *delta, float progress, void *payload) -> int {
            (void)progress;
            if (delta->status == GIT_DELTA_MODIFIED || delta->status == GIT_DELTA_ADDED) {
                auto* ctx = static_cast<std::pair<GitIndexer*, int*>*>(payload);
                GitIndexer* indexer = ctx->first;
                int* scoreOut = ctx->second;
                
                std::string path = delta->new_file.path;
                if (isLockfile(path)) {
                    return 0; // Lockfiles evaluate to score 0
                }

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
                    auto ext = fs::path(path).extension().string();
                    *scoreOut += AstMutationScorer::scoreDiff(oldContent, newContent, ext.empty() ? path : ext);
                }
            }
            return 0;
        };

        std::pair<GitIndexer*, int*> payloadCtx = {this, &commitMutationScore};
        git_diff_foreach(diff, score_cb, nullptr, nullptr, nullptr, &payloadCtx);
        
        if (commitMutationScore > threshold) {
            codex_.beginTransaction();
            modifiedFilesForCurrentCommit_.clear();

            auto index_cb = [](const git_diff_delta *delta, float progress, void *payload) -> int {
                (void)progress;
                if (delta->status == GIT_DELTA_MODIFIED || delta->status == GIT_DELTA_ADDED) {
                    GitIndexer* indexer = static_cast<GitIndexer*>(payload);
                    std::string path = delta->new_file.path;
                    if (isLockfile(path)) {
                        return 0; // Skip lockfiles in vector indexing
                    }

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
                        std::string syntheticMsg = "AI: Structural mutation detected";
                        const char* sysKey = std::getenv("OPENROUTER_API_KEY");
                        if (!sysKey) sysKey = std::getenv("OPENAI_API_KEY"); // fallback
                        std::string apiKey = sysKey ? sysKey : "";
                        if (!apiKey.empty() && !oldContent.empty()) {
                            std::string escapedOld, escapedNew;
                            for (char c : oldContent) {
                                if (c == '"' || c == '\\') escapedOld += '\\';
                                if (c == '\n') escapedOld += "\\n";
                                else escapedOld += c;
                            }
                            for (char c : newContent) {
                                if (c == '"' || c == '\\') escapedNew += '\\';
                                if (c == '\n') escapedNew += "\\n";
                                else escapedNew += c;
                            }
                            std::string prompt = "Summarize this code change in one short sentence detailing why it was made:\\nOld:\\n" + escapedOld + "\\nNew:\\n" + escapedNew;
                            std::string body = "{\"model\":\"openai/gpt-4o\",\"max_tokens\":100,\"messages\":[{\"role\":\"user\",\"content\":\"" + prompt + "\"}]}";
                            
                            std::string tmpFile = "/tmp/chronos_git_" + std::to_string(indexer->getCurrentTimestamp()) + ".json";
                            {
                                std::ofstream out(tmpFile);
                                out << body;
                            }
                            
                            std::string cmd = "curl -s https://openrouter.ai/api/v1/chat/completions -H \"Content-Type: application/json\" -H \"Authorization: Bearer " + apiKey + "\" -d @" + tmpFile;
                            FILE* pipe = popen(cmd.c_str(), "r");
                            if (pipe) {
                                char buffer[128]; std::string result = "";
                                while (!feof(pipe)) { if (fgets(buffer, 128, pipe) != nullptr) result += buffer; }
                                pclose(pipe);
                                std::remove(tmpFile.c_str());
                                size_t choicesPos = result.find("\"choices\"");
                                if (choicesPos != std::string::npos) {
                                    size_t contentPos = result.find("\"content\":", choicesPos);
                                    if (contentPos != std::string::npos) {
                                        size_t startPos = result.find("\"", contentPos + 10);
                                        if (startPos != std::string::npos) {
                                            startPos++;
                                            std::string out;
                                            for (size_t i = startPos; i < result.size(); ++i) {
                                                if (result[i] == '\\' && i + 1 < result.size()) {
                                                    if (result[i+1] == 'n') { out += '\n'; i++; continue; }
                                                    if (result[i+1] == '"') { out += '"'; i++; continue; }
                                                    out += result[i + 1]; i++; continue;
                                                }
                                                if (result[i] == '"') break;
                                                out += result[i];
                                            }
                                            if (!out.empty()) syntheticMsg = out;
                                        }
                                    }
                                }
                            }
                        }
                        auto nodes = indexer->getAstIndexer().indexBuffer(newContent, path, indexer->getCurrentCommitHash(), indexer->getCurrentTimestamp());
                        for (const auto& nodeId : nodes) {
                            indexer->getCodex().recordHistory(nodeId, indexer->getCurrentCommitHash(), indexer->getCurrentTimestamp(), syntheticMsg);
                        }
                        if (!nodes.empty()) indexer->modifiedFilesForCurrentCommit_.push_back(path);
                    }
                }
                return 0;
            };

            git_diff_foreach(diff, index_cb, nullptr, nullptr, nullptr, this);
            codex_.commitTransaction();

            if (!modifiedFilesForCurrentCommit_.empty()) {
                std::string msgStr = "Chronos: Structural Keyframe [Score: " + std::to_string(commitMutationScore) + "]";
                std::string noteCmd = "git -C " + repoRoot_ + " notes add -f -m '" + msgStr + "' " + oid_str + " 2>/dev/null";
                system(noteCmd.c_str());
            }
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

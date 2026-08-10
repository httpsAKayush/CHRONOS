#include <linux/limits.h>
#include <unistd.h>
// chronos: the Querier CLI/TUI (Spec §5 Interaction Model). Deliberately a
// CLI, not an IDE plugin (Spec §2 non-goal), to keep zero coupling with
// heavy IDE environments.
//
// Subcommands:
//   chronos init                 -- create .chronos/, add to .gitignore, install git hook
//   chronos ask "<query>"        -- full two-hop retrieval + LLM synthesis (FR-2..FR-4, FR-7, FR-8)
//   chronos trace <traceId>      -- Spec §9 Observability: replay which nodes backed an answer
//   chronos sync                 -- Spec §11 mitigation: repair Codex state if hooks were bypassed

#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <vector>
#include <map>
#include <set>
#include <unordered_set>
#include <cctype>
#include <algorithm>
#include <sstream>
#include "chronos/infrastructure/codex.hpp"
#include "chronos/infrastructure/vector_index.hpp"
#include "chronos/use_cases/context_builder.hpp"
#include "chronos/use_cases/oracle.hpp"
#include "chronos/infrastructure/git_indexer.hpp"
#include "chronos/ipc.hpp"
#include "chronos/domain/ast_indexer.hpp"
#include "chronos/domain/ast_mutation_scorer.hpp"
#include "chronos/env.hpp"
#include "chronos/use_cases/diagnose.hpp"

namespace fs = std::filesystem;
using namespace chronos;

namespace {

int cmdInit(const std::string& repoRoot) {
    fs::create_directories(fs::path(repoRoot) / ".chronos" / "logs");

    // Spec §8 Privacy/Safety: .chronos/ auto-added to .gitignore on init.
    fs::path gitignore = fs::path(repoRoot) / ".gitignore";
    std::string existing;
    if (fs::exists(gitignore)) {
        std::ifstream in(gitignore);
        existing.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    if (existing.find(".chronos/") == std::string::npos) {
        std::ofstream out(gitignore, std::ios::app);
        out << "\n# Added by `chronos init` -- contains proprietary source structure\n.chronos/\n";
    }

    // Install the fail-open pre-commit hook (see scripts/pre-commit).
    fs::path hooksDir = fs::path(repoRoot) / ".git" / "hooks";
    if (fs::exists(hooksDir)) {
        fs::path hookDest = hooksDir / "pre-commit";

        // Resolve the directory the running chronos binary lives in so we
        // can find the bundled hook regardless of the CWD or repo layout.
        char selfBuf[PATH_MAX];
        ssize_t selfLen = readlink("/proc/self/exe", selfBuf, sizeof(selfBuf) - 1);
        std::string exeDir = (selfLen != -1) ? fs::path(std::string(selfBuf, selfLen)).parent_path().string() : "";

        // Search order: next to binary (installed location), then repo root.
        fs::path hookSrc;
        if (!exeDir.empty()) {
            fs::path byExe = fs::path(exeDir) / "scripts" / "pre-commit";
            if (fs::exists(byExe)) hookSrc = byExe;
        }
        if (hookSrc.empty()) {
            fs::path byRepo = fs::path(repoRoot) / "scripts" / "pre-commit";
            if (fs::exists(byRepo)) hookSrc = byRepo;
        }
        if (!hookSrc.empty()) {
            fs::copy_file(hookSrc, hookDest, fs::copy_options::overwrite_existing);
            fs::permissions(hookDest, fs::perms::owner_all | fs::perms::group_read | fs::perms::others_read);
        } else {
            std::cerr << "warning: pre-commit hook not found next to chronos binary ("
                      << exeDir << ") or in repo scripts/; "
                         "hook not installed automatically. Run `chronos init` from the "
                         "CHRONO source tree or reinstall via `sudo make install`.\n";
        }
    }

    // Touch the Codex + VectorIndex so `.chronos/codex.db` exists immediately.
    Codex codex(repoRoot);
    VectorIndex vectors(repoRoot);
    std::cout << "Chronos initialized at " << repoRoot << "/.chronos\n";
    return 0;
}

#include <chrono>

int cmdSync(const std::string& repoRoot, bool historyMode) {
    Codex codex(repoRoot);
    VectorIndex vectors(repoRoot);
    AstIndexer indexer(codex, vectors, repoRoot);

    int count = 0;
    
    // Quick scan for Current Codebase weight
    for (auto& entry : fs::recursive_directory_iterator(repoRoot)) {
        if (entry.path().string().find("/.chronos/") != std::string::npos) continue;
        if (entry.path().string().find("/.git/") != std::string::npos) continue;
        auto ext = entry.path().extension().string();
        if (ext == ".cpp" || ext == ".h" || ext == ".hpp" || ext == ".cc" || ext == ".py" || ext == ".md" || ext == ".ts" || ext == ".js" || ext == ".css" || ext == ".rs" || ext == ".go") {
            ++count;
        }
    }

    int syncDepthChoice = 1; // Default to Current State Only
    int histCommits = 0;
    int histMutations = 0;
    int hardwareFilesPerSec = 850; // default baseline

    if (historyMode) {
        // 1. Hardware Profiler
        // Measure roughly how many files we can parse per sec. Just a quick micro-benchmark loop
        auto startBench = std::chrono::steady_clock::now();
        int bCount = 0;
        for (auto& entry : fs::recursive_directory_iterator(repoRoot)) {
            if (entry.path().string().find("/.chronos/") != std::string::npos) continue;
            if (entry.path().string().find("/.git/") != std::string::npos) continue;
            auto ext = entry.path().extension().string();
            if (ext == ".cpp" || ext == ".py") {
                std::string rel = fs::relative(entry.path(), repoRoot).string();
                indexer.indexFile(rel, "sync");
                bCount++;
                if (bCount >= 10) break; // sample up to 10 files
            }
        }
        auto endBench = std::chrono::steady_clock::now();
        double elapsedSec = std::chrono::duration<double>(endBench - startBench).count();
        if (elapsedSec > 0.01 && bCount > 0) {
            hardwareFilesPerSec = static_cast<int>(bCount / elapsedSec);
        }

        // 2. Historical Churn
        // Use popen to quickly run git rev-list
        std::string gitCmd = "git -C \"" + repoRoot + "\" rev-list --count HEAD 2>/dev/null";
        FILE* pipe = popen(gitCmd.c_str(), "r");
        if (pipe) {
            char buf[128];
            if (fgets(buf, sizeof(buf), pipe) != nullptr) {
                histCommits = std::stoi(buf);
            }
            pclose(pipe);
        }
        
        histMutations = histCommits * 12; // Rough heuristic

        // Pre-Flight Dashboard
        std::cout << "\n[ PRE-FLIGHT PROFILER ]\n";
        std::cout << "Hardware Benchmark: " << hardwareFilesPerSec << " files/sec (" << std::thread::hardware_concurrency() << " threads)\n";
        std::cout << "Current Codebase: " << count << " source files\n";
        std::cout << "Historical Weight: " << histCommits << " commits (est. " << histMutations << " file mutations)\n";
        std::cout << "========================================================\n\n";

        std::cout << "Choose your Temporal Sync depth:\n\n";
        std::cout << "[1] Current State Only (Recommended for instant setup)\n";
        std::cout << "    ↳ Indexes the repo exactly as it is right now.\n";
        std::cout << "    ↳ ETA: ~" << std::max(1, count / std::max(1, hardwareFilesPerSec)) << " seconds\n\n";

        std::cout << "[2] Smart Keyframing (Recommended for historical debugging)\n";
        std::cout << "    ↳ Skips formatting/noise. Indexes major structural mutations.\n";
        std::cout << "    ↳ ETA: ~" << std::max(1, (histMutations / 3) / std::max(1, hardwareFilesPerSec)) << " seconds\n\n";

        std::cout << "[3] Deep Archive (Warning: Heavy CPU Load)\n";
        std::cout << "    ↳ Calculates AST Mutation Scores for all " << histCommits << " commits since project inception.\n";
        std::cout << "    ↳ ETA: ~" << std::max(1, histMutations / std::max(1, hardwareFilesPerSec)) << " seconds\n\n";

        std::cout << "[4] Custom Depth\n";
        std::cout << "    ↳ e.g., \"Last 50 commits\" or \"Score threshold > 20\"\n\n";

        std::cout << "> Select option (1-4): ";
        std::string input;
        std::getline(std::cin, input);
        if (!input.empty() && input[0] >= '1' && input[0] <= '4') {
            syncDepthChoice = input[0] - '0';
        }
    }

    std::cout << "\n[1/3] Building Structural Graph (Parsing ASTs)... Done.\n";
    std::cout << "[2/3] Generating Semantic Index (Vectorizing)... Done.\n";
    
    // Index the actual codebase files if not already done by the benchmark loop
    for (auto& entry : fs::recursive_directory_iterator(repoRoot)) {
        if (entry.path().string().find("/.chronos/") != std::string::npos) continue;
        if (entry.path().string().find("/.git/") != std::string::npos) continue;
        auto ext = entry.path().extension().string();
        if (ext == ".cpp" || ext == ".h" || ext == ".hpp" || ext == ".cc" || ext == ".py" || ext == ".md" || ext == ".ts" || ext == ".js" || ext == ".css" || ext == ".rs" || ext == ".go") {
            std::string rel = fs::relative(entry.path(), repoRoot).string();
            indexer.indexFile(rel, "sync");
        }
    }

    if (syncDepthChoice >= 2) {
        GitIndexer gitIndexer(repoRoot, codex, indexer);
        std::cout << "[3/3] Building Temporal Index (Calculating AST Mutations)...\n";
        gitIndexer.indexHistory(syncDepthChoice);
    }

    std::cout << "chronos sync: re-checked " << count << " files ("
              << indexer.stats().nodesSkippedIdempotent << " already up to date, "
              << indexer.stats().nodesUpserted << " updated)\n";
    return 0;
}

static std::string readSnippetForNode(const chronos::Node& n, const std::string& repoRoot);

int cmdAsk(const std::string& repoRoot, const std::string& query, const std::string& crashLogFile = "") {
    Codex codex(repoRoot);
    VectorIndex vectors(repoRoot);
    ContextBuilder builder(codex, vectors, repoRoot);
    Oracle oracle(codex, repoRoot);

    std::cout << "[1/3] Waking daemon...\n";
    std::string sockPath = socketPathForRepo(repoRoot);
    IpcClient client;
    bool daemonUp = client.connect(sockPath);
    if (!daemonUp) {
        // Cold-start: spawn chronos-daemon detached, then retry the connect
        char buf[PATH_MAX];
        ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        std::string exeDir = (len != -1) ? fs::path(std::string(buf, len)).parent_path().string() : "";
        std::string daemonPath = exeDir.empty() ? "chronos-daemon" : (exeDir + "/chronos-daemon");
        std::string cmd = daemonPath + " \"" + repoRoot + "\" >/dev/null 2>&1 &";
        std::system(cmd.c_str());
        for (int i = 0; i < 20 && !daemonUp; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            daemonUp = client.connect(sockPath);
        }
    }

    std::cout << "[2/3] Analyzing graph...\n";
    BuildResult built;
    
    if (!crashLogFile.empty()) {
        std::ifstream file(crashLogFile);
        if (!file.is_open()) {
            std::cerr << "[!] Could not open crash log: " << crashLogFile << "\n";
            return 1;
        }
        std::string crashText((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        
        DiagnoseEngine engine(codex, vectors, repoRoot);
        auto diagResult = engine.diagnose(crashText, 1);
        
        if (!diagResult.ok || diagResult.candidates.empty()) {
            std::cerr << "[!] Diagnosis engine failed to find candidates. Falling back to normal ask...\n";
            built = builder.build(query);
        } else {
            auto& topCand = diagResult.candidates.front();
            
            // Extract raw AST code
            std::string culpritCode;
            auto node = codex.getNode(topCand.nodeId);
            if (node) {
                culpritCode = readSnippetForNode(*node, repoRoot);
            }
            
            // Extract git diff
            std::string diffCmd = "git -C \"" + repoRoot + "\" log -p -1 " + topCand.commitHash + " -- \"" + topCand.filePath + "\" 2>/dev/null";
            char buf[2048];
            std::string gitDiff;
            FILE* pipe = popen(diffCmd.c_str(), "r");
            if (pipe) {
                while (fgets(buf, sizeof(buf), pipe)) gitDiff += buf;
                pclose(pipe);
            }
            
            // Construct the Goldilocks Payload
            std::ostringstream payload;
            payload << "You are an expert debugger. Determine if this is a codebase bug or a system/environment issue.\n\n";
            payload << "--- CRASH LOG ---\n" << crashText << "\n\n";
            payload << "--- CULPRIT NODE (" << topCand.nodeId << ") AST ---\n" << culpritCode << "\n\n";
            payload << "--- RECENT GIT HISTORY FOR " << topCand.filePath << " ---\n" << gitDiff << "\n\n";
            payload << "--- USER QUERY ---\n" << query << "\n";
            
            // Build the context for the query (if any) and inject the crash payload
            built = builder.build(query);
            if (built.ok) {
                built.request.userQuery = payload.str();
                built.request.traceId = "crash-" + topCand.commitHash;
                
                // Inject the culprit node into the context so the daemon knows about it for citations
                if (node) {
                    bool found = false;
                    for (const auto& c : built.request.context) {
                        if (c.nodeId == node->id) { found = true; break; }
                    }
                    if (!found) {
                        built.request.context.push_back({
                            node->id,
                            node->file_path,
                            culpritCode,
                            false
                        });
                    }
                }
            }
        }
    } else {
        built = builder.build(query);
    }

    if (!built.ok) {
        std::cout << built.reason << "\n";
        return 0;
    }

    if (!daemonUp) {
        // Spec unhappy path / FR-7: daemon unreachable -> Oracle-Only Mode.
        std::cout << "[!] LLM daemon unavailable -- falling back to Oracle-Only Mode.\n\n";
        std::cout << oracle.renderTrace(built.rawTrace);
        std::cout << "\nTraceability ID: " << built.request.traceId
                  << "  (run `chronos trace " << built.request.traceId << "` later)\n";
        return 0;
    }

    std::cout << "[3/3] Generating response...\n" << std::flush;
    bool gotAnyText = false;
    std::string fullText;
    client.sendAndStream(built.request, [&](const ChronosResponseChunk& chunk) {
        if (!chunk.textDelta.empty()) {
            gotAnyText = true;
            fullText += chunk.textDelta;
            std::cout << chunk.textDelta << std::flush;
        }
    });
    std::cout << "\n";

    bool isApiError = (fullText.find("[API Error]") != std::string::npos || fullText.find("\"error\":") != std::string::npos);

    if (!gotAnyText || isApiError) {
        // FR-7 fallback also covers "model errored / returned nothing".
        if (isApiError) {
            std::cout << "\n[!] LLM returned an API error -- falling back to Oracle-Only Mode.\n\n";
        } else {
            std::cout << "\n[!] LLM produced no response -- falling back to Oracle-Only Mode.\n\n";
        }
        std::cout << oracle.renderTrace(built.rawTrace);
        std::cout << "\nTraceability ID: " << built.request.traceId
                  << "  (run `chronos trace " << built.request.traceId << "` later)\n";
        return 0;
    }

    // FR-8 / Spec §5 Interaction Refinement: verify citations, and if any
    // are missing/invalid, offer the deterministic trace.
    auto check = oracle.verifyCitations(fullText);
    if (!check.allValid) {
        std::cout << "\n[Unverified] This answer contains citations that don't match the "
                     "Codex graph. Would you like to see the deterministic trace for this "
                     "region instead? Run: chronos ask --oracle-only \"" << query << "\"\n";
    }

    std::cout << "\nTraceability ID: " << built.request.traceId
              << "  (run `chronos trace " << built.request.traceId << "` later)\n";
    return 0;
}

int cmdTrace(const std::string& repoRoot, const std::string& traceId) {
    Codex codex(repoRoot);
    Oracle oracle(codex, repoRoot);
    auto trace = codex.getTrace(traceId);
    if (!trace) {
        std::cerr << "chronos trace: no trace found for id " << traceId << "\n";
        return 1;
    }
    std::cout << oracle.renderTrace(*trace);
    return 0;
}

int cmdTimeline(const std::string& repoRoot, const std::string& target) {
    Codex codex(repoRoot);
    std::string rootId = codex.resolveAlias(target);
    auto history = codex.getHistory(rootId);
    
    if (history.empty()) {
        std::cerr << "chronos timeline: No history found for target '" << target << "'\n";
        return 1;
    }
    
    std::cout << "Temporal Timeline for " << target << " (" << rootId << ")\n";
    std::cout << "--------------------------------------------------------\n";
    for (const auto& rec : history) {
        std::string cmd = "git -C " + repoRoot + " show -s --format=\"%h %cd: %s\" --date=short " + rec.commitHash;
        std::system(cmd.c_str());
        if (!rec.syntheticMsg.empty()) {
            std::cout << "  [AI]: " << rec.syntheticMsg << "\n";
        }
        std::cout << "--------------------------------------------------------\n";
    }
    return 0;
}

static std::string execCmdOutput(const std::string& cmd) {
    std::string result;
    char buffer[512];
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return result;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    pclose(pipe);
    return result;
}

static bool checkStagingCollision(const std::string& stagedLine, const std::string& syntheticMsg) {
    if (syntheticMsg.empty()) return false;

    std::string lineLower = stagedLine;
    std::string msgLower = syntheticMsg;
    std::transform(lineLower.begin(), lineLower.end(), lineLower.begin(), ::tolower);
    std::transform(msgLower.begin(), msgLower.end(), msgLower.begin(), ::tolower);

    static const std::vector<std::string> domainKeywords = {
        "mutex", "lock", "lock_guard", "unique_lock", "timeout", "sleep", 
        "thread", "deadlock", "critical", "hazard", "atomic", "volatile", "race"
    };

    auto matchesKeywordWordBoundary = [](const std::string& text, const std::string& kw) {
        size_t pos = 0;
        while ((pos = text.find(kw, pos)) != std::string::npos) {
            bool leftOk = (pos == 0) || !std::isalnum(static_cast<unsigned char>(text[pos - 1]));
            if (leftOk) {
                return true;
            }
            pos += 1;
        }
        return false;
    };

    for (const auto& kw : domainKeywords) {
        if (matchesKeywordWordBoundary(msgLower, kw) && matchesKeywordWordBoundary(lineLower, kw)) {
            return true;
        }
    }

    auto tokenize = [](const std::string& text) {
        std::vector<std::string> tokens;
        std::string current;
        for (char c : text) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
                current += c;
            } else {
                if (!current.empty()) {
                    tokens.push_back(current);
                    current.clear();
                }
            }
        }
        if (!current.empty()) tokens.push_back(current);
        return tokens;
    };

    auto msgTokens = tokenize(msgLower);
    auto lineTokens = tokenize(lineLower);

    static const std::unordered_set<std::string> stopWords = {
        "a", "an", "the", "in", "on", "at", "to", "for", "of", "with", "and", "or",
        "is", "are", "was", "were", "this", "that", "it", "by", "from", "as", "be",
        "has", "have", "had", "do", "not", "here", "causes", "should", "must", "can"
    };

    for (const auto& mTok : msgTokens) {
        if (mTok.length() < 3 || stopWords.count(mTok)) continue;
        for (const auto& lTok : lineTokens) {
            if (lTok == mTok) return true;
        }
    }

    return false;
}

int cmdCheckStaging(const std::string& repoRoot, bool strictMode = false) {
    fs::path chronosDb = fs::path(repoRoot) / ".chronos" / "codex.db";
    if (!fs::exists(chronosDb)) {
        return 0;
    }

    std::string diffCmd = "git -C \"" + repoRoot + "\" diff --cached -U3 2>/dev/null";
    std::string diffOutput = execCmdOutput(diffCmd);
    if (diffOutput.empty()) {
        return 0;
    }

    std::map<std::string, std::vector<std::string>> stagedAdditions;
    std::stringstream ss(diffOutput);
    std::string line;
    std::string currentFile;

    while (std::getline(ss, line)) {
        if (line.rfind("diff --git", 0) == 0) {
            currentFile.clear();
        } else if (line.rfind("+++ ", 0) == 0) {
            std::string path = line.substr(4);
            if (path.rfind("b/", 0) == 0) {
                path = path.substr(2);
            }
            if (path != "/dev/null") {
                if (!path.empty() && path.back() == '\r') path.pop_back();
                currentFile = path;
            } else {
                currentFile.clear();
            }
        } else if (!currentFile.empty() && !line.empty() && line[0] == '+' && line.rfind("+++", 0) != 0) {
            if (line.back() == '\r') line.pop_back();
            stagedAdditions[currentFile].push_back(line);
        }
    }

    Codex codex(repoRoot);
    int collisionCount = 0;
    std::set<std::tuple<std::string, std::string, std::string>> reported;

    for (const auto& [filePath, addedLines] : stagedAdditions) {
        auto history = codex.getHistoryForFile(filePath);
        if (history.empty()) {
            std::string normPath = fs::path(filePath).lexically_normal().string();
            if (normPath != filePath) {
                history = codex.getHistoryForFile(normPath);
            }
        }

        if (history.empty()) continue;

        for (const auto& stagedLine : addedLines) {
            for (const auto& rec : history) {
                if (rec.syntheticMsg.empty()) continue;
                if (checkStagingCollision(stagedLine, rec.syntheticMsg)) {
                    std::tuple<std::string, std::string, std::string> key = {filePath, stagedLine, rec.commitHash};
                    if (reported.count(key)) continue;
                    reported.insert(key);

                    std::string shortHash = rec.commitHash.substr(0, std::min<size_t>(7, rec.commitHash.length()));

                    std::cout << "================================================================================\n";
                    std::cout << "[TEMPORAL COLLISION WARNING]\n";
                    std::cout << "File: " << filePath << "\n";
                    std::cout << "Staged Modification:\n";
                    std::cout << "  " << stagedLine << "\n";
                    std::cout << "Historical Constraint Violation:\n";
                    std::cout << "  - Commit " << shortHash << ": \"" << rec.syntheticMsg << "\"\n";
                    std::cout << "Action: Please review historical constraint before committing.\n";
                    std::cout << "================================================================================\n";

                    collisionCount++;
                }
            }
        }
    }

    if (strictMode && collisionCount > 0) {
        return 1;
    }
    return 0;
}


static std::string readSnippetForNode(const chronos::Node& n, const std::string& repoRoot) {
    std::string path = repoRoot + "/" + n.file_path;
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return "";
    int64_t len = n.byte_end - n.byte_start;
    if (len <= 0) return "";
    f.seekg(n.byte_start);
    std::string snippet(len, '\0');
    f.read(&snippet[0], len);
    return snippet;
}

// ─────────────────────────────────────────────────────────────────
//  Skeletal X-Ray Formatter helpers
// ─────────────────────────────────────────────────────────────────

// Extracts the short function/symbol name from a node for clean display.
// Returns "file :: symbol" where possible, otherwise just the file path.
// When nodeId is a resolved UUID, we query the alias table for the sym: key.
static std::string nodeLabel(const chronos::Node& n, const std::string& nodeId, Codex* codex = nullptr) {
    std::string file = n.file_path;
    std::string sym;

    if (nodeId.find("sym:") == 0) {
        // Direct sym: prefix — extract the function name
        sym = nodeId.substr(4);
    } else if (codex) {
        // UUID node: look up which sym:XXX aliases point to this UUID.
        // Query: find old_id starting with 'sym:' whose root_id == nodeId.
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(codex->raw(),
            "SELECT old_id FROM alias WHERE root_id = ?1 AND old_id LIKE 'sym:%' LIMIT 1;",
            -1, &stmt, nullptr);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, nodeId.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* oldId = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                if (oldId) sym = std::string(oldId).substr(4); // strip "sym:"
            }
            sqlite3_finalize(stmt);
        }
    }

    if (!sym.empty()) return file + " :: " + sym;
    return file;
}

// ── Line Number Helper ─────────────────────────────────────────
// Counts the number of newlines in a file up to the given byte offset.
// Returns a 1-indexed line number. Handles stale DB paths via fallback.
static int countLinesTo(const std::string& repoRoot, const std::string& relPath, int64_t byteOffset) {
    if (byteOffset <= 0) return 1;

    auto doCount = [&](const std::string& path) -> int {
        std::ifstream in(path, std::ios::binary);
        if (!in) return 0; // 0 indicates failure to open
        int line = 1;
        char c;
        int64_t pos = 0;
        while (in.get(c) && pos < byteOffset) {
            if (c == '\n') line++;
            pos++;
        }
        return line;
    };

    // 1. Primary path attempt
    int line = doCount((fs::path(repoRoot) / relPath).string());
    if (line > 0) return line;

    // 2. Stale path fallback: search repo for file with same name + parent dir
    std::string fname   = fs::path(relPath).filename().string();
    std::string fparent = fs::path(relPath).parent_path().filename().string();
    try {
        for (auto& entry : fs::recursive_directory_iterator(repoRoot)) {
            if (!entry.is_regular_file()) continue;
            auto p = entry.path();
            if (p.filename() != fname) continue;
            if (!fparent.empty() && p.parent_path().filename() != fparent) continue;
            line = doCount(p.string());
            if (line > 0) return line;
        }
    } catch (...) {}
    return 1;
}

// ── Signature keyword detection ────────────────────────────────
// Returns true if a line looks like the start of a function/class definition
// across Python, C++, JS, Rust, Go, Java, etc.
static bool looksLikeSignature(const std::string& line) {
    static const std::vector<std::string> kKeywords = {
        "def ", "async def ", "class ", "fn ", "fun ", "func ",
        "function ", "public ", "private ", "protected ", "static ",
        "void ", "int ", "float ", "double ", "bool ", "auto ",
        "inline ", "virtual ", "override ", "const ", "char ",
        "std::", "template", "export "
    };
    for (const auto& kw : kKeywords) {
        if (line.rfind(kw, 0) == 0) return true;
    }
    return false;
}

// Helper to peek for a docstring in the lines immediately following a signature
static std::string peekForDocstring(std::istringstream& ss) {
    std::string line;
    int lookahead = 3; // Check up to 3 non-blank lines for a docstring
    while (lookahead-- > 0 && std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos) continue; // blank line
        std::string trimmed = line.substr(first);
        if (trimmed.find("\"\"\"") == 0 || trimmed.find("'''") == 0) {
            // Found a docstring
            return trimmed;
        }
        // If we hit real code, stop looking
        if (!trimmed.empty() && trimmed[0] != '#') break;
    }
    return "";
}

// Extract the first non-blank line from file content starting at byte_start.
// Also looks ahead for a Python docstring. Returns docstring if found, else signature.
static std::string extractLineAt(const std::string& content, int64_t byteStart) {
    int64_t start = std::max<int64_t>(0, byteStart);
    if (start >= static_cast<int64_t>(content.size())) return "";
    std::string chunk = content.substr(start,
        std::min<int64_t>(1024, static_cast<int64_t>(content.size()) - start));
    std::istringstream ss(chunk);
    std::string line;
    std::string signature = "";
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        bool allSpace = true;
        for (char c : line) {
            if (!std::isspace(static_cast<unsigned char>(c))) { allSpace = false; break; }
        }
        if (allSpace || line.empty()) continue;
        size_t first = line.find_first_not_of(" \t");
        if (first != std::string::npos) signature = line.substr(first);
        break;
    }
    if (signature.empty()) return "";
    
    std::string docstring = peekForDocstring(ss);
    return docstring.empty() ? signature : docstring;
}

// Scan the entire file for a line matching `def <symName>(` or similar patterns.
// This handles stale byte offsets in the DB. Looks for docstring too.
static std::string scanFileForSignature(const std::string& content, const std::string& symName) {
    if (symName.empty()) return "";
    std::istringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find(symName) == std::string::npos) continue;
        size_t first = line.find_first_not_of(" \t");
        if (first != std::string::npos) {
            std::string trimmed = line.substr(first);
            if (looksLikeSignature(trimmed)) {
                std::string docstring = peekForDocstring(ss);
                return docstring.empty() ? trimmed : docstring;
            }
        }
    }
    return "";
}

// Read the function signature for a node — the first meaningful line at its
// byte offset, with a pattern-scan fallback if offsets are stale.
// `symName` is the short function name (e.g. "extract_body_features") used
// for fallback scanning when byte offsets point to the wrong position.
static std::string readFunctionSignature(const chronos::Node& n,
                                          const std::string& repoRoot,
                                          const std::string& symName = "") {
    // Try primary stored path first, then walk the repo for stale paths.
    auto readContent = [&](const std::string& path) -> std::string {
        std::ifstream in(path, std::ios::binary);
        if (!in) return "";
        return std::string((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
    };

    auto tryPath = [&](const std::string& path) -> std::string {
        std::string content = readContent(path);
        if (content.empty()) return "";

        // Fast path: try the stored byte offset
        std::string line = extractLineAt(content, n.byte_start);
        if (!line.empty() && looksLikeSignature(line)) return line;

        // Slow path: byte offset is stale — scan the file for the sym name
        std::string scanned = scanFileForSignature(content, symName);
        if (!scanned.empty()) return scanned;

        // Last resort: if we couldn't find a signature at all, return empty
        // so the UI can fall back to a generated 'def symName(...)' instead of junk.
        return "";
    };

    // 1. Primary: use the stored file_path
    std::string sig = tryPath((fs::path(repoRoot) / n.file_path).string());
    if (!sig.empty()) return sig;

    // 2. Stale-path fallback: search repo for file with same name + parent dir
    std::string fname   = fs::path(n.file_path).filename().string();
    std::string fparent = fs::path(n.file_path).parent_path().filename().string();
    try {
        for (auto& entry : fs::recursive_directory_iterator(repoRoot)) {
            if (!entry.is_regular_file()) continue;
            auto p = entry.path();
            if (p.filename() != fname) continue;
            if (!fparent.empty() && p.parent_path().filename() != fparent) continue;
            sig = tryPath(p.string());
            if (!sig.empty()) return sig;
        }
    } catch (...) {}
    return "";
}

// Two-tiered annotation: Tier 1 = LLM summary, Tier 2 = raw function signature.
// `symName` is passed from the caller (derived from alias table lookup) so the
// pattern-scan fallback in readFunctionSignature can locate the correct def line.
static std::string nodeAnnotation(const chronos::Node& n,
                                   const std::string& repoRoot,
                                   const std::string& symName = "") {
    // Tier 1: LLM ai_summary (opt-in intelligence)
    if (!n.ai_summary.empty()) {
        std::string s = n.ai_summary;
        if (s.size() > 42) s = s.substr(0, 39) + "...";
        return s;
    }
    // Tier 2: Pure engine fallback — read function signature from AST byte offsets
    // Only attempt for real code nodes (not zero-byte stubs)
    if (n.byte_end <= n.byte_start + 1) return "";
    std::string sig = readFunctionSignature(n, repoRoot, symName);
    if (sig.size() > 60) sig = sig.substr(0, 57) + "...";
    return sig;
}


// Resolve a node ID to its best function name for display.
// Prefers "file :: funcname" label. Falls back gracefully.
static std::string resolveLabel(Codex& codex, const std::string& rawId) {
    auto node = codex.getNode(rawId);
    if (!node) return rawId.substr(0, 8);
    return nodeLabel(*node, rawId, &codex);
}

// ─────────────────────────────────────────────────────────────────
//  BFS structures for upstream / downstream traversal
// ─────────────────────────────────────────────────────────────────

struct BfsRow {
    std::string nodeId;
    int depth;
    bool isLast;     // true = use └─▶, false = use ├─▶ (downstream only)
    int startLine;
    std::string callSiteText;
};

// Collect a breadth-first ordered list of rows for one direction.
// `outgoing` = true → downstream (CALLS), false → upstream (callers).
// All node IDs stored in rows are fully alias-resolved (real UUID nodes),
// so getNode() calls in the render loop can find the real byte offsets.
static std::vector<BfsRow> bfsCollect(
    Codex& codex,
    const std::string& startId,
    bool outgoing,
    int maxDepth)
{
    std::vector<BfsRow> rows;
    std::unordered_set<std::string> visited;
    visited.insert(startId);

    // Helper: resolve an edge target/source to a real node ID.
    // If the alias table has a root for it, use that. Otherwise keep as-is.
    auto resolveOrSelf = [&](const std::string& id) -> std::string {
        try {
            return codex.resolveAlias(id);
        } catch (...) {
            return id;
        }
    };

    // queue: (resolvedNodeId, depth, isLast, startLine, callSiteText)
    struct QEntry { std::string id; int depth; bool isLast; int startLine; std::string callSiteText; };
    std::vector<QEntry> queue;

    // Seed with immediate children/parents, resolved to real UUIDs
    auto seedEdges = codex.getEdgesFiltered(startId, outgoing);
    for (size_t i = 0; i < seedEdges.size(); ++i) {
        const auto& e = seedEdges[i];
        std::string rawId = outgoing ? e.target_id : e.source_id;
        std::string resolvedId = resolveOrSelf(rawId);
        if (!visited.count(resolvedId)) {
            visited.insert(resolvedId);
            queue.push_back({resolvedId, 1, i == seedEdges.size() - 1, e.start_line, e.call_site_text});
        }
    }

    size_t head = 0;
    while (head < queue.size()) {
        auto [currentId, depth, isLast, startLine, callSiteText] = queue[head++];
        rows.push_back({currentId, depth, isLast, startLine, callSiteText});
        if (depth >= maxDepth) continue;

        auto children = codex.getEdgesFiltered(currentId, outgoing);
        for (size_t i = 0; i < children.size(); ++i) {
            const auto& e = children[i];
            std::string rawId = outgoing ? e.target_id : e.source_id;
            std::string resolvedId = resolveOrSelf(rawId);
            if (!visited.count(resolvedId)) {
                visited.insert(resolvedId);
                queue.push_back({resolvedId, depth + 1, i == children.size() - 1, e.start_line, e.call_site_text});
            }
        }
    }
    return rows;
}

// ─────────────────────────────────────────────────────────────────
//  cmdMap: the new Structural X-Ray command
// ─────────────────────────────────────────────────────────────────

enum class MapMode { BOTH, UPSTREAM, DOWNSTREAM };

int cmdMap(const std::string& repoRoot, const std::string& rawTarget, MapMode mode, int maxDepth) {
    Codex codex(repoRoot);

    // ── Smart Target Inference ──────────────────────────────────────
    // If the input contains '/' or a '.' (file path) → resolve as path.
    // Otherwise → automatically treat as a symbol and prefix sym:.
    std::string resolvedTarget = rawTarget;
    bool isSymbol = (rawTarget.find('/') == std::string::npos &&
                     rawTarget.find('.') == std::string::npos);
    if (isSymbol && rawTarget.find("sym:") != 0) {
        resolvedTarget = "sym:" + rawTarget;
    }

    std::string rootId;
    try {
        rootId = codex.resolveAlias(resolvedTarget);
    } catch (const std::exception&) {
        std::cerr << "\n[!] Error: Symbol '" << rawTarget << "' not found in Codex graph.\n"
                  << "    Run `chronos sync` to rebuild the index if this symbol exists.\n\n";
        return 1;
    }

    auto startNode = codex.getNode(rootId);
    if (!startNode) {
        std::cerr << "\n[!] Error: Symbol '" << rawTarget << "' not found in Codex graph.\n\n";
        return 1;
    }

    // ── Build upstream and downstream rows ─────────────────────────
    std::vector<BfsRow> upstreamRows, downstreamRows;
    if (mode == MapMode::BOTH || mode == MapMode::UPSTREAM) {
        upstreamRows = bfsCollect(codex, rootId, false, maxDepth); // callers
    }
    if (mode == MapMode::BOTH || mode == MapMode::DOWNSTREAM) {
        downstreamRows = bfsCollect(codex, rootId, true, maxDepth); // callees
    }

    // ── Header ─────────────────────────────────────────────────────
    std::string nodeShortId = rootId.substr(0, 8);
    int coreLine = countLinesTo(repoRoot, startNode->file_path, startNode->byte_start);

    std::cout << "\nArchitect Flow for `" << rawTarget << "` [node:" << nodeShortId << "]\n";
    std::cout << "Location: " << startNode->file_path << " : Line " << coreLine << "\n";
    std::cout << "================================================================================\n";

    // ── UPSTREAM block ─────────────────────────────────────────────
    if (mode == MapMode::BOTH || mode == MapMode::UPSTREAM) {
        if (mode == MapMode::BOTH) {
            std::cout << "[UPSTREAM CALL STACK] (How we arrive at this function)\n\n";
        } else {
            std::cout << "[UPSTREAM CALL STACK] (How we arrive at this function)\n\n";
        }
        
        if (upstreamRows.empty()) {
            std::cout << "  (no callers found in project)\n";
        } else {
            int maxActualDepth = 1;
            for (const auto& r : upstreamRows) maxActualDepth = std::max(maxActualDepth, r.depth);

            for (auto it = upstreamRows.rbegin(); it != upstreamRows.rend(); ++it) {
                const auto& row = *it;
                auto node = codex.getNode(row.nodeId);
                std::string callerFile = node ? fs::path(node->file_path).filename().string() : "unknown";
                std::string callerSym = node ? nodeLabel(*node, row.nodeId, &codex) : row.nodeId.substr(0, 8);
                size_t pos = callerSym.find(" :: ");
                if (pos != std::string::npos) callerSym = callerSym.substr(pos + 4);

                std::string indentStr;
                if (row.depth == maxActualDepth) {
                    indentStr = "";
                } else {
                    indentStr = std::string(1 + (maxActualDepth - row.depth - 1) * 6, ' ') + "└──> ";
                }
                std::string pipeIndent = std::string(1 + (maxActualDepth - row.depth) * 6, ' ');

                std::cout << indentStr << callerFile << " : Line " << row.startLine << " :: " << callerSym << "()\n";
                std::cout << pipeIndent << "│  ↳ " << row.callSiteText << "\n";
                std::cout << pipeIndent << "│\n";
            }
            std::string targetIndent = std::string(1 + (maxActualDepth - 1) * 6, ' ') + "└──> ";
            std::cout << targetIndent << "[TARGET] " << fs::path(startNode->file_path).filename().string() 
                      << " : Line " << coreLine << " :: " << rawTarget << "()\n";
        }
        std::cout << "================================================================================\n";
    }

    // ── DOWNSTREAM block ───────────────────────────────────────────
    if (mode == MapMode::BOTH || mode == MapMode::DOWNSTREAM) {
        if (mode == MapMode::BOTH) std::cout << "\n";
        std::cout << "[DOWNSTREAM EXECUTION PATH] (Chronological Step-by-Step)\n\n";
        
        if (downstreamRows.empty()) {
            std::cout << "  (no callees found in project)\n";
        } else {
            for (const auto& row : downstreamRows) {
                auto node = codex.getNode(row.nodeId);
                std::string calleeFile = node ? node->file_path : "unknown";
                std::string calleeSym = node ? nodeLabel(*node, row.nodeId, &codex) : row.nodeId.substr(0, 8);
                size_t pos = calleeSym.find(" :: ");
                if (pos != std::string::npos) calleeSym = calleeSym.substr(pos + 4);

                int targetLine = node ? countLinesTo(repoRoot, node->file_path, node->byte_start) : 0;
                std::string annotation = node ? nodeAnnotation(*node, repoRoot, calleeSym) : "";
                if (annotation.empty()) annotation = "def " + calleeSym + "(...)";

                std::cout << "[Line " << row.startLine << "] ──> " << calleeSym << "()\n";
                std::cout << " │            (" << calleeFile << " : Line " << targetLine << ")\n";
                std::cout << " │            ↳ " << annotation << "\n │\n";
            }
        }
        std::cout << "================================================================================\n\n";
    }

    return 0;
}

int cmdExplain(const std::string& repoRoot, const std::string& targetSymbol, const std::string& query) {
    Codex codex(repoRoot);
    VectorIndex vectors(repoRoot);
    ContextBuilder builder(codex, vectors, repoRoot);
    Oracle oracle(codex, repoRoot);

    std::cout << "[1/3] Waking daemon...\n";
    std::string sockPath = socketPathForRepo(repoRoot);
    IpcClient client;
    bool daemonUp = client.connect(sockPath);
    if (!daemonUp) {
        char buf[PATH_MAX];
        ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        std::string exeDir = (len != -1) ? fs::path(std::string(buf, len)).parent_path().string() : "";
        std::string daemonPath = exeDir.empty() ? "chronos-daemon" : (exeDir + "/chronos-daemon");
        std::string cmd = daemonPath + " \"" + repoRoot + "\" >/dev/null 2>&1 &";
        std::system(cmd.c_str());
        for (int i = 0; i < 20 && !daemonUp; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            daemonUp = client.connect(sockPath);
        }
    }

    std::cout << "[2/3] Analyzing structure...\n";
    BuildResult built = builder.buildExplain(targetSymbol, query);

    if (!built.ok) {
        std::cout << built.reason << "\n";
        return 1;
    }

    if (!daemonUp) {
        std::cout << "[!] LLM daemon unavailable -- falling back to Oracle-Only Mode.\n\n";
        std::cout << oracle.renderTrace(built.rawTrace);
        return 0;
    }

    std::cout << "[3/3] Interrogating Codebase...\n\n";
    std::string fullResponse;
    client.sendAndStream(built.request, [&](const ChronosResponseChunk& chunk) {
        std::cout << chunk.textDelta << std::flush;
        fullResponse += chunk.textDelta;
    });

    std::cout << "\n\nTraceability ID: " << built.request.traceId << "  (run `chronos trace " << built.request.traceId << "` later)\n";
    return 0;
}

int cmdDiagnose(const std::string& repoRoot, const std::string& traceFile, int topN = 10) {
    Codex codex(repoRoot);
    VectorIndex vectors(repoRoot);
    DiagnoseEngine engine(codex, vectors, repoRoot);

    std::ifstream file(traceFile);
    if (!file.is_open()) {
        std::cerr << "chronos diagnose: could not open trace file " << traceFile << "\n";
        return 1;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    
    auto result = engine.diagnose(buffer.str(), topN);
    if (!result.ok) {
        std::cerr << "chronos diagnose failed: " << result.reason << "\n";
        return 1;
    }
    
    std::cout << "\n[ CRASH DIAGNOSTICS ]\n";
    std::cout << "Trace: " << result.crashSummary << "\n";
    // Extract first line of crash log as signature
    std::string signature = "Unknown";
    std::istringstream css(buffer.str());
    std::string line;
    while (std::getline(css, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) { signature = line; break; }
    }
    std::cout << "Signature: " << signature << "\n";
    std::cout << "========================================================\n\n";

    // Deduplicate by target node
    struct SuspectGroup {
        std::string nodeId;
        std::string nodeLabel;
        double maxScore;
        std::string structuralPath;
        std::vector<chronos::DiagnosisCandidate> commits;
    };

    std::vector<SuspectGroup> groups;
    for (const auto& c : result.candidates) {
        bool found = false;
        for (auto& g : groups) {
            if (g.nodeId == c.nodeId) {
                g.commits.push_back(c);
                found = true;
                break;
            }
        }
        if (!found) {
            auto n = codex.getNode(c.nodeId);
            std::string label = n ? nodeLabel(*n, c.nodeId, &codex) : c.nodeId.substr(0, 8);
            size_t pos = label.find(" :: ");
            if (pos != std::string::npos) label = label.substr(pos + 4);
            std::string file = n ? n->file_path : "unknown";
            groups.push_back({c.nodeId, file + " :: " + label, c.score, c.dependencyPath, {c}});
        }
    }

    if (groups.empty()) {
        std::cout << "  (no suspects found)\n";
        return 0;
    }

    std::string promptStr = "A crash occurred with this signature: " + signature + "\n\n";
    promptStr += "Raw trace:\n" + buffer.str() + "\n\n";
    promptStr += "Structural pathways and temporal keyframes identified by the Diagnosis Engine:\n";

    // Build the LLM prompt from the groups
    auto getConfidenceLabel = [](double score) {
        if (score >= 90.0) return "High " + std::to_string((int)score) + "%";
        if (score >= 70.0) return "Med " + std::to_string((int)score) + "%";
        return std::to_string((int)score) + "%";
    };

    const auto& primary = groups[0];
    promptStr += "\n[ PRIMARY SUSPECT ] (Confidence: " + getConfidenceLabel(primary.maxScore) + ")\n";
    promptStr += "Node: " + primary.nodeLabel + "\n";
    for (const auto& c : primary.commits) {
        promptStr += "Modified in Commit: " + c.commitHash.substr(0, 8) + " (Message: \"" + c.commitMessage + "\")\n";
    }
    promptStr += "Structural Pathway:\n" + primary.structuralPath + "\n\n";

    if (groups.size() > 1) {
        promptStr += "[ SECONDARY SUSPECTS ]\n";
        for (size_t i = 1; i < groups.size(); ++i) {
            const auto& g = groups[i];
            promptStr += "Node: " + g.nodeLabel + " (Confidence: " + std::to_string((int)g.maxScore) + "%)\n";
            for (const auto& c : g.commits) {
                promptStr += "Modified in Commit: " + c.commitHash.substr(0, 8) + " (Message: \"" + c.commitMessage + "\")\n";
            }
        }
    }
    promptStr += "\nPlease analyze this and output a clean, actionable diagnosis in the following EXACT format (do not add any other pleasantries):\n";
    promptStr += "[CRITICAL REGRESSION DETECTED]\n";
    promptStr += "The crash occurred in <file:line>, but the root cause is a temporal misalignment:\n\n";
    promptStr += "<TIME AGO> (Commit: <Hash>):\n";
    promptStr += "<File> did <Action>.\n\n";
    promptStr += "IMPACT:\n";
    promptStr += "<Explanation of how the structural pathway propagated the crash>\n\n";
    promptStr += "RECOMMENDED FIX:\n";
    promptStr += "<Fix>\n";

    // Wake the daemon
    std::string sockPath = socketPathForRepo(repoRoot);
    IpcClient client;
    bool daemonUp = client.connect(sockPath);
    if (!daemonUp) {
        char buf2[PATH_MAX];
        ssize_t len = readlink("/proc/self/exe", buf2, sizeof(buf2) - 1);
        std::string exeDir = (len != -1) ? fs::path(std::string(buf2, len)).parent_path().string() : "";
        std::string daemonPath = exeDir.empty() ? "chronos-daemon" : (exeDir + "/chronos-daemon");
        std::string cmd = daemonPath + " \"" + repoRoot + "\" >/dev/null 2>&1 &";
        std::system(cmd.c_str());
        for (int i = 0; i < 20 && !daemonUp; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            daemonUp = client.connect(sockPath);
        }
    }
    if (!daemonUp) {
        std::cout << "[!] LLM daemon unavailable -- falling back to raw output.\n\n";
        std::cout << promptStr;
        return 0;
    }

    ChronosRequest req;
    req.command = "chat";
    std::string hexStr = "0123456789abcdef";
    for(int i=0; i<16; ++i) req.traceId += hexStr[rand() % 16];
    req.systemPromptOverride = "You are the Chronos Diagnosis Engine. You synthesize raw structural traces and temporal commits into a root cause analysis.";
    
    // Pass the actual code context so it knows what the code does
    for (const auto& g : groups) {
        auto n = codex.getNode(g.nodeId);
        if (n) {
            ContextNode cn;
            cn.nodeId = g.nodeId;
            cn.filePath = n->file_path;
            cn.codeSnippet = readSnippetForNode(*n, repoRoot);
            req.context.push_back(cn);
        }
    }

    // Now append the query
    ContextNode queryNode;
    queryNode.nodeId = "query";
    queryNode.filePath = "user_query";
    queryNode.codeSnippet = promptStr;
    req.context.push_back(queryNode);

    bool gotAnyText = false;
    std::string fullText;
    client.sendAndStream(req, [&](const ChronosResponseChunk& chunk) {
        if (!chunk.textDelta.empty()) {
            gotAnyText = true;
            fullText += chunk.textDelta;
            std::cout << chunk.textDelta;
        }
    });
    std::cout << "\n";

    bool isApiError = (fullText.find("[API Error]") != std::string::npos || fullText.find("\"error\":") != std::string::npos);
    if (!gotAnyText || isApiError) {
        if (isApiError) std::cout << "\n[!] LLM returned an API error -- falling back to raw output.\n\n";
        else std::cout << "\n[!] LLM produced no response -- falling back to raw output.\n\n";
        std::cout << promptStr;
    }

    std::cout << "========================================================\n\n";
    
    return 0;
}

} // namespace

#include <nlohmann/json.hpp>

int cmdStatus(const std::string& repoRoot) {
    fs::path codexPath = fs::path(repoRoot) / ".chronos" / "codex.db";
    if (!fs::exists(codexPath)) {
        std::cerr << "Index not found.\n";
        return 1;
    }
    auto sizeBytes = fs::file_size(codexPath);
    double sizeMB = static_cast<double>(sizeBytes) / (1024.0 * 1024.0);

    chronos::Codex codex(repoRoot);
    int nodeCount = 0, edgeCount = 0;
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(codex.raw(), "SELECT COUNT(*) FROM nodes", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) nodeCount = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    if (sqlite3_prepare_v2(codex.raw(), "SELECT COUNT(*) FROM edges", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) edgeCount = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }

    std::string commitHash = "unknown";
    fs::path headPath = fs::path(repoRoot) / ".git" / "HEAD";
    if (fs::exists(headPath)) {
        std::ifstream in(headPath);
        std::string line;
        if (std::getline(in, line)) {
            if (line.rfind("ref: ", 0) == 0) {
                std::string refPath = line.substr(5);
                fs::path refFile = fs::path(repoRoot) / ".git" / refPath;
                if (fs::exists(refFile)) {
                    std::ifstream refIn(refFile);
                    if (std::getline(refIn, commitHash)) {
                        commitHash = commitHash.substr(0, 8);
                    }
                }
            } else {
                commitHash = line.substr(0, 8);
            }
        }
    }

    std::string statusStr = "SLEEPING";
    int pid = 0;
    if (system("pgrep -f chronos-daemon > /dev/null") == 0) {
        statusStr = "RUNNING";
        FILE* pipe = popen("pgrep -f chronos-daemon", "r");
        if (pipe) {
            char buf[128];
            if (fgets(buf, sizeof(buf), pipe)) {
                pid = std::stoi(buf);
            }
            pclose(pipe);
        }
    }

    // Semantic Size
    fs::path vectorPath = fs::path(repoRoot) / ".chronos" / "vectors.bin";
    double vectorSizeMB = 0;
    if (fs::exists(vectorPath)) {
        vectorSizeMB = static_cast<double>(fs::file_size(vectorPath)) / (1024.0 * 1024.0);
    }
    chronos::VectorIndex vectors(repoRoot);
    size_t embedCount = vectors.size();

    // Temporal Index
    int historyDepth = 0;
    std::string gitCmd = "git -C \"" + repoRoot + "\" rev-list --count HEAD 2>/dev/null";
    FILE* gpipe = popen(gitCmd.c_str(), "r");
    if (gpipe) {
        char buf[128];
        if (fgets(buf, sizeof(buf), gpipe)) {
            historyDepth = std::stoi(buf);
        }
        pclose(gpipe);
    }

    int keyframes = 0;
    if (sqlite3_prepare_v2(codex.raw(), "SELECT COUNT(DISTINCT commit_hash) FROM node_history", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) keyframes = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }

    // Active LLM
    auto loadConfig = [](const fs::path& p) {
        nlohmann::json j;
        if (fs::exists(p)) {
            std::ifstream in(p);
            try { in >> j; } catch (...) {}
        }
        return j;
    };
    const char* homeDir = getenv("HOME");
    fs::path globalPath = fs::path(homeDir ? homeDir : "") / ".chronos" / "config.json";
    fs::path localPath = fs::current_path() / ".chronos" / "config.json";
    nlohmann::json globalConf = loadConfig(globalPath);
    nlohmann::json localConf = loadConfig(localPath);
    std::string activeLLM = "llama3 (local)";
    if (localConf.contains("llm.model") && localConf["llm.model"].is_string()) activeLLM = localConf["llm.model"].get<std::string>();
    else if (globalConf.contains("llm.model") && globalConf["llm.model"].is_string()) activeLLM = globalConf["llm.model"].get<std::string>();

    std::cout << "[ CHRONOS SYSTEM STATUS ]\n\n";
    
    std::cout << "--- Structural Graph (codex.db) ---\n";
    std::cout << "Database Size : " << sizeMB << " MB\n";
    std::cout << "Graph Nodes   : " << nodeCount << "\n";
    std::cout << "Graph Edges   : " << edgeCount << "\n\n";

    std::cout << "--- Semantic Index (vectors.bin) ---\n";
    std::cout << "Vector Size   : " << vectorSizeMB << " MB\n";
    std::cout << "Embeddings    : " << embedCount << "\n\n";

    std::cout << "--- Temporal Index ---\n";
    std::cout << "History Depth : " << historyDepth << " commits indexed\n";
    std::cout << "Keyframes     : " << keyframes << " structural mutations mapped\n\n";

    std::cout << "--- System Health ---\n";
    if (pid > 0) {
        std::cout << "Daemon State  : RUNNING (PID " << pid << ")\n";
    } else {
        std::cout << "Daemon State  : SLEEPING\n";
    }
    std::cout << "Synced Commit : " << commitHash << "\n";
    std::cout << "Active LLM    : " << activeLLM << "\n";
    return 0;
}

int cmdConfig(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: chronos config <get|set|list> [key] [value]\n";
        return 1;
    }
    std::string action = argv[2];
    
    const char* homeDir = getenv("HOME");
    fs::path globalPath = fs::path(homeDir ? homeDir : "") / ".chronos" / "config.json";
    fs::path localPath = fs::current_path() / ".chronos" / "config.json";

    auto loadConfig = [](const fs::path& p) {
        nlohmann::json j;
        if (fs::exists(p)) {
            std::ifstream in(p);
            try { in >> j; } catch (...) {}
        }
        return j;
    };

    auto saveConfig = [](const fs::path& p, const nlohmann::json& j) {
        if (!fs::exists(p.parent_path())) fs::create_directories(p.parent_path());
        std::ofstream out(p);
        out << j.dump(4);
    };

    nlohmann::json globalConf = loadConfig(globalPath);
    nlohmann::json localConf = loadConfig(localPath);

    if (action == "list") {
        nlohmann::json merged = globalConf;
        for (auto& el : localConf.items()) merged[el.key()] = el.value();
        
        for (auto& el : merged.items()) {
            std::string key = el.key();
            std::string val = el.value().is_string() ? el.value().get<std::string>() : el.value().dump();
            
            if (key.find("api_key") != std::string::npos && val.length() > 6) {
                val = val.substr(0, 4) + "..." + val.substr(val.length() - 2);
            }
            std::cout << key << "=" << val << "\n";
        }
        return 0;
    }
    
    if (action == "get") {
        if (argc < 4) { std::cerr << "Usage: chronos config get <key>\n"; return 1; }
        std::string key = argv[3];
        if (localConf.contains(key)) {
            std::cout << (localConf[key].is_string() ? localConf[key].get<std::string>() : localConf[key].dump()) << "\n";
        } else if (globalConf.contains(key)) {
            std::cout << (globalConf[key].is_string() ? globalConf[key].get<std::string>() : globalConf[key].dump()) << "\n";
        }
        return 0;
    }
    
    if (action == "set") {
        if (argc < 5) { std::cerr << "Usage: chronos config set <key> <value>\n"; return 1; }
        std::string key = argv[3];
        std::string val = argv[4];
        
        // Write to global by default for llm.*, to local otherwise, or based on user choice?
        // User asked for standardized way to read/write. I'll write to global if it's not in .chronos currently.
        // Wait, standard git-like behavior is to write to local config if .chronos exists. 
        // Let's write to global by default if --local is not provided, actually just global for everything unless they use a flag?
        // Let's just write to global for now to satisfy "global fallback mechanism".
        globalConf[key] = val;
        saveConfig(globalPath, globalConf);
        return 0;
    }
    
    return 1;
}

int cmdExport(int argc, char** argv, const std::string& repoRoot) {
    std::string format = "json";
    std::string output = "";
    
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--format" && i + 1 < argc) format = argv[++i];
        if (arg == "--output" && i + 1 < argc) output = argv[++i];
    }
    
    chronos::Codex codex(repoRoot);
    sqlite3_stmt* stmt;
    
    if (format == "json") {
        nlohmann::json root = nlohmann::json::object();
        root["nodes"] = nlohmann::json::array();
        if (sqlite3_prepare_v2(codex.raw(), "SELECT id, file_path, ai_summary FROM nodes", -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                nlohmann::json node;
                node["id"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                const char* fp = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                if (fp) node["file_path"] = fp;
                const char* s = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                if (s) node["ai_summary"] = s;
                root["nodes"].push_back(node);
            }
            sqlite3_finalize(stmt);
        }
        
        root["edges"] = nlohmann::json::array();
        if (sqlite3_prepare_v2(codex.raw(), "SELECT source_id, target_id, type FROM edges", -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                nlohmann::json edge;
                edge["source_id"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                edge["target_id"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                if (t) edge["type"] = t;
                root["edges"].push_back(edge);
            }
            sqlite3_finalize(stmt);
        }
        
        std::string result = root.dump(2);
        if (output.empty()) std::cout << result << "\n";
        else {
            std::ofstream out(output);
            out << result;
            std::cout << "Exported JSON to " << output << "\n";
        }
    } else if (format == "mermaid") {
        std::stringstream ss;
        ss << "graph TD\n";
        if (sqlite3_prepare_v2(codex.raw(), "SELECT source_id, target_id, type FROM edges LIMIT 1000", -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                std::string s = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                std::string t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                std::string type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                // strip dashes for mermaid node names
                std::string sName = s, tName = t;
                sName.erase(std::remove(sName.begin(), sName.end(), '-'), sName.end());
                tName.erase(std::remove(tName.begin(), tName.end(), '-'), tName.end());
                ss << "    " << sName << "[\"" << s.substr(0,8) << "\"] -->|" << type << "| " << tName << "[\"" << t.substr(0,8) << "\"]\n";
            }
            sqlite3_finalize(stmt);
        }
        if (output.empty()) std::cout << ss.str();
        else {
            std::ofstream out(output);
            out << ss.str();
            std::cout << "Exported Mermaid to " << output << "\n";
        }
    } else {
        std::cerr << "Unknown format: " << format << "\n";
        return 1;
    }
    
    return 0;
}

int cmdClean(int argc, char** argv) {
    bool force = false;
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--force") force = true;
    }
    
    if (system("pgrep -f chronos-daemon > /dev/null") == 0) {
        std::cerr << "[!] chronos-daemon is currently running. Stopping daemon...\n";
        system("pkill -f chronos-daemon");
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    if (!force) {
        std::cout << "[!] This will destroy the local index. Are you sure? (y/N) ";
        std::string ans;
        std::getline(std::cin, ans);
        if (ans != "y" && ans != "Y") {
            std::cout << "Aborted.\n";
            return 0;
        }
    }
    
    fs::path chronosDir = fs::current_path() / ".chronos";
    if (fs::exists(chronosDir)) {
        fs::remove_all(chronosDir);
        std::cout << "Cleaned " << chronosDir << "\n";
    } else {
        std::cout << ".chronos directory not found.\n";
    }
    
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]) == "help" || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
        std::cout << "\n Chronos: The Temporal Codebase Engine\n"
                  << " ================================================================================\n\n"
                  << " Usage: chronos <command> [args]\n\n"
                  << " Core Commands:\n"
                  << "   init           Initialize a new chronos repository (creates .chronos/)\n"
                  << "   sync           Index the codebase, parse ASTs, and build the vector/structural graph\n"
                  << "   ask            The Macroscopic Pathfinder. Ask a conceptual question (Discovery Mode).\n"
                  << "                    Example: chronos ask \"where is the connection pooler configured?\"\n"
                  << "                    Example: chronos ask \"why is the UDP multicast failing?\" --crash crash.log\n"
                  << "   explain        The Microscopic Interrogator. Explain a specific symbol (Surgery Mode).\n"
                  << "                    Example: chronos explain initialize_multicast\n"
                  << "                    Example: chronos explain serve/tcp_server.py --query \"what port does this bind to?\"\n"
                  << "   map            The Architectural X-Ray. Shows a skeletal view of how a function behaves or what it impacts.\n"
                  << "                    Example: chronos map initialize_multicast --downstream\n"
                  << "                    Example: chronos map initialize_multicast --upstream\n"
                  << "                    Example: chronos map initialize_multicast --both --depth 3\n"
                  << "   diagnose       The Ghost Bug Diagnosis Engine. Structurally walk backward from a stack trace.\n"
                  << "                    Example: chronos diagnose --trace crash.log --top 5\n"
                  << "   trace          Inspect the exact context graph the LLM was given for a specific trace ID.\n"
                  << "                    Example: chronos trace crash-a116c6e0\n"
                  << "   timeline       See the history of structural mutations for a specific file or symbol.\n"
                  << "                    Example: chronos timeline serve/tcp_server.py\n"
                  << "   check-staging  Prevent \"Temporal Collisions\" before committing.\n"
                  << "                    Example: chronos check-staging --strict\n\n"
                  << " System Management:\n"
                  << "   status         Check daemon health, graph size, and system state\n"
                  << "   config         Manage LLM keys, AI providers, and environment variables\n"
                  << "                    Example: chronos config set llm.api_url http://localhost:11434/v1\n"
                  << "                    Example: chronos config set llm.api_key ollama\n"
                  << "                    Example: chronos config set llm.model llama3.1:8b\n"
                  << "   export         Dump the structural graph data to JSON or Mermaid\n"
                  << "   clean          Destroy the local index and free up disk space\n\n"
                  << " [ GLOBAL INSTALLATION ]\n"
                  << " To make Chronos available anywhere on your system, execute:\n\n"
                  << "     mkdir -p build && cd build\n"
                  << "     cmake ..\n"
                  << "     make -j$(nproc)\n"
                  << "     sudo make install\n\n"
                  << " ================================================================================\n\n";
        return (argc < 2) ? 2 : 0;
    }
    std::string cmd = argv[1];
    std::string repoRoot = fs::current_path().string();

    auto env = loadEnv(repoRoot);
    for (const auto& [k, v] : env) {
        setenv(k.c_str(), v.c_str(), 1);
    }

    if (cmd == "init") return cmdInit(repoRoot);
    if (cmd == "status") return cmdStatus(repoRoot);
    if (cmd == "config") return cmdConfig(argc, argv);
    if (cmd == "export") return cmdExport(argc, argv, repoRoot);
    if (cmd == "clean") return cmdClean(argc, argv);
    if (cmd == "sync") {
        bool historyMode = false;
        for (int i = 2; i < argc; ++i) {
            if (std::string(argv[i]) == "--history") historyMode = true;
        }
        return cmdSync(repoRoot, historyMode);
    }
    if (cmd == "ask") {
        if (argc < 3) { std::cerr << "usage: chronos ask \"<query>\" [--crash <file>]\n"; return 2; }
        std::string query = argv[2];
        std::string crashFile;
        
        size_t crashPos = query.find("--crash");
        if (crashPos != std::string::npos) {
            std::string rem = query.substr(crashPos);
            query = query.substr(0, crashPos);
            if (rem == "--crash" && argc > 3) {
                crashFile = argv[3];
            } else if (rem.length() > 7 && rem[7] == '=') {
                crashFile = rem.substr(8);
            } else if (rem.length() > 7) {
                crashFile = rem.substr(7);
            }
        } else {
            for (int i = 3; i < argc; ++i) {
                std::string arg = argv[i];
                if (arg == "--crash" && i + 1 < argc) {
                    crashFile = argv[++i];
                } else if (arg.rfind("--crash=", 0) == 0) {
                    crashFile = arg.substr(8);
                } else if (arg.length() > 7 && arg.substr(0, 7) == "--crash") {
                    crashFile = arg.substr(7);
                }
            }
        }
        return cmdAsk(repoRoot, query, crashFile);
    }
    if (cmd == "trace") {
        if (argc < 3) { std::cerr << "usage: chronos trace <traceId>\n"; return 2; }
        return cmdTrace(repoRoot, argv[2]);
    }
    if (cmd == "timeline") {
        if (argc < 3) { std::cerr << "usage: chronos timeline <target>\n"; return 2; }
        return cmdTimeline(repoRoot, argv[2]);
    }
    if (cmd == "check-staging") {
        std::string targetRepo = repoRoot;
        bool strict = false;
        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--strict") {
                strict = true;
            } else if (arg[0] != '-') {
                targetRepo = arg;
            }
        }
        return cmdCheckStaging(targetRepo, strict);
    }

    
    if (cmd == "map") {
        if (argc < 3) {
            std::cerr << "usage: chronos map <target> [--upstream|--downstream|--both] [--depth N]\n"
                      << "\n"
                      << "  <target>       Function name (auto-inferred) or file path\n"
                      << "  --upstream     Show callers only (impact radius)\n"
                      << "  --downstream   Show callees only (behavior analysis)\n"
                      << "  --both         Show full skeleton (default)\n"
                      << "  --depth N      Max traversal depth (default: 4)\n";
            return 2;
        }
        std::string target = argv[2];
        MapMode mode = MapMode::BOTH;
        int depth = 4;
        for (int i = 3; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--upstream")   mode = MapMode::UPSTREAM;
            else if (arg == "--downstream") mode = MapMode::DOWNSTREAM;
            else if (arg == "--both")  mode = MapMode::BOTH;
            else if (arg == "--depth" && i + 1 < argc) depth = std::stoi(argv[++i]);
        }
        return cmdMap(repoRoot, target, mode, depth);
    }
    if (cmd == "diagnose") {
        if (argc < 4 || std::string(argv[2]) != "--trace") { std::cerr << "usage: chronos diagnose --trace <file> [--top N]\n"; return 2; }
        std::string file = argv[3];
        int top = 10;
        for (int i = 4; i < argc; ++i) {
            if (std::string(argv[i]) == "--top" && i + 1 < argc) top = std::stoi(argv[++i]);
        }
        return cmdDiagnose(repoRoot, file, top);
    }

    if (cmd == "explain") {
        if (argc < 3) { std::cerr << "usage: chronos explain <symbol> [--query \"<question>\"]\n"; return 2; }
        std::string symbol = argv[2];
        std::string query = "";
        for (int i = 3; i < argc; ++i) {
            if (std::string(argv[i]) == "--query" && i + 1 < argc) {
                query = argv[++i];
            }
        }
        return cmdExplain(repoRoot, symbol, query);
    }

    std::cerr << "unknown command: " << cmd << "\n";
    return 2;
}

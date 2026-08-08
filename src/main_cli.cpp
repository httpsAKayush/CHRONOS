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
#include "chronos/codex.hpp"
#include "chronos/vector_index.hpp"
#include "chronos/context_builder.hpp"
#include "chronos/oracle.hpp"
#include "chronos/git_indexer.hpp"
#include "chronos/ipc.hpp"
#include "chronos/ast_indexer.hpp"
#include "chronos/ast_mutation_scorer.hpp"
#include "chronos/env.hpp"
#include "chronos/diagnose.hpp"

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
        fs::path hookSrc = fs::path(repoRoot) / "scripts" / "pre-commit";
        if (fs::exists(hookSrc)) {
            fs::copy_file(hookSrc, hookDest, fs::copy_options::overwrite_existing);
            fs::permissions(hookDest, fs::perms::owner_all | fs::perms::group_read | fs::perms::others_read);
        } else {
            std::cerr << "warning: scripts/pre-commit not found next to chronos binary; "
                         "hook not installed automatically. See SETUP.md.\n";
        }
    }

    // Touch the Codex + VectorIndex so `.chronos/codex.db` exists immediately.
    Codex codex(repoRoot);
    VectorIndex vectors(repoRoot);
    std::cout << "Chronos initialized at " << repoRoot << "/.chronos\n";
    return 0;
}

int cmdSync(const std::string& repoRoot) {
    // Spec §11 mitigation for "Index staleness if Git Hook is bypassed":
    // diff HEAD's tracked files against what the Codex currently knows and
    // re-index anything the Codex is missing or that changed. This is a
    // simplified, dependency-free version -- it walks tracked C/C++ files
    // rather than a true libgit2 diff, since correctness (converge to
    // matching state) matters more here than incremental speed.
    Codex codex(repoRoot);
    VectorIndex vectors(repoRoot);
    AstIndexer indexer(codex, vectors, repoRoot);

    int count = 0;
    for (auto& entry : fs::recursive_directory_iterator(repoRoot)) {
        if (entry.path().string().find("/.chronos/") != std::string::npos) continue;
        if (entry.path().string().find("/.git/") != std::string::npos) continue;
        auto ext = entry.path().extension().string();
        if (ext == ".cpp" || ext == ".h" || ext == ".hpp" || ext == ".cc" || ext == ".py" || ext == ".md") {
            std::string rel = fs::relative(entry.path(), repoRoot).string();
            indexer.indexFile(rel, "sync");
            ++count;
        }
    }

    GitIndexer gitIndexer(repoRoot, codex, indexer);
    std::cout << "chronos sync: mapping temporal Git graph...\n";
    gitIndexer.indexHistory();

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
            
            // We bypass the standard context builder and just send the payload directly
            built.ok = true;
            built.request.userQuery = payload.str();
            built.request.traceId = "crash-" + topCand.commitHash;
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

    std::cout << "[3/3] Generating response...\n";
    bool gotAnyText = false;
    std::string fullText;
    client.sendAndStream(built.request, [&](const ChronosResponseChunk& chunk) {
        if (!chunk.textDelta.empty()) {
            gotAnyText = true;
            fullText += chunk.textDelta;
            std::cout << chunk.textDelta;
        }
    });
    std::cout << "\n";

    if (!gotAnyText) {
        // FR-7 fallback also covers "model errored / returned nothing".
        std::cout << "\n[!] LLM produced no response -- falling back to Oracle-Only Mode.\n\n";
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

int cmdMap(const std::string& repoRoot, const std::string& target, int depth, bool excludeExternal, const std::string& flow, const std::string& format) {
    Codex codex(repoRoot);
    std::string rootId;
    try {
        rootId = codex.resolveAlias(target);
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
    auto startNode = codex.getNode(rootId);
    if (!startNode) {
        std::cerr << "chronos map: unknown target '" << target << "'\n";
        return 1;
    }
    
    // Pass 1: Collect nodes to summarize
    std::set<std::string> nodesToPrint;
    std::set<std::string> visitedCollect;
    
    std::function<void(const std::string&, int, bool)> collectTree = 
        [&](const std::string& currentEdgeId, int currentDepth, bool isIncoming) {
        if (currentDepth > depth) return;
        std::string resolvedId = codex.resolveAlias(currentEdgeId);
        
        visitedCollect.insert(resolvedId);
        nodesToPrint.insert(resolvedId);
        if (currentDepth == depth) { visitedCollect.erase(resolvedId); return; }
        
        std::vector<Edge> edges = codex.getEdges(resolvedId, !isIncoming);
        for (const auto& edge : edges) {
            if (excludeExternal && edge.type == "external_symbol") continue;
            std::string nextId = isIncoming ? edge.source_id : edge.target_id;
            std::string nextResolvedId;
            try {
                nextResolvedId = codex.resolveAlias(nextId);
            } catch (const std::exception&) {
                continue;
            }
            if (!visitedCollect.count(nextResolvedId)) {
                collectTree(nextResolvedId, currentDepth + 1, isIncoming);
            }
        }
        visitedCollect.erase(resolvedId);
    };

    if (flow == "upstream" || flow == "") collectTree(startNode->id, 0, true);
    if (flow == "downstream" || flow == "") collectTree(startNode->id, 0, false);
    
    // Batch summarize
    ChronosRequest req;
    req.command = "summarize";
    req.traceId = "map_summary";
    for (const auto& nid : nodesToPrint) {
        auto n = codex.getNode(nid);
        if (n && n->ai_summary.empty() && n->is_active && n->file_path != "external_symbol") {
            ContextNode cn;
            cn.nodeId = nid;
            cn.filePath = n->file_path;
            cn.codeSnippet = readSnippetForNode(*n, repoRoot);
            req.context.push_back(cn);
        }
    }
    
    if (!req.context.empty()) {
        IpcClient client;
        if (client.connect(socketPathForRepo(repoRoot))) {
            std::string fullResponse;
            client.sendAndStream(req, [&](const ChronosResponseChunk& chunk) {
                fullResponse += chunk.textDelta;
            });
            std::istringstream stream(fullResponse);
            std::string line;
            while (std::getline(stream, line)) {
                size_t pos = line.find('|');
                if (pos != std::string::npos) {
                    std::string nid = line.substr(0, pos);
                    std::string summary = line.substr(pos + 1);
                    if (!summary.empty() && summary.back() == '\r') summary.pop_back();
                    if (!summary.empty() && summary.front() == ' ') summary = summary.substr(1);
                    codex.updateAiSummary(nid, summary);
                }
            }
        } else {
            std::cerr << "[!] Could not connect to Chronos daemon. Summaries will be missing.\n";
        }
    }

    std::cout << "\nArchitect Map for " << target << " [node:" << startNode->id.substr(0, 8) << "]\n";
    std::cout << "========================================================\n";

    std::set<std::string> visitedPrint;
    
    std::function<void(const std::string&, int, bool, const std::string&)> printTree = 
        [&](const std::string& currentEdgeId, int currentDepth, bool isIncoming, const std::string& edgeText) {
        
        std::string resolvedId;
        try {
            resolvedId = codex.resolveAlias(currentEdgeId);
        } catch (const std::exception&) {
            return;
        }
        
        // Indentation logic
        std::string prefix = "";
        for (int i = 0; i < currentDepth; ++i) {
            if (i == currentDepth - 1) prefix += " ├── ";
            else prefix += " │   ";
        }
        
        if (currentDepth == 0) {
            std::cout << (isIncoming ? "[UPSTREAM DATA FLOW]\n" : "[DOWNSTREAM DATA FLOW]\n");
        } else {
            // Print the edge call site
            std::cout << prefix << edgeText << "\n";
            
            // Print AI sticky note below it if available
            auto node = codex.getNode(resolvedId);
            if (node && !node->ai_summary.empty()) {
                std::string summaryPrefix = "";
                for (int i = 0; i < currentDepth; ++i) {
                    if (i == currentDepth - 1) summaryPrefix += " │   ";
                    else summaryPrefix += " │   ";
                }
                std::cout << summaryPrefix << "  \033[90m[" << node->ai_summary << "]\033[0m\n";
            }
            std::cout << " │\n";
        }

        if (currentDepth >= depth) return;
        
        visitedPrint.insert(resolvedId);
        
        std::vector<Edge> edges = codex.getEdges(resolvedId, !isIncoming);
        
        // Filter and collect child edges
        std::vector<Edge> children;
        for (const auto& edge : edges) {
            if (excludeExternal && edge.type == "external_symbol") continue;
            std::string nextId = isIncoming ? edge.source_id : edge.target_id;
            std::string nextResolvedId;
            try {
                nextResolvedId = codex.resolveAlias(nextId);
            } catch (const std::exception&) {
                children.push_back(edge);
                continue;
            }
            if (!visitedPrint.count(nextResolvedId)) {
                children.push_back(edge);
            }
        }
        
        for (size_t i = 0; i < children.size(); ++i) {
            const auto& edge = children[i];
            std::string nextId = isIncoming ? edge.source_id : edge.target_id;
            
            std::string site = edge.call_site_text;
            if (site.empty()) {
                auto targetNode = codex.getNode(nextId);
                std::string targetName;
                if (nextId.find("sym:") == 0) {
                    std::string funcName = nextId.substr(4);
                    targetName = funcName + "() in " + (targetNode ? targetNode->file_path : "unknown file");
                } else {
                    targetName = targetNode ? targetNode->file_path : nextId.substr(0,8);
                }
                site = "call to " + targetName;
            }
            std::string lineStr = edge.start_line > 0 ? "[Line " + std::to_string(edge.start_line) + "] ──> " : "";
            std::string eText = lineStr + site;
            
            printTree(nextId, currentDepth + 1, isIncoming, eText);
        }
        
        visitedPrint.erase(resolvedId);
    };

    if (flow == "upstream" || flow == "") {
        printTree(startNode->id, 0, true, "");
        std::cout << "\n";
    }
    if (flow == "downstream" || flow == "") {
        printTree(startNode->id, 0, false, "");
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
    
    std::cout << "Diagnosing Crash Trace...\n";
    std::cout << "Resolved Frames: " << result.crashSummary << "\n\n";
    std::cout << "Top Suspects:\n";
    for (size_t i = 0; i < result.candidates.size(); ++i) {
        const auto& c = result.candidates[i];
        std::cout << "Suspect #" << (i+1) << ": [Score: " << c.score << "]\n";
        std::cout << "  Commit: " << c.commitHash << " (" << c.timestamp << ")\n";
        std::cout << "  Message: " << c.commitMessage << "\n";
        if (c.hasDeferredWork) {
            std::cout << "  [!] Deferred Work Detected\n";
        }
        std::cout << "  Path: " << c.dependencyPath << "\n\n";
    }
    
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: chronos <init|ask|sync|trace|timeline|check-staging> [args]\n";
        return 2;
    }
    std::string cmd = argv[1];
    std::string repoRoot = fs::current_path().string();

    auto env = loadEnv(repoRoot);
    for (const auto& [k, v] : env) {
        setenv(k.c_str(), v.c_str(), 1);
    }

    if (cmd == "init") return cmdInit(repoRoot);
    if (cmd == "sync") return cmdSync(repoRoot);
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
        if (argc < 3) { std::cerr << "usage: chronos map <target> [--depth N] [--exclude-external] [--flow upstream|downstream]\n"; return 2; }
        std::string target = argv[2];
        int depth = 1;
        bool excludeExternal = false;
        std::string flow = "";
        std::string format = "";
        for (int i = 3; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--depth" && i + 1 < argc) depth = std::stoi(argv[++i]);
            else if (arg == "--exclude-external") excludeExternal = true;
            else if (arg == "--flow" && i + 1 < argc) flow = argv[++i];
            else if (arg == "--format" && i + 1 < argc) format = argv[++i];
        }
        return cmdMap(repoRoot, target, depth, excludeExternal, flow, format);
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

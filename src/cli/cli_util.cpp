#include "chronos/cli/cli_util.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <unistd.h>
#include <climits>
#include <cctype>
#include <unordered_set>

namespace fs = std::filesystem;

namespace chronos {

static std::string execCmdOutputImpl(const std::string& cmd) {
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

std::string execCmdOutput(const std::string& cmd) {
    return execCmdOutputImpl(cmd);
}

void wakeDaemon(const std::string& repoRoot) {
    char buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    std::string exeDir = (len != -1) ? fs::path(std::string(buf, len)).parent_path().string() : "";
    std::string daemonPath = exeDir.empty() ? "chronos-daemon" : (exeDir + "/chronos-daemon");
    std::string cmd = daemonPath + " \"" + repoRoot + "\" >/dev/null 2>&1 &";
    std::system(cmd.c_str());
}

std::string readSnippetForNode(const Node& n, const std::string& repoRoot) {
    // Context nodes ([GLOBAL:REPO], [CONTEXT:DIR:...]) have empty or
    // directory paths — they carry no source bytes.
    if (n.file_path.empty() || n.byte_end <= n.byte_start) return "";
    if (n.file_path.size() == 1 && n.file_path[0] == '/') return "";

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

std::string nodeLabel(const Node& n, const std::string& nodeId, Codex* codex) {
    std::string file = n.file_path;
    std::string sym;

    if (nodeId.find("sym:") == 0) {
        sym = nodeId.substr(4);
    } else if (codex) {
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(codex->raw(),
            "SELECT old_id FROM alias WHERE root_id = ?1 AND old_id LIKE 'sym:%' LIMIT 1;",
            -1, &stmt, nullptr);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, nodeId.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* oldId = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                if (oldId) sym = std::string(oldId).substr(4);
            }
            sqlite3_finalize(stmt);
        }
    }

    if (!sym.empty()) return file + " :: " + sym;
    return file;
}

int countLinesTo(const std::string& repoRoot, const std::string& relPath, int64_t byteOffset) {
    if (byteOffset <= 0) return 1;

    auto doCount = [&](const std::string& path) -> int {
        std::ifstream in(path, std::ios::binary);
        if (!in) return 0;
        int line = 1;
        char c;
        int64_t pos = 0;
        while (in.get(c) && pos < byteOffset) {
            if (c == '\n') line++;
            pos++;
        }
        return line;
    };

    int line = doCount((fs::path(repoRoot) / relPath).string());
    if (line > 0) return line;

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

static std::string peekForDocstring(std::istringstream& ss) {
    std::string line;
    int lookahead = 3;
    while (lookahead-- > 0 && std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        std::string trimmed = line.substr(first);
        if (trimmed.find("\"\"\"") == 0 || trimmed.find("'''") == 0) {
            return trimmed;
        }
        if (!trimmed.empty() && trimmed[0] != '#') break;
    }
    return "";
}

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

std::string readFunctionSignature(const Node& n, const std::string& repoRoot, const std::string& symName) {
    auto readContent = [&](const std::string& path) -> std::string {
        std::ifstream in(path, std::ios::binary);
        if (!in) return "";
        return std::string((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
    };

    auto tryPath = [&](const std::string& path) -> std::string {
        std::string content = readContent(path);
        if (content.empty()) return "";

        std::string line = extractLineAt(content, n.byte_start);
        if (!line.empty() && looksLikeSignature(line)) return line;

        std::string scanned = scanFileForSignature(content, symName);
        if (!scanned.empty()) return scanned;

        return "";
    };

    std::string sig = tryPath((fs::path(repoRoot) / n.file_path).string());
    if (!sig.empty()) return sig;

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

std::string nodeAnnotation(const Node& n, const std::string& repoRoot, const std::string& symName) {
    if (!n.ai_summary.empty()) {
        std::string s = n.ai_summary;
        if (s.size() > 42) s = s.substr(0, 39) + "...";
        return s;
    }
    if (n.byte_end <= n.byte_start + 1) return "";
    std::string sig = readFunctionSignature(n, repoRoot, symName);
    if (sig.size() > 60) sig = sig.substr(0, 57) + "...";
    return sig;
}

std::string resolveLabel(Codex& codex, const std::string& rawId) {
    auto node = codex.getNode(rawId);
    if (!node) return rawId.substr(0, 8);
    return nodeLabel(*node, rawId, &codex);
}

std::vector<BfsRow> bfsCollect(Codex& codex, const std::string& startId, bool outgoing, int maxDepth) {
    std::vector<BfsRow> rows;
    std::unordered_set<std::string> visited;
    visited.insert(startId);

    auto resolveOrSelf = [&](const std::string& id) -> std::string {
        try {
            return codex.resolveAlias(id);
        } catch (...) {
            return id;
        }
    };

    struct QEntry { std::string id; int depth; bool isLast; int startLine; std::string callSiteText; };
    std::vector<QEntry> queue;

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

bool checkStagingCollision(const std::string& stagedLine, const std::string& syntheticMsg) {
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

} // namespace chronos
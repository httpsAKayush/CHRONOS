import sys

with open("src/main_cli.cpp", "r") as f:
    content = f.read()

# Add include
content = content.replace('#include "chronos/env.hpp"', '#include "chronos/env.hpp"\n#include "chronos/diagnose.hpp"')

# The new functions
new_funcs = """
static std::string readSnippetForNode(const Node& n, const std::string& repoRoot) {
    std::string path = repoRoot + "/" + n.file_path;
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::string line;
    std::string snippet;
    int cur = 1;
    while (std::getline(f, line)) {
        if (cur >= n.start_line && cur <= n.end_line) {
            snippet += line + "\\n";
        }
        if (cur > n.end_line) break;
        cur++;
    }
    return snippet;
}

int cmdMap(const std::string& repoRoot, const std::string& target, int depth, bool excludeExternal, const std::string& flow, const std::string& format) {
    Codex codex(repoRoot);
    std::string rootId = codex.resolveAlias(target);
    auto startNode = codex.getNode(rootId);
    if (!startNode) {
        std::cerr << "chronos map: unknown target '" << target << "'\\n";
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
            std::string nextResolvedId = codex.resolveAlias(nextId);
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
                    if (!summary.empty() && summary.back() == '\\r') summary.pop_back();
                    if (!summary.empty() && summary.front() == ' ') summary = summary.substr(1);
                    codex.updateAiSummary(nid, summary);
                }
            }
        } else {
            std::cerr << "[!] Could not connect to Chronos daemon. Summaries will be missing.\\n";
        }
    }

    std::cout << "\\nArchitect Map for " << target << " [node:" << startNode->id.substr(0, 8) << "]\\n";
    std::cout << "========================================================\\n";

    std::set<std::string> visitedPrint;
    
    std::function<void(const std::string&, int, bool, const std::string&)> printTree = 
        [&](const std::string& currentEdgeId, int currentDepth, bool isIncoming, const std::string& edgeText) {
        
        std::string resolvedId = codex.resolveAlias(currentEdgeId);
        
        // Indentation logic
        std::string prefix = "";
        for (int i = 0; i < currentDepth; ++i) {
            if (i == currentDepth - 1) prefix += " ├── ";
            else prefix += " │   ";
        }
        
        if (currentDepth == 0) {
            std::cout << (isIncoming ? "[UPSTREAM DATA FLOW]\\n" : "[DOWNSTREAM DATA FLOW]\\n");
        } else {
            // Print the edge call site
            std::cout << prefix << edgeText << "\\n";
            
            // Print AI sticky note below it if available
            auto node = codex.getNode(resolvedId);
            if (node && !node->ai_summary.empty()) {
                std::string summaryPrefix = "";
                for (int i = 0; i < currentDepth; ++i) {
                    if (i == currentDepth - 1) summaryPrefix += " │   ";
                    else summaryPrefix += " │   ";
                }
                std::cout << summaryPrefix << "  \\033[90m[" << node->ai_summary << "]\\033[0m\\n";
            }
            std::cout << " │\\n";
        }

        if (currentDepth >= depth) return;
        
        visitedPrint.insert(resolvedId);
        
        std::vector<Edge> edges = codex.getEdges(resolvedId, !isIncoming);
        
        // Filter and collect child edges
        std::vector<Edge> children;
        for (const auto& edge : edges) {
            if (excludeExternal && edge.type == "external_symbol") continue;
            std::string nextId = isIncoming ? edge.source_id : edge.target_id;
            std::string nextResolvedId = codex.resolveAlias(nextId);
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
                std::string targetName = targetNode ? targetNode->file_path : nextId.substr(0,8);
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
        std::cout << "\\n";
    }
    if (flow == "downstream" || flow == "") {
        printTree(startNode->id, 0, false, "");
    }
    
    return 0;
}

int cmdDiagnose(const std::string& repoRoot, const std::string& traceFile, int topN = 10) {
    Codex codex(repoRoot);
    VectorIndex vectors(repoRoot);
    DiagnoseEngine engine(codex, vectors, repoRoot);

    std::ifstream file(traceFile);
    if (!file.is_open()) {
        std::cerr << "chronos diagnose: could not open trace file " << traceFile << "\\n";
        return 1;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    
    auto result = engine.diagnose(buffer.str(), topN);
    if (!result.ok) {
        std::cerr << "chronos diagnose failed: " << result.reason << "\\n";
        return 1;
    }
    
    std::cout << "Diagnosing Crash Trace...\\n";
    std::cout << "Resolved Frames: " << result.crashSummary << "\\n\\n";
    std::cout << "Top Suspects:\\n";
    for (size_t i = 0; i < result.candidates.size(); ++i) {
        const auto& c = result.candidates[i];
        std::cout << "Suspect #" << (i+1) << ": [Score: " << c.score << "]\\n";
        std::cout << "  Commit: " << c.commitHash << " (" << c.timestamp << ")\\n";
        std::cout << "  Message: " << c.commitMessage << "\\n";
        if (c.hasDeferredWork) {
            std::cout << "  [!] Deferred Work Detected\\n";
        }
        std::cout << "  Path: " << c.dependencyPath << "\\n\\n";
    }
    
    return 0;
}
"""

content = content.replace("} // namespace", new_funcs + "\n} // namespace")

# Add map and diagnose to main
main_add = """
    if (cmd == "map") {
        if (argc < 3) { std::cerr << "usage: chronos map <target> [--depth N] [--exclude-external] [--flow upstream|downstream]\\n"; return 2; }
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
        if (argc < 4 || std::string(argv[2]) != "--trace") { std::cerr << "usage: chronos diagnose --trace <file> [--top N]\\n"; return 2; }
        std::string file = argv[3];
        int top = 10;
        for (int i = 4; i < argc; ++i) {
            if (std::string(argv[i]) == "--top" && i + 1 < argc) top = std::stoi(argv[++i]);
        }
        return cmdDiagnose(repoRoot, file, top);
    }
"""

content = content.replace("std::cerr << \"unknown command: \" << cmd << \"\\n\";", main_add + "\n    std::cerr << \"unknown command: \" << cmd << \"\\n\";")

with open("src/main_cli.cpp", "w") as f:
    f.write(content)

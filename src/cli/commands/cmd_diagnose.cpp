#include "chronos/cli/commands.hpp"
#include "chronos/cli/cli_util.hpp"
#include "chronos/use_cases/diagnose.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>

namespace chronos {

CmdDiagnose::CmdDiagnose(const CliContext& ctx) : ctx_(ctx) {}

int CmdDiagnose::execute(int argc, char** argv) {
    if (argc < 4 || std::string(argv[2]) != "--trace") {
        std::cerr << "usage: chronos diagnose --trace <file> [--top N]\n";
        return 2;
    }
    std::string file = argv[3];
    int topN = 10;
    for (int i = 4; i < argc; ++i) {
        if (std::string(argv[i]) == "--top" && i + 1 < argc) topN = std::stoi(argv[++i]);
    }

    Codex& codex = *ctx_.storage;
    VectorIndex& vectors = *ctx_.vectors;
    DiagnoseEngine engine(codex, vectors, ctx_.repoRoot);

    std::ifstream f(file);
    if (!f.is_open()) {
        std::cerr << "chronos diagnose: could not open trace file " << file << "\n";
        return 1;
    }
    std::stringstream buffer;
    buffer << f.rdbuf();

    auto result = engine.diagnose(buffer.str(), topN);
    if (!result.ok) {
        std::cerr << "chronos diagnose failed: " << result.reason << "\n";
        return 1;
    }

    std::cout << "\n[ CRASH DIAGNOSTICS ]\n";
    std::cout << "Trace: " << result.crashSummary << "\n";
    std::string signature = "Unknown";
    std::istringstream css(buffer.str());
    std::string line;
    while (std::getline(css, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) { signature = line; break; }
    }
    std::cout << "Signature: " << signature << "\n";
    std::cout << "========================================================\n\n";

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
            std::string fpath = n ? n->file_path : "unknown";
            groups.push_back({c.nodeId, fpath + " :: " + label, c.score, c.dependencyPath, {c}});
        }
    }

    if (groups.empty()) {
        std::cout << "  (no suspects found)\n";
        return 0;
    }

    std::string promptStr = "A crash occurred with this signature: " + signature + "\n\n";
    promptStr += "Raw trace:\n" + buffer.str() + "\n\n";
    promptStr += "Structural pathways and temporal keyframes identified by the Diagnosis Engine:\n";

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

    std::string sockPath = socketPathForRepo(ctx_.repoRoot);
    IpcClient client;
    bool daemonUp = client.connect(sockPath);
    if (!daemonUp) {
        wakeDaemon(ctx_.repoRoot);
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
    for (int i = 0; i < 16; ++i) req.traceId += hexStr[rand() % 16];
    req.systemPromptOverride = "You are the Chronos Diagnosis Engine. You synthesize raw structural traces and temporal commits into a root cause analysis.";

    for (const auto& g : groups) {
        auto n = codex.getNode(g.nodeId);
        if (n) {
            ContextNode cn;
            cn.nodeId = g.nodeId;
            cn.filePath = n->file_path;
            cn.codeSnippet = readSnippetForNode(*n, ctx_.repoRoot);
            req.context.push_back(cn);
        }
    }

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
            std::cout << chunk.textDelta << std::flush;
        }
    });
    std::cout << "\n";

    bool isApiError = (fullText.find("[API Error]") != std::string::npos ||
                       fullText.find("[LLM Unavailable]") != std::string::npos ||
                       fullText.find("\"error\":") != std::string::npos);
    if (!gotAnyText || isApiError) {
        if (isApiError) std::cout << "\n[!] LLM returned an API error -- falling back to raw output.\n\n";
        else std::cout << "\n[!] LLM produced no response -- falling back to raw output.\n\n";
        std::cout << promptStr;
    }

    std::cout << "========================================================\n\n";

    return 0;
}

} // namespace chronos
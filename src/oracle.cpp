#include "chronos/oracle.hpp"
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <fstream>
#include <algorithm>

namespace chronos {

std::string Oracle::renderTrace(const TraceResult& traceRes) const {
    std::ostringstream out;
    if (traceRes.any_low_confidence) {
        out << "Structural resolution is uncertain in this region.\n\n";
    }

    std::unordered_map<std::string, std::vector<Edge>> outEdges;
    for (auto& e : traceRes.edges) outEdges[e.source_id].push_back(e);

    for (auto& n : traceRes.nodes) {
        if (!n.is_active) continue;

        out << "\n================================================================================\n";
        out << "FILE: " << n.file_path << "  [" << n.id.substr(0, 8) << "...]\n";
        out << "CONFIDENCE: " << (n.parse_confidence >= 0.5f ? "HIGH (Structured AST Node)" : "LOW (Whole-File Degrade)") << "\n";
        out << "--------------------------------------------------------------------------------\n";

        std::string fullPath = repoRoot_ + "/" + n.file_path;
        std::ifstream in(fullPath, std::ios::binary);
        if (in) {
            std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            int64_t start = std::max<int64_t>(0LL, static_cast<long long>(n.byte_start));
            int64_t end = std::min<int64_t>(static_cast<int64_t>(content.size()), n.byte_end);
            if (end > start) {
                out << content.substr(start, end - start);
                if (content.substr(start, end - start).back() != '\n') out << '\n';
            }
        }
        out << "================================================================================\n";

        auto it = outEdges.find(n.id);
        if (it != outEdges.end()) {
            out << "\n  [Outgoing Calls / Relationships]\n";
            for (auto& e : it->second) {
                out << "  -> " << e.type << " " << e.target_id.substr(0, 8) << "...\n";
            }
        }
    }
    return out.str();
}

CitationCheckResult Oracle::verifyCitations(const std::string& llmText) {
    CitationCheckResult result;
    const std::string marker = "[node:";
    size_t pos = 0;
    std::unordered_set<std::string> seen;
    while ((pos = llmText.find(marker, pos)) != std::string::npos) {
        size_t start = pos + marker.size();
        size_t end = llmText.find(']', start);
        if (end == std::string::npos) break;
        std::string nodeId = llmText.substr(start, end - start);
        pos = end + 1;
        if (seen.count(nodeId)) continue;
        seen.insert(nodeId);

        if (!codex_.getNode(nodeId).has_value()) {
            result.allValid = false;
            result.invalidNodeIds.push_back(nodeId);
        }
    }
    return result;
}

} // namespace chronos

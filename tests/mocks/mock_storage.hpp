#pragma once

#include "chronos/core/IStorage.hpp"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <optional>
#include <cstdint>
#include <algorithm>

namespace chronos {

class MockStorage : public IStorage {
public:
    MockStorage() = default;
    ~MockStorage() override = default;

    // --- Node Operations ---
    void upsertNode(const Node& n) override {
        nodes_[n.id] = n;
    }

    void updateAiSummary(const std::string& nodeId, const std::string& summary) override {
        auto it = nodes_.find(nodeId);
        if (it != nodes_.end()) {
            it->second.ai_summary = summary;
        }
    }

    void upsertContextNode(const std::string& id, const std::string& filePath, const std::string& summary) override {
        Node n;
        n.id = id;
        n.file_path = filePath;
        n.ai_summary = summary;
        n.byte_start = 0;
        n.byte_end = 0;
        nodes_[id] = n;
    }

    void tombstoneNode(const std::string& nodeId) override {
        auto it = nodes_.find(nodeId);
        if (it != nodes_.end()) {
            it->second.is_active = false;
        }
    }

    std::optional<Node> getNode(const std::string& id) override {
        auto it = nodes_.find(id);
        if (it != nodes_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    std::optional<Node> findBySimhash(uint64_t simhash, const std::string& filePath) override {
        for (const auto& [id, n] : nodes_) {
            if (n.is_active && n.simhash == simhash && n.file_path == filePath) {
                return n;
            }
        }
        return std::nullopt;
    }

    std::optional<Node> findBySimhashGlobal(uint64_t simhash) override {
        for (const auto& [id, n] : nodes_) {
            if (n.is_active && n.simhash == simhash) {
                return n;
            }
        }
        return std::nullopt;
    }

    std::vector<Node> queryNodesByPathPrefix(const std::string& prefix) override {
        std::vector<Node> result;
        for (const auto& [id, n] : nodes_) {
            if (n.is_active && n.file_path.rfind(prefix, 0) == 0) {
                result.push_back(n);
            }
        }
        return result;
    }

    // --- Edge Operations ---
    void upsertEdge(const Edge& e) override {
        for (auto& edge : edges_) {
            if (edge.source_id == e.source_id && edge.target_id == e.target_id && edge.type == e.type) {
                edge = e;
                return;
            }
        }
        edges_.push_back(e);
    }

    std::vector<Edge> getEdges(const std::string& nodeId, bool outgoing) override {
        std::vector<Edge> result;
        for (const auto& e : edges_) {
            if (outgoing && e.source_id == nodeId) {
                result.push_back(e);
            } else if (!outgoing && e.target_id == nodeId) {
                result.push_back(e);
            }
        }
        return result;
    }

    std::vector<Edge> getEdgesFiltered(const std::string& nodeId, bool outgoing) override {
        std::vector<Edge> result;
        for (const auto& e : edges_) {
            if (outgoing && e.source_id == nodeId) {
                auto tgtIt = nodes_.find(e.target_id);
                if (tgtIt != nodes_.end() && tgtIt->second.is_active) {
                    result.push_back(e);
                }
            } else if (!outgoing && e.target_id == nodeId) {
                auto srcIt = nodes_.find(e.source_id);
                if (srcIt != nodes_.end() && srcIt->second.is_active) {
                    result.push_back(e);
                }
            }
        }
        return result;
    }

    // --- History Operations ---
    void appendHistory(const HistoryEntry& h) override {
        history_entries_.push_back(h);
    }

    void recordHistory(const std::string& nodeId, const std::string& commitHash, int64_t timestamp, const std::string& msg) override {
        history_records_.push_back(HistoryRecord{nodeId, commitHash, timestamp, msg});
    }

    std::vector<HistoryRecord> getHistory(const std::string& nodeId) override {
        std::vector<HistoryRecord> result;
        for (const auto& rec : history_records_) {
            if (rec.nodeId == nodeId) {
                result.push_back(rec);
            }
        }
        return result;
    }

    std::vector<HistoryRecord> getHistoryForFile(const std::string& filePath) override {
        std::vector<HistoryRecord> result;
        std::unordered_set<std::string> fileNodeIds;
        for (const auto& [id, n] : nodes_) {
            if (n.file_path == filePath) {
                fileNodeIds.insert(id);
            }
        }
        for (const auto& rec : history_records_) {
            if (fileNodeIds.count(rec.nodeId)) {
                result.push_back(rec);
            }
        }
        return result;
    }

    // --- Alias Operations ---
    void recordAlias(const std::string& oldId, const std::string& newId, const std::string& commitHash) override {
        aliases_[oldId] = newId;
        alias_commits_[oldId] = commitHash;
    }

    std::string resolveAlias(const std::string& id) override {
        std::string curr = id;
        std::unordered_set<std::string> visited;
        while (true) {
            auto it = aliases_.find(curr);
            if (it == aliases_.end()) break;
            if (visited.count(curr)) break;
            visited.insert(curr);
            curr = it->second;
        }
        return curr;
    }

    // --- File Import Operations ---
    struct FileImportRecord {
        std::string filePath;
        std::string symbolName;
        std::string sourceModule;
    };

    void insertFileImport(const std::string& filePath, const std::string& symbolName, const std::string& sourceModule) override {
        file_imports_.push_back(FileImportRecord{filePath, symbolName, sourceModule});
    }

    void clearFileImports(const std::string& filePath) override {
        std::erase_if(file_imports_, [&](const FileImportRecord& fi) {
            return fi.filePath == filePath;
        });
    }

    // --- Transaction Control ---
    void beginTransaction() override {
        in_transaction_ = true;
    }

    void commitTransaction() override {
        in_transaction_ = false;
    }

    // --- Trace Operations ---
    void recordTrace(const std::string& traceId, const TraceResult& trace) override {
        traces_[traceId] = trace;
    }

    std::optional<TraceResult> getTrace(const std::string& traceId) override {
        auto it = traces_.find(traceId);
        if (it != traces_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    // --- PPR / Graph Traversal ---
    TraceResult localPushPPR(const std::string& seedNodeId, int budget, double dampingFactor = 0.85) override {
        return localPushPPR(std::vector<std::string>{seedNodeId}, budget, dampingFactor);
    }

    TraceResult localPushPPR(const std::vector<std::string>& seeds, int budget, double dampingFactor = 0.85) override {
        (void)dampingFactor;
        TraceResult result;
        if (seeds.empty() || budget <= 0) return result;

        std::unordered_map<std::string, double> pprScores;
        std::unordered_map<std::string, double> residual;

        for (const auto& seed : seeds) {
            residual[seed] = 1.0 / seeds.size();
        }

        int steps = 0;
        while (steps < budget * 10) {
            std::string topNode;
            double maxRes = 0.0;
            for (const auto& [nid, res] : residual) {
                if (res > maxRes) {
                    maxRes = res;
                    topNode = nid;
                }
            }

            if (maxRes < 1e-6 || topNode.empty()) break;

            double pushVal = residual[topNode];
            residual[topNode] = 0.0;
            pprScores[topNode] += pushVal * (1.0 - dampingFactor);

            double retain = pushVal * dampingFactor;
            auto outEdges = getEdgesFiltered(topNode, true);
            if (outEdges.empty()) {
                pprScores[topNode] += retain;
            } else {
                double share = retain / static_cast<double>(outEdges.size());
                for (const auto& e : outEdges) {
                    residual[e.target_id] += share;
                }
            }
            steps++;
        }

        std::vector<std::pair<std::string, double>> ranked(pprScores.begin(), pprScores.end());
        std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
            return a.second > b.second;
        });

        if (static_cast<int>(ranked.size()) > budget) {
            ranked.resize(budget);
        }

        for (const auto& [nid, score] : ranked) {
            auto n = getNode(nid);
            if (n) {
                if (n->parse_confidence < 0.5f) {
                    result.any_low_confidence = true;
                }
                result.nodes.push_back(*n);
            }
        }

        for (const auto& n : result.nodes) {
            auto outEdges = getEdges(n.id, true);
            for (const auto& e : outEdges) {
                result.edges.push_back(e);
            }
        }

        return result;
    }

    // --- Helper Inspection Accessors ---
    const std::unordered_map<std::string, Node>& nodes() const { return nodes_; }
    const std::vector<Edge>& edges() const { return edges_; }
    const std::vector<HistoryEntry>& historyEntries() const { return history_entries_; }
    const std::vector<HistoryRecord>& historyRecords() const { return history_records_; }
    const std::vector<FileImportRecord>& fileImports() const { return file_imports_; }
    bool inTransaction() const { return in_transaction_; }

private:
    std::unordered_map<std::string, Node> nodes_;
    std::vector<Edge> edges_;
    std::vector<HistoryEntry> history_entries_;
    std::vector<HistoryRecord> history_records_;
    std::unordered_map<std::string, std::string> aliases_;
    std::unordered_map<std::string, std::string> alias_commits_;
    std::vector<FileImportRecord> file_imports_;
    std::unordered_map<std::string, TraceResult> traces_;
    bool in_transaction_ = false;
};

} // namespace chronos

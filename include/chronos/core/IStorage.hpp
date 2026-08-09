#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace chronos {

struct Node {
    std::string id;          // UUID
    std::string file_path;
    int64_t byte_start = 0;
    int64_t byte_end = 0;
    uint64_t simhash = 0;
    bool is_active = true;
    float parse_confidence = 1.0f;
    std::string ai_summary;
};

struct Edge {
    std::string source_id;
    std::string target_id;
    std::string type;
    float probable_target_weight = 1.0f;
    int start_line = 0;
    std::string call_site_text;
};

struct HistoryEntry {
    std::string node_id;
    std::string commit_hash;
    std::string intent_summary;
};

struct TraceResult {
    std::vector<Node> nodes;
    std::vector<Edge> edges;
    bool any_low_confidence = false;
};

struct HistoryRecord {
    std::string nodeId;
    std::string commitHash;
    int64_t timestamp;
    std::string syntheticMsg;
};

class IStorage {
public:
    virtual ~IStorage() = default;

    virtual void upsertNode(const Node& n) = 0;
    virtual void updateAiSummary(const std::string& nodeId, const std::string& summary) = 0;
    virtual void tombstoneNode(const std::string& nodeId) = 0;
    virtual void upsertEdge(const Edge& e) = 0;
    virtual void appendHistory(const HistoryEntry& h) = 0;
    virtual void recordAlias(const std::string& oldId, const std::string& newId, const std::string& commitHash) = 0;
    virtual std::string resolveAlias(const std::string& id) = 0;
    virtual void insertFileImport(const std::string& filePath, const std::string& symbolName, const std::string& sourceModule) = 0;
    virtual void clearFileImports(const std::string& filePath) = 0;
    virtual void recordHistory(const std::string& nodeId, const std::string& commitHash, int64_t timestamp, const std::string& msg) = 0;
    virtual std::vector<HistoryRecord> getHistory(const std::string& nodeId) = 0;
    virtual std::vector<HistoryRecord> getHistoryForFile(const std::string& filePath) = 0;
    virtual void beginTransaction() = 0;
    virtual void commitTransaction() = 0;
    virtual std::optional<Node> getNode(const std::string& id) = 0;
    virtual std::optional<Node> findBySimhash(uint64_t simhash, const std::string& filePath) = 0;
    virtual std::optional<Node> findBySimhashGlobal(uint64_t simhash) = 0;
    virtual std::vector<Edge> getEdges(const std::string& nodeId, bool outgoing) = 0;
    virtual std::vector<Edge> getEdgesFiltered(const std::string& nodeId, bool outgoing) = 0;
    virtual std::vector<Node> queryNodesByPathPrefix(const std::string& prefix) = 0;
    virtual void recordTrace(const std::string& traceId, const TraceResult& trace) = 0;
    virtual std::optional<TraceResult> getTrace(const std::string& traceId) = 0;
    virtual TraceResult localPushPPR(const std::string& seedNodeId, int budget, double dampingFactor = 0.85) = 0;
    virtual TraceResult localPushPPR(const std::vector<std::string>& seeds, int budget, double dampingFactor = 0.85) = 0;
};

} // namespace chronos

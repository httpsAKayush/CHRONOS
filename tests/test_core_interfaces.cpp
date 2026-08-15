#include "test_framework.hpp"
#include "chronos/core/IStorage.hpp"
#include "chronos/core/ILLMClient.hpp"
#include "chronos/core/IGitScanner.hpp"
#include "chronos/core/IConfig.hpp"
#include <memory>
#include <vector>

namespace chronos {

class MockStorage : public IStorage {
public:
    void upsertNode(const Node& n) override { lastNode_ = n; }
    void updateAiSummary(const std::string& nodeId, const std::string& summary) override { (void)nodeId; (void)summary; }
    void upsertContextNode(const std::string& id, const std::string& filePath, const std::string& summary) override { (void)id; (void)filePath; (void)summary; }
    void tombstoneNode(const std::string& nodeId) override { (void)nodeId; }
    void upsertEdge(const Edge& e) override { (void)e; }
    void appendHistory(const HistoryEntry& h) override { (void)h; }
    void recordAlias(const std::string& oldId, const std::string& newId, const std::string& commitHash) override { (void)oldId; (void)newId; (void)commitHash; }
    std::string resolveAlias(const std::string& id) override { return id; }
    void insertFileImport(const std::string& filePath, const std::string& symbolName, const std::string& sourceModule) override { (void)filePath; (void)symbolName; (void)sourceModule; }
    void clearFileImports(const std::string& filePath) override { (void)filePath; }
    void recordHistory(const std::string& nodeId, const std::string& commitHash, int64_t timestamp, const std::string& msg) override { (void)nodeId; (void)commitHash; (void)timestamp; (void)msg; }
    std::vector<HistoryRecord> getHistory(const std::string& nodeId) override { (void)nodeId; return {}; }
    std::vector<HistoryRecord> getHistoryForFile(const std::string& filePath) override { (void)filePath; return {}; }
    void beginTransaction() override {}
    void commitTransaction() override {}
    std::optional<Node> getNode(const std::string& id) override {
        if (lastNode_.has_value() && lastNode_->id == id) return lastNode_;
        return std::nullopt;
    }
    std::optional<Node> findBySimhash(uint64_t simhash, const std::string& filePath) override { (void)simhash; (void)filePath; return std::nullopt; }
    std::optional<Node> findBySimhashGlobal(uint64_t simhash) override { (void)simhash; return std::nullopt; }
    std::vector<Edge> getEdges(const std::string& nodeId, bool outgoing) override { (void)nodeId; (void)outgoing; return {}; }
    std::vector<Edge> getEdgesFiltered(const std::string& nodeId, bool outgoing) override { (void)nodeId; (void)outgoing; return {}; }
    std::vector<Node> queryNodesByPathPrefix(const std::string& prefix) override { (void)prefix; return {}; }
    void recordTrace(const std::string& traceId, const TraceResult& trace) override { (void)traceId; (void)trace; }
    std::optional<TraceResult> getTrace(const std::string& traceId) override { (void)traceId; return std::nullopt; }
    TraceResult localPushPPR(const std::string& seedNodeId, int budget, double dampingFactor = 0.85) override { (void)seedNodeId; (void)budget; (void)dampingFactor; return {}; }
    TraceResult localPushPPR(const std::vector<std::string>& seeds, int budget, double dampingFactor = 0.85) override { (void)seeds; (void)budget; (void)dampingFactor; return {}; }

    // Repo Metadata
    void setRepoMetadata(const std::string& key, const std::string& value) override { (void)key; (void)value; }
    std::optional<std::string> getRepoMetadata(const std::string& key) override { (void)key; return std::nullopt; }

    // FTS5
    std::vector<std::string> ftsSearch(const std::string& query, int limit) override { (void)query; (void)limit; return {}; }

private:
    std::optional<Node> lastNode_;
};

class MockLLMClient : public ILLMClient {
public:
    std::string complete(const std::string& systemPrompt, const std::string& userQuery, int maxTokens) override {
        (void)systemPrompt; (void)maxTokens;
        return "complete:" + userQuery;
    }
    bool stream(const std::string& systemPrompt, const std::string& userQuery, int maxTokens,
                const std::function<void(const std::string&)>& onChunk) override {
        (void)systemPrompt; (void)maxTokens;
        onChunk("chunk");
        return true;
    }
    bool streamChat(const std::vector<ChatMessage>&, int, const std::function<void(const std::string&)>&) override {
        return true;
    }
    std::vector<float> embed(const std::string& text) override { (void)text; return {1.0f}; }
    std::string query(const std::string& prompt) override { return "response:" + prompt; }
    std::string generateSummary(const std::string& codeSnippet) override { return "summary:" + codeSnippet; }
    bool isAvailable() const override { return true; }
};

class MockGitScanner : public IGitScanner {
public:
    std::vector<GitCommit> getCommits(int maxDepth = 100) override { (void)maxDepth; return {GitCommit{"abc", "", "msg", "author", 100}}; }
    std::optional<GitCommit> getCommit(const std::string& hash) override {
        if (hash == "abc") return GitCommit{"abc", "", "msg", "author", 100};
        return std::nullopt;
    }
    std::vector<std::string> getModifiedFiles(const std::string& commitHash) override { (void)commitHash; return {"file.cpp"}; }
    std::string getFileContentAtCommit(const std::string& filePath, const std::string& commitHash) override { (void)filePath; (void)commitHash; return "content"; }
    std::string getCurrentCommitHash() const override { return "abc"; }
    void indexHistory(int syncDepthChoice = 3) override { (void)syncDepthChoice; }
};

class MockConfig : public IConfig {
public:
    std::string get(const std::string& key, const std::string& defaultValue = "") const override {
        auto it = map_.find(key);
        return it != map_.end() ? it->second : defaultValue;
    }
    bool getBool(const std::string& key, bool defaultValue = false) const override {
        auto val = get(key);
        if (val == "true") return true;
        if (val == "false") return false;
        return defaultValue;
    }
    int getInt(const std::string& key, int defaultValue = 0) const override {
        auto val = get(key);
        if (!val.empty()) return std::stoi(val);
        return defaultValue;
    }
    bool has(const std::string& key) const override { return map_.find(key) != map_.end(); }
    std::unordered_map<std::string, std::string> getAll() const override { return map_; }

    void set(const std::string& k, const std::string& v) { map_[k] = v; }

private:
    std::unordered_map<std::string, std::string> map_;
};

} // namespace chronos

void run_core_interfaces_tests() {
    using namespace chronos;

    // Test IStorage
    std::unique_ptr<IStorage> storage = std::make_unique<MockStorage>();
    Node n;
    n.id = "node_1";
    n.file_path = "src/main.cpp";
    storage->upsertNode(n);
    auto retrieved = storage->getNode("node_1");
    CHRONOS_CHECK(retrieved.has_value());
    CHRONOS_CHECK(retrieved->id == "node_1");

    // Test ILLMClient
    std::unique_ptr<ILLMClient> llm = std::make_unique<MockLLMClient>();
    CHRONOS_CHECK(llm->isAvailable());
    CHRONOS_CHECK(llm->query("hello") == "response:hello");

    // Test IGitScanner
    std::unique_ptr<IGitScanner> git = std::make_unique<MockGitScanner>();
    CHRONOS_CHECK(git->getCurrentCommitHash() == "abc");
    CHRONOS_CHECK(git->getCommits().size() == 1);

    // Test IConfig
    MockConfig cfgMock;
    cfgMock.set("debug", "true");
    cfgMock.set("port", "8080");
    std::unique_ptr<IConfig> config = std::make_unique<MockConfig>(cfgMock);
    CHRONOS_CHECK(config->getBool("debug", false) == true);
    CHRONOS_CHECK(config->getInt("port", 0) == 8080);
    CHRONOS_CHECK(config->has("debug"));
    CHRONOS_CHECK(!config->has("nonexistent"));
}

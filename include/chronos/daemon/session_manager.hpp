#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <sqlite3.h>

#include "chronos/core/ILLMClient.hpp"

namespace chronos {

class SessionManager {
public:
    SessionManager(const std::string& repoRoot);
    ~SessionManager();

    std::vector<ChatMessage> getHistory(const std::string& sessionId) const;
    void addMessage(const std::string& sessionId, const std::string& role, const std::string& content);

    int getCurrentTurn(const std::string& sessionId) const;
    void incrementTurn(const std::string& sessionId);

    bool hasNode(const std::string& sessionId, const std::string& nodeId) const;
    void markNode(const std::string& sessionId, const std::string& nodeId);
    void gc(const std::string& sessionId);

    void clear(const std::string& sessionId);

private:
    void initDb();
    void loadState();
    void execOrThrow(const std::string& sql);

    std::string dbPath_;
    sqlite3* db_ = nullptr;
    
    // In-memory state now keyed by session_id
    std::unordered_map<std::string, std::vector<ChatMessage>> history_;
    std::unordered_map<std::string, std::unordered_map<std::string, int>> working_set_;
    std::unordered_map<std::string, int> current_turn_;
    mutable std::mutex mutex_;
};

} // namespace chronos

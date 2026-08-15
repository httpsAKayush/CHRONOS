#include "chronos/daemon/session_manager.hpp"
#include <filesystem>
#include <stdexcept>
#include <iostream>

namespace fs = std::filesystem;

namespace chronos {

SessionManager::SessionManager(const std::string& repoRoot) {
    fs::path chronosDir = fs::path(repoRoot) / ".chronos";
    fs::create_directories(chronosDir);
    dbPath_ = (chronosDir / "session.db").string();
    
    initDb();
    loadState();
}

SessionManager::~SessionManager() {
    if (db_) sqlite3_close(db_);
}

void SessionManager::execOrThrow(const std::string& sql) {
    char* errMsg = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errMsg) != SQLITE_OK) {
        std::string msg = errMsg ? errMsg : "unknown sqlite error";
        sqlite3_free(errMsg);
        throw std::runtime_error("SessionManager SQL error: " + msg + "\nSQL: " + sql);
    }
}

void SessionManager::initDb() {
    if (sqlite3_open(dbPath_.c_str(), &db_) != SQLITE_OK) {
        throw std::runtime_error("Failed to open SessionManager at " + dbPath_);
    }
    
    execOrThrow("PRAGMA journal_mode=WAL;");
    execOrThrow("PRAGMA foreign_keys=ON;");
    
    execOrThrow(R"SQL(
        CREATE TABLE IF NOT EXISTS chat_history (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            session_id TEXT NOT NULL,
            role TEXT NOT NULL,
            content TEXT NOT NULL
        );
        CREATE TABLE IF NOT EXISTS working_set (
            session_id TEXT NOT NULL,
            node_id TEXT NOT NULL,
            last_turn INTEGER NOT NULL,
            PRIMARY KEY (session_id, node_id)
        );
        CREATE TABLE IF NOT EXISTS state (
            session_id TEXT NOT NULL,
            key TEXT NOT NULL,
            value INTEGER NOT NULL,
            PRIMARY KEY (session_id, key)
        );
    )SQL");
}

void SessionManager::loadState() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Load current turn
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, "SELECT session_id, value FROM state WHERE key='current_turn'", -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string sid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            current_turn_[sid] = sqlite3_column_int(stmt, 1);
        }
        sqlite3_finalize(stmt);
    }

    // Load history
    if (sqlite3_prepare_v2(db_, "SELECT session_id, role, content FROM chat_history ORDER BY id ASC", -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string sid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            history_[sid].push_back({
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)),
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2))
            });
        }
        sqlite3_finalize(stmt);
    }

    // Load working set
    if (sqlite3_prepare_v2(db_, "SELECT session_id, node_id, last_turn FROM working_set", -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string sid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            std::string nid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            working_set_[sid][nid] = sqlite3_column_int(stmt, 2);
        }
        sqlite3_finalize(stmt);
    }
}

std::vector<ChatMessage> SessionManager::getHistory(const std::string& sessionId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = history_.find(sessionId);
    if (it != history_.end()) return it->second;
    return {};
}

void SessionManager::addMessage(const std::string& sessionId, const std::string& role, const std::string& content) {
    std::lock_guard<std::mutex> lock(mutex_);
    history_[sessionId].push_back({role, content});
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, "INSERT INTO chat_history (session_id, role, content) VALUES (?, ?, ?)", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, sessionId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, role.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, content.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

int SessionManager::getCurrentTurn(const std::string& sessionId) const {
    auto it = current_turn_.find(sessionId);
    if (it != current_turn_.end()) return it->second;
    return 0;
}

void SessionManager::incrementTurn(const std::string& sessionId) {
    std::lock_guard<std::mutex> lock(mutex_);
    int nextTurn = ++current_turn_[sessionId];
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, "INSERT OR REPLACE INTO state (session_id, key, value) VALUES (?, 'current_turn', ?)", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, sessionId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, nextTurn);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

bool SessionManager::hasNode(const std::string& sessionId, const std::string& nodeId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = working_set_.find(sessionId);
    if (it != working_set_.end()) {
        return it->second.count(nodeId) > 0;
    }
    return false;
}

void SessionManager::markNode(const std::string& sessionId, const std::string& nodeId) {
    std::lock_guard<std::mutex> lock(mutex_);
    int turn = current_turn_[sessionId];
    working_set_[sessionId][nodeId] = turn;
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, "INSERT OR REPLACE INTO working_set (session_id, node_id, last_turn) VALUES (?, ?, ?)", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, sessionId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, nodeId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, turn);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void SessionManager::gc(const std::string& sessionId) {
    std::lock_guard<std::mutex> lock(mutex_);
    int turn = current_turn_[sessionId];
    auto it = working_set_.find(sessionId);
    if (it == working_set_.end()) return;

    std::vector<std::string> toRemove;
    for (const auto& [nodeId, lastTurn] : it->second) {
        if (turn - lastTurn >= 3) {
            toRemove.push_back(nodeId);
        }
    }
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, "DELETE FROM working_set WHERE session_id=? AND node_id=?", -1, &stmt, nullptr) == SQLITE_OK) {
        for (const auto& id : toRemove) {
            sqlite3_bind_text(stmt, 1, sessionId.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_reset(stmt);
            it->second.erase(id);
        }
        sqlite3_finalize(stmt);
    }
}

void SessionManager::clear(const std::string& sessionId) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, "DELETE FROM chat_history WHERE session_id=?", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, sessionId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    if (sqlite3_prepare_v2(db_, "DELETE FROM working_set WHERE session_id=?", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, sessionId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    if (sqlite3_prepare_v2(db_, "UPDATE state SET value=0 WHERE session_id=? AND key='current_turn'", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, sessionId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    
    history_.erase(sessionId);
    working_set_.erase(sessionId);
    current_turn_[sessionId] = 0;
}

} // namespace chronos

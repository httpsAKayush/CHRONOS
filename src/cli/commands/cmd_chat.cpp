#include "chronos/cli/commands.hpp"
#include "chronos/ipc.hpp"
#include "chronos/cli/cli_util.hpp"
#include "../linenoise/linenoise.h"
#include <iostream>
#include <string>
#include <random>
#include <filesystem>
#include <thread>
#include <chrono>
#include <sqlite3.h>
#include <atomic>
#include <mutex>
#include "chronos/daemon/session_manager.hpp"

namespace chronos {

class ANSIStreamFormatter {
    bool inBold = false;
    bool inCode = false;
    bool inBlock = false;
    int backtickCount = 0;
    int starCount = 0;
    std::string citationBuf;
    bool inCitation = false;
    
    bool isLineStart = true;
    bool inHeader = false;
    bool potentialList = false;
public:
    void print(const std::string& chunk) {
        for (char c : chunk) {
            // Newline resets line-level formatting
            if (c == '\n') {
                if (inHeader) {
                    std::cout << "\033[0m";
                    inHeader = false;
                }
                std::cout << c;
                isLineStart = true;
                potentialList = false;
                continue;
            }

            if (isLineStart && c == '#') {
                inHeader = true;
                std::cout << "\033[1;35m#"; // Magenta
                isLineStart = false;
                continue;
            }
            if (inHeader && c == '#') {
                std::cout << c;
                continue;
            }

            if (isLineStart && c == '-') {
                potentialList = true;
                isLineStart = false;
                std::cout << "\033[1;34m-"; // Blue
                continue;
            }
            if (isLineStart && c == '>') {
                potentialList = true;
                isLineStart = false;
                std::cout << "\033[1;32m>"; // Green
                continue;
            }

            if (potentialList) {
                if (c == ' ') {
                    std::cout << " \033[0m"; 
                    if (inHeader) std::cout << "\033[1;35m";
                } else {
                    std::cout << "\033[0m" << c; 
                }
                potentialList = false;
                continue;
            }

            if (c != ' ') {
                isLineStart = false;
            }

            if (inCitation) {
                if (c == ']') {
                    inCitation = false;
                }
                // Swallow all characters inside the citation
                continue;
            } else if (c == '[') {
                if (!citationBuf.empty()) std::cout << citationBuf;
                citationBuf = "[";
                continue;
            } else if (!citationBuf.empty()) {
                citationBuf += c;
                if (citationBuf == "[node:") {
                    // Swallow the citation prefix
                    citationBuf.clear();
                    inCitation = true;
                    continue;
                } else if (std::string("[node:").find(citationBuf) != 0) {
                    std::cout << citationBuf.substr(0, citationBuf.size() - 1);
                    citationBuf.clear();
                } else {
                    continue;
                }
            }

            if (c == '*') {
                starCount++;
                if (starCount == 2) {
                    inBold = !inBold;
                    if (inBold) std::cout << "\033[1;37m**";
                    else std::cout << "**\033[0m";
                    starCount = 0;
                }
                continue;
            } else if (starCount > 0) {
                std::cout << "*";
                starCount = 0;
            }

            if (c == '`') {
                backtickCount++;
                if (backtickCount == 3) {
                    inBlock = !inBlock;
                    if (inBlock) std::cout << "\n\033[38;5;38m```";
                    else std::cout << "```\033[0m";
                    backtickCount = 0;
                }
                continue;
            } else if (backtickCount > 0) {
                if (backtickCount == 1 && !inBlock) {
                    inCode = !inCode;
                    if (inCode) std::cout << "\033[38;5;158m`";
                    else std::cout << "`\033[0m";
                } else if (backtickCount == 2) {
                    std::cout << "``";
                }
                backtickCount = 0;
            }

            std::cout << c;
        }
    }
    void finish() {
        if (!citationBuf.empty()) std::cout << citationBuf;
        if (starCount > 0) std::cout << std::string(starCount, '*');
        if (backtickCount > 0) std::cout << std::string(backtickCount, '`');
        std::cout << "\033[0m" << std::flush;
        inBold = inCode = inBlock = inCitation = false;
        inHeader = potentialList = false;
        isLineStart = true;
        starCount = backtickCount = 0;
        citationBuf.clear();
    }
};

namespace {
    std::string generateUUID() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 15);
        std::uniform_int_distribution<> dis2(8, 11);

        const char* hex = "0123456789abcdef";
        std::string uuid(36, '-');

        for (int i = 0; i < 36; ++i) {
            if (i == 8 || i == 13 || i == 18 || i == 23) continue;
            if (i == 14) {
                uuid[i] = '4';
            } else if (i == 19) {
                uuid[i] = hex[dis2(gen)];
            } else {
                uuid[i] = hex[dis(gen)];
            }
        }
        return "session-" + uuid;
    }
}

CmdChat::CmdChat(const CliContext& ctx) : ctx_(ctx) {}

int CmdChat::execute(int argc, char** argv) {
    std::string sessionId = "";
    bool listMode = false;
    std::string deleteSessionId = "";
    bool deleteAllMode = false;
    bool noHistory = false;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--list" || arg == "-l") {
            listMode = true;
        } else if (arg == "--no-history") {
            noHistory = true;
        } else if (arg == "--delete-all") {
            deleteAllMode = true;
        } else if (arg == "--delete" && i + 1 < argc) {
            deleteSessionId = argv[++i];
        } else if (arg.rfind("--delete=", 0) == 0) {
            deleteSessionId = arg.substr(9);
        } else if (arg == "--session" && i + 1 < argc) {
            sessionId = argv[++i];
        } else if (arg.rfind("--session=", 0) == 0) {
            sessionId = arg.substr(10);
        }
    }

    std::string dbPath = (std::filesystem::path(ctx_.repoRoot) / ".chronos" / "session.db").string();

    if (deleteAllMode) {
        sqlite3* db;
        if (sqlite3_open(dbPath.c_str(), &db) == SQLITE_OK) {
            sqlite3_exec(db, "DELETE FROM chat_history; DELETE FROM working_set; DELETE FROM state;", nullptr, nullptr, nullptr);
            sqlite3_close(db);
            std::cout << "[Chronos] All chat sessions deleted.\n";
        } else {
            std::cerr << "[!] Could not read session database.\n";
            return 1;
        }
        return 0;
    }

    if (!deleteSessionId.empty()) {
        sqlite3* db;
        if (sqlite3_open(dbPath.c_str(), &db) == SQLITE_OK) {
            auto del = [&](const char* sql) {
                sqlite3_stmt* stmt;
                if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_text(stmt, 1, deleteSessionId.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_step(stmt);
                    sqlite3_finalize(stmt);
                }
            };
            del("DELETE FROM chat_history WHERE session_id = ?1;");
            del("DELETE FROM working_set WHERE session_id = ?1;");
            del("DELETE FROM state WHERE session_id = ?1;");
            sqlite3_close(db);
            std::cout << "[Chronos] Session '" << deleteSessionId << "' deleted.\n";
        } else {
            std::cerr << "[!] Could not read session database.\n";
            return 1;
        }
        return 0;
    }

    if (listMode) {
        sqlite3* db;
        if (sqlite3_open(dbPath.c_str(), &db) == SQLITE_OK) {
            std::string sql = "SELECT session_id, MIN(id), content FROM chat_history WHERE role='user' GROUP BY session_id ORDER BY MIN(id) DESC LIMIT 20;";
            sqlite3_stmt* stmt;
            if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
                std::cout << "\nRecent Chat Sessions:\n";
                std::cout << "──────────────────────────────────────────────────────────────────────────────\n";
                int count = 0;
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    count++;
                    const char* s_id = (const char*)sqlite3_column_text(stmt, 0);
                    const char* content = (const char*)sqlite3_column_text(stmt, 2);
                    std::string c_str = content ? content : "";
                    if (c_str.length() > 55) c_str = c_str.substr(0, 52) + "...";
                    
                    std::string id_str = s_id ? s_id : "";
                    if (id_str.length() < 43) id_str.append(43 - id_str.length(), ' ');
                    std::cout << id_str << " | " << c_str << "\n";
                }
                sqlite3_finalize(stmt);
                
                if (count == 0) {
                    std::cout << "  (No sessions found)\n";
                }
                std::cout << "──────────────────────────────────────────────────────────────────────────────\n";
                std::cout << "To resume a session, run: chronos chat --session <session_id>\n\n";
            } else {
                std::cout << "\nRecent Chat Sessions:\n";
                std::cout << "──────────────────────────────────────────────────────────────────────────────\n";
                std::cout << "  (No sessions found)\n";
                std::cout << "──────────────────────────────────────────────────────────────────────────────\n\n";
            }
            sqlite3_close(db);
        } else {
            std::cerr << "[!] Could not read session database.\n";
            return 1;
        }
        return 0;
    }

    if (sessionId.empty()) {
        sessionId = generateUUID();
    }

    std::string repoName = std::filesystem::path(ctx_.repoRoot).filename().string();

    std::cout << "\033[1;34m[Chronos]\033[0m Interactive session started (repo: \033[1;37m" << repoName << "\033[0m)\n";
    std::cout << "\033[1;34m[Chronos]\033[0m Session ID: \033[36m" << sessionId << "\033[0m\n";
    std::cout << "\033[1;34m[Chronos]\033[0m Type \033[1;35m/exit\033[0m to quit.\n\n";

    linenoiseHistorySetMaxLen(100);
    linenoiseSetMultiLine(1);

    ANSIStreamFormatter formatter;

    {
        SessionManager sm(ctx_.repoRoot);
        auto history = sm.getHistory(sessionId);
        if (!noHistory) {
            for (const auto& msg : history) {
                if (msg.role == "user") {
                    std::cout << "\033[38;5;204m>\033[38;5;210m " << msg.content << "\033[0m\n";
                } else {
                    formatter.print(msg.content);
                    formatter.finish();
                    std::cout << "\n\n";
                }
            }
        }
    }

    while (true) {
        // \033[38;5;210m (Light Salmon/Rose) leaves the terminal in a 
        // bright, light maroonish state for the user's typed input!
        char* raw_line = linenoise("\033[38;5;204m>\033[38;5;210m ");
        
        // Immediately reset the terminal color when they hit Enter
        std::cout << "\033[0m";
        if (!raw_line) {
            break;
        }

        std::string line(raw_line);
        linenoiseFree(raw_line);

        if (line.empty()) continue;
        
        linenoiseHistoryAdd(line.c_str());

        if (line == "/exit" || line == "/quit") break;

        ChronosRequest req;
        req.command = "ask_chat";
        req.userQuery = line;
        req.sessionId = sessionId;

        std::string sockPath = socketPathForRepo(ctx_.repoRoot);
        IpcClient client;
        bool daemonUp = client.connect(sockPath);
        if (!daemonUp) {
            std::cout << "[System] Waking daemon...\n";
            wakeDaemon(ctx_.repoRoot);
            for (int i = 0; i < 20 && !daemonUp; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                daemonUp = client.connect(sockPath);
            }
        }

        if (!daemonUp) {
            std::cerr << "[!] Could not connect to chronos-daemon. Run 'chronos sync' first.\n\n";
            continue;
        }

        std::atomic<bool> spinnerActive{true};
        std::string currentStatus = "Thinking...";
        std::mutex statusMutex;
        std::thread spinnerThread([&spinnerActive, &currentStatus, &statusMutex]() {
            const char* spinner = "-\\|/";
            int i = 0;
            while (spinnerActive) {
                std::string status;
                {
                    std::lock_guard<std::mutex> lock(statusMutex);
                    status = currentStatus;
                }
                std::cout << "\r\033[K\033[1;36m" << spinner[i % 4] << "\033[0m \033[3;90m" << status << "\033[0m" << std::flush;
                i++;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });

        bool firstChunk = true;
        bool streamOk = client.sendAndStream(req, [&](const ChronosResponseChunk& chunk) {
            if (chunk.isStatus) {
                std::lock_guard<std::mutex> lock(statusMutex);
                currentStatus = chunk.textDelta;
                return;
            }
            if (firstChunk) {
                spinnerActive = false;
                spinnerThread.join();
                std::cout << "\r\033[K" << std::flush;
                firstChunk = false;
            }
            formatter.print(chunk.textDelta);
        });

        if (firstChunk) {
            spinnerActive = false;
            spinnerThread.join();
            std::cout << "\r\033[K" << std::flush;
        }

        formatter.finish();

        if (!streamOk) {
            std::cerr << "\n[!] Daemon unreachable or failed during stream.\n";
        }
        std::cout << "\n\n";
    }

    return 0;
}

} // namespace chronos

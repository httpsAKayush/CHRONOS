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

namespace chronos {

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

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--list" || arg == "-l") {
            listMode = true;
        } else if (arg == "--session" && i + 1 < argc) {
            sessionId = argv[++i];
        } else if (arg.rfind("--session=", 0) == 0) {
            sessionId = arg.substr(10);
        }
    }

    std::string dbPath = (std::filesystem::path(ctx_.repoRoot) / ".chronos" / "session.db").string();

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

    std::cout << "[Chronos] Interactive session started (repo: " << repoName << ")\n";
    std::cout << "[Chronos] Session ID: " << sessionId << "\n";
    std::cout << "[Chronos] Type /exit to quit.\n\n";

    linenoiseHistorySetMaxLen(100);

    while (true) {
        char* raw_line = linenoise("> ");
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

        bool streamOk = client.sendAndStream(req, [](const ChronosResponseChunk& chunk) {
            std::cout << chunk.textDelta << std::flush;
        });

        if (!streamOk) {
            std::cerr << "\n[!] Daemon unreachable or failed during stream.\n";
        }
        std::cout << "\n\n";
    }

    return 0;
}

} // namespace chronos

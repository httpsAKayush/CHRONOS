#include "chronos/cli/commands.hpp"
#include "chronos/infrastructure/codex.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <algorithm>

namespace chronos {

CmdExport::CmdExport(const CliContext& ctx) : ctx_(ctx) {}

int CmdExport::execute(int argc, char** argv) {
    std::string format = "json";
    std::string output = "";

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--format" && i + 1 < argc) format = argv[++i];
        if (arg == "--output" && i + 1 < argc) output = argv[++i];
    }

    Codex& codex = *ctx_.storage;
    sqlite3_stmt* stmt;

    if (format == "json") {
        nlohmann::json root = nlohmann::json::object();
        root["nodes"] = nlohmann::json::array();
        if (sqlite3_prepare_v2(codex.raw(), "SELECT id, file_path, ai_summary FROM nodes", -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                nlohmann::json node;
                node["id"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                const char* fp = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                if (fp) node["file_path"] = fp;
                const char* s = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                if (s) node["ai_summary"] = s;
                root["nodes"].push_back(node);
            }
            sqlite3_finalize(stmt);
        }

        root["edges"] = nlohmann::json::array();
        if (sqlite3_prepare_v2(codex.raw(), "SELECT source_id, target_id, type FROM edges", -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                nlohmann::json edge;
                edge["source_id"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                edge["target_id"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                if (t) edge["type"] = t;
                root["edges"].push_back(edge);
            }
            sqlite3_finalize(stmt);
        }

        std::string result = root.dump(2);
        if (output.empty()) std::cout << result << "\n";
        else {
            std::ofstream out(output);
            out << result;
            std::cout << "Exported JSON to " << output << "\n";
        }
    } else if (format == "mermaid") {
        std::stringstream ss;
        ss << "graph TD\n";
        if (sqlite3_prepare_v2(codex.raw(), "SELECT source_id, target_id, type FROM edges LIMIT 1000", -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                std::string s = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                std::string t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                std::string type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                std::string sName = s, tName = t;
                sName.erase(std::remove(sName.begin(), sName.end(), '-'), sName.end());
                tName.erase(std::remove(tName.begin(), tName.end(), '-'), tName.end());
                ss << "    " << sName << "[\"" << s.substr(0,8) << "\"] -->|" << type << "| " << tName << "[\"" << t.substr(0,8) << "\"]\n";
            }
            sqlite3_finalize(stmt);
        }
        if (output.empty()) std::cout << ss.str();
        else {
            std::ofstream out(output);
            out << ss.str();
            std::cout << "Exported Mermaid to " << output << "\n";
        }
    } else {
        std::cerr << "Unknown format: " << format << "\n";
        return 1;
    }

    return 0;
}

} // namespace chronos
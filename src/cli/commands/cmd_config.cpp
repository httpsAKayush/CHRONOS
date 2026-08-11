#include "chronos/cli/commands.hpp"
#include "chronos/infrastructure/config.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>

namespace fs = std::filesystem;

namespace chronos {

CmdConfig::CmdConfig(const CliContext& ctx) : ctx_(ctx) {}

int CmdConfig::execute(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: chronos config <get|set|list> [key] [value]\n";
        return 1;
    }
    std::string action = argv[2];

    const char* homeDir = getenv("HOME");
    fs::path globalPath = fs::path(homeDir ? homeDir : "") / ".chronos" / "config.json";

    if (action == "list") {
        auto cfg = static_cast<Config&>(*ctx_.config);

        std::cout << "  LLM Provider Mode: " << cfg.providerMode() << "\n"
                  << "    auto  = try cloud first, fall back to local\n"
                  << "    local = always use local profile only\n"
                  << "    cloud = always use cloud profile only\n";

        // ── Local Profile ─────────────────────────────────────────────
        std::cout << "\n  ┌─ Local Profile (Ollama / localhost) ─────────────\n";
        if (cfg.localConfigured()) {
            std::cout << "  │ url   = " << cfg.localUrl() << "\n"
                      << "  │ key   = " << cfg.localKey() << "\n"
                      << "  │ model = " << cfg.localModel() << "\n";
        } else {
            std::cout << "  │ (not configured — set llm.local.url to enable)\n";
        }
        std::cout << "  └──────────────────────────────────────────────────\n";

        // ── Cloud Profile ─────────────────────────────────────────────
        std::cout << "\n  ┌─ Cloud Profile (NVIDIA / OpenRouter / OpenAI) ────\n";
        if (cfg.cloudConfigured()) {
            std::cout << "  │ url   = " << cfg.cloudUrl() << "\n";
            std::string key = cfg.cloudKey();
            if (key.length() > 8)
                key = key.substr(0, 4) + "..." + key.substr(key.length() - 2);
            std::cout << "  │ key   = " << key << "\n";
            std::cout << "  │ model = " << cfg.cloudModel() << "\n";
        } else {
            std::cout << "  │ (not configured — set llm.cloud.url to enable)\n";
        }
        std::cout << "  └──────────────────────────────────────────────────\n";

        // ── Raw keys (only if --raw flag) ─────────────────────────────
        bool showRaw = false;
        for (int i = 3; i < argc; ++i) {
            if (std::string(argv[i]) == "--raw") showRaw = true;
        }
        if (showRaw) {
            std::cout << "\n  All config keys:\n";
            std::set<std::string> shown;
            auto all = ctx_.config->getAll();
            for (const auto& [key, val] : all) {
                if (shown.count(key)) continue;
                shown.insert(key);
                std::string shown_val = val;
                if ((key.find("key") != std::string::npos ||
                     key.find("KEY") != std::string::npos) && shown_val.length() > 8) {
                    shown_val = shown_val.substr(0, 4) + "..." +
                                shown_val.substr(shown_val.length() - 2);
                }
                std::cout << "  " << key << " = " << shown_val << "\n";
            }
        } else {
            std::cout << "\n  (use `chronos config list --raw` to show all internal keys)\n";
        }
        return 0;
    }

    if (action == "get") {
        if (argc < 4) { std::cerr << "Usage: chronos config get <key>\n"; return 1; }
        std::string key = argv[3];
        if (ctx_.config->has(key)) {
            std::cout << ctx_.config->get(key) << "\n";
        } else {
            std::cerr << key << " is not set.\n";
        }
        return 0;
    }

    if (action == "set") {
        if (argc < 5) { std::cerr << "Usage: chronos config set <key> <value>\n"; return 1; }
        std::string key = argv[3];
        std::string val = argv[4];

        nlohmann::json globalConf;
        if (fs::exists(globalPath)) {
            std::ifstream in(globalPath);
            try { in >> globalConf; } catch (...) {}
        }

        globalConf[key] = val;

        if (!fs::exists(globalPath.parent_path())) fs::create_directories(globalPath.parent_path());
        std::ofstream out(globalPath);
        out << globalConf.dump(4);

        // Restart the daemon so it picks up the new config on next request.
        if (system("pkill -f chronos-daemon >/dev/null 2>&1") == 0) {
            std::cout << key << " = " << val << "\n";
            std::cout << "(daemon restarted — new config active on next request)\n";
        } else {
            std::cout << key << " = " << val << "\n";
        }
        return 0;
    }

    return 1;
}

} // namespace chronos
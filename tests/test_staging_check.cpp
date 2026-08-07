#include "test_framework.hpp"
#include "chronos/codex.hpp"
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace chronos;

// Helper declared in main_cli or test helper for testing logic
static void test_get_history_for_file() {
    std::string tempDirPath = (fs::temp_directory_path() / "chronos_test_history_for_file").string();
    fs::remove_all(tempDirPath);
    fs::create_directories(tempDirPath);

    {
        Codex codex(tempDirPath);

        // Add active node for src/test.cpp
        Node n1;
        n1.id = "node-active-1";
        n1.file_path = "src/test.cpp";
        n1.byte_start = 0;
        n1.byte_end = 100;
        n1.is_active = true;
        codex.upsertNode(n1);

        // Add inactive node for src/test.cpp
        Node n2;
        n2.id = "node-inactive-1";
        n2.file_path = "src/test.cpp";
        n2.byte_start = 101;
        n2.byte_end = 200;
        n2.is_active = false;
        codex.upsertNode(n2);

        // Add active node for src/other.cpp
        Node n3;
        n3.id = "node-other-1";
        n3.file_path = "src/other.cpp";
        n3.byte_start = 0;
        n3.byte_end = 100;
        n3.is_active = true;
        codex.upsertNode(n3);

        // Record history
        codex.recordHistory("node-active-1", "commit111", 1000, "DO NOT MUTEX HERE - deadlock risk");
        codex.recordHistory("node-active-1", "commit222", 2000, "Refactored async logger");
        codex.recordHistory("node-inactive-1", "commit333", 1500, "Historical note on tombstoned node");
        codex.recordHistory("node-other-1", "commit444", 2500, "DO NOT ALTER TIMEOUT");

        // Query history for src/test.cpp
        auto historyTest = codex.getHistoryForFile("src/test.cpp");
        CHRONOS_CHECK(historyTest.size() == 2);
        if (historyTest.size() == 2) {
            // Ordered by timestamp DESC
            CHRONOS_CHECK(historyTest[0].commitHash == "commit222");
            CHRONOS_CHECK(historyTest[0].syntheticMsg == "Refactored async logger");
            CHRONOS_CHECK(historyTest[1].commitHash == "commit111");
            CHRONOS_CHECK(historyTest[1].syntheticMsg == "DO NOT MUTEX HERE - deadlock risk");
        }

        // Query history for src/other.cpp
        auto historyOther = codex.getHistoryForFile("src/other.cpp");
        CHRONOS_CHECK(historyOther.size() == 1);
        if (!historyOther.empty()) {
            CHRONOS_CHECK(historyOther[0].syntheticMsg == "DO NOT ALTER TIMEOUT");
        }

        // Query history for nonexistent file
        auto historyNonexistent = codex.getHistoryForFile("src/nonexistent.cpp");
        CHRONOS_CHECK(historyNonexistent.empty());
    }

    fs::remove_all(tempDirPath);
}

static void test_staging_collision_logic() {
    std::string tempDirPath = (fs::temp_directory_path() / "chronos_test_staging_collision").string();
    fs::remove_all(tempDirPath);
    fs::create_directories(tempDirPath);

    {
        Codex codex(tempDirPath);

        Node n;
        n.id = "worker-node-1";
        n.file_path = "src/worker.cpp";
        n.byte_start = 10;
        n.byte_end = 300;
        n.is_active = true;
        codex.upsertNode(n);

        codex.recordHistory("worker-node-1", "c0mm1t1", 1700000000, "DO NOT MUTEX HERE - causes deadlock with worker thread");

        auto history = codex.getHistoryForFile("src/worker.cpp");
        CHRONOS_CHECK(!history.empty());
        CHRONOS_CHECK(history[0].syntheticMsg.find("MUTEX") != std::string::npos);
    }

    fs::remove_all(tempDirPath);
}

static void test_codex_append_history_alignment() {
    std::string tempDirPath = (fs::temp_directory_path() / "chronos_test_append_history").string();
    fs::remove_all(tempDirPath);
    fs::create_directories(tempDirPath);

    {
        Codex codex(tempDirPath);

        Node n;
        n.id = "append-node-1";
        n.file_path = "src/append.cpp";
        n.byte_start = 0;
        n.byte_end = 50;
        n.is_active = true;
        codex.upsertNode(n);

        HistoryEntry entry;
        entry.node_id = "append-node-1";
        entry.commit_hash = "hash123";
        entry.intent_summary = "CRITICAL: DO NOT MUTEX HERE";

        codex.appendHistory(entry);

        auto history = codex.getHistory("append-node-1");
        CHRONOS_CHECK(history.size() == 1);
        if (!history.empty()) {
            CHRONOS_CHECK(history[0].commitHash == "hash123");
            CHRONOS_CHECK(history[0].syntheticMsg == "CRITICAL: DO NOT MUTEX HERE");
        }
    }

    fs::remove_all(tempDirPath);
}

void run_staging_check_tests() {
    test_get_history_for_file();
    test_staging_collision_logic();
    test_codex_append_history_alignment();
}

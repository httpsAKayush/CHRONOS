#include "mocks/mock_storage.hpp"
#include "test_framework.hpp"
#include <cassert>
#include <iostream>

using namespace chronos;

void test_nodes() {
    MockStorage storage;

    Node n1;
    n1.id = "node-1";
    n1.file_path = "src/main.cpp";
    n1.byte_start = 0;
    n1.byte_end = 100;
    n1.simhash = 12345;
    n1.is_active = true;
    n1.parse_confidence = 0.95f;
    n1.ai_summary = "Main entry point";

    storage.upsertNode(n1);

    auto retNode = storage.getNode("node-1");
    CHRONOS_CHECK(retNode.has_value());
    CHRONOS_CHECK(retNode->id == "node-1");
    CHRONOS_CHECK(retNode->ai_summary == "Main entry point");

    // Update AI summary
    storage.updateAiSummary("node-1", "Updated summary");
    retNode = storage.getNode("node-1");
    CHRONOS_CHECK(retNode.has_value());
    CHRONOS_CHECK(retNode->ai_summary == "Updated summary");

    // Simhash search
    auto simFound = storage.findBySimhash(12345, "src/main.cpp");
    CHRONOS_CHECK(simFound.has_value());
    CHRONOS_CHECK(simFound->id == "node-1");

    auto simGlobal = storage.findBySimhashGlobal(12345);
    CHRONOS_CHECK(simGlobal.has_value());
    CHRONOS_CHECK(simGlobal->id == "node-1");

    // Path prefix query
    auto prefixNodes = storage.queryNodesByPathPrefix("src/");
    CHRONOS_CHECK(prefixNodes.size() == 1);
    CHRONOS_CHECK(prefixNodes[0].id == "node-1");

    // Tombstone node
    storage.tombstoneNode("node-1");
    retNode = storage.getNode("node-1");
    CHRONOS_CHECK(retNode.has_value());
    CHRONOS_CHECK(!retNode->is_active);

    CHRONOS_CHECK(!storage.findBySimhash(12345, "src/main.cpp").has_value());
    CHRONOS_CHECK(!storage.findBySimhashGlobal(12345).has_value());
    CHRONOS_CHECK(storage.queryNodesByPathPrefix("src/").empty());

    std::cout << "[PASS] test_nodes\n";
}

void test_edges() {
    MockStorage storage;

    Node n1{"node-1", "src/a.cpp", 0, 50, 100, true, 1.0f, ""};
    Node n2{"node-2", "src/b.cpp", 0, 50, 200, true, 1.0f, ""};
    Node n3{"node-3", "src/c.cpp", 0, 50, 300, false, 1.0f, ""}; // inactive

    storage.upsertNode(n1);
    storage.upsertNode(n2);
    storage.upsertNode(n3);

    Edge e1{"node-1", "node-2", "calls", 1.0f, 10, "call node-2"};
    Edge e2{"node-1", "node-3", "calls", 1.0f, 15, "call node-3"};

    storage.upsertEdge(e1);
    storage.upsertEdge(e2);

    auto outgoingAll = storage.getEdges("node-1", true);
    CHRONOS_CHECK(outgoingAll.size() == 2);

    auto incomingN2 = storage.getEdges("node-2", false);
    CHRONOS_CHECK(incomingN2.size() == 1);
    CHRONOS_CHECK(incomingN2[0].source_id == "node-1");

    // Filtered edges (n3 is inactive, so e2 should be filtered out)
    auto outgoingFiltered = storage.getEdgesFiltered("node-1", true);
    CHRONOS_CHECK(outgoingFiltered.size() == 1);
    CHRONOS_CHECK(outgoingFiltered[0].target_id == "node-2");

    std::cout << "[PASS] test_edges\n";
}

void test_history() {
    MockStorage storage;

    Node n1{"node-1", "src/a.cpp", 0, 50, 100, true, 1.0f, ""};
    storage.upsertNode(n1);

    HistoryEntry he{"node-1", "commit-abc", "Initial creation"};
    storage.appendHistory(he);
    CHRONOS_CHECK(storage.historyEntries().size() == 1);

    storage.recordHistory("node-1", "commit-abc", 1700000000, "Created node 1");
    storage.recordHistory("node-1", "commit-def", 1700000100, "Updated node 1");

    auto hList = storage.getHistory("node-1");
    CHRONOS_CHECK(hList.size() == 2);
    CHRONOS_CHECK(hList[0].commitHash == "commit-abc");
    CHRONOS_CHECK(hList[1].commitHash == "commit-def");

    auto fileHist = storage.getHistoryForFile("src/a.cpp");
    CHRONOS_CHECK(fileHist.size() == 2);

    std::cout << "[PASS] test_history\n";
}

void test_alias_and_imports() {
    MockStorage storage;

    // Aliases
    storage.recordAlias("sym:foo", "uuid-123", "commit-1");
    storage.recordAlias("uuid-123", "uuid-456", "commit-2");

    CHRONOS_CHECK(storage.resolveAlias("sym:foo") == "uuid-456");
    CHRONOS_CHECK(storage.resolveAlias("uuid-123") == "uuid-456");
    CHRONOS_CHECK(storage.resolveAlias("uuid-456") == "uuid-456");

    // File imports
    storage.insertFileImport("src/main.cpp", "printf", "stdio");
    storage.insertFileImport("src/main.cpp", "cout", "iostream");
    storage.insertFileImport("src/utils.cpp", "helper", "utils");

    CHRONOS_CHECK(storage.fileImports().size() == 3);

    storage.clearFileImports("src/main.cpp");
    CHRONOS_CHECK(storage.fileImports().size() == 1);
    CHRONOS_CHECK(storage.fileImports()[0].filePath == "src/utils.cpp");

    std::cout << "[PASS] test_alias_and_imports\n";
}

void test_transactions_traces_ppr() {
    MockStorage storage;

    // Transactions
    storage.beginTransaction();
    CHRONOS_CHECK(storage.inTransaction());
    storage.commitTransaction();
    CHRONOS_CHECK(!storage.inTransaction());

    // Traces
    TraceResult tr;
    tr.any_low_confidence = false;
    tr.nodes.push_back(Node{"node-1", "src/a.cpp", 0, 50, 100, true, 1.0f, ""});
    storage.recordTrace("trace-100", tr);

    auto fetchedTrace = storage.getTrace("trace-100");
    CHRONOS_CHECK(fetchedTrace.has_value());
    CHRONOS_CHECK(fetchedTrace->nodes.size() == 1);

    // PPR
    Node n1{"node-1", "src/a.cpp", 0, 50, 100, true, 1.0f, ""};
    Node n2{"node-2", "src/b.cpp", 0, 50, 200, true, 0.4f, ""}; // low confidence
    storage.upsertNode(n1);
    storage.upsertNode(n2);
    storage.upsertEdge(Edge{"node-1", "node-2", "calls", 1.0f, 5, "call"});

    auto pprRes = storage.localPushPPR("node-1", 10, 0.85);
    CHRONOS_CHECK(!pprRes.nodes.empty());
    CHRONOS_CHECK(pprRes.any_low_confidence); // n2 confidence is 0.4f

    std::cout << "[PASS] test_transactions_traces_ppr\n";
}

int main() {
    std::cout << "Running MockStorage Unit Tests...\n";
    test_nodes();
    test_edges();
    test_history();
    test_alias_and_imports();
    test_transactions_traces_ppr();

    if (g_failures > 0) {
        std::cerr << "MockStorage Unit Tests FAILED with " << g_failures << " failures.\n";
        return 1;
    }

    std::cout << "All MockStorage Unit Tests PASSED successfully.\n";
    return 0;
}

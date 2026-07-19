#include "test_framework.hpp"
#include "chronos/codex.hpp"
#include <filesystem>

using namespace chronos;
namespace fs = std::filesystem;

void run_codex_alias_tests() {
    fs::path tmp = fs::temp_directory_path() / "chronos_test_codex_alias";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    {
        Codex codex(tmp.string());

        Node n1; n1.id = "node-A"; n1.file_path = "a.cpp"; n1.byte_start = 0; n1.byte_end = 10; n1.simhash = 111;
        Node n2; n2.id = "node-B"; n2.file_path = "a.cpp"; n2.byte_start = 20; n2.byte_end = 30; n2.simhash = 222;
        Node n3; n3.id = "node-C"; n3.file_path = "a.cpp"; n3.byte_start = 40; n3.byte_end = 50; n3.simhash = 333;
        codex.upsertNode(n1);
        codex.upsertNode(n2);
        codex.upsertNode(n3);

        // Simulate a rename chain: A -> B -> C.
        codex.recordAlias("node-A", "node-B", "commit1");
        codex.recordAlias("node-B", "node-C", "commit2");

        // Idempotency: no circular deps, path compression collapses to node-C.
        CHRONOS_CHECK(codex.resolveAlias("node-A") == "node-C");
        CHRONOS_CHECK(codex.resolveAlias("node-B") == "node-C");
        CHRONOS_CHECK(codex.resolveAlias("node-C") == "node-C"); // untouched id resolves to itself

        // Edge validation rule: byte_end > byte_start enforced by CHECK constraint.
        Node bad; bad.id = "node-bad"; bad.file_path = "a.cpp"; bad.byte_start = 10; bad.byte_end = 5; bad.simhash = 1;
        bool threw = false;
        try { codex.upsertNode(bad); } catch (...) { threw = true; }
        CHRONOS_CHECK(threw);
    }

    fs::remove_all(tmp);
}

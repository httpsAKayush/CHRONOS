#include "test_framework.hpp"
#include "chronos/codex.hpp"
#include "chronos/oracle.hpp"
#include <filesystem>

using namespace chronos;
namespace fs = std::filesystem;

void run_oracle_harness_tests() {
    fs::path tmp = fs::temp_directory_path() / "chronos_test_oracle";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    {
        Codex codex(tmp.string());
        Oracle oracle(codex, tmp.string());

        Node n; n.id = "real-node-1"; n.file_path = "x.cpp"; n.byte_start = 0; n.byte_end = 5; n.simhash = 1;
        codex.upsertNode(n);

        // Spec §13: "Every LLM response will be regex-parsed for citations.
        // If a citation doesn't exist in the Codex, the test fails." -- this
        // is that check, exercised directly (this IS the CI harness logic).
        std::string validAnswer = "DB::query is called here [node:real-node-1].";
        auto check1 = oracle.verifyCitations(validAnswer);
        CHRONOS_CHECK(check1.allValid);

        std::string hallucinated = "It also calls Logger::flush [node:does-not-exist].";
        auto check2 = oracle.verifyCitations(hallucinated);
        CHRONOS_CHECK(!check2.allValid);
        CHRONOS_CHECK(check2.invalidNodeIds.size() == 1);
        CHRONOS_CHECK(check2.invalidNodeIds[0] == "does-not-exist");

        // Oracle-Only render must never crash on an empty trace (Reliability, §9).
        TraceResult empty;
        std::string rendered = oracle.renderTrace(empty);
        CHRONOS_CHECK(rendered.empty());
    }

    fs::remove_all(tmp);
}

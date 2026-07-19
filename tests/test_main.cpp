#include "test_framework.hpp"

void run_simhash_tests();
void run_codex_alias_tests();
void run_mmr_tests();
void run_oracle_harness_tests();

int main() {
    run_simhash_tests();
    run_codex_alias_tests();
    run_mmr_tests();
    run_oracle_harness_tests();

    if (g_failures == 0) {
        std::cout << "All Chronos tests passed.\n";
        return 0;
    }
    std::cerr << g_failures << " test(s) failed.\n";
    return 1;
}

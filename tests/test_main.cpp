#include "test_framework.hpp"

void run_simhash_tests();
void run_codex_alias_tests();
void run_mmr_tests();
void run_oracle_harness_tests();
void run_recency_tests();
void run_ast_mutation_scorer_tests();
void run_staging_check_tests();
void run_vector_quantization_tiering_tests();

int main() {
    run_simhash_tests();
    run_codex_alias_tests();
    run_mmr_tests();
    run_oracle_harness_tests();
    run_recency_tests();
    run_ast_mutation_scorer_tests();
    run_staging_check_tests();
    run_vector_quantization_tiering_tests();

    if (g_failures == 0) {
        std::cout << "All Chronos tests passed.\n";
        return 0;
    }
    std::cerr << g_failures << " test(s) failed.\n";
    return 1;
}

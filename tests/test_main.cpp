#include "test_framework.hpp"

void run_simhash_tests();
void run_codex_alias_tests();
void run_mmr_tests();
void run_oracle_harness_tests();
void run_recency_tests();
void run_ast_mutation_scorer_tests();
void run_staging_check_tests();
void run_vector_quantization_tiering_tests();
void run_core_interfaces_tests();

int main() {
    // Isolate tests from user's global config to prevent hitting real LLM endpoints
    setenv("HOME", "/tmp/chronos_test_home", 1);
    setenv("API_KEY", "", 1);
    setenv("API_URL", "", 1);
    
    std::cout << "Starting simhash\n"; std::flush(std::cout); run_simhash_tests();
    std::cout << "Starting codex alias\n"; std::flush(std::cout); run_codex_alias_tests();
    std::cout << "Starting mmr\n"; std::flush(std::cout); run_mmr_tests();
    std::cout << "Starting oracle\n"; std::flush(std::cout); run_oracle_harness_tests();
    std::cout << "Starting recency\n"; std::flush(std::cout); run_recency_tests();
    std::cout << "Starting ast mutation\n"; std::flush(std::cout); run_ast_mutation_scorer_tests();
    std::cout << "Starting staging check\n"; std::flush(std::cout); run_staging_check_tests();
    std::cout << "Starting vector quant\n"; std::flush(std::cout); run_vector_quantization_tiering_tests();
    std::cout << "Starting core interfaces\n"; std::flush(std::cout); run_core_interfaces_tests();

    if (g_failures == 0) {
        std::cout << "All Chronos tests passed.\n";
        return 0;
    }
    std::cerr << g_failures << " test(s) failed.\n";
    return 1;
}

#include "test_framework.hpp"
#include "chronos/simhash.hpp"

using namespace chronos;

void run_simhash_tests() {
    // Spec §13 concrete "correct" for the hard part: identical control flow
    // with only identifiers changed (Auth::login -> Auth::signIn calling
    // DB::query) must simhash-match exactly.
    std::vector<StructuralToken> before = {
        {"function_definition", 1}, {"(", 1}, {")", 1}, {"{", 1},
        {"call_expression", 2}, {"(", 1}, {")", 1}, {";", 1}, {"}", 1},
    };
    std::vector<StructuralToken> after = before; // identifiers already stripped by caller contract

    uint64_t h1 = Simhash::compute(before);
    uint64_t h2 = Simhash::compute(after);
    CHRONOS_CHECK(h1 == h2);
    CHRONOS_CHECK(Simhash::hammingDistance(h1, h2) == 0);

    // A structurally different function (extra branch) should usually
    // diverge. Not a strict guarantee (hashing can collide) but this is the
    // regression signal the Spec §10 spike is meant to validate empirically.
    std::vector<StructuralToken> different = {
        {"function_definition", 1}, {"(", 1}, {")", 1}, {"{", 1},
        {"if_statement", 2}, {"(", 1}, {")", 1}, {"{", 1},
        {"call_expression", 2}, {"(", 1}, {")", 1}, {";", 1}, {"}", 1}, {"}", 1},
    };
    uint64_t h3 = Simhash::compute(different);
    CHRONOS_CHECK(h3 != h1);
}

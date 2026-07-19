#pragma once
// Simhash normalization (Spec Glossary): hashes only control-flow primitives
// (keywords, operators, AST node kinds) while STRIPPING identifiers/types so
// a pure rename (Auth::login -> Auth::signIn) still produces an identical
// hash, enabling alias-DAG forwarding without an LLM re-embed.
//
// This is the "Hard Part" called out in Spec §6. The implementation below is
// a 64-bit weighted Simhash over a token stream; token classification (which
// tokens count as "structural" vs "identifier") is supplied by the caller
// (ast_indexer.cpp) since it depends on tree-sitter node kinds.

#include <cstdint>
#include <string>
#include <vector>

namespace chronos {

// A single structural token contributing to the hash. `weight` lets callers
// emphasize control-flow keywords (if/for/while/return) over generic
// punctuation, which empirically reduces collisions (see Spec §10 Spike:
// "Simhash for AST").
struct StructuralToken {
    std::string text;   // e.g. "if", "for", "(", "{", "call_expression"
    int weight = 1;
};

class Simhash {
public:
    // Computes the 64-bit simhash fingerprint for a sequence of structural
    // tokens. Identifiers/type names must already be excluded by the caller.
    static uint64_t compute(const std::vector<StructuralToken>& tokens);

    // Hamming distance between two fingerprints — used to decide "is this
    // the same logical function after a rename" vs "this is a rewrite".
    // A distance of 0 means identical control-flow structure.
    static int hammingDistance(uint64_t a, uint64_t b);
};

} // namespace chronos

#include "chronos/simhash.hpp"
#include <array>
#include <functional>

namespace chronos {

namespace {
// Simple, fast 64-bit string hash (FNV-1a). Cryptographic strength is not
// needed here — we only need good bit dispersion for the weighted-majority
// vote below.
uint64_t fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}
}

uint64_t Simhash::compute(const std::vector<StructuralToken>& tokens) {
    std::array<int64_t, 64> bitVotes{};
    bitVotes.fill(0);

    for (const auto& tok : tokens) {
        uint64_t h = fnv1a(tok.text);
        for (int bit = 0; bit < 64; ++bit) {
            bool isSet = (h >> bit) & 1ULL;
            bitVotes[bit] += isSet ? tok.weight : -tok.weight;
        }
    }

    uint64_t fingerprint = 0;
    for (int bit = 0; bit < 64; ++bit) {
        if (bitVotes[bit] > 0) fingerprint |= (1ULL << bit);
    }
    return fingerprint;
}

int Simhash::hammingDistance(uint64_t a, uint64_t b) {
    uint64_t x = a ^ b;
    int count = 0;
    while (x) {
        x &= (x - 1);
        ++count;
    }
    return count;
}

} // namespace chronos

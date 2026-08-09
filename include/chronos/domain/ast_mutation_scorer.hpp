#pragma once
#include <string>
#include <vector>

namespace chronos {

class AstMutationScorer {
public:
    // Scores a diff between two file contents.
    // Score 10: Contract-breaking structural changes (return type, visibility, parameters, class deletion).
    // Score 1: Internal logic changes (while loops, inside functions).
    // Score 0: No structural change (formatting, comments).
    static int scoreDiff(const std::string& oldContent, const std::string& newContent, const std::string& extension);
};

} // namespace chronos

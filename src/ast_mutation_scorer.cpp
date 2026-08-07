#include "chronos/ast_mutation_scorer.hpp"

namespace chronos {

int AstMutationScorer::scoreDiff(const std::string& oldContent, const std::string& newContent, const std::string& extension) {
    if (extension != ".py" && extension != ".cpp" && extension != ".hpp" && extension != ".c" && extension != ".h") {
        return 0; // Not a supported language
    }
    
    // Simplistic heuristic for Phase 2:
    // If the files are exactly identical, 0.
    if (oldContent == newContent) return 0;
    
    // If one file is empty and the other is not, it's a major structural change (added/deleted file)
    if (oldContent.empty() || newContent.empty()) return 10;
    
    // In a full implementation, we'd use tree-sitter to parse both and compare AST nodes.
    // For now, we use string heuristics to detect API-breaking changes:
    // Check if the number of functions/classes changed.
    auto countSubstrings = [](const std::string& str, const std::string& sub) {
        int count = 0;
        size_t pos = 0;
        while ((pos = str.find(sub, pos)) != std::string::npos) { count++; pos += sub.length(); }
        return count;
    };
    
    int oldFuncs = countSubstrings(oldContent, "def ") + countSubstrings(oldContent, "class ");
    int newFuncs = countSubstrings(newContent, "def ") + countSubstrings(newContent, "class ");
    
    if (oldFuncs != newFuncs) return 10; // Structural keyframe: function/class added or removed
    
    // Check for visibility changes (C++)
    int oldPub = countSubstrings(oldContent, "public:");
    int newPub = countSubstrings(newContent, "public:");
    if (oldPub != newPub) return 10; // Structural keyframe: visibility changed
    
    // Otherwise it's an internal logic change
    return 1;
}

} // namespace chronos

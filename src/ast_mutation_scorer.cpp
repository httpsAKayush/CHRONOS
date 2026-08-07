#include "chronos/ast_mutation_scorer.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace chronos {

namespace {

bool isLockfile(const std::string& extOrPath) {
    std::string p = extOrPath;
    for (char &c : p) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string filename = fs::path(p).filename().string();
    if (filename == "package-lock.json" || filename == "cargo.lock" ||
        filename == "yarn.lock" || filename == "pnpm-lock.yaml" ||
        filename == "composer.lock" || filename == "gemfile.lock" ||
        filename == "poetry.lock" || filename == "go.sum") {
        return true;
    }
    std::string ext = fs::path(p).extension().string();
    if (ext == ".lock") return true;
    return false;
}

bool isSupportedCodeExtension(const std::string& extOrPath) {
    std::string ext = fs::path(extOrPath).extension().string();
    if (ext.empty()) {
        ext = extOrPath;
    }
    for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext.empty() || ext[0] != '.') {
        ext = "." + ext;
    }
    static const std::unordered_set<std::string> exts = {
        ".cpp", ".hpp", ".c", ".h", ".cc", ".cxx", ".hh", ".hxx",
        ".py", ".pyi", ".js", ".ts", ".jsx", ".tsx", ".java", ".go",
        ".rs", ".cs", ".mjs", ".cjs", ".sh"
    };
    return exts.count(ext) > 0;
}

std::string stripComments(const std::string& src, const std::string& extOrPath) {
    std::string out;
    out.reserve(src.size());
    size_t n = src.size();

    std::string ext = fs::path(extOrPath).extension().string();
    if (ext.empty()) ext = extOrPath;
    for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    bool isPythonOrShell = (ext == ".py" || ext == ".pyi" || ext == ".sh");

    size_t i = 0;
    while (i < n) {
        // Python triple quotes handling (""" or ''')
        if (isPythonOrShell && i + 2 < n &&
            ((src[i] == '"' && src[i+1] == '"' && src[i+2] == '"') ||
             (src[i] == '\'' && src[i+1] == '\'' && src[i+2] == '\''))) {
            std::string q = src.substr(i, 3);
            out += q;
            i += 3;
            while (i < n) {
                if (i + 2 < n && src.substr(i, 3) == q) {
                    out += q;
                    i += 3;
                    break;
                } else if (src[i] == '\\' && i + 1 < n) {
                    out += src[i++];
                    out += src[i++];
                } else {
                    out += src[i++];
                }
            }
            continue;
        }

        // Single or double quote string literal
        if (src[i] == '"' || src[i] == '\'') {
            char quote = src[i];
            out += src[i++];
            while (i < n) {
                if (src[i] == '\\' && i + 1 < n) {
                    out += src[i++];
                    out += src[i++];
                } else if (src[i] == quote) {
                    out += src[i++];
                    break;
                } else {
                    out += src[i++];
                }
            }
            continue;
        }

        // C-style line comment //
        if (!isPythonOrShell && i + 1 < n && src[i] == '/' && src[i+1] == '/') {
            i += 2;
            while (i < n && src[i] != '\n') i++;
            out += ' ';
            continue;
        }

        // C-style block comment /* */
        if (!isPythonOrShell && i + 1 < n && src[i] == '/' && src[i+1] == '*') {
            i += 2;
            while (i + 1 < n && !(src[i] == '*' && src[i+1] == '/')) i++;
            if (i + 1 < n) i += 2;
            else i = n;
            out += ' ';
            continue;
        }

        // Python/shell line comment #
        if (isPythonOrShell && src[i] == '#') {
            while (i < n && src[i] != '\n') i++;
            out += ' ';
            continue;
        }

        out += src[i++];
    }
    return out;
}

std::vector<std::string> tokenize(const std::string& code) {
    std::vector<std::string> tokens;
    size_t i = 0, n = code.size();
    while (i < n) {
        if (std::isspace(static_cast<unsigned char>(code[i]))) {
            i++;
            continue;
        }

        // Triple-quoted strings in Python if remaining in stripped code
        if (i + 2 < n &&
            ((code[i] == '"' && code[i+1] == '"' && code[i+2] == '"') ||
             (code[i] == '\'' && code[i+1] == '\'' && code[i+2] == '\''))) {
            std::string q = code.substr(i, 3);
            std::string tok = q;
            i += 3;
            while (i < n) {
                if (i + 2 < n && code.substr(i, 3) == q) {
                    tok += q;
                    i += 3;
                    break;
                } else if (code[i] == '\\' && i + 1 < n) {
                    tok += code[i++];
                    tok += code[i++];
                } else {
                    tok += code[i++];
                }
            }
            tokens.push_back(tok);
            continue;
        }

        // Quoted string literal token
        if (code[i] == '"' || code[i] == '\'') {
            char quote = code[i];
            std::string tok;
            tok += code[i++];
            while (i < n) {
                if (code[i] == '\\' && i + 1 < n) {
                    tok += code[i++];
                    tok += code[i++];
                } else if (code[i] == quote) {
                    tok += code[i++];
                    break;
                } else {
                    tok += code[i++];
                }
            }
            tokens.push_back(tok);
            continue;
        }

        // Identifiers and numbers
        if (std::isalnum(static_cast<unsigned char>(code[i])) || code[i] == '_') {
            size_t start = i;
            while (i < n && (std::isalnum(static_cast<unsigned char>(code[i])) || code[i] == '_')) {
                i++;
            }
            tokens.push_back(code.substr(start, i - start));
            continue;
        }

        // Multi-char operators
        if (i + 1 < n) {
            std::string op2 = code.substr(i, 2);
            if (op2 == "==" || op2 == "!=" || op2 == "<=" || op2 == ">=" ||
                op2 == "&&" || op2 == "||" || op2 == "++" || op2 == "--" ||
                op2 == "->" || op2 == "::" || op2 == "+=" || op2 == "-=" ||
                op2 == "*=" || op2 == "/=" || op2 == "%=" || op2 == "<<" || op2 == ">>") {
                tokens.push_back(op2);
                i += 2;
                continue;
            }
        }

        // Single character token
        tokens.push_back(std::string(1, code[i++]));
    }
    return tokens;
}

bool isStringLiteralToken(const std::string& tok) {
    if (tok.empty()) return false;
    if (tok.front() == '"' || tok.front() == '\'') return true;
    if (tok.size() >= 2 && tok.back() == '"' &&
        (tok.substr(0, 2) == "R\"" || tok.substr(0, 2) == "L\"" ||
         tok.substr(0, 3) == "u8\"" || tok.substr(0, 2) == "u\"" || tok.substr(0, 2) == "U\"")) {
        return true;
    }
    return false;
}

bool isRawNumberToken(const std::string& tok) {
    if (tok.empty()) return false;
    if (std::isdigit(static_cast<unsigned char>(tok.front()))) return true;
    if (tok.front() == '.' && tok.size() > 1 && std::isdigit(static_cast<unsigned char>(tok[1]))) return true;
    return false;
}

} // namespace

int AstMutationScorer::scoreDiff(const std::string& oldContent, const std::string& newContent, const std::string& extension) {
    if (isLockfile(extension)) {
        return 0;
    }
    if (!isSupportedCodeExtension(extension)) {
        return 0;
    }

    if (oldContent == newContent) {
        return 0;
    }

    std::string oldStripped = stripComments(oldContent, extension);
    std::string newStripped = stripComments(newContent, extension);

    std::vector<std::string> oldTokens = tokenize(oldStripped);
    std::vector<std::string> newTokens = tokenize(newStripped);

    if (oldTokens == newTokens) {
        return 0; // Only whitespace, indentation, comments, or formatting changes
    }

    // If one file is empty (or has no code tokens) and the other has code tokens: major structural change
    if (oldTokens.empty() != newTokens.empty()) {
        return 10;
    }

    // Contract-breaking structural change detection:
    // Check key structural declaration keywords and visibility modifiers
    static const std::unordered_set<std::string> structKeywords = {
        "def", "class", "struct", "enum", "interface", "union",
        "public", "protected", "private", "fn", "func", "function",
        "namespace", "package", "virtual", "override", "extends", "implements"
    };

    auto countKeyword = [](const std::vector<std::string>& tokens, const std::string& kw) {
        int c = 0;
        for (const auto& t : tokens) {
            if (t == kw) c++;
        }
        return c;
    };

    for (const auto& kw : structKeywords) {
        if (countKeyword(oldTokens, kw) != countKeyword(newTokens, kw)) {
            return 10; // Function/class added or removed, or visibility changed
        }
    }

    // Function signature or outer declaration change check:
    std::string ext = fs::path(extension).extension().string();
    if (ext.empty()) ext = extension;
    for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    auto extractSignatures = [&ext](const std::vector<std::string>& tokens) {
        std::vector<std::string> sigs;
        if (ext == ".py" || ext == ".pyi" || ext == ".sh") {
            bool inDefOrClass = false;
            bool inInit = false;
            int parenDepth = 0;
            for (size_t i = 0; i < tokens.size(); ++i) {
                const auto& t = tokens[i];
                if (t == "def" || t == "class") {
                    inDefOrClass = true;
                    inInit = false;
                    parenDepth = 0;
                    sigs.push_back(t);
                    continue;
                }
                if (inDefOrClass) {
                    if (t == ":") {
                        sigs.push_back(t);
                        inDefOrClass = false;
                        inInit = false;
                        continue;
                    }
                    if (t == "(") {
                        parenDepth++;
                        sigs.push_back(t);
                        continue;
                    }
                    if (t == ")") {
                        if (parenDepth > 0) parenDepth--;
                        inInit = false;
                        sigs.push_back(t);
                        continue;
                    }
                    if (t == "=") {
                        inInit = true;
                        sigs.push_back(t);
                        continue;
                    }
                    if (t == ",") {
                        inInit = false;
                        sigs.push_back(t);
                        continue;
                    }
                    if (inInit) {
                        continue; // Skip initializer values after =
                    }
                    if (isStringLiteralToken(t) || isRawNumberToken(t)) {
                        continue; // Skip literals and numbers
                    }
                    sigs.push_back(t);
                }
            }
        } else {
            int depth = 0;
            bool inInit = false;
            int parenDepth = 0;
            int bracketDepth = 0;

            for (const auto& t : tokens) {
                if (t == "{") {
                    if (depth == 0) {
                        sigs.push_back("{");
                    }
                    depth++;
                    inInit = false;
                } else if (t == "}") {
                    if (depth > 0) depth--;
                    if (depth == 0) {
                        sigs.push_back("}");
                    }
                    inInit = false;
                } else if (depth == 0) {
                    if (t == "=") {
                        inInit = true;
                        parenDepth = 0;
                        bracketDepth = 0;
                        sigs.push_back(t);
                        continue;
                    }

                    if (inInit) {
                        if (t == "(") {
                            parenDepth++;
                        } else if (t == ")") {
                            if (parenDepth > 0) parenDepth--;
                            else inInit = false;
                        } else if (t == "[") {
                            bracketDepth++;
                        } else if (t == "]") {
                            if (bracketDepth > 0) bracketDepth--;
                        } else if (t == ";" || t == ",") {
                            if (parenDepth == 0 && bracketDepth == 0) {
                                inInit = false;
                            }
                        }

                        if (inInit) {
                            continue;
                        } else {
                            sigs.push_back(t);
                            continue;
                        }
                    }

                    if (isStringLiteralToken(t) || isRawNumberToken(t)) {
                        continue;
                    }

                    sigs.push_back(t);
                }
            }
        }
        return sigs;
    };

    std::vector<std::string> oldSigs = extractSignatures(oldTokens);
    std::vector<std::string> newSigs = extractSignatures(newTokens);

    if (oldSigs != newSigs) {
        return 10; // Signature modified, function added/removed, or global declaration changed
    }

    // Otherwise, internal logic change inside function body
    return 1;
}

} // namespace chronos

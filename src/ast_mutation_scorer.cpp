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

    auto extractDeclarations = [&ext](const std::vector<std::string>& tokens) {
        std::unordered_set<std::string> decls;
        if (ext == ".py" || ext == ".pyi" || ext == ".sh") {
            bool inDefOrClass = false;
            bool inImport = false;
            std::string currentDecl = "";
            int parenDepth = 0;
            for (size_t i = 0; i < tokens.size(); ++i) {
                const auto& t = tokens[i];
                if (t == "def" || t == "class") {
                    inDefOrClass = true;
                    parenDepth = 0;
                    currentDecl = t;
                    continue;
                }
                if (t == "import" || t == "from") {
                    inImport = true;
                    currentDecl = t;
                    continue;
                }
                
                if (inDefOrClass) {
                    if (t == ":") {
                        decls.insert(currentDecl);
                        inDefOrClass = false;
                        currentDecl = "";
                        continue;
                    }
                    if (t == "(") parenDepth++;
                    if (t == ")") if (parenDepth > 0) parenDepth--;
                    if (t == "=" || t == "->") continue; // skip defaults and return types to focus on signature
                    if (!isStringLiteralToken(t) && !isRawNumberToken(t)) {
                        currentDecl += " " + t;
                    }
                } else if (inImport) {
                    if (t == "\n" || t == ";") { // Although we stripped newlines, wait, tokenize strips newlines. 
                        // imports in python are usually one line. Since newlines are stripped, this is tricky.
                        // For python imports, we might just grab the next 1-3 tokens.
                    }
                    // Since python imports without newlines are hard to terminate, we'll just capture the module name:
                    if (currentDecl.size() < 30) {
                        currentDecl += " " + t;
                    } else {
                        decls.insert(currentDecl);
                        inImport = false;
                    }
                }
            }
            if (inImport && !currentDecl.empty()) decls.insert(currentDecl);
        } else {
            // C++/Java/TS/etc.
            int depth = 0;
            std::string currentDecl = "";
            int parenDepth = 0;
            bool isFunc = false;
            bool isClass = false;

            for (size_t i = 0; i < tokens.size(); ++i) {
                const auto& t = tokens[i];
                if (t == "{") {
                    if (depth == 0 && !currentDecl.empty()) {
                        decls.insert(currentDecl);
                    }
                    depth++;
                    currentDecl = "";
                    isFunc = false;
                    isClass = false;
                    parenDepth = 0;
                } else if (t == "}") {
                    if (depth > 0) depth--;
                } else if (depth == 0) {
                    if (t == "class" || t == "struct" || t == "interface") {
                        isClass = true;
                        currentDecl = t;
                    } else if (t == "import" || t == "#include" || t == "using") {
                        // Capture import up to semicolon
                        std::string imp = t;
                        size_t j = i + 1;
                        while (j < tokens.size() && tokens[j] != ";" && tokens[j] != "{" && imp.size() < 50) {
                            imp += " " + tokens[j];
                            j++;
                        }
                        decls.insert(imp);
                        i = j;
                    } else if (t == "(") {
                        parenDepth++;
                        currentDecl += " " + t;
                        isFunc = true;
                    } else if (t == ")") {
                        if (parenDepth > 0) parenDepth--;
                        currentDecl += " " + t;
                    } else if (t == ";" || t == "=") {
                        currentDecl = "";
                        isFunc = false;
                        isClass = false;
                    } else {
                        if (isClass || isFunc || currentDecl.empty()) {
                            if (!currentDecl.empty()) currentDecl += " ";
                            currentDecl += t;
                        } else {
                            currentDecl += " " + t;
                        }
                    }
                }
            }
        }
        return decls;
    };

    std::unordered_set<std::string> oldSet = extractDeclarations(oldTokens);
    std::unordered_set<std::string> newSet = extractDeclarations(newTokens);
    
    int score = 0;

    auto scoreDeclaration = [](const std::string& decl) -> int {
        if (decl.find("class ") == 0 || decl.find("struct ") == 0) return 40;
        if (decl.find("import ") == 0 || decl.find("from ") == 0 || decl.find("#include ") == 0) return 25;
        if (decl.find("def ") == 0 || decl.find("fn ") == 0 || decl.find("func ") == 0) return 15;
        // Function signatures in C++ don't always start with a keyword.
        if (decl.find("(") != std::string::npos && decl.find(")") != std::string::npos) return 15;
        return 10;
    };

    for (const auto& decl : oldSet) {
        if (!newSet.count(decl)) {
            score += scoreDeclaration(decl);
        }
    }
    for (const auto& decl : newSet) {
        if (!oldSet.count(decl)) {
            score += scoreDeclaration(decl);
        }
    }

    // Low-Weight Internal Logic (Frequency Tally)
    auto tallyTokens = [](const std::vector<std::string>& tokens) {
        int ops = 0, control = 0, vars = 0;
        for (const auto& t : tokens) {
            if (t == "+" || t == "-" || t == "*" || t == "/" || t == "==" || t == "!=") ops++;
            else if (t == "if" || t == "else" || t == "while" || t == "for" || t == "switch") control++;
            else if (t == "=" || t == "+=" || t == "-=") vars++;
        }
        return std::make_tuple(ops, control, vars);
    };

    auto [oldOps, oldCtrl, oldVars] = tallyTokens(oldTokens);
    auto [newOps, newCtrl, newVars] = tallyTokens(newTokens);

    score += std::abs(oldOps - newOps) * 1;
    score += std::abs(oldCtrl - newCtrl) * 2;
    score += std::abs(oldVars - newVars) * 1;

    return score;

}

} // namespace chronos

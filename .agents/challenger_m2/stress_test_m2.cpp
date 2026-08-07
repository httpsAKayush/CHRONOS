#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <git2.h>
#include "chronos/ast_mutation_scorer.hpp"
#include "chronos/codex.hpp"
#include "chronos/vector_index.hpp"
#include "chronos/ast_indexer.hpp"
#include "chronos/git_indexer.hpp"

namespace fs = std::filesystem;
using namespace chronos;

// Duplicated helper matching GitIndexer's anonymous namespace logic for direct verification
bool testIsChoreOrBotCommit(const std::string& msg, const std::string& author, const std::string& email) {
    std::string lowerMsg = msg;
    for (char &c : lowerMsg) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string lowerAuthor = author;
    for (char &c : lowerAuthor) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string lowerEmail = email;
    for (char &c : lowerEmail) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (lowerEmail.find("bot") != std::string::npos ||
        lowerEmail.find("noreply.github.com") != std::string::npos ||
        lowerAuthor.find("[bot]") != std::string::npos ||
        lowerAuthor.find("dependabot") != std::string::npos ||
        lowerAuthor.find("renovate") != std::string::npos ||
        lowerMsg.rfind("auto-generated", 0) == 0) {
        return true;
    }

    if (lowerMsg.rfind("chore:", 0) == 0 ||
        lowerMsg.rfind("chore(", 0) == 0 ||
        lowerMsg.rfind("chore ", 0) == 0 ||
        lowerMsg.rfind("build(deps)", 0) == 0 ||
        lowerMsg.rfind("ci:", 0) == 0 ||
        lowerMsg.rfind("ci(", 0) == 0 ||
        lowerMsg.rfind("ci ", 0) == 0 ||
        lowerMsg.rfind("bump ", 0) == 0) {
        return true;
    }

    return false;
}

struct TestResults {
    int total = 0;
    int passed = 0;
    int failed = 0;
    std::vector<std::string> failureDetails;
};

TestResults g_results;

#define ASSERT_TEST(cond, msg) \
    do { \
        g_results.total++; \
        if (cond) { \
            g_results.passed++; \
        } else { \
            g_results.failed++; \
            std::cout << "  [FAIL] " << msg << std::endl; \
            g_results.failureDetails.push_back(std::string("[FAIL] ") + msg); \
        } \
    } while(0)

// ----------------------------------------------------------------------------
// Test 1: Malformed & Unclosed Comments / String Literals
// ----------------------------------------------------------------------------
void test_malformed_inputs() {
    std::cout << "\n=== Test 1: Malformed / Unclosed Comments & String Literals ===" << std::endl;

    // 1.1 Unclosed C++ block comment
    {
        std::string oldCode = "int main() {\n    /* unclosed block comment\n    return 0;\n}";
        std::string newCode = "int main() {\n    /* unclosed block comment modified\n    return 0;\n}";
        auto start = std::chrono::high_resolution_clock::now();
        int score = AstMutationScorer::scoreDiff(oldCode, newCode, ".cpp");
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start).count();
        std::cout << "  1.1 C++ Unclosed block comment score: " << score << " (took " << elapsed << " us)" << std::endl;
        ASSERT_TEST(score == 0 || score == 1 || score == 10, "C++ unclosed block comment score check");
    }

    // 1.2 Unclosed C++ string literal
    {
        std::string oldCode = "const char* str = \"unclosed string literal;";
        std::string newCode = "const char* str = \"unclosed string literal modified;";
        auto start = std::chrono::high_resolution_clock::now();
        int score = AstMutationScorer::scoreDiff(oldCode, newCode, ".cpp");
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start).count();
        std::cout << "  1.2 C++ Unclosed string literal score: " << score << " (took " << elapsed << " us)" << std::endl;
        ASSERT_TEST(score >= 0, "C++ unclosed string literal terminates cleanly");
    }

    // 1.3 Unclosed Python triple double quote
    {
        std::string oldCode = "def test():\n    \"\"\" unclosed triple double quote\n    return 42\n";
        std::string newCode = "def test():\n    \"\"\" unclosed triple double quote mod\n    return 42\n";
        auto start = std::chrono::high_resolution_clock::now();
        int score = AstMutationScorer::scoreDiff(oldCode, newCode, ".py");
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start).count();
        std::cout << "  1.3 Python Unclosed triple double quote score: " << score << " (took " << elapsed << " us)" << std::endl;
        ASSERT_TEST(score >= 0, "Python unclosed triple double quote terminates cleanly");
    }

    // 1.4 Unclosed Python triple single quote
    {
        std::string oldCode = "def test():\n    ''' unclosed triple single quote\n    return 42\n";
        std::string newCode = "def test():\n    ''' unclosed triple single quote mod\n    return 42\n";
        auto start = std::chrono::high_resolution_clock::now();
        int score = AstMutationScorer::scoreDiff(oldCode, newCode, ".py");
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start).count();
        std::cout << "  1.4 Python Unclosed triple single quote score: " << score << " (took " << elapsed << " us)" << std::endl;
        ASSERT_TEST(score >= 0, "Python unclosed triple single quote terminates cleanly");
    }

    // 1.5 Unclosed Python single quote
    {
        std::string oldCode = "msg = 'unclosed single quote\nx = 10\n";
        std::string newCode = "msg = 'unclosed single quote mod\nx = 10\n";
        int score = AstMutationScorer::scoreDiff(oldCode, newCode, ".py");
        std::cout << "  1.5 Python Unclosed single quote score: " << score << std::endl;
        ASSERT_TEST(score >= 0, "Python unclosed single quote terminates cleanly");
    }
}

// ----------------------------------------------------------------------------
// Test 2: Massive Whitespace / Indentation Diffs (100k lines)
// ----------------------------------------------------------------------------
void test_massive_whitespace_diffs() {
    std::cout << "\n=== Test 2: Massive Whitespace & Indentation Diffs (100k lines) ===" << std::endl;

    // 2.1 100k lines of empty space / tabs / newlines added to C++ file
    {
        std::string oldCode = "int compute(int x) {\n    return x * 2;\n}\n";
        std::string newCode = oldCode;
        newCode.reserve(oldCode.size() + 100000 * 5);
        for (int i = 0; i < 100000; ++i) {
            newCode += (i % 2 == 0) ? "    \n" : "\t\t   \n";
        }

        auto start = std::chrono::high_resolution_clock::now();
        int score = AstMutationScorer::scoreDiff(oldCode, newCode, ".cpp");
        auto elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - start).count();

        std::cout << "  2.1 100k lines whitespace diff score: " << score << " (took " << std::fixed << std::setprecision(2) << elapsedMs << " ms)" << std::endl;
        ASSERT_TEST(score == 0, "100k lines whitespace diff must score 0");
        ASSERT_TEST(elapsedMs < 1000.0, "100k lines whitespace diff must take < 1 second");
    }

    // 2.2 Re-indentation of 5k functions (100k lines total)
    {
        std::string oldCode;
        std::string newCode;
        oldCode.reserve(2000000);
        newCode.reserve(2000000);

        for (int i = 0; i < 5000; ++i) {
            oldCode += "int fn_" + std::to_string(i) + "(int a) {\n    int b = a + 1;\n    return b;\n}\n";
            newCode += "int fn_" + std::to_string(i) + "(int a)\n{\n        int b = a + 1;\n        return b;\n}\n";
        }

        auto start = std::chrono::high_resolution_clock::now();
        int score = AstMutationScorer::scoreDiff(oldCode, newCode, ".cpp");
        auto elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - start).count();

        std::cout << "  2.2 Re-indentation of 5k functions score: " << score << " (took " << std::fixed << std::setprecision(2) << elapsedMs << " ms)" << std::endl;
        ASSERT_TEST(score == 0, "5k functions re-indentation diff must score 0");
        ASSERT_TEST(elapsedMs < 2000.0, "5k functions re-indentation must finish under 2s");
    }
}

// ----------------------------------------------------------------------------
// Test 3: Lockfiles, Unsupported Files, and Extension Matching
// ----------------------------------------------------------------------------
void test_lockfiles_and_extensions() {
    std::cout << "\n=== Test 3: Lockfiles, Unsupported Files & Extension Handling ===" << std::endl;

    std::string codeA = "{\n  \"dependency\": \"v1.0.0\",\n  \"hash\": \"abc\"\n}";
    std::string codeB = "{\n  \"dependency\": \"v2.0.0\",\n  \"hash\": \"xyz\"\n}";

    // 3.1 Known Lockfiles
    std::vector<std::string> lockfiles = {
        "package-lock.json", "cargo.lock", "Cargo.lock", "yarn.lock",
        "pnpm-lock.yaml", "composer.lock", "gemfile.lock", "poetry.lock", "go.sum",
        "PACKAGE-LOCK.JSON", "YARN.LOCK", "sub/dir/Cargo.lock", "custom.lock", "APP.LOCK"
    };

    for (const auto& lf : lockfiles) {
        int score = AstMutationScorer::scoreDiff(codeA, codeB, lf);
        ASSERT_TEST(score == 0, ("Lockfile " + lf + " must score 0").c_str());
    }
    std::cout << "  3.1 Tested " << lockfiles.size() << " lockfile variants: all scored 0." << std::endl;

    // 3.2 Unsupported File Extensions
    std::vector<std::string> unsupported = {
        ".md", ".txt", ".json", ".yaml", ".yml", ".xml", ".csv",
        ".png", ".jpg", ".pdf", ".zip", ".MD", ".TXT", ".JSON",
        "Makefile", "Dockerfile", "README", ".gitignore"
    };

    for (const auto& un : unsupported) {
        int score = AstMutationScorer::scoreDiff(codeA, codeB, un);
        ASSERT_TEST(score == 0, ("Unsupported file type " + un + " must score 0").c_str());
    }
    std::cout << "  3.2 Tested " << unsupported.size() << " unsupported file types: all scored 0." << std::endl;

    // 3.3 Supported Extensions with Case Variations
    std::vector<std::string> supported = {
        ".cpp", ".hpp", ".c", ".h", ".cc", ".cxx", ".hh", ".hxx",
        ".py", ".pyi", ".js", ".ts", ".jsx", ".tsx", ".java", ".go",
        ".rs", ".cs", ".mjs", ".cjs", ".sh",
        ".CPP", ".Py", ".JS", ".TS", ".Go", ".RS"
    };

    std::string pyCode1 = "def foo():\n    return 1\n";
    std::string pyCode2 = "def foo():\n    return 2\n"; // internal change -> score 1
    for (const auto& sup : supported) {
        int score = AstMutationScorer::scoreDiff(pyCode1, pyCode2, sup);
        ASSERT_TEST(score == 1, ("Supported extension " + sup + " internal logic change must score 1 (got " + std::to_string(score) + ")").c_str());
    }
    std::cout << "  3.3 Tested " << supported.size() << " supported code extensions: case insensitive matching verified." << std::endl;
}

// ----------------------------------------------------------------------------
// Test 4: Chore Commit Prefixes and Bot Author Detection
// ----------------------------------------------------------------------------
void test_chore_and_bot_detection() {
    std::cout << "\n=== Test 4: Chore Prefixes & Bot Author Detection ===" << std::endl;

    // 4.1 Valid Chore Prefixes
    std::vector<std::string> choreMsgs = {
        "chore: update dependencies",
        "chore(deps): bump numpy from 1.20 to 1.21",
        "chore update package-lock.json",
        "build(deps): update cargo crates",
        "ci: add github action workflow",
        "ci(test): update test matrix",
        "ci update runner",
        "bump version to 1.0.0",
        "auto-generated documentation update",
        "CHORE: UPPERCASE CHORE",
        "Ci(Build): Uppercase CI"
    };

    for (const auto& msg : choreMsgs) {
        bool result = testIsChoreOrBotCommit(msg, "John Doe", "john@example.com");
        ASSERT_TEST(result == true, ("Chore message '" + msg + "' must be detected").c_str());
    }
    std::cout << "  4.1 Tested " << choreMsgs.size() << " chore/ci message patterns: all detected." << std::endl;

    // 4.2 Bot Authors and Emails
    struct BotCase {
        std::string author;
        std::string email;
    };
    std::vector<BotCase> botCases = {
        {"dependabot[bot]", "49699333+dependabot[bot]@users.noreply.github.com"},
        {"renovate[bot]", "renovate@whitesourcesoftware.com"},
        {"github-actions[bot]", "github-actions[bot]@users.noreply.github.com"},
        {"dependabot", "dependabot@github.com"},
        {"renovate", "renovate@github.com"},
        {"Alice Worker", "my-service-bot@company.com"},
        {"Bob Builder", "user@noreply.github.com"}
    };

    for (const auto& b : botCases) {
        bool result = testIsChoreOrBotCommit("feat: normal commit message", b.author, b.email);
        ASSERT_TEST(result == true, ("Bot author/email '" + b.author + " / " + b.email + "' must be detected").c_str());
    }
    std::cout << "  4.2 Tested " << botCases.size() << " bot author/email patterns: all detected." << std::endl;

    // 4.3 Non-Chore, Non-Bot (Normal Commits)
    struct NormalCase {
        std::string msg;
        std::string author;
        std::string email;
    };
    std::vector<NormalCase> normalCases = {
        {"feat: add new user login endpoint", "Developer One", "dev1@company.com"},
        {"fix: resolve null pointer dereference in parser", "Developer Two", "dev2@company.com"},
        {"refactor: simplify AST node traversal", "Engineer Three", "eng3@company.com"},
        {"docs: update API usage guide", "Doc Writer", "docs@company.com"},
        {"fix chore tracking logic in system", "Developer Four", "dev4@company.com"}, // Contains 'chore' but not prefix
        {"choreography engine implementation", "Developer Five", "dev5@company.com"}  // Starts with choreography, not chore:
    };

    for (const auto& n : normalCases) {
        bool result = testIsChoreOrBotCommit(n.msg, n.author, n.email);
        ASSERT_TEST(result == false, ("Normal commit '" + n.msg + "' must NOT be flagged as chore/bot").c_str());
    }
    std::cout << "  4.3 Tested " << normalCases.size() << " normal developer commits: none flagged as bot/chore." << std::endl;

    // 4.4 Substring false positive analysis: Emails containing "bot" in surname/domain
    {
        std::string author = "Arthur Abbott";
        std::string email = "abbott@company.com"; // Contains "bot" in abbott
        bool result = testIsChoreOrBotCommit("feat: feature", author, email);
        std::cout << "  4.4 Substring edge case check: email 'abbott@company.com' flagged as bot? " 
                  << (result ? "YES (False Positive!)" : "NO") << std::endl;
    }
}

// ----------------------------------------------------------------------------
// Test 5: End-to-End GitIndexer Integration Test on Temp Git Repo
// ----------------------------------------------------------------------------
void test_git_indexer_integration() {
    std::cout << "\n=== Test 5: End-to-End GitIndexer Integration Test ===" << std::endl;

    std::string testRepoDir = "/tmp/chronos_test_repo_m2";
    if (fs::exists(testRepoDir)) {
        fs::remove_all(testRepoDir);
    }
    fs::create_directories(testRepoDir);

    // Initialize git repository using system git command
    system(("git -C " + testRepoDir + " init").c_str());
    system(("git -C " + testRepoDir + " config user.name 'Test Dev'").c_str());
    system(("git -C " + testRepoDir + " config user.email 'dev@test.com'").c_str());

    // Commit 1: Initial C++ file
    {
        std::ofstream out(testRepoDir + "/main.cpp");
        out << "int add(int a, int b) {\n    return a + b;\n}\n";
        out.close();
        system(("git -C " + testRepoDir + " add main.cpp").c_str());
        system(("git -C " + testRepoDir + " commit -m 'feat: initial commit'").c_str());
    }

    // Commit 2: Chore commit (dependabot) on package-lock.json
    {
        std::ofstream out(testRepoDir + "/package-lock.json");
        out << "{\n  \"name\": \"test\",\n  \"version\": \"1.0.0\"\n}\n";
        out.close();
        system(("git -C " + testRepoDir + " add package-lock.json").c_str());
        system(("git -C " + testRepoDir + " commit --author='dependabot[bot] <dependabot@users.noreply.github.com>' -m 'chore(deps): bump lockfile'").c_str());
    }

    // Commit 3: Formatting-only C++ commit
    {
        std::ofstream out(testRepoDir + "/main.cpp");
        out << "int add(int a, int b)\n{\n    return a + b;\n}\n";
        out.close();
        system(("git -C " + testRepoDir + " add main.cpp").c_str());
        system(("git -C " + testRepoDir + " commit -m 'style: reformat main.cpp'").c_str());
    }

    // Commit 4: Structural C++ change (adding new function)
    {
        std::ofstream out(testRepoDir + "/main.cpp");
        out << "int add(int a, int b)\n{\n    return a + b;\n}\n\nint multiply(int a, int b) {\n    return a * b;\n}\n";
        out.close();
        system(("git -C " + testRepoDir + " add main.cpp").c_str());
        system(("git -C " + testRepoDir + " commit -m 'feat: add multiply function'").c_str());
    }

    // Test GitIndexer on testRepoDir
    try {
        Codex codex(testRepoDir);
        VectorIndex vectorIndex(testRepoDir);
        AstIndexer astIndexer(codex, vectorIndex, testRepoDir);
        GitIndexer gitIndexer(testRepoDir, codex, astIndexer);

        gitIndexer.indexHistory();
        std::cout << "  5.1 GitIndexer::indexHistory executed successfully on test repository." << std::endl;
        ASSERT_TEST(true, "GitIndexer run completed without errors");
    } catch (const std::exception& e) {
        std::cout << "  5.1 GitIndexer failed with exception: " << e.what() << std::endl;
        ASSERT_TEST(false, "GitIndexer threw exception");
    }

    // Cleanup
    fs::remove_all(testRepoDir);
}

int main() {
    std::cout << "==========================================================" << std::endl;
    std::cout << "  CHRONOS MILESTONE 2: EMPIRICAL STRESS TEST HARNESS" << std::endl;
    std::cout << "==========================================================" << std::endl;

    test_malformed_inputs();
    test_massive_whitespace_diffs();
    test_lockfiles_and_extensions();
    test_chore_and_bot_detection();
    test_git_indexer_integration();

    std::cout << "\n==========================================================" << std::endl;
    std::cout << "  STRESS TEST SUMMARY: " << g_results.passed << "/" << g_results.total << " PASSED";
    if (g_results.failed > 0) {
        std::cout << " (" << g_results.failed << " FAILED)";
    }
    std::cout << "\n==========================================================" << std::endl;

    for (const auto& err : g_results.failureDetails) {
        std::cout << "  " << err << std::endl;
    }

    return (g_results.failed == 0) ? 0 : 1;
}

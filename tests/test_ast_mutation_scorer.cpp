#include "test_framework.hpp"
#include "chronos/ast_mutation_scorer.hpp"

using namespace chronos;

static void test_whitespace_changes() {
    // C++ whitespace change
    std::string cppOld = "int add(int a, int b) {\n    return a + b;\n}";
    std::string cppNew = "int   add(int  a,  int   b)  {   \n\n\n   return   a   +   b;   \n}";
    CHRONOS_CHECK(AstMutationScorer::scoreDiff(cppOld, cppNew, ".cpp") == 0);
    CHRONOS_CHECK(AstMutationScorer::scoreDiff(cppOld, cppNew, ".hpp") == 0);

    // Python indentation / trailing whitespace change
    std::string pyOld = "def calculate(x, y):\n    result = x + y\n    return result\n";
    std::string pyNew = "\ndef calculate(x, y):\n    result = x + y  \n    return result\n\n";
    CHRONOS_CHECK(AstMutationScorer::scoreDiff(pyOld, pyNew, ".py") == 0);
}

static void test_comment_changes() {
    // C-style single-line and block comments
    std::string cppOld = "// Initial comment\nvoid process() {\n    /* Block comment */\n    int val = 42;\n}";
    std::string cppNew = "/* Changed header comment */\nvoid process() {\n    // Different line comment\n    int val = 42; // inline comment\n}";
    CHRONOS_CHECK(AstMutationScorer::scoreDiff(cppOld, cppNew, ".cpp") == 0);

    // Preserving comments inside string literals
    std::string cppStrOld = "const char* url = \"http://example.com/api\";";
    std::string cppStrNew = "const char* url = \"http://example.com/api\"; // added comment";
    CHRONOS_CHECK(AstMutationScorer::scoreDiff(cppStrOld, cppStrNew, ".cpp") == 0);

    std::string cppStrDiff = "const char* url = \"http://example.com/api/v2\";";
    CHRONOS_CHECK(AstMutationScorer::scoreDiff(cppStrOld, cppStrDiff, ".cpp") == 1);

    // Python comments
    std::string pyOld = "# Comment 1\ndef run():\n    # Comment 2\n    msg = \"# Not a comment\"\n    return msg\n";
    std::string pyNew = "# Updated Comment\ndef run():\n    msg = \"# Not a comment\"\n    # Comment 3\n    return msg\n";
    CHRONOS_CHECK(AstMutationScorer::scoreDiff(pyOld, pyNew, ".py") == 0);
}

static void test_formatting_changes() {
    // Brace placement and newline formatting
    std::string oldCode = "void fn(int a, int b) {\n    if (a > b) { return; }\n}";
    std::string newCode = "void fn(\n    int a,\n    int b\n)\n{\n    if (a > b)\n    {\n        return;\n    }\n}";
    CHRONOS_CHECK(AstMutationScorer::scoreDiff(oldCode, newCode, ".cpp") == 0);
    CHRONOS_CHECK(AstMutationScorer::scoreDiff(oldCode, newCode, ".js") == 0);
    CHRONOS_CHECK(AstMutationScorer::scoreDiff(oldCode, newCode, ".ts") == 0);
}

static void test_lockfiles_and_unsupported_files() {
    std::string oldContent = "{\n  \"name\": \"app\",\n  \"version\": \"1.0.0\"\n}";
    std::string newContent = "{\n  \"name\": \"app\",\n  \"version\": \"1.0.1\"\n}";

    // Lockfiles return 0 regardless of content change
    CHRONOS_CHECK(AstMutationScorer::scoreDiff(oldContent, newContent, "package-lock.json") == 0);
    CHRONOS_CHECK(AstMutationScorer::scoreDiff(oldContent, newContent, "Cargo.lock") == 0);
    CHRONOS_CHECK(AstMutationScorer::scoreDiff(oldContent, newContent, "yarn.lock") == 0);
    CHRONOS_CHECK(AstMutationScorer::scoreDiff(oldContent, newContent, "pnpm-lock.yaml") == 0);
    CHRONOS_CHECK(AstMutationScorer::scoreDiff(oldContent, newContent, "go.sum") == 0);

    // Unsupported file types return 0
    CHRONOS_CHECK(AstMutationScorer::scoreDiff(oldContent, newContent, ".md") == 0);
    CHRONOS_CHECK(AstMutationScorer::scoreDiff(oldContent, newContent, ".txt") == 0);
    CHRONOS_CHECK(AstMutationScorer::scoreDiff(oldContent, newContent, ".json") == 0);
}

static void test_internal_logic_changes() {
    // Expression and statement changes inside function body -> Score 1
    std::string cppOld = "int compute(int limit) {\n    int sum = 0;\n    for (int i = 0; i < limit; ++i) {\n        sum += i;\n    }\n    return sum;\n}";
    std::string cppNew = "int compute(int limit) {\n    int sum = 0;\n    for (int i = 0; i <= limit; ++i) {\n        sum += (i * 2);\n    }\n    return sum;\n}";
    CHRONOS_CHECK(AstMutationScorer::scoreDiff(cppOld, cppNew, ".cpp") == 1);

    std::string pyOld = "def process(items):\n    out = []\n    for x in items:\n        out.append(x * 2)\n    return out\n";
    std::string pyNew = "def process(items):\n    out = []\n    for x in items:\n        if x > 0:\n            out.append(x * 3)\n    return out\n";
    CHRONOS_CHECK(AstMutationScorer::scoreDiff(pyOld, pyNew, ".py") == 1);
}

static void test_contract_breaking_changes() {
    // Function addition -> Score 10
    std::string cppOld = "void foo() {}";
    std::string cppNew = "void foo() {}\nvoid bar() {}";
    CHRONOS_CHECK(AstMutationScorer::scoreDiff(cppOld, cppNew, ".cpp") == 10);

    // Visibility change -> Score 10
    std::string cppVisOld = "class Service {\npublic:\n    void execute();\n};";
    std::string cppVisNew = "class Service {\nprivate:\n    void execute();\n};";
    CHRONOS_CHECK(AstMutationScorer::scoreDiff(cppVisOld, cppVisNew, ".hpp") == 10);

    // Signature change -> Score 10
    std::string cppSigOld = "int calc(int a);";
    std::string cppSigNew = "int calc(int a, int b);";
    CHRONOS_CHECK(AstMutationScorer::scoreDiff(cppSigOld, cppSigNew, ".hpp") == 10);

    // Python function signature change -> Score 10
    std::string pySigOld = "def fetch(url):\n    return get(url)\n";
    std::string pySigNew = "def fetch(url, timeout=30):\n    return get(url, timeout)\n";
    CHRONOS_CHECK(AstMutationScorer::scoreDiff(pySigOld, pySigNew, ".py") == 10);

    // Empty file to content or content to empty file -> Score 10
    CHRONOS_CHECK(AstMutationScorer::scoreDiff("", "int x = 5;", ".cpp") == 10);
    CHRONOS_CHECK(AstMutationScorer::scoreDiff("int x = 5;", "", ".cpp") == 10);
}

static void test_top_level_variable_value_changes() {
    // String literal value change at top-level -> Score 1
    std::string v1 = "const char* url = \"http://api.v1\";";
    std::string v2 = "const char* url = \"http://api.v2\";";
    CHRONOS_CHECK(AstMutationScorer::scoreDiff(v1, v2, ".cpp") == 1);

    // Number literal value change at top-level -> Score 1
    std::string n1 = "const int PORT = 8080;";
    std::string n2 = "const int PORT = 9090;";
    CHRONOS_CHECK(AstMutationScorer::scoreDiff(n1, n2, ".cpp") == 1);

    // Expression initializer value change at top-level -> Score 1
    std::string e1 = "static const int MAX_BUF = 1024 * 4;";
    std::string e2 = "static const int MAX_BUF = 2048 * 4;";
    CHRONOS_CHECK(AstMutationScorer::scoreDiff(e1, e2, ".cpp") == 1);

    // Structural variable name change at top-level -> Score 10
    std::string r1 = "const int PORT = 8080;";
    std::string r2 = "const int SERVER_PORT = 8080;";
    CHRONOS_CHECK(AstMutationScorer::scoreDiff(r1, r2, ".cpp") == 10);

    // Structural variable type change at top-level -> Score 10
    std::string t1 = "const int PORT = 8080;";
    std::string t2 = "const long PORT = 8080;";
    CHRONOS_CHECK(AstMutationScorer::scoreDiff(t1, t2, ".cpp") == 10);
}

void run_ast_mutation_scorer_tests() {
    test_whitespace_changes();
    test_comment_changes();
    test_formatting_changes();
    test_lockfiles_and_unsupported_files();
    test_internal_logic_changes();
    test_contract_breaking_changes();
    test_top_level_variable_value_changes();
}

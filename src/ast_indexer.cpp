#include "chronos/ast_indexer.hpp"
#include "chronos/simhash.hpp"
#include <fstream>
#include <sstream>
#include <random>
#include <filesystem>
#include <chrono>
#include <unordered_set>

#if CHRONOS_HAVE_TREE_SITTER_CPP || CHRONOS_HAVE_TREE_SITTER_PYTHON || CHRONOS_HAVE_TREE_SITTER_JAVASCRIPT || CHRONOS_HAVE_TREE_SITTER_CSS
extern "C" {
  #include <tree_sitter/api.h>
#if CHRONOS_HAVE_TREE_SITTER_CPP
  const TSLanguage* tree_sitter_cpp(void);
#endif
#if CHRONOS_HAVE_TREE_SITTER_PYTHON
  const TSLanguage* tree_sitter_python(void);
#endif
#if CHRONOS_HAVE_TREE_SITTER_JAVASCRIPT
  const TSLanguage* tree_sitter_javascript(void);
#endif
#if CHRONOS_HAVE_TREE_SITTER_CSS
  const TSLanguage* tree_sitter_css(void);
#endif
}
#endif

namespace fs = std::filesystem;

namespace chronos {

namespace {
std::string makeUuid() {
    static std::mt19937_64 rng{std::random_device{}()};
    static const char* hex = "0123456789abcdef";
    std::string s(32, '0');
    for (auto& c : s) c = hex[rng() % 16];
    // Format loosely as UUID4-shaped for readability; uniqueness is what
    // actually matters here, not strict RFC compliance.
    s.insert(8, "-"); s.insert(13, "-"); s.insert(18, "-"); s.insert(23, "-");
    return s;
}

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
}

bool AstIndexer::hasTreeSitter() {
#if CHRONOS_HAVE_TREE_SITTER_CPP || CHRONOS_HAVE_TREE_SITTER_PYTHON
    return true;
#else
    return false;
#endif
}

AstIndexer::AstIndexer(Codex& codex, VectorIndex& vectors, const std::string& repoRoot)
    : codex_(codex), vectors_(vectors), repoRoot_(repoRoot) {}

void AstIndexer::removeFile(const std::string& relativePath) {
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(codex_.raw(), "SELECT id FROM nodes WHERE file_path = ?1 AND is_active = 1;",
                        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, relativePath.c_str(), -1, SQLITE_TRANSIENT);
    std::vector<std::string> ids;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ids.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
    }
    sqlite3_finalize(stmt);

    for (auto& id : ids) {
        codex_.tombstoneNode(id);
        vectors_.remove(id);
        ++stats_.nodesTombstoned;
    }
}

#if CHRONOS_HAVE_TREE_SITTER_CPP || CHRONOS_HAVE_TREE_SITTER_PYTHON
namespace {
// Walks the tree, collecting (byte_start, byte_end, structural tokens) for
// every function_definition node. Structural tokens = the node's own
// grammar-level type name plus its non-identifier child kinds (keeps
// control-flow shape, strips identifier/type text) — this is what makes
// Simhash rename-stable per Spec Glossary "Simhash Normalization".
struct ExtractedCall {
    std::string target;
    int start_line;
    bool is_noise;
    std::string call_site_text;
};

struct FunctionSpan {
    uint32_t byteStart, byteEnd;
    std::string name;
    std::vector<ExtractedCall> outgoingCalls;
    std::vector<StructuralToken> tokens;
};

std::string extractText(TSNode n, const std::string& source) {
    uint32_t s = ts_node_start_byte(n);
    uint32_t e = ts_node_end_byte(n);
    if (e > s && e <= source.size()) return source.substr(s, e - s);
    return "";
}

std::string findFirstIdentifier(TSNode node, const std::string& source) {
    const char* type = ts_node_type(node);
    std::string t(type);
    
    if (t == "attribute" || t == "field_expression") {
        uint32_t count = ts_node_child_count(node);
        if (count > 0) {
            return findFirstIdentifier(ts_node_child(node, count - 1), source);
        }
    }
    
    if (t == "identifier" || t == "field_identifier" || t == "name") return extractText(node, source);
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        std::string res = findFirstIdentifier(ts_node_child(node, i), source);
        if (!res.empty()) return res;
    }
    return "";
}

void extractCalls(TSNode rootNode, const std::string& source, std::vector<ExtractedCall>& out) {
    static const std::unordered_set<std::string> NOISE = {
        "print", "time", "len", "range", "int", "float", "str", "list", "dict", "set", "tuple", "bool",
        "type", "isinstance", "issubclass", "getattr", "setattr", "hasattr", "delattr", "open",
        "round", "sum", "min", "max", "abs", "enumerate", "zip", "map", "filter", "any", "all",
        "Exception", "ValueError", "TypeError", "KeyError", "IndexError", "super"
    };

    std::vector<TSNode> stack;
    stack.push_back(rootNode);

    while (!stack.empty()) {
        TSNode node = stack.back();
        stack.pop_back();
        if (ts_node_is_null(node)) continue;

        const char* type = ts_node_type(node);
        std::string t(type);
        if (t == "call_expression" || t == "call") {
            if (ts_node_child_count(node) > 0) {
                std::string target = findFirstIdentifier(ts_node_child(node, 0), source);
                if (!target.empty()) {
                    ExtractedCall ec;
                    ec.target = target;
                    ec.start_line = ts_node_start_point(node).row + 1;
                    ec.is_noise = (NOISE.count(target) > 0);
                    
                    TSNode stmtNode = node;
                    TSNode parent = ts_node_parent(stmtNode);
                    while (!ts_node_is_null(parent)) {
                        std::string pType(ts_node_type(parent));
                        if (pType == "expression_statement" || pType == "assignment" || pType == "variable_declaration" || pType == "return_statement" || pType == "declaration") {
                            stmtNode = parent;
                            break;
                        }
                        if (pType == "function_definition" || pType == "class_definition" || pType == "block") break;
                        parent = ts_node_parent(parent);
                    }
                    ec.call_site_text = extractText(stmtNode, source);
                    
                    // Trim trailing newlines if any
                    while (!ec.call_site_text.empty() && (ec.call_site_text.back() == '\n' || ec.call_site_text.back() == '\r')) {
                        ec.call_site_text.pop_back();
                    }
                    
                    out.push_back(ec);
                }
            }
        }
        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; ++i) {
            stack.push_back(ts_node_child(node, i));
        }
    }
}

void extractImports(TSNode rootNode, const std::string& source, std::vector<std::pair<std::string, std::string>>& imports) {
    std::vector<TSNode> stack;
    stack.push_back(rootNode);

    while (!stack.empty()) {
        TSNode node = stack.back();
        stack.pop_back();

        if (ts_node_is_null(node)) continue;

        std::string type = ts_node_type(node);
        
        if (type == "import_statement") {
            for (uint32_t i = 0; i < ts_node_child_count(node); ++i) {
                TSNode child = ts_node_child(node, i);
                std::string ctype = ts_node_type(child);
                if (ctype == "dotted_name") {
                    std::string mod = extractText(child, source);
                    if (!mod.empty()) imports.push_back({mod, mod});
                } else if (ctype == "aliased_import") {
                    std::string alias;
                    std::string mod;
                    for (uint32_t j = 0; j < ts_node_child_count(child); ++j) {
                        TSNode g = ts_node_child(child, j);
                        std::string gtype = ts_node_type(g);
                        if (gtype == "dotted_name") mod = extractText(g, source);
                        else if (gtype == "identifier") alias = extractText(g, source);
                    }
                    if (!alias.empty() && !mod.empty()) imports.push_back({alias, mod});
                }
            }
        } else if (type == "import_from_statement") {
            std::string moduleName = "";
            for (uint32_t i = 0; i < ts_node_child_count(node); ++i) {
                TSNode child = ts_node_child(node, i);
                std::string ctype = ts_node_type(child);
                if (ctype == "dotted_name" && moduleName.empty()) {
                    moduleName = extractText(child, source);
                } else if (ctype == "dotted_name" && !moduleName.empty()) {
                    std::string sym = extractText(child, source);
                    imports.push_back({sym, moduleName});
                } else if (ctype == "aliased_import") {
                    std::string alias;
                    std::string orig;
                    for (uint32_t j = 0; j < ts_node_child_count(child); ++j) {
                        TSNode g = ts_node_child(child, j);
                        std::string gtype = ts_node_type(g);
                        if (gtype == "dotted_name" || gtype == "identifier") {
                            if (orig.empty()) orig = extractText(g, source);
                            else alias = extractText(g, source);
                        }
                    }
                    if (!alias.empty()) imports.push_back({alias, moduleName});
                    else if (!orig.empty()) imports.push_back({orig, moduleName});
                }
            }
        }
        
        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; ++i) {
            stack.push_back(ts_node_child(node, i));
        }
    }
}

void collectTokens(TSNode rootNode, std::vector<StructuralToken>& out) {
    std::vector<TSNode> stack;
    stack.push_back(rootNode);
    while (!stack.empty()) {
        TSNode node = stack.back();
        stack.pop_back();
        if (ts_node_is_null(node)) continue;

        const char* type = ts_node_type(node);
        std::string t(type);
        // Skip pure identifier/type leaves; keep control structure & operators.
        if (t != "identifier" && t != "type_identifier" && t != "field_identifier") {
            out.push_back({t, 1});
        }
        uint32_t n = ts_node_child_count(node);
        for (uint32_t i = 0; i < n; ++i) {
            stack.push_back(ts_node_child(node, n - 1 - i));
        }
    }
}

void walk(TSNode rootNode, const std::string& source, std::vector<FunctionSpan>& spans) {
    std::vector<TSNode> stack;
    stack.push_back(rootNode);

    while (!stack.empty()) {
        TSNode node = stack.back();
        stack.pop_back();
        if (ts_node_is_null(node)) continue;

        std::string type(ts_node_type(node));
        if (type == "function_definition" || type == "class_specifier" || type == "struct_specifier" || type == "class_definition") {
            FunctionSpan span;
            span.byteStart = ts_node_start_byte(node);
            span.byteEnd = ts_node_end_byte(node);
            span.name = findFirstIdentifier(node, source);
            extractCalls(node, source, span.outgoingCalls);
            collectTokens(node, span.tokens);
            spans.push_back(std::move(span));
            
            // Only return if it's a leaf structure to prevent over-nesting, but we want methods inside classes!
            // So for classes, we should keep walking to find nested methods.
            if (type == "function_definition") continue;
        }
        uint32_t n = ts_node_child_count(node);
        for (uint32_t i = 0; i < n; ++i) {
            stack.push_back(ts_node_child(node, n - 1 - i));
        }
    }
}

static const char* JS_QUERY_STRING = 
"; 1. Catch standard functions\n"
"(function_declaration name: (identifier) @node.name) @node.definition\n"
"; 2. Catch arrow functions assigned to variables\n"
"(lexical_declaration (variable_declarator name: (identifier) @node.name value: (arrow_function))) @node.definition\n"
"; 3. Catch classes\n"
"(class_declaration name: (identifier) @node.name) @node.definition\n"
"; 4. Catch ES6 Imports\n"
"(import_statement) @node.dependency\n";

static const char* CSS_QUERY_STRING =
"; Catch CSS rules\n"
"(rule_set (selectors (class_selector) @node.name)) @node.definition\n"
"(rule_set (selectors (id_selector) @node.name)) @node.definition\n";

void walk_query(TSNode root, const std::string& source, const TSLanguage* lang, const std::string& q_str, std::vector<FunctionSpan>& spans) {
    uint32_t err_offset;
    TSQueryError err_type;
    TSQuery* query = ts_query_new(lang, q_str.c_str(), q_str.length(), &err_offset, &err_type);
    if (!query) {
        printf("TS Query error at offset %u, type %d\n", err_offset, err_type);
        return;
    }

    TSQueryCursor* cursor = ts_query_cursor_new();
    ts_query_cursor_exec(cursor, query, root);

    TSQueryMatch match;
    while (ts_query_cursor_next_match(cursor, &match)) {
        FunctionSpan span;
        span.byteStart = 0;
        span.byteEnd = 0;
        bool has_def = false;

        for (uint16_t i = 0; i < match.capture_count; ++i) {
            TSQueryCapture capture = match.captures[i];
            uint32_t name_len;
            const char* name = ts_query_capture_name_for_id(query, capture.index, &name_len);
            std::string capture_name(name, name_len);

            if (capture_name == "node.definition" || capture_name == "node.dependency") {
                span.byteStart = ts_node_start_byte(capture.node);
                span.byteEnd = ts_node_end_byte(capture.node);
                extractCalls(capture.node, source, span.outgoingCalls);
                collectTokens(capture.node, span.tokens);
                has_def = true;
            } else if (capture_name == "node.name") {
                span.name = extractText(capture.node, source);
            }
        }
        
        if (has_def) {
            spans.push_back(std::move(span));
        }
    }

    ts_query_cursor_delete(cursor);
    ts_query_delete(query);
}
}
#endif

void AstIndexer::indexFile(const std::string& relativePath, const std::string& commitHash, int64_t timestamp) {
    std::string fullPath = (fs::path(repoRoot_) / relativePath).string();
    if (!fs::exists(fullPath)) { removeFile(relativePath); return; }

    std::ifstream in(fullPath, std::ios::binary);
    std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    indexBuffer(source, relativePath, commitHash, timestamp);
}

std::vector<std::string> AstIndexer::indexBuffer(const std::string& source, const std::string& relativePath, const std::string& commitHash, int64_t timestamp) {
    std::vector<std::string> processedNodes;
    ++stats_.filesProcessed;
    if (source.empty()) { removeFile(relativePath); return processedNodes; }

#if CHRONOS_HAVE_TREE_SITTER_CPP || CHRONOS_HAVE_TREE_SITTER_PYTHON || CHRONOS_HAVE_TREE_SITTER_JAVASCRIPT || CHRONOS_HAVE_TREE_SITTER_CSS
    TSParser* parser = nullptr;
    const TSLanguage* lang = nullptr;
    auto ext = fs::path(relativePath).extension().string();
    if (ext == ".py") {
#if CHRONOS_HAVE_TREE_SITTER_PYTHON
        lang = tree_sitter_python();
#endif
    } else if (ext == ".js" || ext == ".jsx" || ext == ".mjs") {
#if CHRONOS_HAVE_TREE_SITTER_JAVASCRIPT
        lang = tree_sitter_javascript();
#endif
    } else if (ext == ".css") {
#if CHRONOS_HAVE_TREE_SITTER_CSS
        lang = tree_sitter_css();
#endif
    } else {
#if CHRONOS_HAVE_TREE_SITTER_CPP
        lang = tree_sitter_cpp();
#endif
    }

    if (lang) {
        parser = ts_parser_new();
        ts_parser_set_language(parser, lang);
    }


    if (parser) {
        TSTree* tree = ts_parser_parse_string(parser, nullptr, source.c_str(),
                                               static_cast<uint32_t>(source.size()));
        if (!tree) {
            // Malformed input that tree-sitter's incremental parser couldn't
            // even produce a (possibly-error-node-laden) tree for at all.
            // Spec §7 Error policy: "log and continue" -- record the
            // failure and degrade to a whole-file low-confidence node
            // rather than losing the file from the index entirely.
            ++stats_.parseFailures;
            ts_parser_delete(parser);
            goto degrade;
        }
        {
            TSNode root = ts_tree_root_node(tree);
            
            codex_.clearFileImports(relativePath);
            std::vector<std::pair<std::string, std::string>> imports;
            extractImports(root, source, imports);
            for (const auto& imp : imports) {
                codex_.insertFileImport(relativePath, imp.first, imp.second);
            }
            
            bool hasSyntaxError = ts_node_has_error(root);
            std::vector<FunctionSpan> spans;
            
            if (ext == ".js" || ext == ".jsx" || ext == ".mjs") {
                walk_query(root, source, lang, JS_QUERY_STRING, spans);
            } else if (ext == ".css") {
                walk_query(root, source, lang, CSS_QUERY_STRING, spans);
            } else {
                walk(root, source, spans);
            }

            if (spans.empty()) {
                // tree-sitter parsed *something* but found no
                // function_definition nodes at all (e.g. a header full of
                // macros/templates it couldn't recognize as such, or a
                // genuinely malformed file where the whole body collapsed
                // into ERROR nodes). Per Structural Uncertainty (Spec
                // Glossary), fall back to a whole-file node instead of
                // silently indexing nothing for this file.
                ts_tree_delete(tree);
                ts_parser_delete(parser);
                if (hasSyntaxError) ++stats_.parseFailures;
                goto degrade;
            }

            for (auto& span : spans) {
                uint64_t hash = Simhash::compute(span.tokens);
                auto existing = codex_.findBySimhash(hash, relativePath);
                std::string nodeId;

                if (existing) {
                    nodeId = existing->id;
                } else {
                    nodeId = makeUuid();

                    auto renamedFrom = codex_.findBySimhashGlobal(hash);
                    if (renamedFrom && renamedFrom->id != nodeId) {
                        codex_.recordAlias(renamedFrom->id, nodeId, commitHash);
                    }

                    std::string snippet = source.substr(span.byteStart, span.byteEnd - span.byteStart);
                    
                    MemoryTier tier = getMemoryTier(timestamp);
                    if (tier != MemoryTier::Cold) {
                        vectors_.upsert({nodeId, embedText(snippet), timestamp, tier});
                    }
                    ++stats_.nodesUpserted;
                }

                Node n;
                n.id = nodeId;
                n.file_path = relativePath;
                n.byte_start = span.byteStart;
                n.byte_end = span.byteEnd;
                n.simhash = hash;
                n.is_active = true;
                // A function that individually parsed clean still inherits
                // the file's overall syntax-error state as a soft signal --
                // e.g. a malformed sibling function elsewhere in the file
                // can indicate the grammar mis-recovered around this one
                // too, even though this span itself looked well-formed.
                n.parse_confidence = hasSyntaxError ? 0.5f : 1.0f;
                codex_.upsertNode(n);

                // Outgoing CALLS edges + stub target nodes are recorded
                // exactly once per newly-discovered node (fixed: this used
                // to run twice back-to-back, double-writing every edge).
                if (!existing) {
                    for (const auto& call : span.outgoingCalls) {
                        std::string targetSym = "sym:" + call.target;
                        // Ensure a stub exists
                        if (!codex_.getNode(targetSym)) {
                            Node stub;
                            stub.id = targetSym;
                            stub.file_path = relativePath;
                            stub.byte_start = 0;
                            stub.byte_end = 1;
                            stub.is_active = false;
                            stub.parse_confidence = 0.0f;
                            codex_.upsertNode(stub);
                        }
                        std::string edgeType = call.is_noise ? "external_symbol" : "CALLS";
                        codex_.upsertEdge({nodeId, targetSym, edgeType, 1.0f, call.start_line, call.call_site_text});
                    }
                }

                if (!span.name.empty()) {
                    std::string sym = "sym:" + span.name;
                    if (!codex_.getNode(sym)) {
                        Node stub;
                        stub.id = sym;
                        stub.file_path = relativePath;
                        stub.byte_start = 0;
                        stub.byte_end = 1;
                        stub.is_active = false;
                        stub.parse_confidence = 0.0f;
                        codex_.upsertNode(stub);
                    }
                    codex_.upsertEdge({nodeId, sym, "IMPLEMENTS", 1.0f});
                    codex_.recordAlias(sym, nodeId, commitHash);
                }

                processedNodes.push_back(nodeId);
            }

            ts_tree_delete(tree);
            ts_parser_delete(parser);
            return processedNodes;
        }
    }
#endif

degrade:
    // Graceful degrade (no tree-sitter grammar linked, or the file couldn't
    // be resolved into any function span above): whole-file node with
    // parse_confidence = 0.0, per Structural Uncertainty (Spec Glossary).
    {
        std::vector<StructuralToken> wholeFileTokens{{relativePath, 1}};
        uint64_t hash = Simhash::compute(wholeFileTokens);
        auto existing = codex_.findBySimhash(hash, relativePath);
        if (existing) {
            ++stats_.nodesSkippedIdempotent;
            processedNodes.push_back(existing->id);
            return processedNodes;
        }

        Node n;
        n.id = makeUuid();
        n.file_path = relativePath;
        n.byte_start = 0;
        n.byte_end = static_cast<int64_t>(source.size());
        n.simhash = hash;
        n.is_active = true;
        n.parse_confidence = 0.0f;
        codex_.upsertNode(n);
        ++stats_.nodesUpserted;
        
        MemoryTier tier = getMemoryTier(timestamp);
        if (tier != MemoryTier::Cold) {
            vectors_.upsert({n.id, embedText(source), timestamp, tier});
        }

        processedNodes.push_back(n.id);
        return processedNodes;
    }
}

} // namespace chronos
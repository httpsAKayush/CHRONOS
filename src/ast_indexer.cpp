#include "chronos/ast_indexer.hpp"
#include "chronos/simhash.hpp"
#include <fstream>
#include <sstream>
#include <random>
#include <filesystem>

#if CHRONOS_HAVE_TREE_SITTER_CPP || CHRONOS_HAVE_TREE_SITTER_PYTHON
extern "C" {
  #include <tree_sitter/api.h>
#if CHRONOS_HAVE_TREE_SITTER_CPP
  const TSLanguage* tree_sitter_cpp(void);
#endif
#if CHRONOS_HAVE_TREE_SITTER_PYTHON
  const TSLanguage* tree_sitter_python(void);
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
struct FunctionSpan {
    uint32_t byteStart, byteEnd;
    std::string name;
    std::vector<std::string> outgoingCalls;
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
    if (t == "identifier" || t == "field_identifier" || t == "name") return extractText(node, source);
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        std::string res = findFirstIdentifier(ts_node_child(node, i), source);
        if (!res.empty()) return res;
    }
    return "";
}

void extractCalls(TSNode node, const std::string& source, std::vector<std::string>& out) {
    const char* type = ts_node_type(node);
    std::string t(type);
    if (t == "call_expression" || t == "call") {
        if (ts_node_child_count(node) > 0) {
            std::string target = findFirstIdentifier(ts_node_child(node, 0), source);
            if (!target.empty()) out.push_back(target);
        }
    }
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        extractCalls(ts_node_child(node, i), source, out);
    }
}

void collectTokens(TSNode node, std::vector<StructuralToken>& out) {
    const char* type = ts_node_type(node);
    std::string t(type);
    // Skip pure identifier/type leaves; keep control structure & operators.
    if (t != "identifier" && t != "type_identifier" && t != "field_identifier") {
        out.push_back({t, 1});
    }
    uint32_t n = ts_node_child_count(node);
    for (uint32_t i = 0; i < n; ++i) collectTokens(ts_node_child(node, i), out);
}

void walk(TSNode node, const std::string& source, std::vector<FunctionSpan>& spans) {
    std::string type(ts_node_type(node));
    if (type == "function_definition") {
        FunctionSpan span;
        span.byteStart = ts_node_start_byte(node);
        span.byteEnd = ts_node_end_byte(node);
        span.name = findFirstIdentifier(node, source);
        extractCalls(node, source, span.outgoingCalls);
        collectTokens(node, span.tokens);
        spans.push_back(std::move(span));
        return; // don't descend into nested lambdas as separate top-level nodes for v1
    }
    uint32_t n = ts_node_child_count(node);
    for (uint32_t i = 0; i < n; ++i) walk(ts_node_child(node, i), source, spans);
}
}
#endif

void AstIndexer::indexFile(const std::string& relativePath, const std::string& commitHash) {
    std::string fullPath = (fs::path(repoRoot_) / relativePath).string();
    if (!fs::exists(fullPath)) { removeFile(relativePath); return; }

    std::ifstream in(fullPath, std::ios::binary);
    std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    indexBuffer(source, relativePath, commitHash);
}

void AstIndexer::indexBuffer(const std::string& source, const std::string& relativePath, const std::string& commitHash) {
    ++stats_.filesProcessed;
    if (source.empty()) { removeFile(relativePath); return; }

#if CHRONOS_HAVE_TREE_SITTER_CPP || CHRONOS_HAVE_TREE_SITTER_PYTHON
    TSParser* parser = nullptr;
    auto ext = fs::path(relativePath).extension().string();
    if (ext == ".py") {
#if CHRONOS_HAVE_TREE_SITTER_PYTHON
        parser = ts_parser_new();
        ts_parser_set_language(parser, tree_sitter_python());
#endif
    } else {
#if CHRONOS_HAVE_TREE_SITTER_CPP
        parser = ts_parser_new();
        ts_parser_set_language(parser, tree_sitter_cpp());
#endif
    }
    
    if (parser) {
        TSTree* tree = ts_parser_parse_string(parser, nullptr, source.c_str(),
                                               static_cast<uint32_t>(source.size()));
        if (!tree) {
            ++stats_.parseFailures;
            ts_parser_delete(parser);
            return;
        }
        TSNode root = ts_tree_root_node(tree);
        std::vector<FunctionSpan> spans;
        walk(root, source, spans);

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
                vectors_.upsert({nodeId, embedText(snippet)});
                ++stats_.nodesUpserted;
            }

            Node n;
            n.id = nodeId;
            n.file_path = relativePath;
            n.byte_start = span.byteStart;
            n.byte_end = span.byteEnd;
            n.simhash = hash;
            n.is_active = true;
            n.parse_confidence = 1.0f;
            codex_.upsertNode(n);

            if (!existing) {
                // Record outgoing edges only for new nodes
                for (const auto& callTarget : span.outgoingCalls) {
                    if (callTarget.empty()) continue;
                    std::string targetSym = "sym:" + callTarget;
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
                    codex_.upsertEdge({nodeId, targetSym, "CALLS", 1.0f});
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
                codex_.recordAlias(sym, nodeId, "definition");
            }

            for (const auto& callTarget : span.outgoingCalls) {
                if (callTarget.empty()) continue;
                std::string targetSym = "sym:" + callTarget;
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
                codex_.upsertEdge({nodeId, targetSym, "CALLS", 1.0f});
            }

            std::string snippet = source.substr(span.byteStart, span.byteEnd - span.byteStart);
            vectors_.upsert({nodeId, embedText(snippet)});
        }

        ts_tree_delete(tree);
        ts_parser_delete(parser);
        return;
    }
#endif

    // Graceful degrade
    std::vector<StructuralToken> wholeFileTokens{{relativePath, 1}};
    uint64_t hash = Simhash::compute(wholeFileTokens);
    auto existing = codex_.findBySimhash(hash, relativePath);
    if (existing) { ++stats_.nodesSkippedIdempotent; return; }

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
    vectors_.upsert({n.id, embedText(source)});
}

} // namespace chronos

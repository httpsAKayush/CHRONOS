// chronos-indexer: the async worker referenced in Spec §6 "The Indexer
// (Async)". Invoked detached by scripts/pre-commit (fail-open, never on the
// blocking commit path) with a list of changed files + the new commit hash.
//
// Usage:
//   chronos-indexer <repoRoot> <commitHash> <changedFile1> [changedFile2 ...]
//   chronos-indexer <repoRoot> <commitHash> --deleted <deletedFile1> ...
//
// Exit code is always 0 on recoverable errors (parse failures, missing
// files) — per Spec §7 Error policy "log and continue" — so a cron/launchd
// drain job never gets stuck retrying a poison file forever. Only truly
// fatal errors (can't open the Codex DB at all) exit non-zero.

#include <iostream>
#include <fstream>
#include <filesystem>
#include <vector>
#include <string>
#include "chronos/codex.hpp"
#include "chronos/vector_index.hpp"
#include "chronos/ast_indexer.hpp"

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: chronos-indexer <repoRoot> <commitHash> [--deleted] <files...>\n";
        return 2;
    }
    std::string repoRoot = argv[1];
    std::string commitHash = argv[2];

    std::vector<std::string> changed, deleted;
    bool inDeletedSection = false;
    for (int i = 3; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--deleted") { inDeletedSection = true; continue; }
        (inDeletedSection ? deleted : changed).push_back(arg);
    }

    fs::path logPath = fs::path(repoRoot) / ".chronos" / "errors.log";
    fs::create_directories(logPath.parent_path());
    std::ofstream errLog(logPath, std::ios::app);

    try {
        chronos::Codex codex(repoRoot);
        chronos::VectorIndex vectors(repoRoot);
        chronos::AstIndexer indexer(codex, vectors, repoRoot);

        for (auto& f : deleted) {
            try {
                indexer.removeFile(f);
            } catch (const std::exception& e) {
                errLog << "[removeFile] " << f << ": " << e.what() << "\n";
            }
        }
        for (auto& f : changed) {
            try {
                indexer.indexFile(f, commitHash);
            } catch (const std::exception& e) {
                errLog << "[indexFile] " << f << ": " << e.what() << "\n";
            }
        }

        const auto& s = indexer.stats();
        std::cout << "chronos-indexer: files=" << s.filesProcessed
                  << " upserted=" << s.nodesUpserted
                  << " tombstoned=" << s.nodesTombstoned
                  << " idempotent-skips=" << s.nodesSkippedIdempotent
                  << " parse-failures=" << s.parseFailures << "\n";
    } catch (const std::exception& e) {
        errLog << "[fatal] " << e.what() << "\n";
        std::cerr << "chronos-indexer: fatal error, see .chronos/errors.log\n";
        return 1;
    }
    return 0;
}

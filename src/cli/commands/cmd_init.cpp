#include "chronos/cli/commands.hpp"
#include "chronos/cli/cli_util.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

namespace chronos {

namespace {

// Embedded copy of scripts/pre-commit — written directly to .git/hooks/
// on every `chronos init`, eliminating the need for the script file to
// exist next to the binary or in the repo. Kept in sync with scripts/pre-commit.
const char* kPreCommitHook = R"HOOK(#!/usr/bin/env bash
# Chronos pre-commit hook — Spec §2: must execute in <500ms.
# Fail-open / async: collects changed files, spawns chronos-indexer
# detached, never blocks the commit.

set -uo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" || exit 0
COMMIT_HASH="$(git rev-parse HEAD 2>/dev/null || echo "PENDING")"

CHANGED=$(git diff --cached --name-only --diff-filter=ACMR -- '*.cpp' '*.h' '*.hpp' '*.cc' '*.py' 2>/dev/null)
DELETED=$(git diff --cached --name-only --diff-filter=D -- '*.cpp' '*.h' '*.hpp' '*.cc' '*.py' 2>/dev/null)

if [ -z "$CHANGED" ] && [ -z "$DELETED" ]; then
  exit 0
fi

if command -v chronos >/dev/null 2>&1; then
  chronos check-staging "$REPO_ROOT" || true
elif [ -x "$REPO_ROOT/build/chronos" ]; then
  "$REPO_ROOT/build/chronos" check-staging "$REPO_ROOT" || true
fi

if ! command -v chronos-indexer >/dev/null 2>&1; then
  exit 0
fi

ARGS=("$REPO_ROOT" "$COMMIT_HASH")
if [ -n "$CHANGED" ]; then
  while IFS= read -r f; do ARGS+=("$f"); done <<< "$CHANGED"
fi
if [ -n "$DELETED" ]; then
  ARGS+=("--deleted")
  while IFS= read -r f; do ARGS+=("$f"); done <<< "$DELETED"
fi

nohup setsid chronos-indexer "${ARGS[@]}" >/dev/null 2>>"$REPO_ROOT/.chronos/errors.log" &
disown

exit 0
)HOOK";

} // namespace

CmdInit::CmdInit(const CliContext& ctx) : ctx_(ctx) {}

int CmdInit::execute(int argc, char** argv) {
    (void)argc; (void)argv;
    const std::string& repoRoot = ctx_.repoRoot;

    fs::create_directories(fs::path(repoRoot) / ".chronos" / "logs");

    fs::path gitignore = fs::path(repoRoot) / ".gitignore";
    std::string existing;
    if (fs::exists(gitignore)) {
        std::ifstream in(gitignore);
        existing.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    if (existing.find(".chronos/") == std::string::npos) {
        std::ofstream out(gitignore, std::ios::app);
        out << "\n# Added by `chronos init` -- contains proprietary source structure\n.chronos/\n";
    }

    fs::path hooksDir = fs::path(repoRoot) / ".git" / "hooks";
    if (fs::exists(hooksDir)) {
        fs::path hookDest = hooksDir / "pre-commit";
        std::ofstream out(hookDest);
        out << kPreCommitHook;
        out.close();
        fs::permissions(hookDest, fs::perms::owner_all | fs::perms::group_read | fs::perms::others_read);
    }

    if (ctx_.storage) {
        (void)ctx_.storage;
    } else {
        Codex codex(repoRoot);
        VectorIndex vectors(repoRoot);
        (void)vectors;
    }
    std::cout << "Chronos initialized at " << repoRoot << "/.chronos\n";
    return 0;
}

} // namespace chronos

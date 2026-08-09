#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CHRONOS_BIN="$REPO_ROOT/build/chronos"

# Ensure binary is built
if [ ! -f "$CHRONOS_BIN" ]; then
    echo "Building chronos CLI binary..."
    cmake -B "$REPO_ROOT/build" -S "$REPO_ROOT"
    cmake --build "$REPO_ROOT/build" --target chronos
fi

SNAPSHOT_DIR="$SCRIPT_DIR/snapshots"
mkdir -p "$SNAPSHOT_DIR"

STATUS_SNAPSHOT="$SNAPSHOT_DIR/status_snapshot.txt"
SYNC_SNAPSHOT="$SNAPSHOT_DIR/sync_snapshot.txt"

DUMMY_DIR="/tmp/chronos_e2e_parity_dummy"
rm -rf "$DUMMY_DIR"
mkdir -p "$DUMMY_DIR"

(
    cd "$DUMMY_DIR"
    git init -b main >/dev/null
    git config user.name "Test User"
    git config user.email "test@example.com"

    export GIT_AUTHOR_NAME="Test User"
    export GIT_AUTHOR_EMAIL="test@example.com"
    export GIT_COMMITTER_NAME="Test User"
    export GIT_COMMITTER_EMAIL="test@example.com"
    export GIT_AUTHOR_DATE="2026-01-01T00:00:00Z"
    export GIT_COMMITTER_DATE="2026-01-01T00:00:00Z"

    cat << 'EOF' > main.cpp
#include <iostream>

int compute_sum(int a, int b) {
    return a + b;
}

int main() {
    std::cout << compute_sum(10, 20) << std::endl;
    return 0;
}
EOF

    cat << 'EOF' > utils.hpp
#pragma once
#include <string>

inline std::string get_version() {
    return "1.0.0";
}
EOF

    git add main.cpp utils.hpp
    git commit -m "Initial commit of main and utils" >/dev/null

    export GIT_AUTHOR_DATE="2026-01-02T00:00:00Z"
    export GIT_COMMITTER_DATE="2026-01-02T00:00:00Z"

    cat << 'EOF' >> main.cpp

void helper() {
    std::cout << "helper" << std::endl;
}
EOF
    git add main.cpp
    git commit -m "Add helper function" >/dev/null
)

TMP_SYNC_OUT="/tmp/chronos_sync_current.txt"
TMP_STATUS_OUT="/tmp/chronos_status_current.txt"

(
    cd "$DUMMY_DIR"
    "$CHRONOS_BIN" sync > "$TMP_SYNC_OUT"
    "$CHRONOS_BIN" status > "$TMP_STATUS_OUT"
)

if [ "$1" == "--generate" ] || [ ! -f "$STATUS_SNAPSHOT" ] || [ ! -f "$SYNC_SNAPSHOT" ]; then
    echo "Generating baseline snapshots..."
    cp "$TMP_STATUS_OUT" "$STATUS_SNAPSHOT"
    cp "$TMP_SYNC_OUT" "$SYNC_SNAPSHOT"
    echo "Baseline snapshots saved to $SNAPSHOT_DIR"
    exit 0
fi

echo "Verifying CLI output against baseline snapshots..."
DEVIATION=0

if ! diff -u "$STATUS_SNAPSHOT" "$TMP_STATUS_OUT"; then
    echo "ERROR: status output deviates from baseline!"
    DEVIATION=1
fi

if ! diff -u "$SYNC_SNAPSHOT" "$TMP_SYNC_OUT"; then
    echo "ERROR: sync output deviates from baseline!"
    DEVIATION=1
fi

if [ $DEVIATION -eq 0 ]; then
    echo "Snapshot parity check PASSED."
    exit 0
else
    echo "Snapshot parity check FAILED."
    exit 1
fi

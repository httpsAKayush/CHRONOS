#!/usr/bin/env python3
import os
import sys
import shutil
import sqlite3
import subprocess
import time
import tempfile

CHRONOS_BIN = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../build/chronos"))
PRE_COMMIT_SCRIPT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../scripts/pre-commit"))

def run_cmd(cmd, cwd=None, env=None):
    res = subprocess.run(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    return res.returncode, res.stdout, res.stderr

def init_git_repo(repo_dir):
    os.makedirs(repo_dir, exist_ok=True)
    run_cmd(["git", "init"], cwd=repo_dir)
    run_cmd(["git", "config", "user.name", "Test User"], cwd=repo_dir)
    run_cmd(["git", "config", "user.email", "test@example.com"], cwd=repo_dir)

def setup_chronos_db(repo_dir):
    chronos_dir = os.path.join(repo_dir, ".chronos")
    os.makedirs(chronos_dir, exist_ok=True)
    db_path = os.path.join(chronos_dir, "codex.db")
    
    conn = sqlite3.connect(db_path)
    cur = conn.cursor()
    cur.execute("PRAGMA user_version = 1;")
    cur.execute("""
        CREATE TABLE IF NOT EXISTS nodes (
            id TEXT PRIMARY KEY,
            file_path TEXT NOT NULL,
            byte_start INTEGER NOT NULL,
            byte_end INTEGER NOT NULL,
            simhash INTEGER NOT NULL,
            is_active INTEGER NOT NULL,
            parse_confidence REAL NOT NULL
        );
    """)
    cur.execute("""
        CREATE TABLE IF NOT EXISTS history (
            node_id TEXT NOT NULL,
            commit_hash TEXT NOT NULL,
            timestamp INTEGER NOT NULL,
            synthetic_msg TEXT,
            PRIMARY KEY(node_id, commit_hash)
        );
    """)
    cur.execute("CREATE INDEX IF NOT EXISTS idx_nodes_file ON nodes(file_path);")
    conn.commit()
    return conn, db_path

def add_node_and_history(conn, node_id, file_path, is_active, commit_hash, synthetic_msg, timestamp=1000):
    cur = conn.cursor()
    cur.execute("""
        INSERT OR REPLACE INTO nodes (id, file_path, byte_start, byte_end, simhash, is_active, parse_confidence)
        VALUES (?, ?, 0, 100, 12345, ?, 1.0);
    """, (node_id, file_path, 1 if is_active else 0))
    
    cur.execute("""
        INSERT OR REPLACE INTO history (node_id, commit_hash, timestamp, synthetic_msg)
        VALUES (?, ?, ?, ?);
    """, (node_id, commit_hash, timestamp, synthetic_msg))
    conn.commit()

class StressTester:
    def __init__(self):
        self.results = []
        self.passed = 0
        self.failed = 0

    def log(self, test_name, success, detail=""):
        status = "PASS" if success else "FAIL"
        if success:
            self.passed += 1
        else:
            self.failed += 1
        self.results.append((test_name, status, detail))
        print(f"[{status}] {test_name}" + (f" - {detail}" if detail else ""))

    def test_empty_staging_diff(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            init_git_repo(tmpdir)
            conn, _ = setup_chronos_db(tmpdir)
            add_node_and_history(conn, "n1", "src/test.cpp", True, "c1", "DO NOT MUTEX HERE")
            conn.close()

            rc_non_strict, out1, err1 = run_cmd([CHRONOS_BIN, "check-staging", tmpdir])
            rc_strict, out2, err2 = run_cmd([CHRONOS_BIN, "check-staging", tmpdir, "--strict"])

            ok = (rc_non_strict == 0) and (rc_strict == 0) and ("TEMPORAL COLLISION" not in out1) and ("TEMPORAL COLLISION" not in out2)
            self.log("Edge Case 1: Empty Staging Diff", ok, f"non-strict rc={rc_non_strict}, strict rc={rc_strict}")

    def test_missing_db(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            init_git_repo(tmpdir)
            file_path = os.path.join(tmpdir, "src", "foo.cpp")
            os.makedirs(os.path.dirname(file_path), exist_ok=True)
            with open(file_path, "w") as f:
                f.write("int main() { return 0; }\n")
            run_cmd(["git", "add", "src/foo.cpp"], cwd=tmpdir)

            rc_non_strict, out1, _ = run_cmd([CHRONOS_BIN, "check-staging", tmpdir])
            rc_strict, out2, _ = run_cmd([CHRONOS_BIN, "check-staging", tmpdir, "--strict"])

            ok = (rc_non_strict == 0) and (rc_strict == 0) and (out1 == "") and (out2 == "")
            self.log("Edge Case 2: Missing Codex DB (Fail-Open)", ok, f"non-strict rc={rc_non_strict}, strict rc={rc_strict}")

    def test_tombstoned_nodes(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            init_git_repo(tmpdir)
            conn, _ = setup_chronos_db(tmpdir)
            # Inactive / tombstoned node
            add_node_and_history(conn, "n1", "src/worker.cpp", False, "c1", "DO NOT MUTEX HERE - deadlock risk")
            conn.close()

            file_path = os.path.join(tmpdir, "src", "worker.cpp")
            os.makedirs(os.path.dirname(file_path), exist_ok=True)
            with open(file_path, "w") as f:
                f.write("std::mutex mtx;\n")
            run_cmd(["git", "add", "src/worker.cpp"], cwd=tmpdir)

            rc_strict, out, _ = run_cmd([CHRONOS_BIN, "check-staging", tmpdir, "--strict"])

            ok = (rc_strict == 0) and ("TEMPORAL COLLISION" not in out)
            self.log("Edge Case 3: Tombstoned/Inactive Nodes Ignored", ok, f"rc={rc_strict}, collision_found={'TEMPORAL COLLISION' in out}")

    def test_strict_vs_non_strict_return_codes(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            init_git_repo(tmpdir)
            conn, _ = setup_chronos_db(tmpdir)
            add_node_and_history(conn, "n1", "src/worker.cpp", True, "c100", "DO NOT MUTEX HERE - deadlock risk")
            conn.close()

            file_path = os.path.join(tmpdir, "src", "worker.cpp")
            os.makedirs(os.path.dirname(file_path), exist_ok=True)
            with open(file_path, "w") as f:
                f.write("std::lock_guard<std::mutex> lock(mtx);\n")
            run_cmd(["git", "add", "src/worker.cpp"], cwd=tmpdir)

            rc_non_strict, out_ns, _ = run_cmd([CHRONOS_BIN, "check-staging", tmpdir])
            rc_strict, out_s, _ = run_cmd([CHRONOS_BIN, "check-staging", tmpdir, "--strict"])

            ok = (rc_non_strict == 0) and (rc_strict == 1) and ("TEMPORAL COLLISION" in out_ns) and ("TEMPORAL COLLISION" in out_s)
            self.log("Edge Case 4: Non-Strict (rc=0) vs --strict (rc=1) Return Codes", ok,
                     f"non-strict rc={rc_non_strict}, strict rc={rc_strict}")

    def test_complex_multi_file_diff(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            init_git_repo(tmpdir)
            conn, _ = setup_chronos_db(tmpdir)

            # File 1: Collision expected
            add_node_and_history(conn, "n1", "src/a.cpp", True, "commitA", "DO NOT ALTER TIMEOUT setting")
            # File 2: No collision expected (no matching keywords)
            add_node_and_history(conn, "n2", "src/b.cpp", True, "commitB", "DO NOT ALTER TIMEOUT setting")
            # File 3: Collision expected (keyword "mutex")
            add_node_and_history(conn, "n3", "src/c.cpp", True, "commitC", "thread deadlock risk when locking worker")
            # File 4: No history
            # File 5: Tombstoned node -> no collision
            add_node_and_history(conn, "n5", "src/e.cpp", False, "commitE", "DO NOT MUTEX HERE")
            conn.close()

            files_content = {
                "src/a.cpp": "int timeout = 5000;\n",
                "src/b.cpp": "double speed = 1.5;\n",
                "src/c.cpp": "std::unique_lock<std::mutex> lk(m);\n",
                "src/d.cpp": "int val = 42;\n",
                "src/e.cpp": "std::mutex mtx;\n",
            }

            for rel_path, content in files_content.items():
                abs_p = os.path.join(tmpdir, rel_path)
                os.makedirs(os.path.dirname(abs_p), exist_ok=True)
                with open(abs_p, "w") as f:
                    f.write(content)
                run_cmd(["git", "add", rel_path], cwd=tmpdir)

            rc_strict, out, _ = run_cmd([CHRONOS_BIN, "check-staging", tmpdir, "--strict"])

            has_a = "File: src/a.cpp" in out
            has_b = "File: src/b.cpp" in out
            has_c = "File: src/c.cpp" in out
            has_d = "File: src/d.cpp" in out
            has_e = "File: src/e.cpp" in out

            ok = (rc_strict == 1) and has_a and (not has_b) and has_c and (not has_d) and (not has_e)
            self.log("Edge Case 5: Complex Multi-File Staged Diffs", ok,
                     f"rc={rc_strict}, reported: a={has_a}, b={has_b}, c={has_c}, d={has_d}, e={has_e}")

    def test_deleted_and_new_files(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            init_git_repo(tmpdir)
            conn, _ = setup_chronos_db(tmpdir)
            add_node_and_history(conn, "n1", "src/deleted.cpp", True, "c1", "DO NOT REMOVE LOGGING")
            add_node_and_history(conn, "n2", "src/new.cpp", True, "c2", "DO NOT MUTEX HERE")
            conn.close()

            # Create existing file and commit it, then delete it
            del_path = os.path.join(tmpdir, "src", "deleted.cpp")
            os.makedirs(os.path.dirname(del_path), exist_ok=True)
            with open(del_path, "w") as f:
                f.write("int old = 1;\n")
            run_cmd(["git", "add", "src/deleted.cpp"], cwd=tmpdir)
            run_cmd(["git", "commit", "-m", "initial"], cwd=tmpdir)
            run_cmd(["git", "rm", "src/deleted.cpp"], cwd=tmpdir)

            # Create new file and stage it
            new_path = os.path.join(tmpdir, "src", "new.cpp")
            os.makedirs(os.path.dirname(new_path), exist_ok=True)
            with open(new_path, "w") as f:
                f.write("std::mutex mtx;\n")
            run_cmd(["git", "add", "src/new.cpp"], cwd=tmpdir)

            rc, out, err = run_cmd([CHRONOS_BIN, "check-staging", tmpdir, "--strict"])

            ok = (rc == 1) and ("File: src/new.cpp" in out) and ("File: src/deleted.cpp" not in out)
            self.log("Edge Case 6: Deleted Files and New Files Handling", ok,
                     f"rc={rc}, new_file_warn={'File: src/new.cpp' in out}, del_file_warn={'File: src/deleted.cpp' in out}")

    def test_performance_latency_constraint(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            init_git_repo(tmpdir)
            conn, _ = setup_chronos_db(tmpdir)

            # Insert 5000 nodes and 10000 history records across 500 files
            num_files = 500
            nodes_per_file = 10
            conn.execute("BEGIN TRANSACTION;")
            for f_idx in range(num_files):
                file_path = f"src/module_{f_idx}/code.cpp"
                for n_idx in range(nodes_per_file):
                    node_id = f"node_{f_idx}_{n_idx}"
                    conn.execute("""
                        INSERT INTO nodes (id, file_path, byte_start, byte_end, simhash, is_active, parse_confidence)
                        VALUES (?, ?, ?, ?, ?, 1, 1.0);
                    """, (node_id, file_path, n_idx * 100, (n_idx + 1) * 100, f_idx * 1000 + n_idx))

                    commit_hash = f"commit_{f_idx}_{n_idx}"
                    msg = f"Critical safety constraint for module {f_idx}: DO NOT MUTEX HERE or deadlock occurs"
                    conn.execute("""
                        INSERT INTO history (node_id, commit_hash, timestamp, synthetic_msg)
                        VALUES (?, ?, ?, ?);
                    """, (node_id, commit_hash, 1600000000 + f_idx, msg))
            conn.execute("COMMIT;")
            conn.close()

            # Create staged changes in 50 files
            for f_idx in range(50):
                file_path = os.path.join(tmpdir, f"src/module_{f_idx}/code.cpp")
                os.makedirs(os.path.dirname(file_path), exist_ok=True)
                with open(file_path, "w") as f:
                    f.write(f"// Modified module {f_idx}\nstd::lock_guard<std::mutex> lk(mtx);\n")
                run_cmd(["git", "add", f"src/module_{f_idx}/code.cpp"], cwd=tmpdir)

            # Run benchmark: 20 iterations
            durations = []
            for _ in range(20):
                start = time.perf_counter()
                rc, out, _ = run_cmd([CHRONOS_BIN, "check-staging", tmpdir, "--strict"])
                end = time.perf_counter()
                durations.append((end - start) * 1000.0) # ms

            avg_ms = sum(durations) / len(durations)
            max_ms = max(durations)
            min_ms = min(durations)
            sorted_d = sorted(durations)
            p95_ms = sorted_d[int(0.95 * len(sorted_d))]

            under_500ms = max_ms < 500.0
            self.log("Performance: Latency < 500ms Constraint", under_500ms,
                     f"Avg={avg_ms:.2f}ms, Max={max_ms:.2f}ms, Min={min_ms:.2f}ms, P95={p95_ms:.2f}ms (Target: <500ms)")

    def test_pre_commit_hook_script(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            init_git_repo(tmpdir)
            conn, _ = setup_chronos_db(tmpdir)
            add_node_and_history(conn, "n1", "src/main.cpp", True, "c1", "DO NOT MUTEX HERE")
            conn.close()

            file_path = os.path.join(tmpdir, "src", "main.cpp")
            os.makedirs(os.path.dirname(file_path), exist_ok=True)
            with open(file_path, "w") as f:
                f.write("std::mutex mtx;\n")
            run_cmd(["git", "add", "src/main.cpp"], cwd=tmpdir)

            # Install hook
            hooks_dir = os.path.join(tmpdir, ".git", "hooks")
            os.makedirs(hooks_dir, exist_ok=True)
            hook_dest = os.path.join(hooks_dir, "pre-commit")
            shutil.copyfile(PRE_COMMIT_SCRIPT, hook_dest)
            os.chmod(hook_dest, 0o755)

            # Add chronos binary location to PATH for test
            env = os.environ.copy()
            env["PATH"] = os.path.dirname(CHRONOS_BIN) + ":" + env.get("PATH", "")

            rc, out, err = run_cmd([hook_dest], cwd=tmpdir, env=env)

            # Pre-commit hook must always exit 0 (fail-open)
            ok = (rc == 0) and ("TEMPORAL COLLISION WARNING" in out or "TEMPORAL COLLISION WARNING" in err)
            self.log("Pre-commit Hook Integration Test", ok, f"rc={rc}, warning_outputted={'TEMPORAL COLLISION WARNING' in (out+err)}")

    def test_repo_path_with_spaces(self):
        with tempfile.TemporaryDirectory() as parent_dir:
            space_repo = os.path.join(parent_dir, "space repo path")
            init_git_repo(space_repo)
            conn, _ = setup_chronos_db(space_repo)
            add_node_and_history(conn, "n1", "src/space_file.cpp", True, "c1", "DO NOT ALTER TIMEOUT")
            conn.close()

            file_path = os.path.join(space_repo, "src", "space_file.cpp")
            os.makedirs(os.path.dirname(file_path), exist_ok=True)
            with open(file_path, "w") as f:
                f.write("int timeout = 100;\n")
            run_cmd(["git", "add", "src/space_file.cpp"], cwd=space_repo)

            rc_strict, out, err = run_cmd([CHRONOS_BIN, "check-staging", space_repo, "--strict"])

            ok = (rc_strict == 1) and ("TEMPORAL COLLISION WARNING" in out)
            self.log("Edge Case 7: Repo Path with Spaces", ok, f"rc={rc_strict}, collision_found={'TEMPORAL COLLISION WARNING' in out}")

    # --- ADVERSARIAL DISCOVERY TESTS ---

    def test_adversarial_same_line_multi_file_dedup_bug(self):
        """
        Adversarial Test: Multi-file deduplication key collision.
        If file A and file B both stage `+ std::mutex mtx;` against commit `c1`,
        `reported` key `{stagedLine, commitHash}` suppresses warning for file B!
        """
        with tempfile.TemporaryDirectory() as tmpdir:
            init_git_repo(tmpdir)
            conn, _ = setup_chronos_db(tmpdir)
            add_node_and_history(conn, "n1", "src/file_a.cpp", True, "c1", "DO NOT MUTEX HERE")
            add_node_and_history(conn, "n2", "src/file_b.cpp", True, "c1", "DO NOT MUTEX HERE")
            conn.close()

            p1 = os.path.join(tmpdir, "src", "file_a.cpp")
            p2 = os.path.join(tmpdir, "src", "file_b.cpp")
            os.makedirs(os.path.dirname(p1), exist_ok=True)
            with open(p1, "w") as f: f.write("+ std::mutex mtx;\n")
            with open(p2, "w") as f: f.write("+ std::mutex mtx;\n")
            run_cmd(["git", "add", "src/file_a.cpp", "src/file_b.cpp"], cwd=tmpdir)

            rc_strict, out, err = run_cmd([CHRONOS_BIN, "check-staging", tmpdir, "--strict"])
            
            has_file_a = "File: src/file_a.cpp" in out
            has_file_b = "File: src/file_b.cpp" in out

            # Expect warnings for BOTH file_a and file_b
            ok = has_file_a and has_file_b
            self.log("Adversarial Test 1: Cross-File Line Deduplication Bug", ok,
                     f"File A reported={has_file_a}, File B reported={has_file_b}")

    def test_adversarial_keyword_substring_false_positive(self):
        """
        Adversarial Test: Keyword Substring False Positive.
        Domain keyword 'lock' matches inside 'clock' or 'block'.
        If commit msg has 'Block user login' and staged line is 'int clock_count = 0;',
        checkStagingCollision matches 'lock' in both, triggering false positive.
        """
        with tempfile.TemporaryDirectory() as tmpdir:
            init_git_repo(tmpdir)
            conn, _ = setup_chronos_db(tmpdir)
            add_node_and_history(conn, "n1", "src/clock.cpp", True, "c1", "Block user login on bad password")
            conn.close()

            p = os.path.join(tmpdir, "src", "clock.cpp")
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "w") as f: f.write("int clock_count = 0;\n")
            run_cmd(["git", "add", "src/clock.cpp"], cwd=tmpdir)

            rc_strict, out, err = run_cmd([CHRONOS_BIN, "check-staging", tmpdir, "--strict"])

            # False positive occurs if collision is reported for 'clock_count' vs 'Block'
            false_positive = ("TEMPORAL COLLISION WARNING" in out)
            ok = not false_positive
            self.log("Adversarial Test 2: Substring Keyword Match False Positive", ok,
                     f"False positive triggered: {false_positive} (matches 'lock' in 'Block' & 'clock')")

def main():
    print("========================================================================")
    print("Chronos Milestone 3: Staging Area Check Empirical Stress Test Suite")
    print(f"Chronos binary: {CHRONOS_BIN}")
    print(f"Pre-commit script: {PRE_COMMIT_SCRIPT}")
    print("========================================================================")

    tester = StressTester()
    tester.test_empty_staging_diff()
    tester.test_missing_db()
    tester.test_tombstoned_nodes()
    tester.test_strict_vs_non_strict_return_codes()
    tester.test_complex_multi_file_diff()
    tester.test_deleted_and_new_files()
    tester.test_performance_latency_constraint()
    tester.test_pre_commit_hook_script()
    tester.test_repo_path_with_spaces()
    print("--- Adversarial Vulnerability / Edge Case Tests ---")
    tester.test_adversarial_same_line_multi_file_dedup_bug()
    tester.test_adversarial_keyword_substring_false_positive()

    print("========================================================================")
    print(f"SUMMARY: Total Tests: {tester.passed + tester.failed} | Passed: {tester.passed} | Failed: {tester.failed}")
    print("========================================================================")

    return 0 if tester.failed == 0 else 1

if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""
latv-sh Automated Acceptance Test Suite
Verifies all functional, architectural, and edge-case requirements
specified in docs/phase1-architecture.md and presentation slides.
"""

import os
import subprocess
import sys
import time

SHELL_BIN = "./latv-sh"

def run_shell(commands, input_text=None, timeout=5):
    """Run latv-sh with the given command string or stdin."""
    full_input = commands if input_text is None else commands + "\n" + input_text
    proc = subprocess.run(
        [SHELL_BIN],
        input=full_input,
        text=True,
        capture_output=True,
        timeout=timeout
    )
    return proc.returncode, proc.stdout, proc.stderr

def test_1_builtin_cd_pwd():
    print("Running Test 1: Builtins cd and pwd (parent execution)...", end=" ")
    cmd = """cd /tmp
pwd
cd -not_existing_dir_xyz
echo $?
"""
    code, out, err = run_shell(cmd)
    lines = [l.strip() for l in out.strip().splitlines() if l.strip()]
    assert len(lines) >= 2, f"Unexpected output: {out}"
    assert lines[0] == "/tmp", f"Expected /tmp, got {lines[0]}"
    assert lines[1] == "1", f"Expected error code 1, got {lines[1]}"
    print("PASS")

def test_2_multistage_pipeline():
    print("Running Test 2: Multi-stage pipeline (seq | grep | wc)...", end=" ")
    cmd = """seq 1 5 | grep 3 | wc -l
"""
    code, out, err = run_shell(cmd)
    lines = [l.strip() for l in out.strip().splitlines() if l.strip()]
    assert lines == ["1"], f"Expected ['1'], got {lines}"
    print("PASS")

def test_3_sigpipe_deadlock_defense():
    print("Running Test 3: SIGPIPE and write-end deadlock defense (yes | head -1)...", end=" ")
    start = time.time()
    cmd = """yes | head -1
echo $?
"""
    code, out, err = run_shell(cmd, timeout=3)
    duration = time.time() - start
    assert duration < 2.0, f"Took too long ({duration}s), possible deadlock!"
    lines = [l.strip() for l in out.strip().splitlines() if l.strip()]
    assert len(lines) >= 1 and lines[0] == "y", f"Expected 'y', got {lines}"
    print(f"PASS ({duration:.2f}s)")

def test_4_redirection_roundtrip():
    print("Running Test 4: Redirection round-trip (<, >)...", end=" ")
    test_file = "/tmp/latv_test_roundtrip.txt"
    if os.path.exists(test_file):
        os.remove(test_file)
    cmd = f"""echo latverian_decree > {test_file}
cat < {test_file}
"""
    code, out, err = run_shell(cmd)
    lines = [l.strip() for l in out.strip().splitlines() if l.strip()]
    assert lines == ["latverian_decree"], f"Expected ['latverian_decree'], got {lines}"
    if os.path.exists(test_file):
        os.remove(test_file)
    print("PASS")

def test_5_quotes_and_expansion():
    print("Running Test 5: Quotes and variable expansion ($?, $USER, single/double quotes)...", end=" ")
    cmd = """echo "a b" '$HOME' $USER
false
echo $?
echo ""
"""
    code, out, err = run_shell(cmd)
    lines = [l.strip() for l in out.strip().splitlines()]
    user = os.environ.get("USER", "")
    assert lines[0] == f"a b $HOME {user}".strip(), f"Got {lines[0]}"
    assert lines[1] == "1", f"Expected status 1, got {lines[1]}"
    print("PASS")

def test_6_status_codes():
    print("Running Test 6: POSIX exit codes (127 not found, custom exit code)...", end=" ")
    cmd = """no_such_binary_cmd_12345
echo $?
exit 77
"""
    code, out, err = run_shell(cmd)
    lines = [l.strip() for l in out.strip().splitlines() if l.strip()]
    assert lines == ["127"], f"Expected status 127, got {lines}"
    assert code == 77, f"Expected process exit code 77, got {code}"
    print("PASS")

def test_7_zombie_defense():
    print("Running Test 7: Zombie defense across 50 pipelines...", end=" ")
    commands = "\n".join(["echo hi | cat | wc -l" for _ in range(50)])
    code, out, err = run_shell(commands, timeout=5)
    
    # Check for any zombie/defunct processes owned by current user
    ps = subprocess.run(["ps", "-eo", "stat,comm"], capture_output=True, text=True)
    defunct_count = sum(1 for line in ps.stdout.splitlines() if "Z" in line.split()[0])
    assert defunct_count == 0, f"Found {defunct_count} zombie processes!"
    print("PASS (0 zombies)")

def test_8_syntax_error_and_edge_cases():
    print("Running Test 8: Edge-case syntax errors (unsupported ops, empty pipe stages)...", end=" ")
    # 1. Pipe stage with no command: echo a | > /tmp/out
    code, out, err = run_shell("echo a | > /tmp/out\necho $?\n")
    assert "syntax error near '|'" in err, f"Expected syntax error, got: {err}"
    assert "2" in out, f"Expected status 2 for syntax error, got: {out}"

    # 2. Stray pipes: a | | b
    code, out, err = run_shell("echo a | | wc -l\necho $?\n")
    assert "syntax error near '|'" in err, f"Expected syntax error, got: {err}"
    assert "2" in out, f"Expected status 2, got: {out}"

    # 3. Unsupported operator >>
    code, out, err = run_shell("echo a >> /tmp/out\necho $?\n")
    assert "'>>' redirection is not supported" in err
    assert "2" in out

    # 4. Unterminated quotes
    code, out, err = run_shell("echo 'hello\necho $?\n")
    assert "unterminated single quote" in err
    assert "2" in out

    # 5. Spaces only
    code, out, err = run_shell("   \n   \t  \necho ok\n")
    lines = [l.strip() for l in out.strip().splitlines() if l.strip()]
    assert lines == ["ok"], f"Expected ['ok'], got {lines}"

    print("PASS")

def test_9_sigint_ctrl_c():
    import signal
    print("Running Test 9: Live Demo 4 - Ctrl+C (SIGINT) interruption and status 130...", end=" ")
    p = subprocess.Popen(
        [SHELL_BIN],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        preexec_fn=os.setsid
    )
    p.stdin.write("sleep 10\n")
    p.stdin.flush()
    time.sleep(0.3)

    # Send SIGINT to the process group (simulating terminal Ctrl+C)
    os.killpg(os.getpgid(p.pid), signal.SIGINT)
    time.sleep(0.3)

    p.stdin.write("echo $?\nexit 0\n")
    p.stdin.flush()
    out, err = p.communicate(timeout=3)
    lines = [l.strip() for l in out.strip().splitlines() if l.strip()]
    assert "130" in lines, f"Expected 130 in output, got: {out}"
    print("PASS ($? == 130)")

if __name__ == "__main__":
    tests = [
        test_1_builtin_cd_pwd,
        test_2_multistage_pipeline,
        test_3_sigpipe_deadlock_defense,
        test_4_redirection_roundtrip,
        test_5_quotes_and_expansion,
        test_6_status_codes,
        test_7_zombie_defense,
        test_8_syntax_error_and_edge_cases,
        test_9_sigint_ctrl_c,
    ]
    
    print("=" * 60)
    print("Starting latv-sh acceptance test suite")
    print("=" * 60)
    
    passed = 0
    for t in tests:
        try:
            t()
            passed += 1
        except Exception as e:
            print(f"FAIL: {e}")
            sys.exit(1)
            
    print("=" * 60)
    print(f"All {passed}/{len(tests)} tests PASSED successfully!")
    print("=" * 60)

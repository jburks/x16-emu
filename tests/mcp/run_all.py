#!/usr/bin/env python3
"""Runs the MCP server tests. Usage: run_all.py [path/to/x16emu] [-v] [name ...]

The emulator runs windowless (-nowindow), so no windows appear and no display is needed.
Exit status is 0 only if every test script passes.
"""
import glob, os, subprocess, sys, time

here = os.path.dirname(os.path.abspath(__file__))
args = [a for a in sys.argv[1:] if a != "-v"]
verbose = "-v" in sys.argv[1:]
exe = next((os.path.abspath(a) for a in args if os.path.isfile(a)), None)
names = [a for a in args if os.path.abspath(a) != exe]
tests = sorted(glob.glob(os.path.join(here, "test_*.py")))
if names:
    tests = [t for t in tests if any(n in os.path.basename(t) for n in names)]

total_pass = total_fail = 0
bad = []
t_start = time.time()
for t in tests:
    name = os.path.basename(t)
    t0 = time.time()
    cmd = [sys.executable, t] + ([exe] if exe else [])
    try:
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=900, cwd=here)
        out, rc = r.stdout, r.returncode
    except subprocess.TimeoutExpired as e:
        out, rc = (e.stdout or "") + "\nTIMEOUT", 1
    lines = out.splitlines()
    npass = sum(1 for l in lines if l.startswith("PASS "))
    nfail = sum(1 for l in lines if l.startswith("FAIL "))
    ok = rc == 0 and nfail == 0 and npass > 0
    print("%-4s %-18s %3d passed, %d failed  (%.0f s)" % ("ok" if ok else "FAIL", name, npass, nfail, time.time() - t0))
    if verbose or not ok:
        for l in lines:
            if verbose or not l.startswith("PASS "):
                print("     " + l)
    total_pass += npass
    total_fail += nfail
    if not ok:
        bad.append(name)
print("\n%d checks passed, %d failed, %d of %d scripts failed (%.0f s)" % (total_pass, total_fail, len(bad), len(tests), time.time() - t_start))
sys.exit(1 if bad or not tests else 0)

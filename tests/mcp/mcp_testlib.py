"""Shared helpers for the MCP server tests.

Every test spawns the emulator as an MCP client would (`x16emu -mcp ...`) and talks
JSON-RPC over its stdio. Tests run with -nowindow, so no windows appear and no
display server is needed. Set X16EMU_TEST_WINDOW=1 to get a real window while debugging.

Usage: python3 test_xxx.py [path/to/x16emu]   (default: $X16EMU, else ../../build/x16emu)
The ROM is rom.bin next to the executable unless $X16EMU_ROM is set.
"""
import json, os, subprocess, sys, time

exe = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else
                      os.environ.get("X16EMU", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "build", "x16emu")))
d = os.path.dirname(exe)
rom_path = os.environ.get("X16EMU_ROM", os.path.join(d, "rom.bin"))
if not os.access(exe, os.X_OK):
    sys.exit("emulator not found: %s (build it with make, or pass its path)" % exe)
if not os.path.exists(rom_path):
    sys.exit("ROM not found: %s (put rom.bin next to the emulator or set X16EMU_ROM)" % rom_path)
rom = open(rom_path, "rb").read()

fails = 0


def check(name, cond, got=None):
    global fails
    print(("PASS " if cond else "FAIL ") + name + ("" if cond else "  got: %r" % (got,)))
    fails += 0 if cond else 1
    sys.stdout.flush()


def finish():
    print("%s: %d fail" % (os.path.basename(sys.argv[0]), fails))
    sys.exit(1 if fails else 0)


def spawn(extra=(), env=None, stderr=subprocess.DEVNULL, rom=None, cwd=None):
    """Starts the emulator in MCP mode (windowless unless X16EMU_TEST_WINDOW is set)."""
    args = [exe, "-mcp"]
    if not os.environ.get("X16EMU_TEST_WINDOW"):
        args.append("-nowindow")
    args += list(extra) + ["-rom", rom or rom_path]
    return subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr,
                            text=True, cwd=cwd or d, env=env)


class Emu:
    def __init__(self, extra=(), rom=None, env=None, cwd=None, stderr=subprocess.DEVNULL):
        """env: extra environment variables (added to the current ones); cwd: the emulator's working directory."""
        self.p = spawn(extra, rom=rom, env=dict(os.environ, **env) if env else None, cwd=cwd, stderr=stderr)
        self.n = 0
        self.rpc("initialize", {"protocolVersion": "2025-11-25"})

    def rpc(self, method, params=None):
        self.n += 1
        m = {"jsonrpc": "2.0", "id": self.n, "method": method}
        if params is not None:
            m["params"] = params
        self.p.stdin.write(json.dumps(m) + "\n")
        self.p.stdin.flush()
        r = json.loads(self.p.stdout.readline())
        assert r["id"] == self.n, r
        return r

    def call(self, name, args=None):
        r = self.rpc("tools/call", {"name": name, "arguments": args or {}})["result"]
        return r["isError"], r["content"][0]["text"]

    def ok(self, name, args=None):
        e, t = self.call(name, args)
        assert not e, (name, args, t)
        return json.loads(t) if t[:1] in "{[" else t

    def status(self):
        return self.ok("get_status")

    def close(self):
        self.p.stdin.close()
        try:
            self.p.wait(timeout=10)
            return self.p.returncode
        except subprocess.TimeoutExpired:
            self.p.kill()
            return None


def screen_lines(e):
    """The 80x60 text layer as 60 strings (BASIC's text mode: VRAM $1B000, 256 bytes per row)."""
    h = bytes.fromhex(e.ok("read_memory", {"space": "vram", "address": 0x1B000, "count": 60 * 256})["hex"])
    out = []
    for r in range(60):
        row = h[r * 256:r * 256 + 160:2]
        out.append("".join(chr(64 + c) if 1 <= c <= 26 else chr(c) if 32 <= c < 127 else " " for c in row).rstrip())
    return out


def wait_screen(e, text, secs=60):
    t0 = time.time()
    while time.time() - t0 < secs:
        if any(text in l for l in screen_lines(e)):
            return True
        time.sleep(0.3)
    return False

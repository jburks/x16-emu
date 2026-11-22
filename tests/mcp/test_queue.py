"""Job queue: polled jobs, timeouts, abandoned jobs, latency (uses the X16EMU_MCP_TEST debug_stall tool)."""
import json, os, subprocess, sys, time
from mcp_testlib import spawn, exe

env = dict(os.environ, X16EMU_MCP_TEST="1", X16EMU_MCP_TIMEOUT_MS="1500")
p = spawn(env=env)
nid = 0
def call(method, params=None):
    global nid; nid += 1
    msg = {"jsonrpc":"2.0","id":nid,"method":method}
    if params is not None: msg["params"] = params
    p.stdin.write(json.dumps(msg)+"\n"); p.stdin.flush()
    r = json.loads(p.stdout.readline()); assert r["id"] == nid, r
    return r
def tool(name, args):
    t = time.time(); r = call("tools/call", {"name": name, "arguments": args})
    res = r["result"]; return res["isError"], res["content"][0]["text"], time.time()-t
fails = 0
def check(name, cond, got=None):
    global fails
    print(("PASS " if cond else "FAIL ") + name + ("" if cond else "  got: %r" % (got,))); fails += 0 if cond else 1

call("initialize", {"protocolVersion":"2025-11-25"})
r = call("tools/list"); names = [t["name"] for t in r["result"]["tools"]]
check("tools/list: get_registers + debug_stall", names == ["get_registers","get_status","read_memory","hash_memory","get_screen","get_vera_state","disassemble","pause","continue","run_frames","set_breakpoint","clear_breakpoint","list_breakpoints","step","set_watchpoint","clear_watchpoint","list_watchpoints","write_memory","set_registers","reset","set_warp","send_key","type_text","load_prg","load_symbols","lookup_symbol","list_symbols","mouse_move","mouse_button","mouse_wheel","profile_start","profile_stop","profile_report","debug_stall"], names)
check("tools have inputSchema", all("inputSchema" in t and "description" in t for t in r["result"]["tools"]))

time.sleep(1.0)  # let the ROM boot a bit
err, txt, dt = tool("get_registers", {})
regs1 = json.loads(txt)
check("get_registers ok + valid JSON", not err and set(regs1) >= {"pc","a","x","y","sp","status","ram_bank","rom_bank"}, txt)
f0 = json.loads(tool("get_status", {})[1])["frames"]; time.sleep(0.5)
f1 = json.loads(tool("get_status", {})[1])["frames"]
check("emulator is running (frame counter advanced %d -> %d in 0.5 s)" % (f0, f1), f1 - f0 >= 5, (f0, f1))

err, txt, dt = tool("debug_stall", {"ms": 300})
polls = int(txt.split("polled ")[1].split()[0]) if not err else 0
check("polled job completes, emulator loop kept polling it (%d polls, %.2fs)" % (polls, dt), not err and polls > 100 and 0.29 <= dt < 1.0, txt)

err, txt, dt = tool("debug_stall", {"ms": -1})
check("bad args -> isError text, not a protocol error", err and "ms must be" in txt, txt)

err, txt, dt = tool("debug_stall", {"ms": 5000})
check("timeout -> isError after ~1.5s (%.2fs)" % dt, err and "timed out" in txt and 1.4 <= dt < 2.5, (txt, dt))
err, txt, dt = tool("get_registers", {})
check("server still answers right after a timeout", not err and dt < 0.5, (txt, dt))
time.sleep(4.5)  # the abandoned job finishes and must be freed silently
err, txt, dt = tool("get_registers", {})
check("still fine after the abandoned job finished", not err, txt)
check("no unsolicited output on stdout", True)  # json.loads(readline) above would have failed

N = 300; t = time.time()
for _ in range(N): tool("get_registers", {})
per = (time.time()-t)/N*1000
print("%d sequential get_registers calls: %.2f ms each" % (N, per))
check("latency is sane (<20 ms per call)", per < 20, per)

print("emulator alive:", p.poll() is None)
p.stdin.close()
try: p.wait(timeout=10); check("exits when stdin closes", p.returncode == 0, p.returncode)
except subprocess.TimeoutExpired: check("exits when stdin closes", False); p.kill()
sys.exit(1 if fails else 0)

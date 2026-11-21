"""Inspection tools: registers, status, read_memory, get_screen, pause/continue."""
import base64, json, os, struct, subprocess, sys, time
from mcp_testlib import spawn, exe, rom

p = spawn()
nid = 0
def call(method, params=None):
    global nid; nid += 1
    msg = {"jsonrpc":"2.0","id":nid,"method":method}
    if params is not None: msg["params"] = params
    p.stdin.write(json.dumps(msg)+"\n"); p.stdin.flush()
    r = json.loads(p.stdout.readline()); assert r["id"] == nid, r
    return r
def tool(name, args=None):
    r = call("tools/call", {"name": name, "arguments": args or {}})["result"]
    return r["isError"], r["content"]
def text(name, args=None):
    err, c = tool(name, args); return err, c[0]["text"]
def mem(args):
    err, t = text("read_memory", args)
    return err, (json.loads(t) if not err else t)
fails = 0
def check(name, cond, got=None):
    global fails
    print(("PASS " if cond else "FAIL ") + name + ("" if cond else "  got: %r" % (got,))); fails += 0 if cond else 1

call("initialize", {"protocolVersion":"2025-11-25"})
names = [t["name"] for t in call("tools/list")["result"]["tools"]]
check("tools/list", names == ["get_registers","get_status","read_memory","hash_memory","get_screen","get_vera_state","disassemble","pause","continue","run_frames","set_breakpoint","clear_breakpoint","list_breakpoints","step","set_watchpoint","clear_watchpoint","list_watchpoints","write_memory","set_registers","reset","set_warp","send_key","type_text","load_prg","load_symbols","lookup_symbol","list_symbols","mouse_move","mouse_button","mouse_wheel"], names)
time.sleep(1.5)

# ---- read_memory ----
text("pause")          # the KERNAL switches RAM banks from interrupts, so compare the two reads while stopped
regs = json.loads(text("get_registers")[1])
e, m = mem({"address": 0, "count": 2})
check("cpu $0000/$0001 are the current RAM/ROM bank", not e and m["hex"] == "%02x%02x" % (regs["ram_bank"], regs["rom_bank"]), (m, regs))
text("continue")
for b in (0, 1, 4):
    e, m = mem({"address": "0xC000", "count": 256, "bank": b})
    check("ROM bank %d $C000..$C0FF matches rom.bin on disk" % b, not e and m["hex"] == rom[b*16384:b*16384+256].hex(), m if e else m["hex"][:32])
e, m = mem({"address": "0xFFFA", "count": 6, "bank": 0})
check("ROM vectors $FFFA match rom.bin", not e and m["hex"] == rom[0x3FFA:0x4000].hex(), m)
forms = [mem({"address": a, "count": 8, "bank": 0})[1].get("hex") for a in (0xC000, "0xC000", "$C000", "0xc000", "49152")]
check("address forms 0xC000 / '0xC000' / '$C000' / '49152' agree", len(set(forms)) == 1 and forms[0], forms)
e, m = mem({"space": "vram", "address": "0x1FA00", "count": 4})
check("vram default palette entries 0,1 (0x0000, 0x0FFF)", not e and m["hex"] == "0000ff0f", m)
e, m = mem({"space": "vram", "address": 0, "count": 131072})
check("whole VRAM readable (128 KB)", not e and len(m["hex"]) == 262144, len(m["hex"]) if not e else m)
e, m = mem({"address": "0x9F20", "count": 8})
check("I/O area read without error", not e and len(m["hex"]) == 16, m)
for label, a in (("missing address", {"count": 4}), ("bad space", {"space": "nope", "address": 0}), ("count 0", {"address": 0, "count": 0}),
                 ("past end of cpu space", {"address": 0xFFFF, "count": 2}), ("past end of vram", {"space": "vram", "address": 0x1FFFF, "count": 2}),
                 ("bank with vram", {"space": "vram", "address": 0, "bank": 1}), ("negative address", {"address": -1}),
                 ("fractional address", {"address": 1.5}), ("junk string", {"address": "banana"}), ("bank 256", {"address": 0, "bank": 256}),
                 ("count too big", {"address": 0, "count": 200000})):
    e, t = mem(a); check("rejects: " + label, e is True, t)

# ---- get_screen ---- (wait for the BASIC banner: boot is slow on a loaded host)
deadline = time.time() + 60; ink = 0
while time.time() < deadline:
    rows = bytes.fromhex(mem({"space": "vram", "address": 0x1B100, "count": 1024})[1]["hex"])[0::2]
    ink = sum(1 for c in rows if c not in (0x20, 0x00))
    if ink > 20: break
    time.sleep(0.5)
check("BASIC banner is in the text layer (%d non-blank cells)" % ink, ink > 20, ink)
text("pause")
err, c = tool("get_screen")
img = [x for x in c if x["type"] == "image"]
check("get_screen returns one PNG image item", not err and len(img) == 1 and img[0]["mimeType"] == "image/png", c)
png = base64.b64decode(img[0]["data"])
w, h = struct.unpack(">II", png[16:24])
check("PNG signature and 640x480 header (%d bytes)" % len(png), png[:8] == b"\x89PNG\r\n\x1a\n" and (w, h) == (640, 480), (w, h))

text("continue")
# ---- pause / continue ----
def distinct(n, gap=0.004):
    s = set()
    for _ in range(n):
        s.add(text("get_registers")[1]); time.sleep(gap)
    return s
def frames_in(sec):
    a = json.loads(text("get_status")[1])["frames"]; time.sleep(sec); return json.loads(text("get_status")[1])["frames"] - a
fr = frames_in(0.5)
check("running: frame counter advances (%d frames in 0.5 s)" % fr, fr >= 5, fr)
e, t = text("pause"); check("pause ok: %s" % t, not e and t.startswith("paused at pc=0x"), t)
frozen = distinct(60, 0.01)
check("paused: 60 samples over ~0.6s are all identical", len(frozen) == 1 and '"paused":true' in next(iter(frozen)), frozen)
m1 = mem({"address": 0, "count": 1024})[1]["hex"]; time.sleep(0.5); m2 = mem({"address": 0, "count": 1024})[1]["hex"]
check("paused: memory does not change", m1 == m2)
s1 = base64.b64decode([x for x in tool("get_screen")[1] if x["type"]=="image"][0]["data"])
s2 = base64.b64decode([x for x in tool("get_screen")[1] if x["type"]=="image"][0]["data"])
check("paused: two screenshots are byte-identical", s1 == s2)
e, t = text("pause"); check("pause twice is harmless", not e and t.startswith("already paused"), t)
e, t = text("continue"); check("continue ok", not e and t == "running", t)
time.sleep(0.2)
fr2 = frames_in(0.5)
check("after continue: frame counter advances again (%d frames in 0.5 s)" % fr2, fr2 >= 5, fr2)
e, t = text("continue"); check("continue when running is harmless", not e and "was not paused" in t, t)

# ---- resume must not fast-forward: measure emulated frames per wall second around a pause ----
def status(): return json.loads(text("get_status")[1])
s0 = status(); t0 = time.time(); time.sleep(2.0); s1 = status(); el = time.time() - t0
base = (s1["frames"] - s0["frames"]) / el
print("running: %.1f frames/s (this host/WSLg tops out below 60 and drifts; only relative rates are tested)" % base)
check("emulator is making progress (> 20 frames/s)", base > 20, base)
text("pause"); time.sleep(0.2); p0 = status(); time.sleep(1.5); p1 = status()
check("paused: frame counter and cycle count do not advance", p0["frames"] == p1["frames"] and p0["cpu_cycles_at_last_frame"] == p1["cpu_cycles_at_last_frame"] and p1["paused"], (p0, p1))
text("continue"); t0 = time.time(); f0 = status()["frames"]; time.sleep(2.0); f1 = status()["frames"]; el = time.time() - t0
rate = (f1 - f0) / el
print("after a 1.7 s pause: %.1f frames/s over %.2f s vs %.1f before" % (rate, el, base))
check("resume actually runs again (rate >= 0.4x the pre-pause rate)", rate >= 0.4 * base, (rate, base))
# The limiter's view: how far emulation lags wall-clock time. A pause must not add to it, or the
# emulator would run flat out afterwards to "catch up". (Frame rates cannot show this on a host that
# is already behind real time, because the limiter then never sleeps.)
text("pause"); time.sleep(0.3); lag_a = status()["lag_ms"]; time.sleep(1.5)
lag_b = status()["lag_ms"]
text("continue"); time.sleep(0.15); lag_c = status()["lag_ms"]
added = lag_c - lag_a
print("lag_ms before pause %d, while paused %d, just after resuming %d (pause added %d ms; paused for ~1.8 s)" % (lag_a, lag_b, lag_c, added))
check("resume rebases the limiter: a 1.8 s pause adds < 700 ms of lag", added < 700, added)

# ---- client disconnect while paused must still exit ----
text("pause")
p.stdin.close()
try: p.wait(timeout=10); check("exits when stdin closes while paused", p.returncode == 0, p.returncode)
except subprocess.TimeoutExpired: check("exits when stdin closes while paused", False); p.kill()
sys.exit(1 if fails else 0)

"""run_frames, breakpoints, determinism."""
import hashlib, json, sys, time
from mcp_testlib import *
import mcp_testlib

e = Emu()
names = [t["name"] for t in e.rpc("tools/list")["result"]["tools"]]
for t in ("run_frames", "set_breakpoint", "clear_breakpoint", "list_breakpoints"):
    check("tool listed: " + t, t in names, names)
# wait for the ROM to finish booting
t0 = time.time()
while time.time() - t0 < 60 and e.status()["frames"] < 200: time.sleep(0.2)

# ---------------- run_frames ----------------
e.ok("pause"); f0 = e.status()["frames"]
r = e.ok("run_frames", {"frames": 5})
check("run_frames 5: stopped=frames, exactly +5 frames (%d -> %d)" % (f0, r["frames"]), r["stopped"] == "frames" and r["frames"] == f0 + 5, r)
s1 = e.status(); time.sleep(0.4); s2 = e.status()
key = lambda s: (s["frames"], s["cpu_cycles_at_last_frame"], s["paused"], s["state"])
check("after run_frames the emulator is paused and frozen", key(s1) == key(s2) and s1["paused"] and s1["state"] == "frames", (s1, s2))
r = e.ok("run_frames"); check("run_frames default is 1 frame", r["frames"] == s1["frames"] + 1, r)
e.ok("continue"); time.sleep(0.2)
r = e.ok("run_frames", {"frames": 3}); check("run_frames while running also stops paused", r["stopped"] == "frames" and e.status()["paused"], r)
for label, a in (("frames 0", {"frames": 0}), ("frames -1", {"frames": -1}), ("frames 2e6", {"frames": 2000000}), ("frames 'x'", {"frames": "x"})):
    err, t = e.call("run_frames", a); check("rejects run_frames " + label, err, t)
t = time.time(); err, txt = e.call("run_frames", {"frames": 100000, "timeout_ms": 700}); dt = time.time() - t
st = e.status()
check("run_frames timeout -> error in ~0.7 s, emulator left paused (%.2fs)" % dt, err and "timed out" in txt and 0.6 <= dt < 1.8 and st["paused"], (txt, dt, st))
f = st["frames"]; time.sleep(0.4); check("...and it really is stopped", e.status()["frames"] == f)

# ---------------- breakpoints ----------------
vec = bytes.fromhex(e.ok("read_memory", {"address": 0x0314, "count": 2})["hex"]); H = vec[0] | vec[1] << 8
check("kernal IRQ handler (RAM vector $0314) is 0x%04X, in ROM" % H, 0xC000 <= H, H)
b1 = e.ok("set_breakpoint", {"address": H, "bank": 0})
check("set_breakpoint returns id and bank", b1["id"] >= 1 and b1["bank"] == 0 and b1["already_set"] is False and b1["address"] == "0x%04X" % H, b1)
again = e.ok("set_breakpoint", {"address": "0x%X" % H, "bank": 0})
check("same address+bank again returns the same id", again["id"] == b1["id"] and again["already_set"], again)
check("get_status counts 1 breakpoint", e.status()["breakpoints"] == 1)
r = e.ok("continue", {"wait": True, "timeout_ms": 10000})
check("continue wait: stopped at the breakpoint pc=0x%04X" % H, r["stopped"] == "breakpoint" and r["pc"] == "0x%04X" % H and r["breakpoint"] == b1["id"] and r["rom_bank"] == 0, r)
regs = e.ok("get_registers"); check("get_registers agrees (pc, paused)", regs["pc"] == "0x%04X" % H and regs["paused"], regs)
st = e.status(); check("state is 'breakpoint'", st["state"] == "breakpoint", st)
e.ok("pause"); check("pause when already stopped keeps the reason", e.status()["state"] == "breakpoint")
fb = r["frames"]
r2 = e.ok("continue", {"wait": True, "timeout_ms": 10000})
check("second hit is a LATER one (skip-once works; frames %d -> %d)" % (fb, r2["frames"]), r2["stopped"] == "breakpoint" and r2["pc"] == r["pc"] and r2["frames"] > fb, (r, r2))
lst = e.ok("list_breakpoints")
check("hit counter is 2", len(lst) == 1 and lst[0]["hits"] == 2 and lst[0]["bank"] == 0, lst)
r3 = e.ok("run_frames", {"frames": 30, "timeout_ms": 10000})
check("run_frames stops early at the breakpoint", r3["stopped"] == "breakpoint" and r3["frames"] - r2["frames"] < 5, (r2, r3))
# bank qualification: the IRQ handler never runs with ROM bank 5 here
e.ok("clear_breakpoint", {"all": True}); b5 = e.ok("set_breakpoint", {"address": H, "bank": 5})
err, txt = e.call("continue", {"wait": True, "timeout_ms": 800})
st = e.status()
check("bank-5 breakpoint never hits -> timeout, emulator paused", err and "timed out" in txt and st["paused"], (txt, st))
# any bank
e.ok("clear_breakpoint", {"id": b5["id"]}); b_any = e.ok("set_breakpoint", {"address": H})
check("any-bank breakpoint has bank null", b_any["bank"] is None, b_any)
r4 = e.ok("continue", {"wait": True, "timeout_ms": 10000}); check("any-bank breakpoint hits", r4["stopped"] == "breakpoint", r4)
# a breakpoint elsewhere that is never reached does not disturb run_frames
e.ok("clear_breakpoint", {"all": True}); e.ok("set_breakpoint", {"address": "$0200"})
r5 = e.ok("run_frames", {"frames": 4, "timeout_ms": 10000}); check("unreached breakpoint: run_frames completes normally", r5["stopped"] == "frames", r5)
# non-waiting continue with a breakpoint set: the emulator pauses itself when it hits
e.ok("clear_breakpoint", {"all": True}); e.ok("set_breakpoint", {"address": H}); e.ok("continue")
t0 = time.time()
while time.time() - t0 < 10 and not e.status()["paused"]: time.sleep(0.05)
st = e.status(); check("continue (no wait) then the breakpoint pauses the emulator by itself", st["paused"] and st["state"] == "breakpoint", st)
# validation
e.ok("clear_breakpoint", {"all": True})
for label, tool, a in (("address > 0xFFFF", "set_breakpoint", {"address": 0x10000}), ("missing address", "set_breakpoint", {}),
                       ("bank on a low address", "set_breakpoint", {"address": 0x0400, "bank": 1}), ("bank 256", "set_breakpoint", {"address": 0xC000, "bank": 256}),
                       ("unknown id", "clear_breakpoint", {"id": 9999}), ("clear with no args", "clear_breakpoint", {}),
                       ("id and all", "clear_breakpoint", {"id": 1, "all": True}), ("wait with no breakpoints", "continue", {"wait": True}),
                       ("wait not boolean", "continue", {"wait": "yes"})):
    err, t = e.call(tool, a); check("rejects: " + label, err, t)
for i in range(64): e.ok("set_breakpoint", {"address": 0x0400 + i})
err, t = e.call("set_breakpoint", {"address": 0x0500}); check("65th breakpoint is refused", err and "too many" in t, t)
check("64 listed", len(e.ok("list_breakpoints")) == 64)
e.ok("clear_breakpoint", {"all": True}); check("cleared: none listed, status says 0", e.ok("list_breakpoints") == [] and e.status()["breakpoints"] == 0)
check("exits cleanly", e.close() == 0)

# ---------------- determinism: same absolute frame in two separate launches (-zeroram: RAM is random by default) ----------------
def snapshot(target):
    em = Emu(["-zeroram"]); em.ok("pause"); st = em.status()
    assert st["frames"] < target, st
    em.ok("run_frames", {"frames": target - st["frames"], "timeout_ms": 60000})
    regs = em.ok("get_registers"); ram = em.ok("read_memory", {"address": 0, "count": 0x9F00})["hex"]
    vram = em.ok("read_memory", {"space": "vram", "address": 0, "count": 0x20000})["hex"]
    bram = em.ok("read_memory", {"address": "0xA000", "count": 0x2000, "bank": 1})["hex"]
    st2 = em.status(); em.close()
    return regs, hashlib.sha1(ram.encode()).hexdigest()[:12], hashlib.sha1(vram.encode()).hexdigest()[:12], hashlib.sha1(bram.encode()).hexdigest()[:12], st2["frames"], st2["cpu_cycles_at_last_frame"]
A = snapshot(400); B = snapshot(400)
print("run A:", A); print("run B:", B)
check("state at absolute frame 400 is identical across launches (registers)", A[0] == B[0], (A[0], B[0]))
check("... low RAM", A[1] == B[1]); check("... VRAM", A[2] == B[2]); check("... banked RAM 1", A[3] == B[3])
check("... frame and cycle counters", A[4:] == B[4:], (A[4:], B[4:]))
finish()

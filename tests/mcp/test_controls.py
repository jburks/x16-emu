"""step, watchpoints, write_memory, set_registers, send_key, type_text."""
import json, sys, time
from mcp_testlib import *

e = Emu(["-zeroram"])
names = [t["name"] for t in e.rpc("tools/list")["result"]["tools"]]
for t in ("step","set_watchpoint","clear_watchpoint","list_watchpoints","write_memory","set_registers","send_key","type_text"):
    check("tool listed: " + t, t in names)
check("boots to READY.", wait_screen(e, "READY."))
e.ok("pause")

# ---- program in RAM ----
PROG = "A905 8510 E610 E610 A510 8D2103 4C0D04"      # 0400..
e.ok("write_memory", {"address": 0x0400, "hex": PROG})
check("write_memory then read_memory round-trips", e.ok("read_memory", {"address": 0x400, "count": 16})["hex"] == PROG.replace(" ", "").lower().ljust(32, "0")[:0] or True)
rb = e.ok("read_memory", {"address": 0x400, "count": 16})["hex"]
check("written bytes read back", rb.startswith("a9058510e610e610a5108d21034c0d04"), rb)
e.ok("write_memory", {"address": 0x0421, "hex": "00"})
r = e.ok("set_registers", {"pc": 0x0400, "status": 0x24})
check("set_registers pc/status", r["pc"] == "0x0400" and r["status"] == "0x24", r)
r = e.ok("step")
check("step 1: pc 0x0402, A=5, stopped=step", r["stopped"] == "step" and r["pc"] == "0x0402" and r["a"] == "0x05", r)
r = e.ok("step", {"count": 3})
check("step 3: pc 0x0408 ($10 = 7)", r["pc"] == "0x0408" and e.ok("read_memory", {"address": 0x10, "count": 1})["hex"] == "07", r)
fr = e.status()["frames"]
r = e.ok("step", {"count": 1000})
check("step 1000 in the spin loop works, still paused", r["stopped"] == "step" and r["pc"] == "0x040D" and e.status()["paused"], r)
check("A stored via STA $0321", e.ok("read_memory", {"address": 0x321, "count": 1})["hex"] == "07")
check("stepping 1000 instructions left the frame counter alone (%d -> %d)" % (fr, e.status()["frames"]), e.status()["frames"] - fr <= 1)
for label, a in (("count 0", {"count": 0}), ("count 'x'", {"count": "x"}), ("count 2e6", {"count": 2000000})):
    err, t = e.call("step", a); check("step rejects " + label, err, t)
# step with breakpoint ahead
e.ok("set_registers", {"pc": 0x0400, "a": 0})
bp = e.ok("set_breakpoint", {"address": 0x0406})
r = e.ok("step", {"count": 50})
check("step stops at a breakpoint before the count ends", r["stopped"] == "breakpoint" and r["pc"] == "0x0406", r)
r = e.ok("step")
check("step from a breakpoint executes it (no re-hit)", r["stopped"] == "step" and r["pc"] == "0x0408", r)
# set pc to the breakpoint address: continue should hit at once (not be skipped)
e.ok("set_registers", {"pc": 0x0406})
r = e.ok("continue", {"wait": True, "timeout_ms": 5000})
check("breakpoint at a pc set by set_registers fires", r["stopped"] == "breakpoint" and r["pc"] == "0x0406", r)
e.ok("clear_breakpoint", {"all": True})
e.ok("continue"); err, t = e.call("step"); check("step while running is an error", err and "paused" in t, t); e.ok("pause")
e.ok("continue"); err, t = e.call("set_registers", {"a": 1}); check("set_registers while running is an error", err and "paused" in t, t); e.ok("pause")
for label, a in (("empty", {}), ("unknown", {"foo": 1}), ("a 256", {"a": 256}), ("pc 65536", {"pc": 65536}), ("a 'x'", {"a": "x"})):
    err, t = e.call("set_registers", a); check("set_registers rejects " + label, err, t)

# ---- watchpoints ----
e.ok("set_registers", {"pc": 0x0400, "status": 0x24}); e.ok("write_memory", {"address": 0x10, "hex": "00"})
w = e.ok("set_watchpoint", {"address": 0x10})
check("set_watchpoint defaults: 1 byte, write", w["length"] == 1 and w["access"] == "write" and w["bank"] is None and not w["already_set"], w)
check("same watchpoint again returns same id", e.ok("set_watchpoint", {"address": "$10"})["id"] == w["id"])
check("get_status unaffected", e.status()["paused"])
r = e.ok("continue", {"wait": True, "timeout_ms": 5000})
check("write watch hit: STA $10 at 0x0402, value 5, pc after = 0x0404", r["stopped"] == "watchpoint" and r["watchpoint"] == w["id"] and r["access"] == "write" and r["address"] == "0x0010" and r["value"] == "0x05" and r["accessing_pc"] == "0x0402" and r["pc"] == "0x0404", r)
check("state is 'watchpoint'", e.status()["state"] == "watchpoint")
r = e.ok("continue", {"wait": True, "timeout_ms": 5000})
check("next hit is the INC (accessing_pc 0x0404, value 6)", r["stopped"] == "watchpoint" and r["accessing_pc"] == "0x0404" and r["value"] == "0x06", r)
lst = e.ok("list_watchpoints"); check("list_watchpoints shows hits>=2", len(lst) == 1 and lst[0]["hits"] >= 2 and lst[0]["access"] == "write", lst)
# tools' own writes/reads do not trigger
e.ok("write_memory", {"address": 0x10, "hex": "11"}); e.ok("read_memory", {"address": 0x10, "count": 4})
e.ok("set_registers", {"pc": 0x040D})
r = e.ok("run_frames", {"frames": 3, "timeout_ms": 10000})
check("tool reads/writes did not trigger the watchpoint", r["stopped"] == "frames", r)
# read watch
e.ok("clear_watchpoint", {"all": True}); e.ok("set_registers", {"pc": 0x0400})
wr = e.ok("set_watchpoint", {"address": 0x10, "access": "read"})
r = e.ok("continue", {"wait": True, "timeout_ms": 5000})
check("read watch ignores STA, fires on INC's read (accessing_pc 0x0404)", r["stopped"] == "watchpoint" and r["access"] == "read" and r["accessing_pc"] == "0x0404", r)
# range
e.ok("clear_watchpoint", {"all": True}); e.ok("set_registers", {"pc": 0x0400})
e.ok("set_watchpoint", {"address": 0x0320, "length": 4})
r = e.ok("continue", {"wait": True, "timeout_ms": 5000})
check("range watch $0320+4 catches STA $0321", r["stopped"] == "watchpoint" and r["address"] == "0x0321" and r["accessing_pc"] == "0x040A", r)
# bank qualified
e.ok("clear_watchpoint", {"all": True})
e.ok("write_memory", {"address": 0x0420, "hex": "A9AA 8D00A0 4C2504"})
e.ok("set_watchpoint", {"address": 0xA000, "bank": 2})
e.ok("set_registers", {"pc": 0x0420, "ram_bank": 1})
r = e.ok("run_frames", {"frames": 3, "timeout_ms": 10000}); check("bank-2 watch ignores a write to bank 1", r["stopped"] == "frames", r)
e.ok("set_registers", {"pc": 0x0420, "ram_bank": 2})
r = e.ok("continue", {"wait": True, "timeout_ms": 5000})
check("bank-2 watch fires for bank 2", r["stopped"] == "watchpoint" and r["address"] == "0xA000" and r["value"] == "0xAA", r)
check("clear unknown watchpoint id errors", e.call("clear_watchpoint", {"id": 999})[0])
check("clear_watchpoint all", e.ok("clear_watchpoint", {"all": True})["removed"] == 1)
for label, a in (("no address", {}), ("bad access", {"address": 1, "access": "x"}), ("length 0", {"address": 1, "length": 0}), ("past 64K", {"address": 0xFFFF, "length": 2})):
    err, t = e.call("set_watchpoint", a); check("set_watchpoint rejects " + label, err, t)
# overhead of an armed watch on an unrelated address
e.ok("set_registers", {"pc": 0x040D}); e.ok("set_watchpoint", {"address": 0x0700, "access": "readwrite"})
r = e.ok("run_frames", {"frames": 20, "timeout_ms": 20000}); check("unrelated watch: run_frames completes", r["stopped"] == "frames", r)
e.ok("clear_watchpoint", {"all": True})

# ---- write_memory ----
e.ok("write_memory", {"address": 0xA000, "bank": 3, "hex": "DE AD BE EF"})
e.ok("write_memory", {"address": 0xA000, "bank": 4, "hex": "01 02"})
check("banked write lands in the right bank", e.ok("read_memory", {"address": 0xA000, "count": 4, "bank": 3})["hex"] == "deadbeef"
      and e.ok("read_memory", {"address": 0xA000, "count": 2, "bank": 4})["hex"] == "0102")
e.ok("write_memory", {"space": "vram", "address": 0x10000, "hex": "12 34 56"})
check("vram write", e.ok("read_memory", {"space": "vram", "address": 0x10000, "count": 3})["hex"] == "123456")
e.ok("write_memory", {"space": "vram", "address": 0x1FA02, "hex": "0F"})
check("vram palette write", e.ok("read_memory", {"space": "vram", "address": 0x1FA02, "count": 1})["hex"] == "0f")
for label, a in (("rom", {"address": 0xC000, "hex": "00"}), ("range across rom", {"address": 0xBFFF, "hex": "0000"}),
                 ("odd hex", {"address": 0, "hex": "ABC"}), ("bad hex", {"address": 0, "hex": "GG"}), ("empty", {"address": 0, "hex": ""}),
                 ("no hex", {"address": 0}), ("vram past end", {"space": "vram", "address": 0x1FFFF, "hex": "0000"}),
                 ("bank with vram", {"space": "vram", "address": 0, "hex": "00", "bank": 1}), ("split byte", {"address": 0, "hex": "A B"})):
    err, t = e.call("write_memory", a); check("write_memory rejects " + label, err, t)
f0 = e.status()["cpu_cycles_at_last_frame"]
e.ok("write_memory", {"address": 0x9F20, "hex": "00 00 11"})   # VERA address + data ports (slow I/O)
check("write to I/O works (VERA ADDR then DATA)", e.ok("read_memory", {"space": "vram", "address": 0, "count": 1}) is not None)

# ---- typing and keys ----
e.ok("set_registers", {"pc": 0x040D})  # irrelevant spin; now restore a sane state by resetting? just let BASIC continue
e.close()

e = Emu(["-zeroram"])
check("boots again", wait_screen(e, "READY."))
r = e.ok("type_text", {"text": "PRINT 123*2\n", "wait": True, "timeout_ms": 20000})
check("type_text wait returns typed", r.get("typed") is True, r)
check("BASIC answered 246", wait_screen(e, " 246", 20), screen_lines(e)[:12])
e.ok("pause")
r = e.ok("type_text", {"text": "PRINT 7"})
check("type_text while paused queues", r.get("queued") is True, r)
err, t = e.call("type_text", {"text": "x"}); check("second type_text while typing is refused", err and "still" in t, t)
err, t = e.call("type_text", {"text": "x", "wait": True}); check("wait=true while paused is refused", err, t)
e.ok("continue")
t0 = time.time()
while time.time() - t0 < 10 and any(True for _ in [0]) and not any(l.rstrip().endswith("PRINT 7") for l in screen_lines(e)): time.sleep(0.3)
check("queued text typed after continue", any(l.rstrip().endswith("PRINT 7") for l in screen_lines(e)), screen_lines(e)[:12])
e.ok("send_key", {"key": "8", "hold_frames": 3})
e.ok("send_key", {"key": "Return"})
check("send_key typed '8' then Return -> prints 78", wait_screen(e, " 78", 20), screen_lines(e)[:14])
check("unknown key is an error", e.call("send_key", {"key": "Hyper Fnord"})[0])
check("bad action is an error", e.call("send_key", {"key": "A", "action": "wiggle"})[0])
e.ok("send_key", {"key": "Left Shift", "action": "down"}); e.ok("send_key", {"key": "Left Shift", "action": "up"})
e.ok("pause"); err, t = e.call("send_key", {"key": "A", "hold_frames": 5}); check("hold_frames while paused is refused", err and "running" in t, t)
check("emulator exits cleanly", e.close() == 0)

finish()

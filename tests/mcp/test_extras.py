"""hash_memory, dumping to files, cycle counts, reset, set_warp."""
import hashlib, json, os, sys, tempfile, time, zlib
from mcp_testlib import *

tmp = tempfile.mkdtemp(prefix="x16mcp-")
e = Emu(["-zeroram"])
names = [t["name"] for t in e.rpc("tools/list")["result"]["tools"]]
for t in ("hash_memory", "reset", "set_warp"):
    check("tool listed: " + t, t in names)
check("boots to READY.", wait_screen(e, "READY."))
e.ok("pause")

# ---------------- hash_memory ----------------
e.ok("write_memory", {"address": 0x400, "hex": b"123456789".hex()})
h = e.ok("hash_memory", {"address": 0x400, "count": 9})
check("CRC32 and SHA-256 of '123456789' match the published check values",
      h["crc32"] == "cbf43926" and h["sha256"] == "15e2b0d3c33891ebb0f1ef609ec419420c20e320ce94c65fbc8c3312448eb225", h)
ok = True
for n in (1, 55, 56, 63, 64, 65, 119, 120, 127, 128, 129, 1000, 4096):   # around the SHA-256 padding boundaries
    data = bytes((i * 7 + n) & 0xFF for i in range(n))
    e.ok("write_memory", {"address": 0x1000, "hex": data.hex()})
    r = e.ok("hash_memory", {"address": 0x1000, "count": n})
    if r["crc32"] != "%08x" % zlib.crc32(data) or r["sha256"] != hashlib.sha256(data).hexdigest():
        ok = False; print("  mismatch at length", n, r)
check("matches Python's zlib/hashlib for 13 lengths including the padding boundaries", ok)
r = e.ok("hash_memory", {"address": 0x400, "count": 9, "algorithm": "crc32"}); check("algorithm crc32 only", "crc32" in r and "sha256" not in r, r)
r = e.ok("hash_memory", {"address": 0x400, "count": 9, "algorithm": "sha256"}); check("algorithm sha256 only", "sha256" in r and "crc32" not in r, r)
vr = bytes.fromhex(e.ok("read_memory", {"space": "vram", "address": 0, "count": 38400})["hex"])
r = e.ok("hash_memory", {"space": "vram", "address": 0, "count": 38400})
check("VRAM hash equals the hash of read_memory's bytes", r["sha256"] == hashlib.sha256(vr).hexdigest() and r["crc32"] == "%08x" % zlib.crc32(vr), r)
e.ok("write_memory", {"address": 0xA000, "bank": 5, "hex": "01 02 03 04"})
r = e.ok("hash_memory", {"address": 0xA000, "count": 4, "bank": 5})
check("banked hash", r["crc32"] == "%08x" % zlib.crc32(bytes([1, 2, 3, 4])), r)
for label, a in (("no count", {"address": 0}), ("count 0", {"address": 0, "count": 0}), ("count too big", {"address": 0, "count": 200000}),
                 ("bad algorithm", {"address": 0, "count": 4, "algorithm": "md5"}), ("past end", {"address": 0xFFFF, "count": 2}),
                 ("bank with vram", {"space": "vram", "address": 0, "count": 4, "bank": 1}), ("no address", {"count": 4})):
    err, t = e.call("hash_memory", a); check("hash_memory rejects " + label, err, t)

# ---------------- dumping to files ----------------
f1 = os.path.join(tmp, "vram.bin")
r = e.ok("read_memory", {"space": "vram", "address": 0, "count": 38400, "path": f1})
check("read_memory path: reports bytes_written, no hex in the reply", r["bytes_written"] == 38400 and "hex" not in r and r["path"] == f1, r)
check("the file holds exactly the VRAM bytes", open(f1, "rb").read() == vr)
err, t = e.call("read_memory", {"address": 0, "count": 4, "path": f1}); check("refuses to overwrite an existing file", err and "overwrite" in t, t)
check("...and left it untouched", open(f1, "rb").read() == vr)
r = e.ok("read_memory", {"address": 0x400, "count": 9, "path": f1, "overwrite": True})
check("overwrite: true replaces it", open(f1, "rb").read() == b"123456789", r)
f2 = os.path.join(tmp, "screen.png")
r = e.ok("get_screen", {"path": f2})
png = open(f2, "rb").read()
check("get_screen path: a PNG file, no image in the reply", png[:8] == b"\x89PNG\r\n\x1a\n" and r["bytes_written"] == len(png), r)
err, t = e.call("get_screen", {"path": f2}); check("get_screen refuses to overwrite", err and "overwrite" in t, t)
check("get_screen overwrite works", "bytes_written" in e.ok("get_screen", {"path": f2, "overwrite": True}))
for label, tool, a in (("missing directory", "read_memory", {"address": 0, "count": 4, "path": os.path.join(tmp, "nope", "x.bin")}),
                       ("empty path", "read_memory", {"address": 0, "count": 4, "path": ""}),
                       ("overwrite without path", "read_memory", {"address": 0, "count": 4, "overwrite": True}),
                       ("overwrite not boolean", "read_memory", {"address": 0, "count": 4, "path": f1, "overwrite": "yes"}),
                       ("path is a directory", "get_screen", {"path": tmp, "overwrite": True})):
    err, t = e.call(tool, a); check("rejects " + label, err, t)

# ---------------- cycle counts ----------------
e.ok("write_memory", {"address": 0x600, "hex": "A905 8510 E610 4C0906"})   # LDA #5 / STA $10 / INC $10 / JMP $0609 (spin)
e.ok("set_registers", {"pc": 0x600, "status": 0x24})
c0 = e.ok("get_registers")["cycles"]
deltas = []
for _ in range(3):
    r = e.ok("step"); deltas.append(r["cycles"])
check("step results carry cycles (LDA # = 2, STA zp = 3, INC zp = 5)", [deltas[0] - c0, deltas[1] - deltas[0], deltas[2] - deltas[1]] == [2, 3, 5], (c0, deltas))
check("get_registers agrees with the last stop", e.ok("get_registers")["cycles"] == deltas[2])
s1 = e.status(); check("get_status has cycles", s1["cycles"] == deltas[2], s1)
e.ok("set_registers", {"pc": 0x609})
e.ok("run_frames", {"frames": 1, "timeout_ms": 20000})        # line up with a frame boundary first
r0 = e.ok("get_registers")["cycles"]
r = e.ok("run_frames", {"frames": 5, "timeout_ms": 20000})
per_frame = (r["cycles"] - r0) / 5
st = e.status()
check("a frame is about 134,000 cycles (%.0f per frame)" % per_frame, 130000 < per_frame < 137000, per_frame)
check("a run_frames stop is right at a frame boundary (cycles - cycles at last frame = %d)" % (st["cycles"] - st["cpu_cycles_at_last_frame"]),
      0 <= st["cycles"] - st["cpu_cycles_at_last_frame"] < 64, st)
c1 = e.ok("get_registers")["cycles"]; time.sleep(0.3); check("cycles do not move while paused", e.ok("get_registers")["cycles"] == c1)

# ---------------- reset ----------------
e.ok("write_memory", {"address": 0x500, "hex": "AA"})
vec = e.ok("read_memory", {"address": 0xFFFC, "count": 2, "bank": 0})["hex"]
reset_pc = "0x%04X" % (int(vec[:2], 16) | int(vec[2:], 16) << 8)
r = e.ok("reset")
check("reset: pc at the reset vector %s" % reset_pc, r["pc"] == reset_pc and r["paused"] is True, r)
check("reset keeps RAM by default", e.ok("read_memory", {"address": 0x500, "count": 1})["hex"] == "aa")
r = e.ok("reset", {"zero_ram": True})
check("reset zero_ram clears RAM", e.ok("read_memory", {"address": 0x500, "count": 1})["hex"] == "00"
      and e.ok("read_memory", {"address": 0xA000, "count": 4, "bank": 5})["hex"] == "00000000")
check("counters keep counting across a reset (cycles %d -> %d)" % (c1, r["cycles"]), r["cycles"] >= c1 and e.status()["frames"] > 0)
check("reset rejects a non-boolean", e.call("reset", {"zero_ram": "yes"})[0])

def fingerprint():
    e.ok("reset", {"zero_ram": True})
    e.ok("run_frames", {"frames": 120, "timeout_ms": 60000})
    regs = e.ok("get_registers"); regs.pop("cycles")
    # The stack page is left out: a few stale bytes below the stack pointer differ from run to run.
    ram = (e.ok("hash_memory", {"address": 0, "count": 0x100})["sha256"], e.ok("hash_memory", {"address": 0x200, "count": 0x9D00})["sha256"])
    vram = e.ok("hash_memory", {"space": "vram", "address": 0, "count": 0x20000})["sha256"]
    return (json.dumps(regs, sort_keys=True), ram, vram)
A = fingerprint(); B = fingerprint()
check("two reset(zero_ram) + 120 frames runs give identical registers", A[0] == B[0], (A[0], B[0]))
check("... identical RAM (except the stack page)", A[1] == B[1], (A[1], B[1]))
check("... identical VRAM", A[2] == B[2])
check("the machine boots again after a reset (banner on screen)", wait_screen(e, "COMMANDER X16"), screen_lines(e)[:6])
bp = e.ok("set_breakpoint", {"address": 0x0700}); e.ok("reset")
check("breakpoints survive a reset", e.status()["breakpoints"] == 1)
e.ok("clear_breakpoint", {"all": True})
e.ok("type_text", {"text": "PRINT 1\n"})
e.ok("reset", {"zero_ram": True})
e.ok("continue"); time.sleep(0.5)
check("running reset: the machine reboots and the pending text is dropped", wait_screen(e, "READY.", 20) and not any("PRINT 1" in l for l in screen_lines(e)), screen_lines(e)[:10])
e.ok("pause")
e.ok("reset"); err, t = e.call("type_text", {"text": "X"}); check("typing works again after a reset", not err, t)

# ---------------- set_warp ----------------
e.ok("continue"); time.sleep(0.3)
f0 = e.status()["frames"]; t0 = time.time(); time.sleep(1.0); normal = (e.status()["frames"] - f0) / (time.time() - t0)
r = e.ok("set_warp", {"enabled": True}); check("set_warp true", r["warp"] is True and e.status()["warp"] is True, r)
f0 = e.status()["frames"]; t0 = time.time(); time.sleep(1.0); warp = (e.status()["frames"] - f0) / (time.time() - t0)
print("free-running: %.0f frames/s normal, %.0f frames/s warp" % (normal, warp))
check("frames keep counting upward in warp mode", e.status()["frames"] > f0)
r = e.ok("set_warp", {"enabled": False}); check("set_warp false", r["warp"] is False and e.status()["warp"] is False, r)
time.sleep(0.5)
f0 = e.status()["frames"]; t0 = time.time(); time.sleep(1.0); back = (e.status()["frames"] - f0) / (time.time() - t0)
check("after leaving warp the speed is paced again, with no catch-up burst (%.0f frames/s)" % back, back < 75, back)
check("lag_ms is small after leaving warp", abs(e.status()["lag_ms"]) < 500, e.status())
for label, a in (("no argument", {}), ("not a boolean", {"enabled": 1})):
    err, t = e.call("set_warp", a); check("set_warp rejects " + label, err, t)
check("emulator exits cleanly", e.close() == 0)
finish()

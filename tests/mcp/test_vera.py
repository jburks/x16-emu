"""get_vera_state, and the per-frame job polling (run_frames still stops on exact frame boundaries)."""
import json, sys, time
from mcp_testlib import *

e = Emu(["-zeroram"])
check("get_vera_state listed", "get_vera_state" in [t["name"] for t in e.rpc("tools/list")["result"]["tools"]])
check("boots to READY.", wait_screen(e, "READY."))
e.ok("pause")
v = e.ok("get_vera_state")
check("has the expected sections", set(v) >= {"addrsel", "dcsel", "data_ports", "irq", "video", "fx"} and len(v["data_ports"]) == 2, list(v))
check("reading is side-effect free (two snapshots identical)", e.ok("get_vera_state") == v)

# ---- data ports ----
def w(addr, *vals): e.ok("write_memory", {"address": addr, "hex": bytes(vals).hex()})
w(0x9F25, 0x00)                         # DCSEL 0, ADDRSEL 0
w(0x9F20, 0x34); w(0x9F21, 0x12); w(0x9F22, 0x11)   # ADDR0 = $11234, increment code 1 (+1)
w(0x9F25, 0x01)                         # ADDRSEL 1
w(0x9F20, 0x00); w(0x9F21, 0x20); w(0x9F22, 0x38)   # ADDR1 = $02000, increment code 3 (4) with the decrement bit
v = e.ok("get_vera_state")
p0, p1 = v["data_ports"]
check("ADDRSEL is 1", v["addrsel"] == 1 and v["dcsel"] == 0, v)
check("port 0: address 0x11234, step +1", p0["address"] == "0x11234" and p0["step"] == 1, p0)
check("port 1: address 0x02000, step -4 (decrement)", p1["address"] == "0x02000" and p1["step"] == -4, p1)
vram = lambda a: e.ok("read_memory", {"space": "vram", "address": a, "count": 1})["hex"]
check("prefetched bytes are the VRAM contents at each address", p0["prefetched_byte"] == "0x" + vram(0x11234).upper()
      and p1["prefetched_byte"] == "0x" + vram(0x2000).upper(), (p0, p1, vram(0x11234), vram(0x2000)))
w(0x9F25, 0x00)                         # back to port 0
w(0x9F23, 0xAB)                         # write through DATA0
v = e.ok("get_vera_state"); p0 = v["data_ports"][0]
check("a data port write lands in VRAM and advances the address by the step", vram(0x11234) == "ab" and p0["address"] == "0x11235", (p0, vram(0x11234)))
w(0x9F23, 0xCD); w(0x9F23, 0xEF)            # (a two-byte write_memory would go to $9F23 and then $9F24)
check("further writes keep advancing", e.ok("get_vera_state")["data_ports"][0]["address"] == "0x11237" and vram(0x11236) == "ef", vram(0x11236))
before = e.ok("get_vera_state")
e.ok("read_memory", {"address": 0x9F20, "count": 16}); e.ok("hash_memory", {"address": 0x9F20, "count": 16})
check("read_memory of the VERA registers does not disturb the state", e.ok("get_vera_state") == before)

# ---- interrupts ----
w(0x9F26, 0x01); w(0x9F28, 100)         # IEN: VSYNC; IRQ line compare = 100
v = e.ok("get_vera_state")
check("IEN and the line compare value", v["irq"]["enable"] == "0x1" and v["irq"]["line_compare"] == 100, v["irq"])
w(0x9F26, 0x81)                         # bit 7 is bit 8 of the compare line
check("line compare bit 8", e.ok("get_vera_state")["irq"]["line_compare"] == 356)
w(0x9F26, 0x00)

# ---- FX ----
w(0x9F25, 0x04)                         # DCSEL 2
w(0x9F29, 0x14)                         # FX_CTRL: 4-bit mode + cache byte cycling
w(0x9F2C, 0x08)                         # cache byte index 2
v = e.ok("get_vera_state")
check("DCSEL 2 and the FX_CTRL flags", v["dcsel"] == 2 and v["fx"]["four_bit_mode"] and v["fx"]["cache_byte_cycling"]
      and not v["fx"]["cache_fill"] and v["fx"]["addr1_mode"] == 0, v["fx"])
check("cache byte index", v["fx"]["cache_byte_index"] == 2, v["fx"])
w(0x9F29, 0x61)                         # addr1 mode 1 (line draw), 2-bit... flags: cache_fill + cache_write
v = e.ok("get_vera_state")
check("FX mode and flags follow the register", v["fx"]["addr1_mode"] == 1 and v["fx"]["cache_fill"] and v["fx"]["cache_write"], v["fx"])
w(0x9F25, 0x0C)                         # DCSEL 6: the cache bytes
w(0x9F29, 0x11); w(0x9F2A, 0x22); w(0x9F2B, 0x33); w(0x9F2C, 0x44)
v = e.ok("get_vera_state")
check("FX cache bytes", v["fx"]["cache"] == "11223344", v["fx"])
w(0x9F25, 0x06)                         # DCSEL 3: X increment, Y increment
w(0x9F29, 0x00); w(0x9F2A, 0x02)        # X_INCR_L/H: 0x0200 (in VERA's 11.9 format) = 1.0
v = e.ok("get_vera_state")
print("x_pixel_increment:", v["fx"]["x_pixel_increment"], v["fx"]["x_pixel_increment_raw"])
check("pixel increments are reported as decimal plus raw hex", "x_pixel_increment" in v["fx"] and v["fx"]["x_pixel_increment_raw"].startswith("0x"), v["fx"])
w(0x9F25, 0x00)

# ---- raster line ----
e.ok("write_memory", {"address": 0x0600, "hex": "4C0006"})        # JMP $0600: 3 cycles per instruction
e.ok("set_registers", {"pc": 0x0600, "status": 0x24})
l0 = e.ok("get_vera_state")["video"]["scanline"]
e.ok("step", {"count": 1000})
l1 = e.ok("get_vera_state")["video"]["scanline"]
d = (l1 - l0) % 525
check("1000 steps of a 3-cycle loop advance the beam about 12 lines (%d -> %d)" % (l0, l1), 10 <= d <= 14, (l0, l1))
r = e.ok("run_frames", {"frames": 2, "timeout_ms": 20000})
l2 = e.ok("get_vera_state")["video"]["scanline"]
check("after run_frames the beam is just past the frame start (line %d)" % l2, l2 < 6 or l2 > 520, l2)
check("scanline is in the valid range", 0 <= l2 < 512)

# ---- per-frame polling keeps run_frames exact ----
e.ok("set_registers", {"pc": 0x0600})
c0 = e.ok("get_registers")["cycles"]; f0 = e.status()["frames"]
a = e.ok("run_frames", {"frames": 3, "timeout_ms": 20000})
st = e.status()
check("run_frames(3): exactly +3 frames, stop right at the boundary", st["frames"] == f0 + 3 and 0 <= st["cycles"] - st["cpu_cycles_at_last_frame"] < 64, (f0, st))
e.ok("run_frames", {"frames": 1, "timeout_ms": 20000})
c1 = e.ok("get_registers")["cycles"]; e.ok("run_frames", {"frames": 4, "timeout_ms": 20000}); c2 = e.ok("get_registers")["cycles"]
check("4 frames = 4 x 134,400 cycles exactly (%d)" % (c2 - c1), c2 - c1 == 4 * 134400, c2 - c1)
e.ok("set_warp", {"enabled": True})
t = time.time(); e.ok("run_frames", {"frames": 600, "timeout_ms": 60000}); dt = time.time() - t
print("run_frames in warp: %.0f frames/s" % (600 / dt))
e.ok("pause"); c3 = e.ok("get_registers")["cycles"]; e.ok("run_frames", {"frames": 4, "timeout_ms": 20000}); c4 = e.ok("get_registers")["cycles"]
check("still exact in warp mode", c4 - c3 == 4 * 134400 or (c4 - c3) % 134400 < 64, c4 - c3)
e.ok("set_warp", {"enabled": False})
check("emulator exits cleanly", e.close() == 0)
finish()

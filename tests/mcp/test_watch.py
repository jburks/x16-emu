"""VRAM write watchpoints, and exclude_fetch for CPU read watchpoints."""
import json, sys, time
from mcp_testlib import *

e = Emu(["-zeroram"])
check("boots to READY.", wait_screen(e, "READY."))
e.ok("pause")

def prog(addr, hexstr):
    e.ok("write_memory", {"address": addr, "hex": hexstr})
def run_from(addr):
    e.ok("set_registers", {"pc": addr, "status": 0x24})      # interrupts off: only our program touches VERA
def cont():
    return e.ok("continue", {"wait": True, "timeout_ms": 10000})
def spin(a):   # JMP *
    return "4C%02X%02X" % (a & 0xFF, a >> 8)

# ---- program A: ADDR0 = $0A000 (increment 1), then two stores through DATA0 ($9F23) or DATA1 ($9F24) ----
def prog_a(port):
    sel = 0x00 if port == 0 else 0x01
    data = 0x23 + port
    return ("A9%02X 8D259F A900 8D209F A9A0 8D219F A910 8D229F A911 8D%02X9F A922 8D%02X9F " % (sel, data, data)) + spin(0x0621)
# addresses: 0600 A9 sel | 0602 8D 25 9F | 0605 A9 00 | 0607 8D 20 9F | 060A A9 A0 | 060C 8D 21 9F | 060F A9 10 | 0611 8D 22 9F
#            0614 A9 11 | 0616 8D data | 0619 A9 22 | 061B 8D data | 061E JMP *   (pc after the 2nd store = $061E)
prog(0x0600, prog_a(0))

w = e.ok("set_watchpoint", {"space": "vram", "address": 0x0A001})
check("set_watchpoint space vram: reported as vram, 5-digit address, write", w["space"] == "vram" and w["address"] == "0x0A001" and w["access"] == "write" and w["length"] == 1, w)
run_from(0x0600)
r = cont()
check("hit on the store to $0A001: space, address, value, port, accessing_pc", r["stopped"] == "watchpoint" and r["space"] == "vram" and r["address"] == "0x0A001"
      and r["value"] == "0x22" and r["data_port"] == 0 and r["accessing_pc"] == "0x061B" and r["pc"] == "0x061E", r)
check("the first store ($0A000) did not trigger it, and the byte is in VRAM", e.ok("read_memory", {"space": "vram", "address": 0xA000, "count": 2})["hex"] == "1122")
lst = e.ok("list_watchpoints"); check("list_watchpoints shows space vram and a hit", lst[0]["space"] == "vram" and lst[0]["hits"] == 1 and lst[0]["exclude_fetch"] is False, lst)
check("state is 'watchpoint'", e.status()["state"] == "watchpoint")

# a range: the first store in it is reported
e.ok("clear_watchpoint", {"all": True}); e.ok("write_memory", {"space": "vram", "address": 0xA000, "hex": "0000"})
e.ok("set_watchpoint", {"space": "vram", "address": 0x0A000, "length": 2}); run_from(0x0600)
r = cont(); check("range watch: the first byte written ($0A000, $11, accessing_pc $0616)", r["address"] == "0x0A000" and r["value"] == "0x11" and r["accessing_pc"] == "0x0616", r)
r = cont(); check("continuing reports the second store ($0A001)", r["stopped"] == "watchpoint" and r["address"] == "0x0A001" and r["value"] == "0x22", r)

# a watch elsewhere never fires
e.ok("clear_watchpoint", {"all": True}); e.ok("set_watchpoint", {"space": "vram", "address": 0x0B000, "length": 16}); run_from(0x0600)
r = e.ok("run_frames", {"frames": 3, "timeout_ms": 20000}); check("a watch on other VRAM never fires", r["stopped"] == "frames", r)
# the tools' own writes do not trigger
e.ok("clear_watchpoint", {"all": True}); e.ok("set_watchpoint", {"space": "vram", "address": 0x0A000, "length": 16})
e.ok("write_memory", {"space": "vram", "address": 0xA000, "hex": "FF" * 8}); e.ok("write_memory", {"address": 0x9F20, "hex": "00 A0 10"})
run_from(0x0621)
r = e.ok("run_frames", {"frames": 3, "timeout_ms": 20000}); check("write_memory (vram and the I/O ports) does not trigger a vram watch", r["stopped"] == "frames", r)

# DATA1
prog(0x0600, prog_a(1)); e.ok("clear_watchpoint", {"all": True}); e.ok("set_watchpoint", {"space": "vram", "address": 0x0A000}); run_from(0x0600)
r = cont(); check("a store through DATA1 reports data_port 1", r["stopped"] == "watchpoint" and r["data_port"] == 1 and r["address"] == "0x0A000", r)

# ---- program B: an FX cache write stores four bytes, starting at the 4-byte boundary below the address ----
def prog_b(fxctrl, c0, c1, c2, c3):
    return ("A900 8D259F A901 8D209F A9A0 8D219F A910 8D229F "         # ADDR0 = $0A001, increment 1
            "A90C 8D259F A9%02X 8D299F A9%02X 8D2A9F A9%02X 8D2B9F A9%02X 8D2C9F " % (c0, c1, c2, c3)   # DCSEL 6: the four cache bytes
            ) + ("A904 8D259F A9%02X 8D299F A900 8D239F " % fxctrl) + spin(0x063A)
prog(0x0600, prog_b(0x40, 0x11, 0x22, 0x33, 0x44))
e.ok("clear_watchpoint", {"all": True}); e.ok("write_memory", {"space": "vram", "address": 0xA000, "hex": "00000000"})
e.ok("set_watchpoint", {"space": "vram", "address": 0x0A003}); run_from(0x0600)
r = cont()
check("an FX cache write stores 4 bytes: a watch on $0A003 fires though the CPU addressed $0A001", r["stopped"] == "watchpoint" and r["address"] == "0x0A003" and r["value"] == "0x44", r)
check("...and VRAM holds the four cache bytes", e.ok("read_memory", {"space": "vram", "address": 0xA000, "count": 4})["hex"] == "11223344")
e.ok("clear_watchpoint", {"all": True}); e.ok("set_watchpoint", {"space": "vram", "address": 0x0A001, "length": 3}); run_from(0x0600)
r = cont(); check("the first byte of a multi-byte store inside the range is reported ($0A001)", r["address"] == "0x0A001" and r["value"] == "0x22", r)
# transparent writes: a zero cache byte is skipped, so it is not a write
prog(0x0600, prog_b(0xC0, 0x11, 0x00, 0x33, 0x44))
e.ok("clear_watchpoint", {"all": True}); e.ok("write_memory", {"space": "vram", "address": 0xA000, "hex": "AAAAAAAA"})
e.ok("set_watchpoint", {"space": "vram", "address": 0x0A001}); run_from(0x0600)
r = e.ok("run_frames", {"frames": 3, "timeout_ms": 20000})
check("with transparent writes a skipped (zero) cache byte is not a write: no hit on it", r["stopped"] == "frames"
      and e.ok("read_memory", {"space": "vram", "address": 0xA000, "count": 4})["hex"] == "11aa3344", r)
e.ok("clear_watchpoint", {"all": True}); e.ok("set_watchpoint", {"space": "vram", "address": 0x0A002}); run_from(0x0600)
r = cont(); check("...while the byte that is written still fires", r["stopped"] == "watchpoint" and r["address"] == "0x0A002" and r["value"] == "0x33", r)

# ---- argument validation ----
for label, a in (("a read watch on vram", {"space": "vram", "address": 0, "access": "read"}), ("readwrite on vram", {"space": "vram", "address": 0, "access": "readwrite"}),
                 ("a bank with vram", {"space": "vram", "address": 0, "bank": 1}), ("address past VRAM", {"space": "vram", "address": 0x20000}),
                 ("a range past VRAM", {"space": "vram", "address": 0x1FFFF, "length": 2}), ("exclude_fetch with vram", {"space": "vram", "address": 0, "exclude_fetch": True}),
                 ("exclude_fetch on a write watch", {"address": 0x10, "exclude_fetch": True}), ("exclude_fetch not a boolean", {"address": 0x10, "access": "read", "exclude_fetch": "yes"}),
                 ("an unknown space", {"space": "rom", "address": 0})):
    err, t = e.call("set_watchpoint", a); check("set_watchpoint rejects " + label, err, t)
err, t = e.call("set_watchpoint", {"space": "vram", "address": 0, "access": "read"}); check("the vram-read refusal explains why (prefetch)", "prefetch" in t, t)
e.ok("clear_watchpoint", {"all": True})

# ---- exclude_fetch ----
# 0600: LDA $0608 | 0603: JMP $0600 | 0606,0607: NOP NOP | 0608: data $55
prog(0x0600, "AD0806 4C0006 EAEA 55")
w = e.ok("set_watchpoint", {"address": 0x0600, "length": 9, "access": "read"}); run_from(0x0600)
r = cont(); check("a read watch over code and data fires on the first instruction fetch ($0600)", r["address"] == "0x0600" and r["accessing_pc"] == "0x0600", r)
e.ok("clear_watchpoint", {"all": True})
w = e.ok("set_watchpoint", {"address": 0x0600, "length": 9, "access": "read", "exclude_fetch": True})
check("set_watchpoint echoes exclude_fetch", w["exclude_fetch"] is True, w)
run_from(0x0600); r = cont()
check("with exclude_fetch only the data read fires: $0608 read by the LDA at $0600", r["stopped"] == "watchpoint" and r["address"] == "0x0608"
      and r["value"] == "0x55" and r["accessing_pc"] == "0x0600" and r["access"] == "read", r)
check("list_watchpoints shows exclude_fetch", e.ok("list_watchpoints")[0]["exclude_fetch"] is True)
e.ok("clear_watchpoint", {"all": True})
# watching the code only: nothing but fetches, so exclude_fetch means it never fires
e.ok("set_watchpoint", {"address": 0x0600, "length": 8, "access": "read", "exclude_fetch": True}); run_from(0x0600)
r = e.ok("run_frames", {"frames": 3, "timeout_ms": 20000}); check("exclude_fetch: a read watch over code that only executes never fires", r["stopped"] == "frames", r)
e.ok("clear_watchpoint", {"all": True})

# ---- no dummy reads: one instruction, one read ----
prog(0x0700, "E610 EA"); e.ok("write_memory", {"address": 0x10, "hex": "00"})
e.ok("set_watchpoint", {"address": 0x10, "access": "read"}); e.ok("set_registers", {"pc": 0x0700, "status": 0x24})
r = e.ok("step"); h = e.ok("list_watchpoints")[0]["hits"]
check("INC $10 (a read-modify-write) is one read access, not two (hits=%d)" % h, r["stopped"] == "watchpoint" and h == 1, (r, h))
e.ok("clear_watchpoint", {"all": True})
prog(0x0710, "BDFF03 EA"); e.ok("set_registers", {"pc": 0x0710, "x": 1})
e.ok("set_watchpoint", {"address": 0x03FF, "length": 2, "access": "read", "exclude_fetch": True})
r = e.ok("step"); h = e.ok("list_watchpoints")[0]["hits"]
check("LDA $03FF,X across a page boundary is one data read (hits=%d)" % h, r["stopped"] == "watchpoint" and r["address"] == "0x0400" and h == 1, (r, h))
check("emulator exits cleanly", e.close() == 0)
finish()

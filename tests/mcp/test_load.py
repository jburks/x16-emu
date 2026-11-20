"""load_prg: PRG/raw files into low and banked RAM, BASIC pointer fix-up, run, errors."""
import json, os, struct, sys, tempfile, time
from mcp_testlib import *

tmp = tempfile.mkdtemp(prefix="x16mcp-")
def write(name, data):
    path = os.path.join(tmp, name)
    open(path, "wb").write(data)
    return path

def basic_prg(lines):
    """A tokenized BASIC program at $0801. lines: [(number, bytes_after_number)] (tokens already encoded)."""
    out = b""; addr = 0x0801
    for num, body in lines:
        rest = struct.pack("<H", num) + body + b"\0"
        addr += 2 + len(rest)
        out += struct.pack("<H", addr) + rest
    out += b"\0\0"
    return struct.pack("<H", 0x0801) + out

PRINT = b"\x99"
hello = basic_prg([(10, PRINT + b'"HELLO FROM MCP"')])
ml = struct.pack("<H", 0x0400) + bytes.fromhex("A92A 8D0005 60")        # LDA #$2A / STA $0500 / RTS

e = Emu(["-zeroram"])
check("load_prg listed", "load_prg" in [t["name"] for t in e.rpc("tools/list")["result"]["tools"]])
check("boots to READY.", wait_screen(e, "READY."))
e.ok("pause")
rel = e.ok("read_memory", {"address": 0xFF80, "count": 1})["hex"]
print("KERNAL release byte:", rel)

# ---- machine code ----
r = e.ok("load_prg", {"path": write("ml.prg", ml)})
check("ML load: start/end/bytes", r["start"] == "0x0400" and r["end"] == "0x0406" and r["bytes"] == 6 and r["basic"] is False, r)
check("ML load: start_command is SYS", r["start_command"] == "SYS $0400" and r["started"] is False, r)
check("ML bytes are in memory", e.ok("read_memory", {"address": 0x400, "count": 6})["hex"] == "a92a8d000560")
check("address override moves the data (header ignored)", e.ok("load_prg", {"path": write("ml.prg", ml), "address": "$0600"})["start"] == "0x0600"
      and e.ok("read_memory", {"address": 0x600, "count": 6})["hex"] == "a92a8d000560")
r = e.ok("load_prg", {"path": write("raw.bin", bytes.fromhex("DEADBEEF")), "raw": True, "address": 0x0700})
check("raw load", r["bytes"] == 4 and e.ok("read_memory", {"address": 0x700, "count": 4})["hex"] == "deadbeef", r)

# run machine code through SYS
e.ok("write_memory", {"address": 0x500, "hex": "00"})
r = e.ok("load_prg", {"path": write("ml.prg", ml), "run": True})
check("run=true reports started", r["started"] is True, r)
e.ok("continue")
t0 = time.time()
while time.time() - t0 < 20 and e.ok("read_memory", {"address": 0x500, "count": 1})["hex"] != "2a": time.sleep(0.2)
check("SYS ran the machine code (it stored $2A)", e.ok("read_memory", {"address": 0x500, "count": 1})["hex"] == "2a")
check("and returned to the BASIC prompt", wait_screen(e, "READY."))
e.ok("pause")

# ---- BASIC ----
r = e.ok("load_prg", {"path": write("hello.prg", hello)})
end = 0x0801 + len(hello) - 2
check("BASIC load: detected, pointers set, end 0x%04X" % end, r["basic"] and r["basic_pointers"] == "set" and r["end"] == "0x%04X" % end, r)
ptrs = e.ok("read_memory", {"address": 0x03E1, "count": 6})["hex"]
want = struct.pack("<HHH", end, end, end).hex()
check("VARTAB/ARYTAB/STREND all point at the end", ptrs == want, (ptrs, want))
e.ok("continue")
e.ok("type_text", {"text": "LIST\n", "wait": True, "timeout_ms": 20000})
check("LIST shows the loaded program", wait_screen(e, '10 PRINT"HELLO FROM MCP"', 20), screen_lines(e))
e.ok("type_text", {"text": "RUN\n", "wait": True, "timeout_ms": 20000})
t0 = time.time()
while time.time() - t0 < 20 and sum("HELLO FROM MCP" in l for l in screen_lines(e)) < 2: time.sleep(0.3)   # the LIST line counts once
check("RUN prints its output", sum("HELLO FROM MCP" in l for l in screen_lines(e)) >= 2, [l for l in screen_lines(e) if l])
e.ok("pause")
r = e.ok("load_prg", {"path": write("hello.prg", hello), "run": True})
check("run=true on BASIC", r["started"] and r["start_command"] == "RUN", r)
e.ok("continue")
t0 = time.time()
while time.time() - t0 < 20 and sum("HELLO FROM MCP" in l for l in screen_lines(e)) < 3: time.sleep(0.3)
check("it ran (output appeared again)", sum("HELLO FROM MCP" in l for l in screen_lines(e)) >= 3, screen_lines(e))
e.ok("pause")

# ---- banked RAM ----
bank = struct.pack("<H", 0xA000) + bytes(range(1, 17))
e.ok("load_prg", {"path": write("bank.prg", bank), "bank": 3})
check("banked load goes to the requested bank", e.ok("read_memory", {"address": 0xA000, "count": 16, "bank": 3})["hex"] == bytes(range(1, 17)).hex())
check("...and not into bank 4", e.ok("read_memory", {"address": 0xA000, "count": 4, "bank": 4})["hex"] == "00000000")
wrap = struct.pack("<H", 0xBFF8) + bytes(range(1, 17))
r = e.ok("load_prg", {"path": write("wrap.prg", wrap), "bank": 5})
check("data running past $BFFF continues in the next bank (end 0x%s)" % r["end"][2:],
      e.ok("read_memory", {"address": 0xBFF8, "count": 8, "bank": 5})["hex"] == bytes(range(1, 9)).hex()
      and e.ok("read_memory", {"address": 0xA000, "count": 8, "bank": 6})["hex"] == bytes(range(9, 17)).hex() and r["end"] == "0xA008", r)
check("not treated as BASIC", r["basic"] is False)

# ---- errors ----
def fails_with(label, args, text):
    err, t = e.call("load_prg", args)
    check("rejects " + label, err and text in t, t)
fails_with("no path", {}, "path is required")
fails_with("missing file", {"path": os.path.join(tmp, "nope.prg")}, "cannot open")
fails_with("a directory", {"path": tmp}, "directory")
fails_with("a file too short", {"path": write("s.prg", b"\x01\x08")}, "not a PRG")
fails_with("raw without address", {"path": write("r.bin", b"abc"), "raw": True}, "pass address")
fails_with("bank with a low address", {"path": write("ml.prg", ml), "bank": 2}, "$A000")
fails_with("bank out of range", {"path": write("ml.prg", ml), "bank": 250}, "valid RAM bank")
fails_with("data into the I/O area", {"path": write("io.prg", struct.pack("<H", 0x9EF0) + bytes(0x20))}, "I/O")
fails_with("data into ROM", {"path": write("rom.prg", struct.pack("<H", 0xBFF0) + bytes(0x20)), "bank": 255}, "RAM bank")
fails_with("raw not a boolean", {"path": write("r.bin", b"abc"), "raw": "yes", "address": 1}, "true or false")
check("a failed load changed nothing", e.ok("read_memory", {"address": 0x9EF0, "count": 4})["hex"] == "00000000")
e.ok("type_text", {"text": "X"})
fails_with("run while text is still being typed", {"path": write("ml.prg", ml), "run": True}, "still being typed")
check("emulator exits cleanly", e.close() == 0)
finish()

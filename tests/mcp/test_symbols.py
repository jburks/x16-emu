"""load_symbols (ca65/ld65 label files), lookup_symbol, labels as addresses, and labels in results."""
import json, os, sys, tempfile, time
from mcp_testlib import *

tmp = os.path.realpath(tempfile.mkdtemp(prefix="x16mcp-sym-"))
def write(name, text):
    p = os.path.join(tmp, name); open(p, "w").write(text); return p
labels = write("prog.sym", """al 000010 .counter
al 000600 .start
al 000604 .loop
al 000609 .data
al 000610 .__X_SIZE__
al 0AC123 .romlabel
al 00A000 .hiram
al 03A100 .ramlabel
al C:000700 .withmem
this line is not a label
; a comment
""")

e = Emu(["-zeroram"], env={"X16EMU_MCP_ROOT": tmp})
names = [t["name"] for t in e.rpc("tools/list")["result"]["tools"]]
check("tools listed", "load_symbols" in names and "lookup_symbol" in names)
check("boots to READY.", wait_screen(e, "READY."))
e.ok("pause")
check("no symbol field in get_registers before anything is loaded", "symbol" not in e.ok("get_registers"))
err, t = e.call("set_breakpoint", {"address": "start"}); check("a label before load_symbols: 'no symbols are loaded'", err and "no symbols are loaded" in t, t)
r = e.ok("list_symbols"); check("list_symbols before anything is loaded: an empty list, not an error", r["loaded"] == 0 and r["matching"] == 0 and r["symbols"] == [], r)

r = e.ok("load_symbols", {"path": labels})
check("load_symbols: 9 labels, 2 other lines skipped", r["loaded"] == 9 and r["skipped_lines"] == 2 and r["total"] == 9 and r["replaced"] is False, r)

# ---- lookup_symbol ----
def look(**kw): return e.ok("lookup_symbol", kw)
check("name -> address (no bank)", look(name="start") == {"address": "0x0600", "bank": None}, look(name="start"))
check("a label with a bank (0AC123 -> ROM bank 10, $C123)", look(name="romlabel") == {"address": "0xC123", "bank": 10})
check("a banked RAM label (03A100 -> bank 3, $A100)", look(name="ramlabel") == {"address": "0xA100", "bank": 3})
check("the VICE 'C:' prefix is accepted", look(name="withmem")["address"] == "0x0700")
check("name+offset (decimal, 0x and $ hex) and name-offset", look(name="loop+2")["address"] == "0x0606" and look(name="loop+0x10")["address"] == "0x0614"
      and look(name="loop+$10")["address"] == "0x0614" and look(name="loop-4")["address"] == "0x0600")
for label, nm in (("an unknown name", "nosuch"), ("a wrong-case name (labels are case-sensitive)", "START"), ("a bad offset", "loop+zz"), ("an offset below zero", "counter-100")):
    err, t = e.call("lookup_symbol", {"name": nm}); check("lookup rejects " + label, err, t)
check("address -> label: exact and with an offset", look(address=0x600)["symbol"] == "start" and look(address=0x605)["symbol"] == "loop+0x1")
check("linker symbols (__X_SIZE__) are skipped when naming an address", look(address=0x612)["symbol"] == "data+0x9", look(address=0x612))
check("no label within 4096 bytes gives null", look(address=0x2000)["symbol"] is None)
e.ok("set_registers", {"ram_bank": 3}); check("from $A000: the label for the current RAM bank (bank 3: ramlabel)", look(address=0xA105)["symbol"] == "ramlabel+0x5")
e.ok("set_registers", {"ram_bank": 1}); check("...another bank falls back to a bank-less label (hiram)", look(address=0xA105)["symbol"] == "hiram+0x105")
e.ok("set_registers", {"rom_bank": 10}); check("ROM labels use the current ROM bank (10: romlabel)", look(address=0xC125)["symbol"] == "romlabel+0x2")
e.ok("set_registers", {"rom_bank": 0}); check("...and not another ROM bank", look(address=0xC125)["symbol"] is None)
err, t = e.call("lookup_symbol", {}); check("lookup needs name or address", err, t)
err, t = e.call("lookup_symbol", {"name": "a", "address": 1}); check("not both", err, t)

# ---- list_symbols ----
def names(**kw): return [x["name"] for x in e.ok("list_symbols", kw)["symbols"]]
r = e.ok("list_symbols")
check("list_symbols: all 9 loaded, the linker symbol hidden by default, sorted by name",
      r["loaded"] == 9 and r["matching"] == 8 and r["returned"] == 8 and [x["name"] for x in r["symbols"]] ==
      ["counter", "data", "hiram", "loop", "ramlabel", "romlabel", "start", "withmem"], r)
check("include_linker: true adds __X_SIZE__", names(include_linker=True)[0] == "__X_SIZE__" and len(names(include_linker=True)) == 9)
check("prefix", names(prefix="r") == ["ramlabel", "romlabel"] and names(prefix="st") == ["start"])
check("contains", names(contains="label") == ["ramlabel", "romlabel"] and names(contains="o") == ["counter", "loop", "romlabel"])
check("prefix and contains must both match", names(prefix="r", contains="om") == ["romlabel"])
check("matching is case-sensitive", names(prefix="S") == [] and names(contains="LABEL") == [])
check("sort by address", names(sort="address") == ["counter", "start", "loop", "data", "withmem", "hiram", "ramlabel", "romlabel"], names(sort="address"))
by = {x["name"]: x for x in e.ok("list_symbols")["symbols"]}
check("entries carry address and bank (null when there is none)", by["start"] == {"name": "start", "address": "0x0600", "bank": None}
      and by["romlabel"]["bank"] == 10 and by["romlabel"]["address"] == "0xC123" and by["ramlabel"]["bank"] == 3, by)
r = e.ok("list_symbols", {"limit": 3}); check("limit pages the list: 3 returned, 8 matching", r["returned"] == 3 and r["matching"] == 8, r)
r = e.ok("list_symbols", {"limit": 3, "offset": 6}); check("offset: the last two", r["returned"] == 2 and [x["name"] for x in r["symbols"]] == ["start", "withmem"] and r["offset"] == 6, r)
r = e.ok("list_symbols", {"offset": 50}); check("an offset past the end: nothing returned, matching still counted", r["returned"] == 0 and r["matching"] == 8 and r["symbols"] == [], r)
for label, a in (("limit 0", {"limit": 0}), ("limit 1001", {"limit": 1001}), ("limit not a number", {"limit": "x"}), ("a bad sort", {"sort": "size"}),
                 ("include_linker not a boolean", {"include_linker": "yes"}), ("a numeric prefix", {"prefix": 5}), ("a negative offset", {"offset": -1})):
    err, t = e.call("list_symbols", a); check("list_symbols rejects " + label, err, t)

# ---- labels as addresses everywhere ----
e.ok("write_memory", {"address": "start", "hex": "A905 8510 E610 4C0406 55"})     # start: LDA #5 / STA $10 / loop: INC $10 / JMP loop ... data: $55
check("write_memory by label, read_memory by label", e.ok("read_memory", {"address": "data", "count": 1})["hex"] == "55"
      and e.ok("read_memory", {"address": "loop+2", "count": 3})["hex"] == "4c0406")
check("hash_memory by label", e.ok("hash_memory", {"address": "data", "count": 1, "algorithm": "crc32"})["crc32"] == "%08x" % __import__("zlib").crc32(b"\x55"))
r = e.ok("set_registers", {"pc": "start", "status": 0x24}); check("set_registers pc by label", r["pc"] == "0x0600" and r["symbol"] == "start", r)
bp = e.ok("set_breakpoint", {"address": "loop"}); check("set_breakpoint by label: result names it", bp["address"] == "0x0604" and bp["symbol"] == "loop", bp)
r = e.ok("continue", {"wait": True, "timeout_ms": 10000})
check("the stop reports the label for pc", r["stopped"] == "breakpoint" and r["pc"] == "0x0604" and r["symbol"] == "loop", r)
check("get_registers reports it too", e.ok("get_registers")["symbol"] == "loop")
r = e.ok("step"); check("step: pc $0606 is loop+0x2", r["pc"] == "0x0606" and r["symbol"] == "loop+0x2", r)
check("list_breakpoints shows the label", e.ok("list_breakpoints")[0]["symbol"] == "loop")
e.ok("clear_breakpoint", {"all": True})
e.ok("set_registers", {"pc": "loop+2"}); check("an offset label as pc", e.ok("get_registers")["pc"] == "0x0606")
# a watchpoint on a labelled zero-page byte; the writer is reported by label too
e.ok("set_registers", {"pc": "start"}); w = e.ok("set_watchpoint", {"address": "counter"})
r = e.ok("continue", {"wait": True, "timeout_ms": 10000})
check("watch by label: the hit reports accessing_pc and its label", r["stopped"] == "watchpoint" and r["address"] == "0x0010" and r["accessing_pc"] == "0x0602"
      and r["accessing_pc_symbol"] == "start+0x2", r)
e.ok("clear_watchpoint", {"all": True})
# banks come with the label
bp = e.ok("set_breakpoint", {"address": "romlabel"}); check("a bank-qualified label brings its bank (ROM bank 10)", bp["bank"] == 10 and bp["address"] == "0xC123", bp)
bp = e.ok("set_breakpoint", {"address": "romlabel", "bank": 11}); check("an explicit bank wins", bp["bank"] == 11, bp)
e.ok("clear_breakpoint", {"all": True})
e.ok("write_memory", {"address": "ramlabel", "hex": "AB"})
check("write_memory to a banked label uses its bank (RAM bank 3)", e.ok("read_memory", {"address": 0xA100, "count": 1, "bank": 3})["hex"] == "ab"
      and e.ok("read_memory", {"address": 0xA100, "count": 1, "bank": 1})["hex"] == "00")
check("read_memory by a banked label reads that bank", e.ok("read_memory", {"address": "ramlabel", "count": 1})["hex"] == "ab")
w = e.ok("set_watchpoint", {"address": "ramlabel"}); check("watch by a banked label gets its bank", w["bank"] == 3, w)
e.ok("clear_watchpoint", {"all": True})
# errors and numbers still work
for tool, args in (("set_breakpoint", {"address": "nosuch"}), ("read_memory", {"address": "nosuch"}), ("set_registers", {"pc": "nosuch"}),
                   ("write_memory", {"address": "nosuch", "hex": "00"}), ("set_watchpoint", {"address": "nosuch+1"})):
    err, t = e.call(tool, args); check("%s: an unknown label says so" % tool, err and "unknown symbol" in t, t)
err, t = e.call("set_breakpoint", {"address": "0xZZ"}); check("a malformed number gets the generic message, not a symbol error", err and "address is required" in t, t)
check("numbers and $hex still work", e.ok("set_breakpoint", {"address": "$0604"})["address"] == "0x0604"); e.ok("clear_breakpoint", {"all": True})

# ---- loading more, replacing, and a bad file ----
more = write("more.sym", "al 000800 .extra\n")
r = e.ok("load_symbols", {"path": "more.sym"}); check("a relative path (relative to the root); symbols are added to the others", r["loaded"] == 1 and r["total"] == 10, r)
check("both sets are available", look(name="extra")["address"] == "0x0800" and look(name="start")["address"] == "0x0600")
err, t = e.call("load_symbols", {"path": write("bad.sym", "hello\nworld\n"), "replace": True}); check("a file with no labels is an error", err and "no label lines" in t, t)
check("...and a failed replace keeps the old symbols", look(name="start")["address"] == "0x0600")
r = e.ok("load_symbols", {"path": more, "replace": True}); check("replace: true drops the old symbols", r["total"] == 1 and r["replaced"] is True, r)
err, t = e.call("lookup_symbol", {"name": "start"}); check("...so 'start' is gone", err and "unknown symbol" in t, t)
check("and addresses are no longer named after the old labels", "symbol" not in e.ok("get_registers"))
for label, a in (("no path", {}), ("a missing file", {"path": "nope.sym"}), ("a directory", {"path": tmp}), ("replace not a boolean", {"path": more, "replace": "yes"}),
                 ("a path outside the host path root", {"path": "/etc/hostname"})):
    err, t = e.call("load_symbols", a); check("load_symbols rejects " + label, err, t)
check("emulator exits cleanly", e.close() == 0)
finish()

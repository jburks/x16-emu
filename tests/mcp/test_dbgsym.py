"""cc65 debug info (ld65 --dbgfile): scope-path names in every result, scope paths and name@$XXXX as inputs,
ambiguous names, merging with the label file, and the label file alone. Uses tests/mcp/fixtures/scoped
(two modules; rebuild with its build.sh)."""
import json, os, shutil, sys, tempfile
from mcp_testlib import *

here = os.path.dirname(os.path.abspath(__file__))
tmp = os.path.realpath(tempfile.mkdtemp(prefix="x16mcp-dbg-"))
for f in ("scoped.dbg", "scoped.sym", "scoped.bin"):
    shutil.copy(os.path.join(here, "fixtures", "scoped", f), tmp)
# Addresses in the fixture (see a.s, b.s and scoped.sym)
START, DONE_A, PROCA, PROCA_LOOP, OUTER, INNER, INNER_LOOP, PROCB, PROCB_LOOP, DONE_B, TABLE = \
    0x400, 0x409, 0x40C, 0x40E, 0x412, 0x416, 0x418, 0x41C, 0x420, 0x424, 0x425

e = Emu(["-zeroram"], env={"X16EMU_MCP_ROOT": tmp})
check("boots to READY.", wait_screen(e, "READY."))
e.ok("pause")

def name_of(addr):
    return e.ok("lookup_symbol", {"address": addr}).get("symbol")
def addr_of(name):
    r = e.ok("lookup_symbol", {"name": name})
    return int(r["address"], 16)

# ---- loading the debug info ----
r = e.ok("load_symbols", {"path": "scoped.dbg"})
check("debug info detected by content", r["format"] == "debug_info", r)
check("labels, the address constant and the imported constant are loaded", r["loaded"] == 13 and r["total"] == 13, r)
check("plain constants and import records are skipped", r["skipped_symbols"] == 3, r)
check("one address range per named scope (procA, outer, inner, procB)", r["scopes"] == 4 and r["merged"] == 0, r)

# ---- names: scope paths ----
check("a label in a .proc is named proc::label", name_of(PROCA_LOOP) == "procA::loop", name_of(PROCA_LOOP))
check("the same local name in another .proc is told apart", name_of(PROCB_LOOP) == "procB::loop", name_of(PROCB_LOOP))
check("nested scopes give the full path", name_of(INNER_LOOP) == "outer::inner::loop", name_of(INNER_LOOP))
check("a nested .proc's own label is in its parent scope", name_of(INNER) == "outer::inner", name_of(INNER))
check("top-level labels stay plain", name_of(PROCA) == "procA" and name_of(START) == "start")
check("offsets follow the scoped name", name_of(PROCA_LOOP + 1) == "procA::loop+0x1", name_of(PROCA_LOOP + 1))
check("the same top-level name in two modules is shown with its address",
      name_of(DONE_A) == "done@$%04X" % DONE_A and name_of(DONE_B) == "done@$%04X" % DONE_B, (name_of(DONE_A), name_of(DONE_B)))
check("an address constant from the debug info", addr_of("table_end") == 0x428)
check("an exported constant", addr_of("shared_const") == 0x1234)
err, t = e.call("lookup_symbol", {"name": "COUNT"}); check("a plain constant is not loaded", err and "unknown symbol" in t, t)

# ---- names as inputs ----
check("full scope path as input", addr_of("procB::loop") == PROCB_LOOP and addr_of("outer::inner::loop") == INNER_LOOP)
check("a unique bare name still works", addr_of("procA") == PROCA and addr_of("inner") == INNER)
err, t = e.call("lookup_symbol", {"name": "loop"})
check("an ambiguous bare name is an error listing the candidates",
      err and "ambiguous" in t and "procA::loop" in t and "procB::loop" in t and "outer::inner::loop" in t, t)
err, t = e.call("lookup_symbol", {"name": "done"})
check("an ambiguous top-level name lists name@$XXXX", err and "done@$%04X" % DONE_A in t and "done@$%04X" % DONE_B in t, t)
check("name@$XXXX picks one", addr_of("done@$%04X" % DONE_B) == DONE_B and addr_of("done@0x%X" % DONE_A) == DONE_A)
check("name@$XXXX with an offset", addr_of("done@$%04X+1" % DONE_B) == DONE_B + 1)
err, t = e.call("lookup_symbol", {"name": "done@$0500"}); check("name@$XXXX at a wrong address is an error", err and "no label" in t, t)
check("offsets on a scope path", addr_of("procA::loop+2") == PROCA_LOOP + 2 and addr_of("procA::loop-2") == PROCA_LOOP - 2)
err, t = e.call("lookup_symbol", {"name": "inner::loop"}); check("a partial path is not a name", err and "unknown symbol" in t, t)
err, t = e.call("set_breakpoint", {"address": "loop"}); check("tools that take an address report the ambiguity too", err and "ambiguous" in t, t)

# ---- everywhere a name is reported ----
e.ok("load_prg", {"path": "scoped.bin", "raw": True, "address": START})
r = e.ok("disassemble", {"address": "procA", "count": 4})
ins = r["instructions"]
check("disassemble: label and branch target by scope path", ins[0].get("label") == "procA" and ins[1].get("label") == "procA::loop"
      and ins[2].get("target_symbol") == "procA::loop", [(i.get("label"), i.get("target_symbol")) for i in ins])
bp = e.ok("set_breakpoint", {"address": "procB::loop"})
check("set_breakpoint by scope path", bp.get("address") == "0x%04X" % PROCB_LOOP and bp.get("symbol") == "procB::loop", bp)
e.ok("set_registers", {"pc": "start", "status": 0x24})
r = e.ok("continue", {"wait": True})
check("stop result names the scoped label", r["stopped"] == "breakpoint" and r.get("symbol") == "procB::loop", r)
check("get_registers names it too", e.ok("get_registers").get("symbol") == "procB::loop")
e.ok("clear_breakpoint", {"all": True})
w = e.ok("set_watchpoint", {"address": "table", "access": "read"})
e.ok("clear_watchpoint", {"all": True})
check("set_watchpoint accepts a label", w.get("address") == "0x%04X" % TABLE, w)
ls = e.ok("list_symbols", {"sort": "address", "limit": 100})
names = [s["name"] for s in ls["symbols"]]
check("list_symbols shows scoped and address-qualified names",
      "procA::loop" in names and "outer::inner::loop" in names and "done@$%04X" % DONE_A in names, names)
check("list_symbols prefix matches the scope path", [s["name"] for s in e.ok("list_symbols", {"prefix": "outer::"})["symbols"]] == ["outer::inner", "outer::inner::loop"])

# profile: every scoped label is its own entry
e.ok("set_registers", {"pc": "start", "status": 0x24})
e.ok("set_breakpoint", {"address": "done@$%04X" % DONE_A})
e.ok("profile_start")
e.ok("continue", {"wait": True})
e.ok("profile_stop")
e.ok("clear_breakpoint", {"all": True})
pr = e.ok("profile_report", {"limit": 100})
got = sorted(x["symbol"] for x in pr["entries"])
check("profile_report names the scoped labels", got == ["outer", "outer::inner", "outer::inner::loop", "procA", "procA::loop", "procB", "procB::loop", "start"], got)
pa = e.ok("profile_report", {"by": "address", "within": "procB::loop", "sort": "address"})
check("profile_report within a scope path", [x["address"] for x in pa["entries"]] == ["0x%04X" % a for a in (PROCB_LOOP, PROCB_LOOP + 1, PROCB_LOOP + 3)], pa["entries"])

# ---- the label file on top: merged, scoped names kept ----
r = e.ok("load_symbols", {"path": "scoped.sym"})
check("the label file (14 lines, procA and procB twice) merges into the debug info", r["format"] == "labels" and r["loaded"] == 14 and r["merged"] == 14 and r["total"] == 13, r)
check("scoped names survive", name_of(PROCA_LOOP) == "procA::loop" and name_of(INNER_LOOP) == "outer::inner::loop")

# ---- the label file alone ----
r = e.ok("load_symbols", {"path": "scoped.sym", "replace": True})
check("replace with the label file: its repeated labels are kept once", r["total"] == 12 and r["scopes"] == 0 and r["merged"] == 2, r)
check("without scopes, a shared name is shown with its address", name_of(PROCA_LOOP) == "loop@$%04X" % PROCA_LOOP, name_of(PROCA_LOOP))
err, t = e.call("lookup_symbol", {"name": "loop"}); check("and is ambiguous as input", err and "loop@$%04X" % PROCB_LOOP in t, t)
check("name@$XXXX works for the label file", addr_of("loop@$%04X" % INNER_LOOP) == INNER_LOOP)
check("unique names are unchanged", name_of(PROCA) == "procA" and name_of(TABLE) == "table")

# ---- errors ----
open(os.path.join(tmp, "v3.dbg"), "w").write("version\tmajor=3,minor=0\nsym\tid=0,name=\"x\",val=0x10,type=lab\n")
err, t = e.call("load_symbols", {"path": "v3.dbg"}); check("an unsupported debug info version is refused", err and "version" in t, t)
open(os.path.join(tmp, "empty.dbg"), "w").write("version\tmajor=2,minor=0\ninfo\tcsym=0\n")
err, t = e.call("load_symbols", {"path": "empty.dbg"}); check("debug info without labels is refused", err and "no labels" in t, t)
check("a refused file leaves the symbols as they were", e.ok("list_symbols")["loaded"] == 12)
r = e.ok("load_symbols", {"path": "scoped.dbg", "replace": True})
check("replace with debug info", r["total"] == 13 and r["scopes"] == 4, r)

e.close()
finish()

"""cc65 debug info (ld65 --dbgfile): scope-path names in every result, scope paths and name@$XXXX as inputs,
ambiguous names, merging with the label file, and the label file alone. Uses tests/mcp/fixtures/scoped
(four modules: two with scopes, one with macros and .repeat, one in C; rebuild with its build.sh). Also the
source positions (file/line, macro_file/macro_line) in results."""
import json, os, shutil, sys, tempfile
from mcp_testlib import *

here = os.path.dirname(os.path.abspath(__file__))
tmp = os.path.realpath(tempfile.mkdtemp(prefix="x16mcp-dbg-"))
for f in ("scoped.dbg", "scoped.sym", "scoped.bin"):
    shutil.copy(os.path.join(here, "fixtures", "scoped", f), tmp)
# Addresses in the fixture (see a.s, b.s and scoped.sym)
START, DONE_A, PROCA, PROCA_LOOP, OUTER, INNER, INNER_LOOP, PROCB, PROCB_LOOP, DONE_B, TABLE = \
    0x400, 0x409, 0x40C, 0x40E, 0x412, 0x416, 0x418, 0x41C, 0x420, 0x424, 0x425
# c.s (macros at $428): the nested macro's four instructions, the single macro's clc, the .repeat's three inx
NESTED, ADD2_CLC, REPEAT = (0x42A, 0x42B, 0x42D, 0x42E), 0x430, 0x433
# d.c (_count3 at $437): an instruction of each C line, 6, 7, 8 (two) and 9
C_LINES = (0x437, 0x43A, 0x43D, 0x43F, 0x446)

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
check("labels, the address constant and the imported constant are loaded", r["loaded"] == 16 and r["total"] == 16, r)
check("plain constants and import records are skipped", r["skipped_symbols"] == 3, r)
check("one address range per named scope (procA, outer, inner, procB, macros, _count3)", r["scopes"] == 6 and r["merged"] == 0, r)

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

# profile by scope: a routine with its inner labels is one entry
sc = e.ok("profile_report", {"by": "scope", "limit": 100})
ent = {x["scope"] if x["scope"] else "label:" + x["symbol"]: x for x in sc["entries"]}
check("by scope: one entry per routine (innermost), top-level code by label",
      sorted(ent) == ["label:start", "outer", "outer::inner", "procA", "procB"], sorted(ent))
check("by scope: an entry gives the scope's start address", ent["procA"]["address"] == "0x%04X" % PROCA and ent["outer::inner"]["address"] == "0x%04X" % INNER, ent["procA"])
bysym = {x["symbol"]: x for x in pr["entries"]}
check("by scope: a routine adds up its inner labels", ent["procA"]["cycles"] == bysym["procA"]["cycles"] + bysym["procA::loop"]["cycles"]
      and ent["procA"]["instructions"] == bysym["procA"]["instructions"] + bysym["procA::loop"]["instructions"], (ent["procA"], bysym["procA"], bysym["procA::loop"]))
check("by scope: the nested scope is apart from its parent (innermost)", ent["outer"]["cycles"] == bysym["outer"]["cycles"]
      and ent["outer::inner"]["cycles"] == bysym["outer::inner"]["cycles"] + bysym["outer::inner::loop"]["cycles"])
check("by scope: code outside every scope has scope null and its label", ent["label:start"]["scope"] is None and ent["label:start"]["cycles"] == bysym["start"]["cycles"])
check("by scope: entries add up to instruction_cycles", sum(x["cycles"] for x in sc["entries"]) == sc["instruction_cycles"])
check("by scope: highest cycles first", [x["cycles"] for x in sc["entries"]] == sorted((x["cycles"] for x in sc["entries"]), reverse=True))
so = e.ok("profile_report", {"by": "scope", "scope_level": "outermost", "limit": 100})
ento = {x["scope"] if x["scope"] else "label:" + x["symbol"]: x for x in so["entries"]}
check("scope_level outermost rolls nested scopes up", sorted(ento) == ["label:start", "outer", "procA", "procB"]
      and ento["outer"]["cycles"] == ent["outer"]["cycles"] + ent["outer::inner"]["cycles"], sorted(ento))
wa = e.ok("profile_report", {"by": "address", "within": "procA", "sort": "address"})
check("within a scope path: the whole routine, inner labels included",
      [x["address"] for x in wa["entries"]] == ["0x%04X" % a for a in (PROCA, PROCA + 2, PROCA + 3, PROCA + 5)], wa["entries"])
wo = e.ok("profile_report", {"by": "address", "within": "outer"})
check("within a scope path includes nested scopes", sorted(x["symbol"].split("+")[0] for x in wo["entries"]) ==
      sorted(["outer", "outer", "outer::inner", "outer::inner::loop", "outer::inner::loop", "outer::inner::loop"]), [x["symbol"] for x in wo["entries"]])
wl = e.ok("profile_report", {"by": "address", "within": "procA::loop", "sort": "address"})
check("within a label that is not a scope: that label's addresses", [x["address"] for x in wl["entries"]] == ["0x%04X" % a for a in (PROCA_LOOP, PROCA_LOOP + 1, PROCA_LOOP + 3)], wl["entries"])
err, t = e.call("profile_report", {"scope_level": "outermost"}); check("scope_level needs by scope", err and "scope_level" in t, t)
err, t = e.call("profile_report", {"by": "scope", "scope_level": "x"}); check("scope_level must be innermost or outermost", err and "scope_level" in t, t)

# ---- source positions ----
def src(addr):
    r = e.ok("lookup_symbol", {"address": addr})
    return (r.get("file"), r.get("line"), r.get("macro_file"), r.get("macro_line"))
check("load_symbols counts the source lines", e.ok("load_symbols", {"path": "scoped.dbg"})["source_lines"] == 49)
check("plain code: file and line, no macro fields", src(PROCA_LOOP) == ("a.s", 16, None, None) and src(PROCB_LOOP) == ("b.s", 9, None, None),
      (src(PROCA_LOOP), src(PROCB_LOOP)))
check("data: the line that defines it", src(TABLE + 2) == ("b.s", 14, None, None), src(TABLE + 2))
check("a macro: the call site, and the line in the macro body", src(ADD2_CLC) == ("c.s", 18, "c.s", 5) and src(ADD2_CLC + 1) == ("c.s", 18, "c.s", 6),
      (src(ADD2_CLC), src(ADD2_CLC + 1)))
check("nested macros: the outermost call site and the innermost body line",
      [src(a) for a in NESTED] == [("c.s", 17, "c.s", 5), ("c.s", 17, "c.s", 6), ("c.s", 17, "c.s", 5), ("c.s", 17, "c.s", 6)], [src(a) for a in NESTED])
check(".repeat: the .endrepeat line (as ca65 records it) and the body line", [src(REPEAT + i) for i in range(3)] == [("c.s", 21, "c.s", 20)] * 3,
      [src(REPEAT + i) for i in range(3)])
check("after the macros: plain again", src(REPEAT + 3) == ("c.s", 22, None, None), src(REPEAT + 3))
check("C compiled by cc65: the C source line, not the generated .s line",
      [src(a)[:2] for a in C_LINES] == [("d.c", 6), ("d.c", 7), ("d.c", 8), ("d.c", 8), ("d.c", 9)], [src(a) for a in C_LINES])
r = e.ok("lookup_symbol", {"address": 0x500})
check("an address no line covers has no file", "file" not in r and "line" not in r, r)
r = e.ok("lookup_symbol", {"name": "macros"})
check("lookup_symbol by name gives the source position too", r.get("file") == "c.s" and r.get("line") == 16, r)
pa = e.ok("profile_report", {"by": "address", "within": "procB::loop", "sort": "address"})
check("profile_report by address gives file and line", [(x.get("file"), x.get("line")) for x in pa["entries"]] == [("b.s", 9), ("b.s", 10), ("b.s", 11)], pa["entries"])
e.ok("set_breakpoint", {"address": NESTED[1]})
e.ok("set_registers", {"pc": "macros", "status": 0x24})
r = e.ok("continue", {"wait": True})
check("a stop result gives the source position", (r.get("file"), r.get("line"), r.get("macro_file"), r.get("macro_line")) == ("c.s", 17, "c.s", 6), r)
g = e.ok("get_registers")
check("get_registers too", (g.get("file"), g.get("line"), g.get("macro_line")) == ("c.s", 17, 6), g)
e.ok("clear_breakpoint", {"all": True})
e.ok("set_watchpoint", {"address": "_counter", "access": "write"})
e.ok("set_registers", {"pc": "_count3", "status": 0x24})
r = e.ok("continue", {"wait": True})
check("a watchpoint stop gives the accessing instruction's position too",
      r.get("stopped") == "watchpoint" and (r.get("accessing_pc_file"), r.get("accessing_pc_line")) == ("d.c", 6)
      and (r.get("file"), r.get("line")) == ("d.c", 7), r)
e.ok("clear_watchpoint", {"all": True})

# ---- the label file on top: merged, scoped names kept ----
r = e.ok("load_symbols", {"path": "scoped.sym"})
check("the label file (20 lines, five labels twice) merges into the debug info", r["format"] == "labels" and r["loaded"] == 20 and r["merged"] == 20 and r["total"] == 16, r)
check("scoped names survive", name_of(PROCA_LOOP) == "procA::loop" and name_of(INNER_LOOP) == "outer::inner::loop")

# ---- the label file alone ----
r = e.ok("load_symbols", {"path": "scoped.sym", "replace": True})
check("replace with the label file: its repeated labels are kept once", r["total"] == 15 and r["scopes"] == 0 and r["merged"] == 5 and r["source_lines"] == 0, r)
check("without scopes, a shared name is shown with its address", name_of(PROCA_LOOP) == "loop@$%04X" % PROCA_LOOP, name_of(PROCA_LOOP))
err, t = e.call("lookup_symbol", {"name": "loop"}); check("and is ambiguous as input", err and "loop@$%04X" % PROCB_LOOP in t, t)
check("name@$XXXX works for the label file", addr_of("loop@$%04X" % INNER_LOOP) == INNER_LOOP)
check("unique names are unchanged", name_of(PROCA) == "procA" and name_of(TABLE) == "table")

err, t = e.call("profile_report", {"by": "scope"}); check("by scope without debug info says to load it", err and "debug info" in t, t)

# ---- errors ----
open(os.path.join(tmp, "v3.dbg"), "w").write("version\tmajor=3,minor=0\nsym\tid=0,name=\"x\",val=0x10,type=lab\n")
err, t = e.call("load_symbols", {"path": "v3.dbg"}); check("an unsupported debug info version is refused", err and "version" in t, t)
open(os.path.join(tmp, "empty.dbg"), "w").write("version\tmajor=2,minor=0\ninfo\tcsym=0\n")
err, t = e.call("load_symbols", {"path": "empty.dbg"}); check("debug info without labels is refused", err and "no labels" in t, t)
check("a refused file leaves the symbols as they were", e.ok("list_symbols")["loaded"] == 15)
r = e.ok("load_symbols", {"path": "scoped.dbg", "replace": True})
check("replace with debug info", r["total"] == 16 and r["scopes"] == 6, r)

e.close()
finish()

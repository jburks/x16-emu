"""profile_start / profile_stop / profile_report: exact per-address cycles and counts, symbol grouping, filters, paging,
file output, accumulation, WAI and interrupt accounting."""
import json, os, sys, tempfile
from mcp_testlib import *

tmp = os.path.realpath(tempfile.mkdtemp(prefix="x16mcp-prof-"))
e = Emu(["-zeroram"], env={"X16EMU_MCP_ROOT": tmp})
names = [t["name"] for t in e.rpc("tools/list")["result"]["tools"]]
check("profile tools listed", all(n in names for n in ("profile_start", "profile_stop", "profile_report")), names)
err, t = e.call("profile_report"); check("report before any profile_start is an error", err and "profile_start" in t, t)
err, t = e.call("profile_stop"); check("stop before any profile_start is an error", err and "profile_start" in t, t)
check("get_status: profiling false", e.status().get("profiling") is False, e.status())
check("boots to READY.", wait_screen(e, "READY."))
e.ok("pause")

def report(**kw):
    return e.ok("profile_report", kw)

# ---- a run with interrupts at the BASIC prompt: the totals add up to the window exactly ----
s0 = e.status()["cycles"]
st = e.ok("profile_start")
check("profile_start result", st["profiling"] is True and st["reset"] is True and st["cycles_at_start"] == s0, st)
check("get_status: profiling true", e.status()["profiling"] is True)
e.ok("run_frames", {"frames": 3})
s1 = e.status()["cycles"]
sp = e.ok("profile_stop")
check("stop: window = cycles elapsed", sp["cycles"] == s1 - s0, (sp, s1 - s0))
check("stop: instruction + wait + other + unmapped = window",
      sp["instruction_cycles"] + sp["wait_cycles"] + sp["other_cycles"] + sp["unmapped_cycles"] == sp["cycles"], sp)
check("interrupt entries show up as other cycles, 7 each", sp["other_cycles"] > 0 and sp["other_cycles"] % 7 == 0, sp)
check("instructions were counted", sp["instructions"] > 1000 and sp["profiling"] is False, sp)
check("get_status: profiling false after stop", e.status()["profiling"] is False)
full = report(limit=1000)
check("without symbols, cycles are grouped per region", full["entries"] and all(x["symbol"] is None and "region" in x for x in full["entries"]), full["entries"][:3])
check("the KERNAL's ROM bank 0 is a region", any(x["region"] == "ROM bank 0" and x["bank"] == 0 for x in full["entries"]), [x["region"] for x in full["entries"]])
check("entries add up to instruction_cycles", sum(x["cycles"] for x in full["entries"]) == full["instruction_cycles"], full)
check("entries add up to the instruction count", sum(x["instructions"] for x in full["entries"]) == full["instructions"])
ad = report(by="address", limit=1000)
check("address view: cycles descending", [x["cycles"] for x in ad["entries"]] == sorted((x["cycles"] for x in ad["entries"]), reverse=True))
check("address view: ROM entries carry their bank and text", all("text" in x for x in ad["entries"]) and any(x["bank"] == 0 for x in ad["entries"]), ad["entries"][:2])
e.ok("run_frames", {"frames": 2})
check("stopped profiler counts nothing more", report()["instructions"] == sp["instructions"])

# ---- a routine with known costs ----
#  0400 A2 0A     LDX #$0A     2
#  0402 20 10 04  JSR $0410    6   x10
#  0405 CA        DEX          2   x10
#  0406 D0 FA     BNE $0402    3 taken (x9), 2 not taken (x1)
#  0408 4C 08 04  JMP $0408        (breakpoint: not executed)
#  0410 A9 01     LDA #$01     2   x10
#  0412 85 20     STA $20      3   x10
#  0414 60        RTS          6   x10
e.ok("write_memory", {"address": 0x400, "hex": "A2 0A 20 10 04 CA D0 FA 4C 08 04"})
e.ok("write_memory", {"address": 0x410, "hex": "A9 01 85 20 60"})
e.ok("set_breakpoint", {"address": 0x408})
WANT = [(0x400, 1, 2), (0x402, 10, 60), (0x405, 10, 20), (0x406, 10, 29), (0x410, 10, 20), (0x412, 10, 30), (0x414, 10, 60)]

def run_routine():
    e.ok("set_registers", {"pc": 0x400, "status": 0x24})  # interrupts off: nothing else runs
    r = e.ok("continue", {"wait": True})
    return r

st = e.ok("profile_start")
r = run_routine()
check("routine stopped at the breakpoint", r["stopped"] == "breakpoint" and r["pc"] == "0x0408", r)
sp = e.ok("profile_stop")
check("window is exactly the routine's 221 cycles", sp["cycles"] == 221 and sp["instruction_cycles"] == 221, sp)
check("no wait or other cycles with interrupts off", sp["wait_cycles"] == 0 and sp["other_cycles"] == 0 and sp["unmapped_cycles"] == 0, sp)
check("61 instructions", sp["instructions"] == 61, sp)
check("cycles_at_start + window = cycles now", st["cycles_at_start"] + 221 == e.ok("get_registers")["cycles"])
ad = report(by="address", sort="address")
got = [(int(x["address"], 16), x["count"], x["cycles"]) for x in ad["entries"]]
check("per-address counts and cycles are exact", got == WANT, got)
txt = {x["address"]: x["text"] for x in ad["entries"]}
check("text: the instruction at each address", txt.get("0x0412") == "STA $20" and txt.get("0x0406") == "BNE $0402" and txt.get("0x0402") == "JSR $0410", txt)
check("low memory has bank null", all(x["bank"] is None for x in ad["entries"]))
check("percent of the window", abs([x for x in ad["entries"] if x["address"] == "0x0402"][0]["percent"] - 100.0 * 60 / 221) < 0.01)
ac = report(by="address")
check("default sort is by cycles, ties by address", [x["address"] for x in ac["entries"]][:3] == ["0x0402", "0x0414", "0x0412"], [x["address"] for x in ac["entries"]])
check("no symbol field without symbols", all("symbol" not in x for x in ac["entries"]))
sy = report()
check("symbol view without symbols: one low-memory group", len(sy["entries"]) == 1 and sy["entries"][0]["region"] == "low memory" and sy["entries"][0]["cycles"] == 221, sy["entries"])

sym = os.path.join(tmp, "p.sym")
open(sym, "w").write("al 000400 .main\nal 000410 .sub\nal 000408 .__FAKE_LINKER__\n")
e.ok("load_symbols", {"path": sym})
sy = report()
ent = [(x["symbol"], x["address"], x["cycles"], x["instructions"], x["percent"]) for x in sy["entries"]]
check("symbol view: main and sub with their own cycles, highest first", ent == [("main", "0x0400", 111, 31, 50.23), ("sub", "0x0410", 110, 30, 49.77)], ent)
w = report(by="address", within="sub", sort="address")
check("within: only the label's addresses, named name+0xN", [(x["address"], x["symbol"]) for x in w["entries"]] == [("0x0410", "sub"), ("0x0412", "sub+0x2"), ("0x0414", "sub+0x4")], w["entries"])
check("within: entries_total counts the filtered entries", w["entries_total"] == 3)
f = report(by="address", sort="address", **{"from": "sub", "to": "sub+2"})
check("from/to: inclusive range, labels allowed", [x["address"] for x in f["entries"]] == ["0x0410", "0x0412"], f["entries"])
f = report(**{"from": 0x400, "to": 0x407})
check("from/to in the symbol view", [x["symbol"] for x in f["entries"]] == ["main"], f["entries"])
p = report(by="address", limit=2)
check("paging: limit", p["returned"] == 2 and p["entries_total"] == 7 and p["offset"] == 0, {k: v for k, v in p.items() if k != "entries"})
p = report(by="address", limit=2, offset=6)
check("paging: offset", p["returned"] == 1 and p["offset"] == 6 and p["entries"][0]["address"] == "0x0400", p)
p = report(by="address", offset=50)
check("paging: offset past the end returns nothing", p["returned"] == 0 and p["entries"] == [])
check("totals repeated in every report", p["cycles"] == 221 and p["instruction_cycles"] == 221)

out = os.path.join(tmp, "prof.json")
r = report(by="address", limit=1, path="prof.json")
check("path: totals and byte count returned, no entries", "entries" not in r and r["entries_total"] == 7 and r["path"] == out and r["bytes"] == os.path.getsize(out), r)
d = json.load(open(out))
check("path: the file holds every entry, ignoring limit", len(d["entries"]) == 7 and d["returned"] == 7 and d["cycles"] == 221, {k: v for k, v in d.items() if k != "entries"})
err, t = e.call("profile_report", {"path": "prof.json"}); check("path: an existing file is not replaced", err and "exists" in t, t)
r = report(path="prof.json", overwrite=True)
check("path: replaced with overwrite", json.load(open(out))["by"] == "symbol" and r["bytes"] == os.path.getsize(out))
err, t = e.call("profile_report", {"path": "/etc/x16prof.json"}); check("path: outside the host path root is refused", err, t)

# ---- argument errors ----
for args, word in [({"by": "x"}, "by"), ({"sort": "x", "by": "address"}, "sort"), ({"sort": "address"}, "sort"), ({"limit": 0}, "limit"),
                   ({"from": 0x500, "to": 0x400}, "from"), ({"bank": 300}, "bank"), ({"within": ""}, "within"), ({"from": "nosuch"}, "unknown symbol")]:
    err, t = e.call("profile_report", args)
    check("bad argument rejected: %s" % json.dumps(args), err and word in t, t)
err, t = e.call("profile_start", {"reset": 1}); check("profile_start: reset must be boolean", err and "reset" in t, t)

# ---- accumulation with reset false, and nothing counted while stopped ----
e.ok("profile_start", {"reset": False})
run_routine()
sp = e.ok("profile_stop")
check("reset false adds to the counts", sp["cycles"] == 442 and sp["instruction_cycles"] == 442 and sp["instructions"] == 122, sp)
check("per-address counts doubled", [(int(x["address"], 16), x["count"]) for x in report(by="address", sort="address")["entries"]] == [(a, 2 * c) for a, c, _ in WANT])
run_routine()
check("a run while stopped is not counted", report()["cycles"] == 442)
e.ok("profile_start")
check("profile_start resets by default", report()["cycles"] == 0 and report()["entries_total"] == 0)
run_routine()
check("a running profile can be reported", report()["profiling"] is True and report()["instruction_cycles"] == 221)
e.ok("profile_start", {"reset": False})  # restarting while on keeps the window continuous
run_routine()
check("profile_start reset false while running keeps counting", e.ok("profile_stop")["cycles"] == 442)

# ---- WAI: waiting cycles are counted apart ----
e.ok("clear_breakpoint", {"all": True})
e.ok("write_memory", {"address": 0x420, "hex": "CB 4C 21 04"})  # WAI ; JMP $0421
e.ok("set_breakpoint", {"address": 0x421})
e.ok("write_memory", {"address": 0x9F27, "hex": "FF"})  # acknowledge VERA interrupts, so WAI waits for the next one
e.ok("set_registers", {"pc": 0x420, "status": 0x24})
e.ok("profile_start")
r = e.ok("continue", {"wait": True})
sp = e.ok("profile_stop")
check("WAI woke up and reached the breakpoint", r["stopped"] == "breakpoint" and r["pc"] == "0x0421", r)
check("WAI: one instruction, its cycles apart from the wait", sp["instructions"] == 1 and 0 < sp["instruction_cycles"] < 10, sp)
check("WAI: the wait for the interrupt is in wait_cycles", sp["wait_cycles"] > 1000 and sp["other_cycles"] == 0, sp)
check("WAI: totals add up", sp["instruction_cycles"] + sp["wait_cycles"] == sp["cycles"], sp)
check("WAI: its own cycles are at its address", [(x["address"], x["count"]) for x in report(by="address")["entries"]] == [("0x0420", 1)])

e.close()
finish()

"""Profiler call tracking (profile_start calls: true, profile_report by "routine"): nested calls, recursion,
a dropped return address, an RTS jump table, tail calls (with debug info) and interrupts. Uses
tests/mcp/fixtures/calls (rebuild with its build.sh)."""
import os, shutil, tempfile
from mcp_testlib import *

here = os.path.dirname(os.path.abspath(__file__))
tmp = os.path.realpath(tempfile.mkdtemp(prefix="x16mcp-calls-"))
for f in ("calls.dbg", "calls.sym", "calls.bin"):
    shutil.copy(os.path.join(here, "fixtures", "calls", f), tmp)

e = Emu(["-zeroram"], env={"X16EMU_MCP_ROOT": tmp})
check("boots to READY.", wait_screen(e, "READY."))
e.ok("pause")
e.ok("load_symbols", {"path": "calls.dbg"})
e.ok("load_prg", {"path": "calls.bin", "raw": True, "address": 0x400})

def run(calls=True):
    e.ok("set_registers", {"pc": "start", "status": 0x24})
    e.ok("set_breakpoint", {"address": "done"})
    e.ok("profile_start", {"calls": calls})
    r = e.ok("continue", {"wait": True, "timeout_ms": 20000})
    e.ok("clear_breakpoint", {"all": True})
    return r

err, t = e.call("profile_report", {"by": "routine"}); check("by routine before any profile is an error", err, t)
r = run(calls=False)
check("runs to done", r.get("stopped") == "breakpoint" and r.get("symbol") == "done", r)
err, t = e.call("profile_report", {"by": "routine"}); check("by routine without call tracking says how to start it", err and "calls: true" in t, t)
err, t = e.call("profile_start", {"calls": "yes"}); check("calls must be a boolean", err, t)

st = e.ok("profile_start", {"calls": True})
check("profile_start reports calls", st.get("calls") is True, st)
e.ok("profile_stop")
run()
rep = e.ok("profile_report", {"by": "routine", "limit": 100})
ct = rep.get("call_tracking", {})
R = {x["routine"]: x for x in rep["entries"]}
named = sorted(n for n in R if n)
check("routines: everything called (plus unnamed ROM routines the interrupt handler called)",
      named == sorted(["outer", "leaf", "rec", "mid", "dropper", "dispatch", "tailer", "waiter"]), named)
rom = [x for x in rep["entries"] if x["routine"] is None]
check("ROM routines called by the interrupt handler have callers from interrupt", rom and all(x["address"] >= "0xC000" for x in rom), rom[:3])
check("leaf: called twice by outer and once by the tail call (the RTS jump table is not a call)", R["leaf"]["calls"] == 3, R["leaf"])
check("leaf: 8 cycles a call (lda #, rts), min = max = 8", (R["leaf"]["cycles"], R["leaf"]["min_cycles"], R["leaf"]["max_cycles"], R["leaf"]["cycles_per_call"]) == (24, 8, 8, 8), R["leaf"])
check("self_cycles counts the routine's code however it was reached: leaf's 3 calls plus its run through the RTS jump table",
      R["leaf"]["self_cycles"] == 24 + 8, R["leaf"])
check("outer: its own cycles plus its callees'", R["outer"]["cycles"] == R["outer"]["self_cycles"] + 16, R["outer"])
check("recursion: 3 calls, cycles counted once (= its own instructions)", R["rec"]["calls"] == 3 and R["rec"]["cycles"] == R["rec"]["self_cycles"], R["rec"])
check("recursion: the outermost call is the longest", R["rec"]["max_cycles"] == R["rec"]["cycles"] and R["rec"]["min_cycles"] < R["rec"]["max_cycles"], R["rec"])
check("dropped return address: mid and dropper both end at dropper's rts", R["mid"]["calls"] == 1 and R["dropper"]["calls"] == 1 and "running" not in R["mid"]
      and R["mid"]["cycles"] == R["mid"]["self_cycles"] + R["dropper"]["cycles"], (R["mid"], R["dropper"]))
check("RTS jump table: dispatch includes the leaf code it reached", R["dispatch"]["calls"] == 1 and R["dispatch"]["cycles"] == R["dispatch"]["self_cycles"] + 8, R["dispatch"])
check("tail call: tailer ends at its jmp (only its own cycles)", R["tailer"]["cycles"] == R["tailer"]["self_cycles"], R["tailer"])
check("interrupts were taken during waiter", ct.get("interrupts", 0) > 0 and ct.get("interrupt_cycles", 0) > 0, ct)
check("interrupts are left out: waiter's cycles = its own instructions exactly", R["waiter"]["cycles"] == R["waiter"]["self_cycles"], R["waiter"])
check("tail_calls counted", ct.get("tail_calls") == 1, ct)
check("no frames left open at done", ct.get("open_frames") == 0 and ct.get("depth_overflows") == 0, ct)
check("max_depth: start -> rec -> rec -> rec is 3, an interrupt during waiter can add more", ct.get("max_depth", 0) >= 3, ct)
check("entries ordered by cycles, highest first", [x["cycles"] for x in rep["entries"]] == sorted((x["cycles"] for x in rep["entries"]), reverse=True))
check("entries carry address, bank, percent and source", R["leaf"]["address"] == "0x0417" and R["leaf"]["bank"] is None and "percent" in R["leaf"]
      and R["leaf"].get("file") == "calls.s", R["leaf"])

# one routine: callers and callees
d = e.ok("profile_report", {"routine": "leaf"})
lr = d["routines"][0]
callers = {c["routine"]: c for c in lr["callers"]}
check("routine leaf: callers outer (2 calls, 16 cycles) and tailer (1 call, 8)", set(callers) == {"outer", "tailer"}
      and (callers["outer"]["calls"], callers["outer"]["cycles"], callers["tailer"]["calls"], callers["tailer"]["cycles"]) == (2, 16, 1, 8), lr["callers"])
check("routine leaf: no callees", lr["callees"] == [], lr["callees"])
d = e.ok("profile_report", {"routine": "mid"})
check("routine mid: called from outside any routine, calls dropper", d["routines"][0]["callers"][0].get("from") == "outside"
      and [c["routine"] for c in d["routines"][0]["callees"]] == ["dropper"], d["routines"][0])
d = e.ok("profile_report", {"routine": 0x421})
check("routine by address; rec calls itself", d["routines"][0]["routine"] == "rec" and [c["routine"] for c in d["routines"][0]["callees"]] == ["rec"]
      and {c.get("routine") for c in d["routines"][0]["callers"]} == {None, "rec"}, d["routines"][0])
err, t = e.call("profile_report", {"routine": "done"}); check("a label that was never called is an error", err and "no call" in t, t)
err, t = e.call("profile_report", {"by": "address", "routine": "leaf"}); check("routine needs by routine", err, t)
err, t = e.call("profile_report", {"by": "address", "sort": "calls"}); check("sort calls needs by routine", err, t)
r = e.ok("profile_report", {"by": "routine", "sort": "calls"})
check("sort calls: most calls first", [x["calls"] for x in r["entries"]] == sorted((x["calls"] for x in r["entries"]), reverse=True), [x["calls"] for x in r["entries"]])
r = e.ok("profile_report", {"by": "routine", "from": "mid", "to": "dispatch"})
check("from/to filter the routines", sorted(x["routine"] for x in r["entries"]) == ["dispatch", "dropper", "mid"], [x["routine"] for x in r["entries"]])
r = e.ok("profile_report", {"by": "routine", "path": "routines.json"})
check("by routine to a file", r.get("bytes", 0) > 0 and os.path.exists(os.path.join(tmp, "routines.json")), r)
e.ok("profile_stop")

# without debug info there are no scopes, so no tail calls: leaf runs inside tailer's frame
e.ok("load_symbols", {"path": "calls.sym", "replace": True})
run()
rep = e.ok("profile_report", {"by": "routine", "limit": 100})
R = {x["routine"]: x for x in rep["entries"]}
check("without debug info: no tail calls; leaf is called twice", rep["call_tracking"]["tail_calls"] == 0 and R["leaf"]["calls"] == 2, (rep["call_tracking"], R["leaf"]))
check("...and tailer includes the leaf code it jumped to", R["tailer"]["cycles"] == R["tailer"]["self_cycles"] + 8, R["tailer"])

# a jump into a nested .proc (and a loop back to its start) stays within the routine
e.ok("load_symbols", {"path": "calls.dbg", "replace": True})
e.ok("set_registers", {"pc": "nest_start", "status": 0x24})
e.ok("set_breakpoint", {"address": "nest_done"})
e.ok("profile_start", {"calls": True})
e.ok("continue", {"wait": True})
e.ok("clear_breakpoint", {"all": True})
r = e.ok("profile_report", {"by": "routine"})
check("a jump into a nested .proc is not a tail call", r["call_tracking"]["tail_calls"] == 0 and [x["routine"] for x in r["entries"]] == ["nestjump"], r)
e.ok("profile_stop")

# profile_stop ends calls still running; reset clears call data
e.ok("set_breakpoint", {"address": "leaf+2"})      # leaf's rts, inside start -> outer -> leaf
e.ok("set_registers", {"pc": "start", "status": 0x24})
e.ok("profile_start", {"calls": True})
e.ok("continue", {"wait": True})
r = e.ok("profile_report", {"by": "routine"})
R = {x["routine"]: x for x in r["entries"]}
check("calls still running are counted up to now", r["call_tracking"]["open_frames"] == 2 and R["leaf"].get("running") == 1 and R["outer"].get("running") == 1, r)
e.ok("clear_breakpoint", {"all": True})
s = e.ok("profile_stop")
check("profile_stop ends them", s["call_tracking"]["open_frames"] == 0, s)
e.ok("profile_start", {"calls": False})
err, t = e.call("profile_report", {"by": "routine"}); check("profile_start with reset and without calls clears the call data", err and "calls: true" in t, t)
e.ok("profile_stop")

e.close()
finish()

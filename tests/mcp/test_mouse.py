"""mouse_move, mouse_button, mouse_wheel."""
import json, sys, time
from mcp_testlib import *

def mouse(e):
    e.ok("type_text", {"text": "PRINT MX;MY;MB\n", "wait": True, "timeout_ms": 20000})
    t0 = time.time()
    while time.time() - t0 < 10:
        L = [l for l in screen_lines(e) if l.strip()]
        for i, l in enumerate(L):
            if l.strip() == "PRINT MX;MY;MB" and i + 1 < len(L) and L[i+1].strip() and L[i+1].strip() != "READY.":
                v = L[i+1].split()
                if len(v) == 3: e.ok("type_text", {"text": "\n", "wait": True}); return tuple(int(x) for x in v)
        time.sleep(0.2)
    return None
def clear(e): e.ok("type_text", {"text": "PRINT CHR$(147)\n", "wait": True, "timeout_ms": 20000}); time.sleep(0.3)

e = Emu(["-zeroram"])
names = [t["name"] for t in e.rpc("tools/list")["result"]["tools"]]
for t in ("mouse_move", "mouse_button", "mouse_wheel"): check("tool listed: " + t, t in names)
check("boots to READY.", wait_screen(e, "READY."))
e.ok("type_text", {"text": "MOUSE 1\n", "wait": True, "timeout_ms": 20000})
r = e.ok("mouse_move", {"x": 100, "y": 50}); check("absolute move reports consumed", r["moved"] and r["consumed"], r)
check("absolute (100,50)", mouse(e)[:2] == (100, 50))
clear(e)
e.ok("mouse_move", {"dx": 30, "dy": -10}); check("relative +30,-10 -> (130,40)", mouse(e)[:2] == (130, 40))
clear(e)
e.ok("mouse_move", {"dx": -500}); check("relative move past the edge clamps at x=0", mouse(e)[:2] == (0, 40))
clear(e)
e.ok("mouse_move", {"x": 600, "y": 400}); check("long absolute move (600,400)", mouse(e)[:2] == (600, 400))
clear(e)
e.ok("mouse_move", {"dx": 0, "dy": 0}); check("zero move is fine", True)
for b, mask in (("left", 1), ("right", 2), ("middle", 4)):
    e.ok("mouse_button", {"button": b, "action": "down"}); clear(e)
    v = mouse(e); check("%s down -> MB=%d" % (b, mask), v and v[2] == mask, v)
    e.ok("mouse_button", {"button": b, "action": "up"}); clear(e)
    v = mouse(e); check("%s up -> MB=0" % b, v and v[2] == 0, v)
r = e.ok("mouse_button", {}); check("default tap (left) completes", r["consumed"] is True, r)
r = e.ok("mouse_button", {"hold_frames": 6}); check("tap with hold_frames 6 completes", r["consumed"] is True, r)
v = mouse(e); check("buttons released afterwards", v and v[2] == 0, v)
e.ok("mouse_button", {"button": "left", "action": "down"})
e.ok("mouse_move", {"dx": 40, "dy": 20}); clear(e)
v = mouse(e); check("drag: button held while moving: MB=1 and x moved right", v and v[2] == 1 and v[0] > 600, v)
e.ok("mouse_button", {"button": "left", "action": "up"})
r = e.call("mouse_wheel", {"notches": 3}); check("wheel scroll is delivered", not r[0] and "scrolled" in r[1], r)
r = e.call("mouse_wheel", {"notches": -20}); check("long wheel scroll (3 packets) is delivered", not r[0] and "consumed\":true" in r[1], r)
# argument validation
for label, a in (("neither", {}), ("both", {"dx": 1, "x": 1}), ("only x", {"x": 5}), ("x too big", {"x": 5000, "y": 0}),
                 ("dx too big", {"dx": 5000}), ("float dx", {"dx": 1.5}), ("string dx", {"dx": "3"}), ("negative abs", {"x": -1, "y": 0})):
    err, t = e.call("mouse_move", a); check("mouse_move rejects " + label, err, t)
for label, a in (("button", {"button": "side"}), ("action", {"action": "wiggle"}), ("hold", {"hold_frames": -1})):
    err, t = e.call("mouse_button", a); check("mouse_button rejects " + label, err, t)
for label, a in (("zero", {"notches": 0}), ("missing", {}), ("huge", {"notches": 5000})):
    err, t = e.call("mouse_wheel", a); check("mouse_wheel rejects " + label, err, t)
# paused behaviour
e.ok("pause")
err, t = e.call("mouse_button", {"hold_frames": 3}); check("paused: tap with hold_frames is refused", err and "running" in t, t)
r = e.ok("mouse_move", {"dx": 10, "dy": 0}); check("paused: small move is queued, consumed=false", r["consumed"] is False, r)
r = e.ok("mouse_button"); check("paused: default tap queues press+release", r.get("queued") is True, r)
err, t = e.call("mouse_move", {"dx": 2000}); check("paused: oversized move fails clearly", err and "paused" in t, t)
e.ok("continue"); time.sleep(1.0)
check("emulator exits cleanly", e.close() == 0)

# mouse not enabled by the program: nothing consumes the packets
e = Emu(["-zeroram"])
check("boots again", wait_screen(e, "READY."))
e.ok("type_text", {"text": "MOUSE 0\n", "wait": True, "timeout_ms": 20000})
t0 = time.time(); err, t = e.call("mouse_move", {"dx": 2000, "dy": 0}); dt = time.time() - t0
check("mouse disabled: long move fails (not consumed) instead of hanging (%.1fs)" % dt, (err and "not consuming" in t) or "consumed\":false" in t, t)
check("emulator still responsive", e.status()["frames"] > 0)
e.close()

finish()

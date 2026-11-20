"""Host path restriction: the default root, -fsroot, X16EMU_MCP_ROOT, X16EMU_MCP_UNRESTRICTED, drive roots, symlinks."""
import json, os, struct, sys, tempfile, time
from mcp_testlib import *

base = os.path.realpath(tempfile.mkdtemp(prefix="x16mcp-paths-"))
proj = os.path.join(base, "proj"); sub = os.path.join(proj, "sub"); outside = os.path.join(base, "outside")
evil = os.path.join(base, "proj-evil"); other = os.path.join(base, "other")
for dd in (proj, sub, outside, evil, other, os.path.join(other, 'sub')): os.makedirs(dd)
os.symlink(outside, os.path.join(proj, "link"))                 # a link inside the root that points outside it
prg = struct.pack("<H", 0x0400) + bytes.fromhex("A92A8D000560")
open(os.path.join(proj, "ok.prg"), "wb").write(prg)
open(os.path.join(outside, "far.prg"), "wb").write(prg)

def start(extra=(), env=None, cwd=proj):
    err = open(os.path.join(base, "stderr-%d.txt" % start.n), "w+"); start.n += 1
    e = Emu(["-zeroram", *extra], env=env, cwd=cwd, stderr=err)
    e.errfile = err
    return e
start.n = 0
def stderr_of(e):
    e.close(); e.errfile.seek(0); return e.errfile.read()
def dump(e, path, **kw):
    return e.call("read_memory", dict({"address": 0x400, "count": 4, "path": path}, **kw))
def refused(res): return res[0] and ("outside" in res[1] or "disabled" in res[1])

# ---------------- default root: the working directory ----------------
e = start()
st = e.status()
check("default: host_paths is restricted and host_root is the working directory", st["host_paths"] == "restricted" and st["host_root"] == proj, st)
r = dump(e, "dump.bin"); check("a relative path is written inside the root", not r[0] and os.path.exists(os.path.join(proj, "dump.bin")), r)
check("...and the result reports the absolute path", json.loads(r[1])["path"] == os.path.join(proj, "dump.bin"), r)
check("an absolute path inside the root works", not dump(e, os.path.join(sub, "a.bin"))[0] and os.path.exists(os.path.join(sub, "a.bin")))
check("'./' and 'sub/..' inside the root are fine", not dump(e, "./b.bin")[0] and not dump(e, "sub/../c.bin")[0] and os.path.exists(os.path.join(proj, "c.bin")))
for label, p in (("an absolute path outside", os.path.join(outside, "x.bin")), ("../ out of the root", "../outside/x.bin"),
                 ("sub/../.. out of the root", "sub/../../outside/y.bin"), ("an absolute path that climbs out", os.path.join(sub, "..", "..", "outside", "z.bin")),
                 ("a sibling whose name starts with the root's", os.path.join(evil, "x.bin")), ("/etc", "/etc/x16mcp-should-not-exist"),
                 ("the filesystem root", "/x16mcp-should-not-exist")):
    r = dump(e, p, overwrite=True); check("rejects " + label, refused(r), r)
check("nothing was created outside", not any(os.path.exists(os.path.join(dd, n)) for dd in (outside, evil) for n in ("x.bin", "y.bin", "z.bin"))
      and not os.path.exists("/etc/x16mcp-should-not-exist"))
r = dump(e, "x" * 5000); check("an over-long path is an error, not a crash", r[0], r[1][:80])
check("a path with ../ that stays inside is accepted (sub/../../proj/d.bin)", not dump(e, "sub/../../proj/d.bin")[0])
# load_prg and get_screen follow the same policy
r = e.call("load_prg", {"path": "ok.prg"}); check("load_prg: a file inside the root loads", not r[0], r)
r = e.call("load_prg", {"path": os.path.join(outside, "far.prg")}); check("load_prg: a file outside is refused", refused(r), r)
r = e.call("get_screen", {"path": "shot.png"}); check("get_screen: path inside the root", not r[0] and os.path.exists(os.path.join(proj, "shot.png")), r)
r = e.call("get_screen", {"path": os.path.join(outside, "shot.png")}); check("get_screen: path outside is refused", refused(r), r)
# symbolic links are not examined: a link inside the root that points outside is followed
r = dump(e, "link/viaLink.bin"); check("a symlink inside the root that points outside is followed (documented)", not r[0] and os.path.exists(os.path.join(outside, "viaLink.bin")), r)
r = e.call("load_prg", {"path": "link/far.prg"}); check("...for reading too", not r[0], r)
check("tools that take no host path are unaffected", e.ok("get_registers")["pc"].startswith("0x"))
log = stderr_of(e); check("a line on stderr states the root and where it came from", "limited to " + proj in log and "current directory" in log, log[:300])

# ---------------- -fsroot ----------------
e = start(["-fsroot", other])
st = e.status(); check("-fsroot sets the root", st["host_root"] == other and st["host_paths"] == "restricted", st)
r = dump(e, "f.bin"); check("relative paths are relative to the root, not the working directory", not r[0] and os.path.exists(os.path.join(other, "f.bin")) and not os.path.exists(os.path.join(proj, "f.bin")), r)
check("the working directory is no longer allowed", refused(dump(e, os.path.join(proj, "g.bin"))))
check("stderr says where the root came from", "-fsroot" in stderr_of(e))

# ---------------- X16EMU_MCP_ROOT ----------------
e = start(["-fsroot", other], env={"X16EMU_MCP_ROOT": outside})
st = e.status(); check("X16EMU_MCP_ROOT overrides -fsroot", st["host_root"] == outside, st)
check("...and its folder works", not dump(e, "h.bin")[0] and os.path.exists(os.path.join(outside, "h.bin")))
e.close()
e = start(["-fsroot", os.path.join(other, ".", "sub", "..")])
check("the root is normalised ('x/./sub/..' becomes 'x')", e.status()["host_root"] == other, e.status()); e.close()

# ---------------- X16EMU_MCP_UNRESTRICTED ----------------
e = start(env={"X16EMU_MCP_UNRESTRICTED": "1"})
st = e.status(); check("X16EMU_MCP_UNRESTRICTED=1: unrestricted, no root", st["host_paths"] == "unrestricted" and st["host_root"] is None, st)
check("any path works", not dump(e, os.path.join(outside, "u.bin"))[0] and os.path.exists(os.path.join(outside, "u.bin"))
      and not dump(e, "../outside/u2.bin")[0] and os.path.exists(os.path.join(outside, "u2.bin")))
check("relative paths are then relative to the working directory", not dump(e, "rel.bin")[0] and os.path.exists(os.path.join(proj, "rel.bin")))
check("stderr says so", "UNRESTRICTED" in stderr_of(e))
e = start(env={"X16EMU_MCP_UNRESTRICTED": "0"})
check("X16EMU_MCP_UNRESTRICTED=0 does not turn it off", e.status()["host_paths"] == "restricted"); e.close()

# ---------------- drive root ----------------
e = start(cwd="/")
st = e.status(); check("working directory '/': host paths are disabled", st["host_paths"] == "disabled" and st["host_root"] is None, st)
r = dump(e, "/tmp/x16mcp-should-not-exist.bin"); check("...and a path tool says why", r[0] and "drive root" in r[1] and "UNRESTRICTED" in r[1], r)
r = e.call("load_prg", {"path": os.path.join(proj, "ok.prg")}); check("...for load_prg too", r[0] and "drive root" in r[1], r)
check("...but everything else still works", e.ok("get_registers")["pc"].startswith("0x") and "disabled" in stderr_of(e))
e = start(env={"X16EMU_MCP_ROOT": "/"}, cwd=proj)
check("an explicit X16EMU_MCP_ROOT=/ is refused as well", e.status()["host_paths"] == "disabled"); e.close()
e = start(env={"X16EMU_MCP_UNRESTRICTED": "1"}, cwd="/")
check("a drive root with X16EMU_MCP_UNRESTRICTED=1 is allowed (explicit)", e.status()["host_paths"] == "unrestricted"
      and not dump(e, os.path.join(outside, "root-ok.bin"))[0]); e.close()
e = start(["-fsroot", "/"], cwd=proj)
check("-fsroot / is a drive root too: disabled", e.status()["host_paths"] == "disabled"); e.close()

# ---------------- home directory warning ----------------
e = start(env={"HOME": proj}, cwd=proj)
log = stderr_of(e); check("a root that is the home directory prints a warning", "home directory" in log, log[:300])
finish()

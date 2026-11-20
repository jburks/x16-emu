"""Protocol basics: handshake, ping, notifications, errors, clean exit, stdout purity."""
import json, subprocess, sys, time
from mcp_testlib import spawn, exe

p = spawn(stderr=subprocess.PIPE)
def send(raw):
    p.stdin.write(raw + "\n"); p.stdin.flush()
def recv():
    line = p.stdout.readline()
    return json.loads(line)  # raises if stdout is polluted
fails = 0
def check(name, cond, got):
    global fails
    print(("PASS " if cond else "FAIL ") + name + ("" if cond else "  got: %r" % (got,)))
    fails += 0 if cond else 1

send(json.dumps({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"t","version":"0"}}}))
r = recv()
check("initialize echoes known version", r["id"]==1 and r["result"]["protocolVersion"]=="2024-11-05", r)
check("initialize serverInfo", r["result"]["serverInfo"]["name"]=="x16emu" and "tools" in r["result"]["capabilities"], r)
send(json.dumps({"jsonrpc":"2.0","method":"notifications/initialized"}))
send(json.dumps({"jsonrpc":"2.0","id":"abc","method":"ping"}))
r = recv(); check("ping, string id, no reply to notification", r["id"]=="abc" and r["result"]=={}, r)
send(json.dumps({"jsonrpc":"2.0","id":2,"method":"tools/list"}))
r = recv(); check("tools/list includes get_registers", r["id"]==2 and "get_registers" in [t["name"] for t in r["result"]["tools"]], r["id"])
send(json.dumps({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"nope","arguments":{}}}))
r = recv(); check("unknown tool -> -32602", r["id"]==3 and r["error"]["code"]==-32602, r)
send(json.dumps({"jsonrpc":"2.0","id":4,"method":"bogus"}))
r = recv(); check("unknown method -> -32601", r["id"]==4 and r["error"]["code"]==-32601, r)
send('{"jsonrpc":"2.0","id":5,"method":')
r = recv(); check("bad JSON -> -32700, id null", r["id"] is None and r["error"]["code"]==-32700, r)
send('[{"jsonrpc":"2.0","id":6,"method":"ping"}]')
r = recv(); check("batch rejected -> -32600", r["id"] is None and r["error"]["code"]==-32600, r)
send('{"jsonrpc":"2.0","id":{"x":1},"method":"ping"}')
r = recv(); check("object id -> -32600", r["error"]["code"]==-32600, r)
send('{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"caf\\u00e9 \\ud83d\\ude00 \\"q\\" \\n"}}')
r = recv(); check("unicode/escapes round trip", r["id"]==7 and "café 😀 \"q\" \n" in r["error"]["message"], r)
send(json.dumps({"jsonrpc":"2.0","id":1.5e3,"method":"ping"}))
r = recv(); check("numeric id echoed", r["id"]==1500, r)
big = "x"*100000
send(json.dumps({"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":big}}))
r = recv(); check("100 KB line handled", r["id"]==8, r["error"]["message"][:40] if "error" in r else r)
send('{"jsonrpc":"2.0","id":9,"method":"ping"}\r')
r = recv(); check("CRLF line ending", r["id"]==9, r)
print("emulator alive:", p.poll() is None)
p.stdin.close()
try:
    rc = p.wait(timeout=10); check("exits when stdin closes", True, rc); print("exit code", rc)
except subprocess.TimeoutExpired:
    check("exits when stdin closes", False, "still running"); p.kill()
err = p.stderr.read()
print("--- stderr (first lines) ---"); print("\n".join(err.splitlines()[:8]))
sys.exit(1 if fails else 0)

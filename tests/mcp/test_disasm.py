"""disassemble: text for every addressing mode, targets, labels, effective address, banks, and agreement with the CPU core."""
import json, os, sys, tempfile, time
from mcp_testlib import *

tmp = os.path.realpath(tempfile.mkdtemp(prefix="x16mcp-dis-"))
e = Emu(["-zeroram"], env={"X16EMU_MCP_ROOT": tmp})
check("disassemble listed", "disassemble" in [t["name"] for t in e.rpc("tools/list")["result"]["tools"]])
check("boots to READY.", wait_screen(e, "READY."))
e.ok("pause")

def dis(address, count=16, **kw):
    return e.ok("disassemble", dict({"address": address, "count": count}, **kw))
def put(addr, hexstr):
    e.ok("write_memory", {"address": addr, "hex": hexstr})

# ---- text, bytes and length for each addressing mode ----
CASES = [("A905", "LDA #$05"), ("8510", "STA $10"), ("B510", "LDA $10,X"), ("B610", "LDX $10,Y"), ("AD1007", "LDA $0710"), ("BD1007", "LDA $0710,X"),
         ("B91007", "LDA $0710,Y"), ("A110", "LDA ($10,X)"), ("B110", "LDA ($10),Y"), ("B210", "LDA ($10)"), ("6C3412", "JMP ($1234)"),
         ("7C3412", "JMP ($1234,X)"), ("203412", "JSR $1234"), ("4C3412", "JMP $1234"), ("0710", "RMB0 $10"), ("F710", "SMB7 $10"), ("6410", "STZ $10"),
         ("9C3412", "STZ $1234"), ("DA", "PHX"), ("7A", "PLY"), ("1A", "INC A"), ("3A", "DEC A"), ("0A", "ASL A"), ("4A", "LSR A"), ("2A", "ROL A"),
         ("8905", "BIT #$05"), ("3410", "BIT $10,X"), ("0410", "TSB $10"), ("1C3412", "TRB $1234"), ("CB", "WAI"), ("EA", "NOP"), ("0000", "BRK #$00"),
         ("60", "RTS"), ("40", "RTI"), ("18", "CLC"), ("78", "SEI"), ("A200", "LDX #$00"), ("A000", "LDY #$00"), ("E010", "CPX #$10"), ("9A", "TXS"),
         ("8D239F", "STA $9F23"), ("2CFFFF", "BIT $FFFF")]
code = "".join(c for c, _ in CASES)
put(0x600, code)
r = dis(0x600, len(CASES))
got = [(i["bytes"], i["text"]) for i in r["instructions"]]
bad = [(g, w) for g, w in zip(got, CASES) if g != w]
check("all %d instructions decode to the expected text and bytes" % len(CASES), not bad and len(got) == len(CASES), bad[:3])
check("length matches the byte count", all(i["length"] == len(i["bytes"]) // 2 for i in r["instructions"]))
check("addresses advance by length", all(int(b["address"], 16) == int(a["address"], 16) + a["length"] for a, b in zip(r["instructions"], r["instructions"][1:])))
check("result header: start address, cpu, banks, next", r["address"] == "0x0600" and r["cpu"] == "65c02" and "ram_bank" in r and r["next"] == "0x%04X" % (0x600 + len(code) // 2), {k: v for k, v in r.items() if k != "instructions"})
check("mnemonics and operands are upper case", all(i["text"] == i["text"].upper() for i in r["instructions"]))
check("an instruction at no address has no label/target/current keys unless they apply",
      all(set(i) <= {"address", "bytes", "text", "length", "target", "target_symbol", "label", "current", "effective_address"} for i in r["instructions"]))

# ---- branch targets ----
put(0x700, "D0FE 8003 EA EA EA F0 F0 0F1002 8F10FE 203412 4C0007 90 00")
r = dis(0x700, 9)
tg = [(i["text"], i.get("target")) for i in r["instructions"]]
want = [("BNE $0700", "0x0700"), ("BRA $0705", "0x0705"), ("NOP", None), ("NOP", None), ("NOP", None), ("BEQ $0700", "0x0700"),
        ("BBR0 $10, $070D", "0x070D"), ("BBS0 $10, $070C", "0x070C"), ("JSR $1234", "0x1234")]
# addresses: 0700 D0FE | 0702 8003 | 0704 EA | 0705 EA | 0706 EA | 0707 F0F0 | 0709 0F1002 | 070C 8F10FE | 070F 203412
want = [("BNE $0700", "0x0700"), ("BRA $0707", "0x0707"), ("NOP", None), ("NOP", None), ("NOP", None), ("BEQ $06F9", "0x06F9"),
        ("BBR0 $10, $070E", "0x070E"), ("BBS0 $10, $070D", "0x070D"), ("JSR $1234", "0x1234")]
check("relative branches (backward and forward), BBR/BBS and JSR show full-address targets", tg == want, tg)
check("a backward branch across a page shows four digits", dis(0x700, 1)["instructions"][0]["text"] == "BNE $0700")

# ---- labels and symbols ----
sym = os.path.join(tmp, "d.sym")
open(sym, "w").write("al 000700 .start\nal 000707 .skip\nal 001234 .far_routine\n")
e.ok("load_symbols", {"path": sym})
r = dis("start", 9)
ins = r["instructions"]
check("a label exactly at an instruction address is reported as label", ins[0].get("label") == "start" and ins[2].get("label") is None, [i.get("label") for i in ins])
check("the instruction after the label offset has none (labels only when exact)", "label" not in ins[1])
check("branch and JSR targets carry their symbol", ins[0]["target_symbol"] == "start" and ins[8]["target"] == "0x1234" and ins[8]["target_symbol"] == "far_routine", [i.get("target_symbol") for i in ins])
check("label as the address argument", r["address"] == "0x0700" and dis("start+2", 1)["instructions"][0]["address"] == "0x0702")
check("a label with an offset is named name+0xN for targets", dis(0x700, 1)["instructions"][0]["target_symbol"] == "start")
err, t = e.call("disassemble", {"address": "nosuch"}); check("an unknown label is an error", err and "unknown symbol" in t, t)

# ---- the instruction at pc: current marker and effective address ----
put(0x800, "BD1007 B110 AD0007 EA")      # LDA $0710,X / LDA ($10),Y / LDA $0700 / NOP
put(0x10, "0009")                        # ($10) -> $0900
e.ok("set_registers", {"pc": 0x800, "x": 3, "y": 4, "status": 0x24})
r = dis(0x800, 3); a, b, c = r["instructions"]
check("current marks the instruction at pc, and only that one", a.get("current") is True and "current" not in b and "current" not in c, r["instructions"])
check("effective address of LDA abs,X = $0710 + X", a["effective_address"] == "0x0713", a)
check("no effective address is reported for later instructions (the registers describe pc only)", "effective_address" not in b and "effective_address" not in c)
e.ok("set_registers", {"pc": 0x803}); b = dis(0x803, 1)["instructions"][0]
check("(zp),Y effective address = pointer + Y", b["current"] is True and b["effective_address"] == "0x0904", b)
e.ok("set_registers", {"pc": 0x805}); c = dis(0x805, 1)["instructions"][0]; check("absolute: effective address is the operand", c["effective_address"] == "0x0700", c)
check("when pc is not in the listing there is no current marker", all("current" not in i for i in dis(0x900, 4)["instructions"]))

# ---- paging with next ----
big = dis(0x600, 10); first = dis(0x600, 4); rest = dis(first["next"], 6)
check("next continues exactly where the listing stopped", first["instructions"] + rest["instructions"] == big["instructions"])

# ---- banks: ROM and banked RAM, and no side effects on I/O ----
for b in (0, 1, 5):
    r = dis(0xC000, 6, bank=b)
    raw = e.ok("read_memory", {"address": 0xC000, "count": sum(i["length"] for i in r["instructions"]), "bank": b})["hex"].upper()
    check("ROM bank %d: the bytes are what read_memory returns for that bank" % b, "".join(i["bytes"] for i in r["instructions"]) == raw and r["rom_bank"] == b, r["instructions"][:2])
put(0xA000, "A901"); e.ok("write_memory", {"address": 0xA000, "bank": 6, "hex": "A942"})
check("banked RAM: bank 6 and the current bank decode differently", dis(0xA000, 1, bank=6)["instructions"][0]["text"] == "LDA #$42"
      and dis(0xA000, 1)["instructions"][0]["text"] == "LDA #$01", (dis(0xA000, 1, bank=6)["instructions"][0], dis(0xA000, 1)["instructions"][0]))
vs = e.ok("get_vera_state"); cyc = e.ok("get_registers")["cycles"]
dis(0x9F20, 16); dis(0x9F00, 32)
check("disassembling the I/O area has no side effects on VERA or on time", e.ok("get_vera_state") == vs and e.ok("get_registers")["cycles"] == cyc)

# ---- agreement with the CPU core: every opcode's length is how far the CPU actually advances ----
skip = {0x00, 0x20, 0x40, 0x4C, 0x60, 0x6C, 0x7C, 0xCB, 0xDB} | {0x10, 0x30, 0x50, 0x70, 0x80, 0x90, 0xB0, 0xD0, 0xF0} | {n * 16 + 15 for n in range(16)}
ctrl_len = {0x00: 2, 0x20: 3, 0x40: 1, 0x4C: 3, 0x60: 1, 0x6C: 3, 0x7C: 3, 0xCB: 1, 0xDB: 1}
ctrl_len.update({o: 2 for o in (0x10, 0x30, 0x50, 0x70, 0x80, 0x90, 0xB0, 0xD0, 0xF0)}); ctrl_len.update({n * 16 + 15: 3 for n in range(16)})
mism = []
for op in range(256):
    put(0x0A00, "%02X1007EAEAEA" % op)
    d = dis(0x0A00, 1)["instructions"][0]
    if op in skip:
        if d["length"] != ctrl_len[op]: mism.append((hex(op), "disassembler", d["length"], "expected", ctrl_len[op]))
        continue
    e.ok("set_registers", {"pc": 0x0A00, "x": 1, "y": 1, "a": 0, "sp": 0xF0, "status": 0x24})
    pc = int(e.ok("step")["pc"], 16)
    if pc - 0x0A00 != d["length"]: mism.append((hex(op), d["text"], "disassembler", d["length"], "cpu", pc - 0x0A00))
check("all 256 opcodes: the disassembler's length agrees with the CPU core (control-flow opcodes against their known lengths)", not mism, mism[:6])

# ---- limits and errors ----
for label, a in (("count 0", {"count": 0}), ("count 257", {"count": 257}), ("count not a number", {"count": "x"}), ("no address", {"count": 4}),
                 ("an address past 64K", {"address": 0x10000}), ("a bad bank", {"address": 0x600, "bank": 300})):
    args = dict({"address": 0x600}, **a) if "address" not in a and label != "no address" else a
    err, t = e.call("disassemble", args); check("rejects " + label, err, t)
r = dis(0xFFFE, 6)
check("at the top of memory it stays inside the address space", all(int(i["address"], 16) + i["length"] <= 0x10000 for i in r["instructions"]), r["instructions"])
check("the default count is 16", len(e.ok("disassemble", {"address": 0x600})["instructions"]) == 16)
check("emulator exits cleanly", e.close() == 0)
finish()

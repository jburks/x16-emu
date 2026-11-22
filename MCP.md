# MCP server

The emulator can run as an [MCP](https://modelcontextprotocol.io) (Model Context
Protocol) server, so an AI agent or any other MCP client can inspect and drive a
running Commander X16: read memory and the screen, pause, single-step, set
breakpoints and watchpoints, poke memory, and type on the keyboard or move the mouse.

Start the emulator with `-mcp`. The normal emulator window opens, and the
emulator then speaks MCP on its standard input and output. There are no extra
dependencies; the JSON parser is part of the source (`src/mcp_json.c`).

```
x16emu -mcp
```

All the usual options still work (`-rom`, `-sdcard`, `-prg`, `-run`, `-zeroram`,
`-warp`, `-nowindow`, ...), except `-testbench`, which cannot be combined with `-mcp`.

The server is a build option, `ENABLE_MCP` (on by default; not available for
the WebAssembly build). Build without it with `cmake -DENABLE_MCP=OFF`: the
emulator then contains none of the MCP code, its hooks in the CPU memory access
and VERA write paths compile to nothing, and `-mcp` exits with a message saying
the server is not built in. The CMake build summary shows the setting as
`MCP Server`. The profiler (see "Profile") is a second option, `ENABLE_PROFILER`
(on by default), built only together with the server.

## Attaching an agent

The server uses the **stdio** transport: the client starts the emulator as a
child process and talks to it over the pipe, newline-delimited JSON-RPC 2.0. You
do not start the emulator yourself; you tell your client how to launch it. The
emulator needs a display unless you pass `-nowindow` (see below), because the window is real.

### Claude Code

```
claude mcp add x16 -- /path/to/x16emu -mcp -zeroram
```

Add `--scope project` to share it through a `.mcp.json` file in a repository.
Restart the session afterwards; `/mcp` should list `x16` as connected.

### Other clients (Cursor, Cline, Windsurf, Claude Desktop, ...)

Most clients take a JSON block like this in their MCP settings file:

```json
{
  "mcpServers": {
    "x16": {
      "command": "/path/to/x16emu",
      "args": ["-mcp", "-zeroram"]
    }
  }
}
```

Use an absolute path for `command`. If the emulator cannot find `rom.bin`, add
`"-rom", "/path/to/rom.bin"` to `args`; by default it looks next to the
executable.

### Giving the emulator a disk

The emulated machine's drive is a host folder: by default the emulator's
current directory, or whatever you pass with `-fsroot /path/to/folder`. BASIC
`LOAD` and `SAVE`, and programs that open files, read and write there. Add
`-fsroot` to the arguments in any of the examples above to choose the folder
(an empty scratch directory is a good choice for an agent). It complements
`load_prg`: `load_prg` puts a program straight into memory, while the drive lets
programs find their own data files. The same folder is the default **host path
root**: `load_prg` and the file dumps are limited to it (see "Host files and
permissions").

### Running in the background (no window)

By default the emulator opens a window, which can pop up in front of what you
are doing. Add `-nowindow` to run without one:

```
claude mcp add x16 -- /path/to/x16emu -mcp -nowindow -zeroram
```

`-nowindow` selects SDL's dummy video and audio drivers. There is no window, no
focus stealing and no sound, and it needs no display server at all, so it also
works over SSH, in containers and in CI. The machine is still emulated in full:
video is rendered, the frame counter and timing work, and `get_screen`,
`read_memory` of VRAM, the keyboard and mouse tools behave exactly as with a
window. The only things you lose are the ones you would see and hear yourself.
(The flag is not specific to MCP, but without `-mcp` nothing can reach the
emulator.)

For test automation, where many 60-frame runs would otherwise take real time,
add `-warp` as well: emulated time then passes as fast as the host can manage,
about twice real time or more. Nothing about the emulated machine changes. You
can also toggle it at runtime with `set_warp`.

### Scripting it directly

```python
import json, subprocess
p = subprocess.Popen(["x16emu", "-mcp"], stdin=subprocess.PIPE,
                     stdout=subprocess.PIPE, text=True)
def rpc(id, method, params=None):
    p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": id, "method": method,
                              "params": params or {}}) + "\n")
    p.stdin.flush()
    return json.loads(p.stdout.readline())
rpc(1, "initialize", {"protocolVersion": "2025-11-25"})
print(rpc(2, "tools/call", {"name": "get_registers", "arguments": {}}))
```

Closing the emulator's stdin makes it exit.

### Host files and permissions

The emulator runs with the permissions of the user who starts it, and so does
anything an agent asks it to do. Three tools touch host files: `load_prg` reads
a PRG, and `read_memory` and `get_screen` with `path` write a file. These are
**limited to one folder, the host path root**, unless you turn the restriction
off.

**Where the root is.** In order of preference:

1. `X16EMU_MCP_ROOT`, if set, names the folder.
2. Otherwise the folder of the emulated drive: the `-fsroot` folder, or the
   emulator's current directory when there is no `-fsroot` (the same folder the
   emulated machine's own `LOAD` and `SAVE` use, so the tools add no access the
   agent does not already have through BASIC).

`get_status` shows the result as `host_root` and `host_paths`. The emulator also
prints one line about it to stderr at startup. Relative paths are taken relative
to the root, and a path must lie inside it: `..` segments are resolved first, so
`../elsewhere/file` is refused, and so is any absolute path outside the root.

**Turning it off.** Set `X16EMU_MCP_UNRESTRICTED=1` and every path is allowed
(relative paths are then relative to the emulator's working directory). The
value `0` does not turn it off.

**A drive root is refused.** If the root works out to a drive root (`/`, or
`C:\`), for example because the client launched the emulator from `/`, there
would be nothing to restrict, so the three path features are switched off and
say why. Pass `-fsroot` or set `X16EMU_MCP_ROOT` to a project folder, or use
`X16EMU_MCP_UNRESTRICTED=1` if you really mean to allow everything. A root that
is your home directory is allowed, with a warning on stderr.

**Symbolic links are not examined.** A path is judged by its text. A symbolic
link inside the root that points outside it is followed, and so is `link/..`
when `link` is one. The restriction keeps a mistaken or manipulated agent from
wandering by accident or by an instruction to write to `/etc`; it is not a
sandbox against a hostile program, and there is a small gap between checking a
path and opening it. If you need real isolation, run the emulator as an
unprivileged user, in a container, or in a scratch directory.

On Windows, `\` and `/` are both accepted and paths are compared without regard
to case. That code path has not been tested here.

The emulated machine has its own route to the host: an agent that types `SAVE`
or `LOAD` at the BASIC prompt reads and writes files in the emulated drive
folder, and can scratch (delete) them. Use `-fsroot` with a scratch directory.
Everything else (memory, registers, keyboard, mouse) acts only on the emulated
machine.

### Details for client authors

- **Output:** stdout carries only protocol messages. The emulator's own output
  goes to stderr. Set `X16EMU_MCP_LOG=1` to log the traffic on stderr.
- **Protocol versions:** `2025-11-25` is verified against a real client (Claude
  Code). `2025-06-18`, `2025-03-26` and `2024-11-05` are accepted but untested.
  The newer stateless `server/discover` handshake is not implemented; clients
  fall back to `initialize`.
- **Capabilities:** tools only. The `initialize` result includes short usage
  `instructions` for the agent.
- **Requests are served one at a time.** A tool call that waits (`continue` with
  `wait`, `run_frames`, `step`, `type_text` with `wait`) blocks later requests
  until it finishes or times out.
- **Timeouts:** waiting tools default to 30 s (override with the environment
  variable `X16EMU_MCP_TIMEOUT_MS`) and accept a per-call `timeout_ms` of 1 to
  600000. On timeout the call returns a tool error and the emulator is left
  **paused**, so you can inspect where it got to.
- **Errors:** bad arguments and failed operations are returned as normal tool
  results with `isError: true` and a message, not as protocol errors.
- **Environment variables:** `X16EMU_MCP_ROOT` (host path root),
  `X16EMU_MCP_UNRESTRICTED` (turn the host path restriction off),
  `X16EMU_MCP_TIMEOUT_MS` (default timeout) and `X16EMU_MCP_LOG` (traffic log).
- **Cost when idle:** without `-mcp` nothing runs. With `-mcp` and nothing
  stopped, the emulator loop makes one cheap check per pass. Watchpoints add a
  test to every CPU memory access, so avoid leaving them set when you do not
  need them.

## How it works: running and paused

The emulator is either **running** or **paused**. Pausing happens between two
instructions; while paused, nothing changes and the window stays open.

- `pause` and `continue` switch between the two. Resuming does not make the
  emulator "catch up" for the paused time.
- `run_frames(n)` runs exactly `n` video frames (about 1/60 s of emulated time
  each) and leaves the emulator paused, right after the instruction that
  completed the last frame. This is the tool for deterministic stepping of
  time.
- `step(n)` executes `n` instructions from the paused state.
- Breakpoints and watchpoints pause the emulator when they hit. `get_status`
  reports why it last stopped (`state`: `running`, `paused`, `breakpoint`,
  `watchpoint`, `step` or `frames`).

Tips:

- **Pause before you look.** Memory read while the emulator runs can change
  between reads, and `get_screen` may show a half-drawn frame.
- **Reproducible runs.** RAM contents are random at startup unless you pass
  `-zeroram`. With `-zeroram`, two launches advanced to the same frame with
  `run_frames` have identical registers, RAM, VRAM and cycle counts. `reset` with
  `zero_ram: true` gets you back to the same kind of start without relaunching;
  VRAM, banked RAM and all of low RAM except a few stale bytes below the stack
  pointer then come out identical from run to run.
- **Timing is paced, even without a window.** Emulated time runs at the normal
  60 frames per second whether or not there is a window, so `run_frames(60)`
  takes about a second and 600 frames about ten. Boot to the `READY.` prompt
  takes under a second windowless (about 36 frames); with a window on a slow
  host it can take several seconds. Before typing at BASIC, wait for `READY.` in
  the text layer (VRAM `$1B000`, 256 bytes per row, the character code at every
  other byte), or run frames until it appears.
- **Going faster.** `set_warp` (or starting with `-warp`) lifts the speed limit,
  so frames pass as fast as the host can emulate them, around twice real time or
  more on the development host. This works for free-running emulation and for
  `run_frames`, which checks its target once per frame rather than after every
  instruction.
- **Measure in emulated time.** Use `run_frames`, the frame counter and the
  `cycles` field (exact CPU cycles, in `get_registers`, `get_status` and every
  stop result) rather than wall-clock time. `lag_ms` in `get_status` shows how far
  emulation is behind real time on a slow host.

## Tools

Addresses and values may be given as JSON integers or as strings such as
`"0x9F20"` or `"$9F20"`; addresses may also be label names once symbols are
loaded (see "Symbols"). Results are JSON text, except `get_screen`, which
returns a PNG image (with `path` it writes the file instead and returns JSON text
giving the path and the byte count).

### Inspect

| Tool | Arguments | What it does |
|---|---|---|
| `get_registers` | none | pc, a, x, y, sp, status, current RAM and ROM bank, paused flag, `symbol` for pc (when symbols are loaded), and `cycles`: the exact number of CPU clock cycles executed since start. Subtract two values to measure what a routine costs; it is an unsigned 64-bit count, so it does not wrap in practice. Tool calls take no emulated time, so `cycles` only moves while the emulator runs, and it keeps counting across `reset` and `set_warp`. |
| `get_status` | none | `frames` completed; `cycles` (the exact count right now, same as in `get_registers`); `cpu_cycles_at_last_frame` (the count when the last frame completed, which only changes once per frame); clock in MHz; `warp`; `lag_ms`; `paused`; `state`; number of breakpoints; `host_paths` (`restricted`, `unrestricted` or `disabled`) and `host_root` (the folder host paths must stay inside) |
| `read_memory` | `address`\*, `space`, `count`, `bank`, `path`, `overwrite` | Reads 1 to 131072 bytes (`count`, default 16; the same limit applies with `path`) without side effects (I/O is not triggered). `space` is `cpu` (default; `$A000-$BFFF` banked RAM, `$C000-$FFFF` banked ROM, using the current banks unless `bank` is given) or `vram` (the 128 KB VERA space, including palette at `0x1FA00` and sprite attributes at `0x1FC00`). Returns one lowercase hex string. With `path`, the raw bytes are written to that host file instead and only the byte count is returned, which keeps large buffers (a 38,400-byte frame buffer is about 77 KB as hex) out of the conversation. An existing file is replaced only with `overwrite: true`. The path must lie inside the host path root (see "Host files and permissions"). |
| `hash_memory` | `address`\*, `count`\* (1 to 131072), `space`, `bank`, `algorithm` | CRC32 and/or SHA-256 (`algorithm`: `crc32`, `sha256` or `both`, the default) of a range, read like `read_memory`. Compare a buffer against a known value without transferring it. The CRC32 is the standard one (`zlib.crc32`). |
| `get_vera_state` | none | VERA's internal state, which the registers at `$9F20-$9F3F` do not show: both data ports at once (address, signed step, increment field, prefetched byte, nibble flags), `addrsel` and `dcsel`, the IRQ enable and status bits, the line-compare value and whether IRQ is asserted, the current raster line, and the FX state (mode, flags, the four cache bytes as hex, pixel increments and positions as decimals plus `_raw` hex, polygon length, affine bases, multiplier accumulator). Reading has no side effects. |
| `disassemble` | `address`\*, `count`, `bank` | Disassembles `count` instructions (default 16, at most 256) from `address` (a number or a label), using the emulator's own disassembler (65C02, or 65C816 if the emulator runs in that mode). Reads without side effects, so it is safe on I/O and ROM. Each instruction has `address`, `bytes`, `text` (`LDA #$05`), `length`, and where they apply `label` (a label exactly at that address), `target` and `target_symbol` (relative branches, `BBRn`/`BBSn`, absolute `JMP` and `JSR`), `operand_address` and `operand_symbol` (the address a zero-page, absolute, indexed or indirect operand names, and its label), and, for the instruction at `pc` only, `current` and `effective_address`. `next` is where to continue. |
| `get_screen` | `path`, `overwrite` | 640x480 PNG of the current framebuffer. With `path` it is written to that host file instead of being returned, inside the host path root. |

Example `get_vera_state` result, trimmed (a real run, paused at the BASIC prompt
after setting ADDR0 to `$11234` with an increment of 1):

```json
{"addrsel":0, "dcsel":0,
 "data_ports":[
   {"port":0, "address":"0x11234", "step":1, "increment_field":2, "prefetched_byte":"0xEA",
    "nibble_bit":false, "nibble_increment":false},
   {"port":1, "address":"0x00000", "step":0, "increment_field":0, "prefetched_byte":"0x00",
    "nibble_bit":false, "nibble_increment":false}],
 "irq":{"enable":"0x1", "status":"0x02", "line_compare":0, "irq_asserted":false},
 "video":{"scanline":140, "ntsc":false},
 "fx":{"addr1_mode":0, "four_bit_mode":false, "cache_fill":false, "cache_write":false,
       "multiplier":false, "cache_byte_index":0,
       "x_pixel_increment":0.0, "x_pixel_increment_raw":"0x00000000",
       "x_pixel_position":0.5, "x_pixel_position_raw":"0x00008000",
       "poly_fill_length":0, "affine_tile_base":"0x00000", "affine_map_base":"0x00000",
       "cache":"00000000", "multiplier_accumulator":0, "...":"(and the other FX flags)"}}
```

`step` is the signed address step after the decrement bit is applied (`-4` for a
decrement of 4). `increment_field` is the raw 5-bit value (code in the upper four
bits, decrement flag in the lowest). FX pixel values are 16.16 fixed point: each
comes as a decimal and as a `_raw` hex value, and `cache` lists the four cache
bytes in order.

Notes on `disassemble`:

- A disassembler decodes from the address you give it, so start at a real
  instruction boundary: a label, an entry point, or the current `pc`. It cannot
  work backwards, and data decodes as meaningless instructions.
- Banks work as for `read_memory`: the current RAM and ROM banks unless `bank`
  is given (it selects both). A label that carries a bank supplies it.
- Branch targets are shown as full four-digit addresses (`BNE $0612`), and
  every `target` is the address the branch, `JMP` or `JSR` goes to. `JMP (abs)`
  and `JMP (abs,X)` have no static target.
- `text` always shows numbers; names are in the separate fields. `target_symbol`
  names where control goes, `operand_symbol` names the data an instruction
  reads or writes (`LDA $0710,X` gives `operand_address` `0x0710` and, with a
  label there, `operand_symbol` `table`), and `label` names the instruction's own
  address. Immediates (`LDA #$10`) have no operand address. In 65C816 mode
  `operand_*` is not reported.
- A symbol is the nearest label at or below the address (within 4096 bytes), so
  an address inside a table is `table+0x2`, and an unrelated number that happens
  to fall just above a label gets that label with an offset: read the offset
  before trusting the name.
- `effective_address` is the memory address the instruction at `pc` will use
  (for example `$0710 + X` for `LDA $0710,X`), worked out from the registers as
  they are now, so it is given for that one instruction only.
- Opcode `$DB` is shown as `DBG`: the emulator treats it as a debugger trap, not
  as the 65C02 `STP`. The mnemonics are shown in upper case, whatever the core
  prints.
- The tests check every one of the 256 opcodes against the CPU core: the
  disassembler's length must equal how far the CPU's `pc` really advances.

### Run control

| Tool | Arguments | What it does |
|---|---|---|
| `pause` | none | Pauses between two instructions |
| `continue` | `wait`, `timeout_ms` | Resumes at normal speed. With `wait: true` it blocks until a breakpoint or watchpoint stops the emulator, and returns where. |
| `run_frames` | `frames`, `timeout_ms` | Runs exactly that many frames (default 1), then pauses. Stops early at a breakpoint or watchpoint. Works from paused or running. |
| `step` | `count`, `timeout_ms` | Executes `count` instructions (default 1) from the paused state. A WAI/STP that is waiting for an interrupt is stepped over to the next real instruction, usually the IRQ handler. |

The result of `continue` (with `wait`), `run_frames` and `step` describes where
the emulator stopped:

```json
{"stopped":"breakpoint","pc":"0xC1C6","frames":212,"ram_bank":1,"rom_bank":0,
 "cycles":28491203,"a":"0x00","x":"0x01","y":"0x08","sp":"0x01EE","status":"0x32","breakpoint":1}
```

A watchpoint stop adds `watchpoint`, `access`, `address`, `value` and
`accessing_pc`.

### Breakpoints and watchpoints

| Tool | Arguments | What it does |
|---|---|---|
| `set_breakpoint` | `address`\*, `bank` | Pauses just before the instruction at `address` executes. From `$A000` up, `bank` restricts it to that RAM bank (`$A000-$BFFF`) or ROM bank (`$C000-$FFFF`); without it the breakpoint matches in any bank. Max 64. Returns the id (the existing one if already set). |
| `clear_breakpoint` | `id` or `all` | Removes one or all |
| `list_breakpoints` | none | Ids, addresses, banks and hit counts |
| `set_watchpoint` | `address`\*, `length`, `access`, `bank`, `space`, `exclude_fetch` | Pauses after an instruction that accesses the range. `space` is `cpu` (default) or `vram`. For `cpu`, `access` is `read`, `write` (default) or `readwrite`, and `exclude_fetch` ignores instruction fetches. For `vram` only writes are watched. Max 32 watchpoints in all. |
| `clear_watchpoint` | `id` or `all` | Removes one or all |
| `list_watchpoints` | none | Ids, spaces, ranges, access types, banks, `exclude_fetch` flags and hit counts |

Resuming from a breakpoint executes that instruction once without hitting it
again. Resuming after a watchpoint hit does not re-trigger it.

**CPU watchpoints** (`space: "cpu"`):

- They see **CPU accesses**: the stop reports `accessing_pc` (the instruction
  that did it), the address and the value. The emulator stops after that
  instruction completes, so `pc` points at the next one. If an instruction
  touches the range more than once, the first access is reported.
- **Instruction fetches count as reads**, so a read watch on code fires when it
  executes, and so does a read watch on data that sits next to code. Pass
  `exclude_fetch: true` to ignore reads of the bytes of the instruction being
  executed (its opcode and operands). That is a heuristic, and it has
  limits: it treats a read at the opcode's address or within the next three bytes
  as a fetch, whatever the instruction's real length (the window is four bytes
  to cover the 65C816's longest instruction). So a data read can be hidden when
  it lands inside that window: a lookup table placed directly behind the
  instruction that reads it (`LDA table,X` with `table` at the next byte, `X`
  = 0), an instruction that reads its own bytes as data, or code that runs from
  the zero page or stack and reads data within four bytes of itself. Moving the
  table a few bytes away avoids it.
- The CPU core does not model the extra "dummy" reads that some real 65C02
  addressing modes make, so none are reported. The tests confirm one access per
  operand for: `INC`/`ASL` (read-modify-write: one read plus one write, so a
  `readwrite` watch counts 2), absolute, indexed and `(zp),Y` loads and stores,
  each also when the index crosses a page. Other addressing modes have not been
  tested individually.
- A write through the VERA data port shows up here as an access to `$9F23` or
  `$9F24`, not as a VRAM address. Use a VRAM watchpoint for that.

**VRAM watchpoints** (`space: "vram"`, writes only):

- `address` and `length` are in the 128 KB VERA space (`0x00000-0x1FFFF`). A hit
  reports the VRAM `address` of the first byte written inside your range, the
  `value` now stored there, `data_port` (0 for `$9F23`, 1 for `$9F24`) and
  `accessing_pc`. Writes through either data port are covered, including ones
  made while FX is active.
- One CPU store can write several bytes. An FX cache write stores up to four:
  the aligned group of four bytes at or below the address the data port held
  **before** the access (before it auto-increments), so from `$0A001`,
  `$0A002` or `$0A003` the group is `$0A000-$0A003`. The tests check every
  offset from `$0A000` to `$0A007` with a watchpoint on each neighbouring byte.
  A 4-bit write changes only half a byte. A hit is reported if **any byte the store
  writes** lies in the range, even when the CPU's own address does not. A store
  that FX skips (a transparent write of zero, or a nibble masked out) is not a
  write. The reported value is the whole byte as it is now in VRAM.
- Every VRAM write is made by a CPU store to a data port (VERA has no DMA), so
  `accessing_pc` is always the instruction responsible.
- **Reads are not supported.** VERA prefetches the byte when an address is set,
  so a "read" event would not correspond to a CPU access and could name the
  wrong instruction.

Writes made by the tools (`write_memory`, including `space: "vram"`) do not
trigger any watchpoint.

### Change state and send input

| Tool | Arguments | What it does |
|---|---|---|
| `write_memory` | `address`\*, `hex`\*, `space`, `bank` | Writes bytes given as hex (`"A9 01 8D 00 04"`). `cpu` space goes through the normal write path, so I/O registers have their usual side effects, but it takes no emulated time and does not trigger watchpoints. ROM (`$C000` and up) is rejected. `bank` selects the RAM bank for `$A000-$BFFF`. `vram` space writes the VERA address space directly. |
| `set_registers` | any of `pc`, `a`, `x`, `y`, `sp`, `status`, `ram_bank`, `rom_bank` | Paused only. Setting `pc` also ends a WAI/STP wait. A breakpoint at the new `pc` fires when you resume. |
| `reset` | `zero_ram` | Resets the machine as if the reset button were pressed: the CPU restarts at the reset vector, VERA, the VIAs and the input queues are reset, and text still waiting to be typed is dropped. RAM keeps its contents, as on real hardware, unless `zero_ram: true`, which zeroes RAM and banked RAM and refills VRAM as at a fresh `-zeroram` start. A paused emulator stays paused at the reset vector. Breakpoints, watchpoints and the frame and cycle counters are kept. The machine needs about 36 frames after a reset to reach the `READY.` prompt, so run frames before `load_prg` with `run: true` or before typing. |
| `set_warp` | `enabled`\* | Turns warp mode on or off. In warp mode the emulator is not held to real time, so frames pass as fast as the host can emulate them. Behaviour is unchanged; only the speed limiter is bypassed. |
| `send_key` | `key`\*, `action`, `hold_frames`, `timeout_ms` | Sends a key to the emulated PS/2 keyboard. `key` is an [SDL key name](https://wiki.libsdl.org/SDL2/SDL_Scancode) (`Return`, `A`, `Space`, `Left`, `F5`, `Left Shift`, ...). `action` is `tap` (default), `down` or `up`. `hold_frames` keeps a tapped key down for that many frames; use it for programs that poll key state (needs the emulator running). |
| `type_text` | `text`\*, `wait`, `timeout_ms` | Types into the KERNAL keyboard buffer. `\n` is Enter. If the emulator is paused the text is queued and typed after `continue`. With `wait: true` it returns when everything is typed, not when the program it started finishes. |

`type_text` is the easy way to enter BASIC lines and commands. Programs that
read the keyboard directly bypass the KERNAL buffer; use `send_key` for those.

### Load a program

Puts a program on the machine without going through the (emulated) disk drive.

| Tool | Arguments | What it does |
|---|---|---|
| `load_prg` | `path`\*, `address`, `raw`, `bank`, `run` | Loads a PRG file from the host into memory, instantly and without emulated time. The first two bytes of the file are the load address, as with the KERNAL's `LOAD`; `address` overrides it, and `raw: true` treats the whole file as data (`address` required). `bank` selects the RAM bank for loads at `$A000` and up, and data that runs past `$BFFF` continues in the next bank, like the KERNAL. With `run: true` the emulator types `RUN` (BASIC) or `SYS $addr` (machine code). |

The result gives `start`, `end` (exclusive), `bytes`, whether it looks like BASIC,
and the `start_command` to use if you did not pass `run`.

- A load at `$0801` is treated as a **BASIC program**. BASIC's end-of-program
  pointers are updated the way the `LOAD` command does, otherwise BASIC would
  store variables on top of the program. Those pointers live at ROM-specific
  addresses; the tool uses the **release 49** layout, assuming later releases
  keep it. The result names the ROM (`rom_release`, read from `$FF80`: `r49`,
  `prerelease 48`, or `custom build`) and adds a `warning` when the ROM is
  detectably **older than r49**, since the pointers may then be at different
  addresses; if a program misbehaves there, load it with `LOAD` instead. A
  custom build (`$FF`) cannot be placed on that scale and gets no warning.
- If BASIC's start-of-program pointer is not where it should be (the ROM has not
  finished booting, or the layout differs), the pointers are not set, the result
  says `basic_pointers: "not set (...)"`, and `run: true` is refused for BASIC
  programs. Wait for the `READY.` prompt and try again.
- `run: true` types `RUN` or `SYS` into the KERNAL keyboard buffer, so it needs
  the emulator running at the BASIC prompt. If the emulator is paused the command
  is queued, exactly like `type_text`, and typed after `continue`. Loading over a
  program that is running will crash it, as it would on real hardware.
- Loads that would overwrite I/O (`$9F00-$9FFF`) or run into ROM are rejected,
  and nothing is changed when a load is rejected. Files up to 1 MB are read.
- The path is on the machine running the emulator and must lie inside the host
  path root (see "Host files and permissions"); relative paths are relative to
  that root.

### Symbols

Load the labels of the program under test and use names instead of addresses.

| Tool | Arguments | What it does |
|---|---|---|
| `load_symbols` | `path`\*, `replace` | Loads labels from a ca65/ld65 label file (`ld65 -Ln file.sym`: VICE-format lines such as `al 000801 .main`; other lines are skipped and counted) or from cc65 debug info (`ld65 --dbgfile file.dbg`), told apart by content. Labels are added to those already loaded unless `replace: true`; a file with no labels changes nothing. Returns `format` (`labels` or `debug_info`), `loaded`, `merged`, `scopes` and `total`. The path must lie inside the host path root. |
| `lookup_symbol` | `name` or `address` | `name` (`main`, `main+4`, `draw::loop`, `next@$1A24`) returns the address and bank; `address` returns the nearest label at or below it, as `name+0xN`, or `null`. |
| `list_symbols` | `prefix`, `contains`, `include_linker`, `sort`, `limit`, `offset` | Lists the loaded symbols, to check what `load_symbols` read or to find entry points without guessing names (`prefix: "vex_"`). `prefix` keeps names that start with it and `contains` names that contain it (case-sensitive; both must match). Linker symbols such as `__BSS_SIZE__` are left out unless `include_linker: true`. `sort` is `name` (default) or `address`. Paged by `limit` (default 100, at most 1000) and `offset`. Returns `loaded`, `matching`, `returned` and the symbols as `name`, `address` and `bank` (`null` if none). |

Once symbols are loaded:

- **Anywhere an address goes**, a label name works: the `address` of
  `read_memory`, `hash_memory`, `write_memory`, `set_breakpoint`,
  `set_watchpoint`, `load_prg` and `disassemble`, the `from`, `to` and `within`
  of `profile_report`, and the `pc` of `set_registers`. `name+N` and `name-N`
  add or subtract an offset (decimal, `0x` or `$` hex). Labels are
  case-sensitive. An unknown name is an error that says so.
- **Banks:** a label whose value has bits above the low 16 carries a bank in the
  upper byte (`al 0AC123 .x` is `$C123` in bank 10, as in the X16 ROM's label
  files). It supplies the bank for breakpoints, watchpoints and memory tools
  when you give none; an explicit `bank` wins. Labels below `$A000` ignore banks.
- **Results name the code.** `get_registers` and every stop result include
  `symbol` for `pc`, for example `"symbol": "draw_line+0x1A"`, and watchpoint
  stops add `accessing_pc_symbol`. `set_breakpoint` and `list_breakpoints` show
  the label for each address. The label is the nearest one at or below the
  address, within 4096 bytes and in the same region: from `$A000` up only labels
  for the current RAM or ROM bank (or without a bank) are used. Linker symbols
  whose names start and end with `__` (such as `__BSS_SIZE__`) are never offered
  when naming an address, since they are sizes and limits, not code.
- `symbol` is not limited to code: a label that names a lookup table or a
  zero-page variable is reported the same way, so `"symbol": "sine_table+0x10"`
  in a result for a data address is a position in the table, not an instruction.
  (Stop results name `pc`, which is code; `lookup_symbol` and the watchpoint
  addresses can be data.)
- Map files are not read.

**Debug info and scopes.** The label file has no scopes: a label inside a
`.proc` comes out as a bare name, so a program that reuses local names (`loop`,
`next`, `done` in many routines) gets several labels with the same name. cc65's
debug info (`ld65 --dbgfile`, add `-g` to `ca65`) keeps the scopes, and
`load_symbols` reads it:

- Labels are named by their **scope path**, in cc65's syntax: `tslope::sign`,
  and `outer::inner::loop` for nested scopes. Top-level labels stay plain
  (`vex_project_verts`). That name is used everywhere a name is reported:
  `symbol` in stop results and `get_registers`, `disassemble`'s `label`,
  `target_symbol` and `operand_symbol`, `profile_report`, `lookup_symbol` and
  `list_symbols`.
- **As input**, give the full path (`procB::loop`), or a bare label when only
  one label has it (`procA`). A partial path (`inner::loop` for
  `outer::inner::loop`) is not a name.
- The debug info gives every label, and every constant that is an address (it
  has a segment) or that another module imports: the same set `ld65 -Ln`
  writes, except the linker's own symbols (`__BSS_SIZE__`, ...), which are only
  in the label file. A constant that is exported but not imported by any module
  cannot be recognised as exported in the debug info and is not loaded. Plain
  number constants (`COUNT = 3`) are never loaded.
- You can load a program's label file and its debug info, in either order. A
  label that is in both (same address, bank and name) is kept once, with its
  scoped name; `merged` counts them.
- Debug info has no X16 banks: labels from it carry none, like a label file
  with four-digit addresses.

**Names several labels share.** Whatever is loaded, a name that more than one
label has is never resolved silently:

- As **input**, it is an error that lists the candidates: `'next' is
  ambiguous: 9 labels have that name: draw_mesh_wire::next, tri_fill::next, ...`.
  Give the scope path, or `name@$XXXX` for the label at that address
  (`next@$1A24`, also with an offset: `next@$1A24+2`).
- In **results**, a name that is still shared (always the case for local names
  when only a label file is loaded) is shown with its address, `next@$1A24`, so
  a report cannot mix two labels up, and the name can be used as input as it
  is.

### Profile

Find out where the CPU's time goes. While the profiler runs, every instruction
the CPU executes adds its cycles, and one execution, to a counter for its
address (and its bank, from `$A000` up). Every instruction is counted and the
cycles are emulated ones, so the profile is exact and the same code gives the
same profile every time.

| Tool | Arguments | What it does |
|---|---|---|
| `profile_start` | `reset` | Starts counting. `reset` (default `true`) clears earlier counts; `false` adds to them. |
| `profile_stop` | none | Stops counting and returns the totals. The counts are kept until the next `profile_start` with `reset`. |
| `profile_report` | `by`, `scope_level`, `sort`, `from`, `to`, `bank`, `within`, `limit`, `offset`, `path`, `overwrite` | The totals and the counted entries, highest cycles first. `by: "symbol"` (default) adds up the addresses of each label; `by: "scope"` adds up each routine (needs debug info); `by: "address"` lists single instructions. |

Every result starts with the totals:

- `cycles`: the profiled window, exactly the cycles that passed while the
  profiler ran (the sum of all segments since the last reset).
- `instruction_cycles`: cycles spent executing instructions. The entries add up
  to this.
- `wait_cycles`: cycles the CPU spent in `WAI` waiting for an interrupt (not
  charged to any address).
- `other_cycles`: cycles that pass between instructions: 7 for every interrupt
  the CPU takes, and the time of KERNAL file calls that the emulator does on
  the host.
- `unmapped_cycles`: instructions outside bank 0 in 65C816 native mode, which
  have no counter.
- `instructions`: instructions executed; `profiling`: whether it is running.

`instruction_cycles + wait_cycles + other_cycles + unmapped_cycles = cycles`.

Entries of `by: "symbol"` give `symbol`, `address` (the label's), `bank`,
`cycles`, `percent` (of `cycles`) and `instructions` (executions). Each address
belongs to the nearest label at or below it, as for `symbol` elsewhere, so load
the program's labels with `load_symbols` first. Addresses without a label are
grouped per memory region (`symbol: null`, `region`: `low memory`, `RAM bank 2`,
`ROM bank 0`, ...). Inner labels (`@loop`, or a plain label inside a routine)
start a group of their own, so a routine with inner labels shows up in parts;
`by: "scope"` puts it back together.

Entries of `by: "scope"` give `scope` (the path, such as `tri_fill` or
`outer::inner`), `address` (where the scope starts), `bank`, `cycles`,
`percent` and `instructions`. Every address inside a `.proc` or `.scope` counts
for that scope, whatever labels are inside it, so each routine is one entry.
This needs the program's cc65 debug info loaded (see "Debug info and scopes"
under Symbols), since label files have no scopes. Nested scopes count for the
innermost one; with `scope_level: "outermost"` they count for the outermost
one instead. Code outside every scope (top-level code, or code from files
without debug info) is grouped by label as in `by: "symbol"`, with
`scope: null`. For example, in one OtterVeX demo the label view showed
`tri_fill::have` 6.8%, `tri_fill::row` 4.5% and `tri_fill::next` 3.5%; the
scope view shows `tri_fill` as one entry at 19.7%.

Entries of `by: "address"` give `address`, `bank`, `cycles`, `percent`, `count`
(times executed), `symbol` (`name+0xN`) and `text` (the instruction as it is in
memory now). With `sort: "address"` they come in address order, to read a
routine with the cost of each instruction.

Filters: `from` and `to` (an inclusive range; numbers or labels), `bank` (for
addresses from `$A000` up), and `within`: a scope path (with debug info loaded)
keeps every address inside that scope, nested scopes included, so
`within: "tri_fill"` is the whole routine; any other label keeps only the
addresses that belong to that label. The report is paged by `limit` (default 50, at most 1000) and
`offset`, and `entries_total` says how many there are. With `path` the whole
report, every entry, is written to that host file as JSON (inside the host path
root; an existing file is replaced only with `overwrite: true`), and only the
totals come back.

Tips:

- **Profile exactly the stretch you care about.** Tool calls take no emulated
  time, so `profile_start` while paused at a breakpoint and `profile_stop` at the
  next one profiles exactly the code between them; `profile_start` then
  `run_frames` profiles whole frames.
- **Several runs:** `profile_start` with `reset: false` adds another segment to
  the same counts.
- **Find the hot routines, then the hot instructions:** `by: "scope"` first,
  then `by: "address"` with `within: "<the routine>"` and `sort: "address"`.
- Profiling makes the emulator a little slower; stop it when you are done.
  `get_status` shows `profiling`.
- The profile counts cycles per address only. It does not know which routine
  called which, so the time of a routine's callees is not added to it, and a
  routine reached with `JMP` (instead of `JSR`) is reported under its own label
  like any other code.

### Mouse

| Tool | Arguments | What it does |
|---|---|---|
| `mouse_move` | `dx`/`dy`, or `x` and `y` | Relative move (`dx`, `dy` up to 4096; +x right, +y down), or absolute move to `x`,`y` (0 to 1023) |
| `mouse_button` | `button`, `action`, `hold_frames` | `button` is `left` (default), `right` or `middle`. `action` is `tap` (default), `down` or `up`; use `down` and `up` around `mouse_move` to drag. A tap holds the button for `hold_frames` frames (default 2 while running, 0 while paused). |
| `mouse_wheel` | `notches`\* | Scrolls up (positive) or down (negative) |

How the mouse works on the X16, and what that means for these tools:

- The X16 mouse is a PS/2 mouse that reports **relative movement only**. The
  KERNAL keeps the pointer position, and only after the program has enabled the
  mouse (in BASIC: `MOUSE 1`; then `MX`, `MY` and `MB` read the position and
  buttons). Until then, nothing consumes the movement.
- An **absolute move** is emulated: the pointer is driven into the top-left
  corner (the KERNAL clamps it there) and then out to `x`,`y`. That relies on
  the KERNAL mouse being enabled at its default 1:1 scale. If a program
  configures a different scale, or reads the mouse hardware itself, use
  relative moves and track the position yourself.
- Movement is delivered a packet at a time as the program reads it, so a long
  move takes a few frames and needs the emulator **running**. The reply says
  `consumed: true` once the program has read everything. With the emulator
  paused, only about three packets (roughly 700 units) fit; a bigger move fails
  with a message telling you to continue and repeat.
- If nothing reads the mouse for about 1.5 seconds of emulated time, the call
  fails with "the program is not consuming mouse data".
- Wheel events need the wheel mode (PS/2 IntelliMouse) to have been enabled by
  the program or KERNAL; otherwise `mouse_wheel` returns an error.
- Moving the real mouse over the emulator window at the same time mixes with
  the injected input.

(\* = required)

## Example session

Boot, run a BASIC statement, then catch the IRQ handler:

1. Wait until the text layer shows `READY.` (call `run_frames` with 60 until
   `read_memory` of `space: "vram"`, `address: "0x1B000"`, `count: 15360` has it
   on screen).
2. `type_text` `{"text": "PRINT 123*2\n", "wait": true}`
3. `run_frames` `{"frames": 10}`, then `get_screen` shows ` 246`.
4. Read the KERNAL IRQ vector: `read_memory` `{"address": "0x0314", "count": 2}`.
5. `set_breakpoint` at that address, then `continue` `{"wait": true}`; the
   result gives `pc`, registers and banks at the stop.
6. `step` `{"count": 5}` to walk through the handler, `read_memory` to look at
   what it touched, then `clear_breakpoint` `{"all": true}` and `continue`.

To test a small routine without a build step:

1. `pause`
2. `write_memory` `{"address": "0x0400", "hex": "A9 05 85 10 E6 10 4C 04 04"}`
3. `set_registers` `{"pc": "0x0400", "status": "0x24"}` (the `0x24` status keeps
   interrupts off while stepping)
4. `set_watchpoint` `{"address": "0x10"}`, then `continue` `{"wait": true}`: it
   stops after the `STA $10` and reports `accessing_pc` and the value written.

## Limitations

- No data-dependent conditions on breakpoints or watchpoints, and no VRAM read watchpoints (see above).
- `load_prg` assumes the release 49 layout of BASIC's pointers (see above), and
  warns on ROMs detectably older than that.
- No audio inspection, and no joystick input.
- Requests are handled serially (see above).
- Closing the emulator window ends the session (a `-nowindow` emulator has none; it ends when the client closes it).

## Running the tests

The regression tests live in `tests/mcp/`. They start the emulator as a real MCP
client would and check the protocol, every tool, and the run-control semantics
(determinism, breakpoints, watchpoints, timeouts).

```
make mcp-test
```

builds the emulator and runs them. They always run with `-nowindow`, so no
windows pop up and no display is needed; set `X16EMU_TEST_WINDOW=1` to get a
window when debugging a test. The ROM is the `rom.bin` next to the executable
(`build/rom.bin`), or `X16EMU_ROM`. A full run takes about a minute.

To run them directly, or a subset:

```
python3 tests/mcp/run_all.py build/x16emu            # all, with a summary
python3 tests/mcp/run_all.py build/x16emu -v mouse   # only test_mouse.py, every check
python3 tests/mcp/test_debug.py build/x16emu         # one script on its own
```

The `testbench/` directory is a different mechanism (the emulator's
`-testbench` mode) and cannot be combined with `-mcp`.

New tools should come with checks in the matching `test_*.py`; `mcp_testlib.py`
has the helpers (`Emu`, `check`, `wait_screen`, ...).

## For emulator developers

| File | Contents |
|---|---|
| `src/mcp.h` | The only MCP header the rest of the emulator includes: the entry points and the watchpoint hooks, with empty inline stubs when `MCP` is not defined |
| `src/mcp.c` | Transport, JSON-RPC handling, the reader thread, the job queue and pause handling |
| `src/mcp_tools.c` | The tool implementations and the tool table |
| `src/mcp_debug.c` | Breakpoints, watchpoints, stepping and stop state |
| `src/mcp_json.c` | The JSON parser and writer |
| `src/mcp_hash.c` | SHA-256 for `hash_memory` (CRC32 comes from zlib) |
| `src/mcp_paths.c` | The host path policy: root, restriction, `..` handling |
| `src/mcp_symbols.c` | Label files, name lookup and naming addresses |
| `src/profiler.c`, `src/profiler.h` | The profiler: cycles and counts per instruction address. Not MCP code; the MCP tools are its front end. Without `PROFILER` the header has empty stubs. |

A thread reads and parses requests; each `tools/call` becomes a job that the
**emulator thread** runs at the top of `emulator_loop`, so tools can touch
emulator state without locks. A tool's `run()` returns `true` when it is done;
returning `false` makes it be polled again on the next pass, which is how the
waiting tools run while the emulator keeps executing.

The server is compiled only when `MCP` is defined (CMake `ENABLE_MCP`, which
also decides whether the `src/mcp*.c` files are built). Code outside those files
calls MCP only through `mcp.h`, so it needs no `#ifdef`: without `MCP`,
`mcp_enabled` is the constant `false`, `mcp_cpu_watching()` and
`mcp_vram_watching()` return `false`, and the other functions are empty, so the
compiler removes the hooks. A new hook in emulator code belongs in `mcp.h`, with
a stub in its `#else` branch.

To add a tool: write a `run(mcp_job *job)` function and a `mcp_tool` struct
(name, description, JSON schema), then add it to the table in `mcp_tool_table()`.
Setting the environment variable `X16EMU_MCP_TEST` registers an extra
`debug_stall` tool used by the test suite for exercising the job queue.

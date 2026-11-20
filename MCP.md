# MCP server

The emulator can run as an [MCP](https://modelcontextprotocol.io) (Model Context
Protocol) server, so an AI agent or any other MCP client can inspect and drive a
running Commander X16: read memory and the screen, pause, single-step, set
breakpoints and watchpoints, poke memory, and type on the keyboard.

Start the emulator with `-mcp`. The normal emulator window opens, and the
emulator then speaks MCP on its standard input and output. There are no extra
dependencies; the JSON parser is part of the source (`src/mcp_json.c`).

```
x16emu -mcp
```

All the usual options still work (`-rom`, `-sdcard`, `-prg`, `-run`, `-zeroram`,
`-warp`, ...), except `-testbench`, which cannot be combined with `-mcp`.

## Attaching an agent

The server uses the **stdio** transport: the client starts the emulator as a
child process and talks to it over the pipe, newline-delimited JSON-RPC 2.0. You
do not start the emulator yourself; you tell your client how to launch it. The
emulator needs a display, because the window is real.

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
  `run_frames` have identical registers, RAM, VRAM and cycle counts.
- **Boot takes 3 to 5 seconds** on a typical host. Before typing at BASIC, wait
  for `READY.` to appear in the text layer (VRAM `$1B000`, 256 bytes per row,
  the character code at every other byte) or just run frames until it does.
- **Frame rate.** The emulator targets 60 fps but a slow host may run
  at 40 to 55. Use `run_frames` and the frame counter from `get_status` rather
  than wall-clock time. `lag_ms` shows how far emulation is behind real time.

## Tools

Addresses and values may be given as JSON integers or as strings such as
`"0x9F20"` or `"$9F20"`. Results are JSON text, except `get_screen`, which
returns a PNG image.

### Inspect

| Tool | Arguments | What it does |
|---|---|---|
| `get_registers` | none | pc, a, x, y, sp, status, current RAM and ROM bank, paused flag |
| `get_status` | none | frames, cycle count at last frame, clock in MHz, warp, `lag_ms`, paused, state, number of breakpoints |
| `read_memory` | `address`\*, `space`, `count`, `bank` | Reads up to 131072 bytes without side effects (I/O is not triggered). `space` is `cpu` (default; `$A000-$BFFF` banked RAM, `$C000-$FFFF` banked ROM, using the current banks unless `bank` is given) or `vram` (the 128 KB VERA space, including palette at `0x1FA00` and sprite attributes at `0x1FC00`). Returns one lowercase hex string. |
| `get_screen` | none | 640x480 PNG of the current framebuffer |

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
 "a":"0x00","x":"0x01","y":"0x08","sp":"0x01EE","status":"0x32","breakpoint":1}
```

A watchpoint stop adds `watchpoint`, `access`, `address`, `value` and
`accessing_pc`.

### Breakpoints and watchpoints

| Tool | Arguments | What it does |
|---|---|---|
| `set_breakpoint` | `address`\*, `bank` | Pauses just before the instruction at `address` executes. From `$A000` up, `bank` restricts it to that RAM bank (`$A000-$BFFF`) or ROM bank (`$C000-$FFFF`); without it the breakpoint matches in any bank. Max 64. Returns the id (the existing one if already set). |
| `clear_breakpoint` | `id` or `all` | Removes one or all |
| `list_breakpoints` | none | Ids, addresses, banks and hit counts |
| `set_watchpoint` | `address`\*, `length`, `access`, `bank` | Pauses after an instruction that reads and/or writes the range. `access` is `read`, `write` (default) or `readwrite`. Max 32. |
| `clear_watchpoint` | `id` or `all` | Removes one or all |
| `list_watchpoints` | none | Ids, ranges, access types, banks and hit counts |

Resuming from a breakpoint executes that instruction once without hitting it
again. Resuming after a watchpoint hit does not re-trigger it.

Watchpoint limits to know about:

- Only **CPU accesses** are seen. A VRAM write through the VERA data port shows
  up as an access to `$9F23` or `$9F24`, not as a VRAM address.
- **Instruction fetches count as reads**, so a read watch on code fires when it
  executes.
- Accesses made by the tools themselves (`read_memory`, `write_memory`) are not
  reported.
- The emulator stops after the accessing instruction completes, so `pc` points
  at the next instruction; `accessing_pc` is the one that did the access. If an
  instruction touches the range more than once, the first access is reported.

### Change state and send input

| Tool | Arguments | What it does |
|---|---|---|
| `write_memory` | `address`\*, `hex`\*, `space`, `bank` | Writes bytes given as hex (`"A9 01 8D 00 04"`). `cpu` space goes through the normal write path, so I/O registers have their usual side effects, but it takes no emulated time and does not trigger watchpoints. ROM (`$C000` and up) is rejected. `bank` selects the RAM bank for `$A000-$BFFF`. `vram` space writes the VERA address space directly. |
| `set_registers` | any of `pc`, `a`, `x`, `y`, `sp`, `status`, `ram_bank`, `rom_bank` | Paused only. Setting `pc` also ends a WAI/STP wait. A breakpoint at the new `pc` fires when you resume. |
| `send_key` | `key`\*, `action`, `hold_frames`, `timeout_ms` | Sends a key to the emulated PS/2 keyboard. `key` is an [SDL key name](https://wiki.libsdl.org/SDL2/SDL_Scancode) (`Return`, `A`, `Space`, `Left`, `F5`, `Left Shift`, ...). `action` is `tap` (default), `down` or `up`. `hold_frames` keeps a tapped key down for that many frames; use it for programs that poll key state (needs the emulator running). |
| `type_text` | `text`\*, `wait`, `timeout_ms` | Types into the KERNAL keyboard buffer. `\n` is Enter. If the emulator is paused the text is queued and typed after `continue`. With `wait: true` it returns when everything is typed, not when the program it started finishes. |

`type_text` is the easy way to enter BASIC lines and commands. Programs that
read the keyboard directly bypass the KERNAL buffer; use `send_key` for those.

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

- No watchpoints on VRAM addresses, no data-dependent conditions on breakpoints.
- No tool for loading a PRG file into memory. Pass `-prg` / `-run` on the
  command line, or use `type_text` with `LOAD`.
- No audio inspection, and no mouse or joystick input.
- Requests are handled serially (see above).
- The emulator window must stay open: closing it ends the session.

## For emulator developers

| File | Contents |
|---|---|
| `src/mcp.c` | Transport, JSON-RPC handling, the reader thread, the job queue and pause handling |
| `src/mcp_tools.c` | The tool implementations and the tool table |
| `src/mcp_debug.c` | Breakpoints, watchpoints, stepping and stop state |
| `src/mcp_json.c` | The JSON parser and writer |

A thread reads and parses requests; each `tools/call` becomes a job that the
**emulator thread** runs at the top of `emulator_loop`, so tools can touch
emulator state without locks. A tool's `run()` returns `true` when it is done;
returning `false` makes it be polled again on the next pass, which is how the
waiting tools run while the emulator keeps executing.

To add a tool: write a `run(mcp_job *job)` function and a `mcp_tool` struct
(name, description, JSON schema), then add it to the table in `mcp_tool_table()`.
Setting the environment variable `X16EMU_MCP_TEST` registers an extra
`debug_stall` tool used by the test suite for exercising the job queue.

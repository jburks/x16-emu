# Working on the emulator's MCP server

Notes for an agent (or person) who changes the MCP server code in this fork of the Commander X16 emulator.

**To use the MCP server** (attach a client, call the tools, read the results), read [`MCP.md`](MCP.md)
instead. That is the user documentation: every tool, its arguments and results, and the behaviour clients can
rely on. This file is about the code, the rules for changing it, and the backlog.

State as of 2026-11-23.

## Build with MCP support

The MCP server only exists in builds that include it:

- CMake option `ENABLE_MCP` (default **ON** in this fork; forced off for the WebAssembly build) adds the
  `src/mcp*.c` sources, defines `MCP`, and adds the `mcp-test` target.
- CMake option `ENABLE_PROFILER` (default ON, built only when MCP is on) adds `src/profiler.c` and defines
  `PROFILER`. The test suite expects the profiler to be built.
- `make` in the repository root wraps CMake (Release build type) and writes `build/x16emu`.
- A build without MCP has no MCP code at all. `x16emu -mcp` then prints that the emulator was built without the
  MCP server and exits with status 1.

Run it as a server, for example: `build/x16emu -mcp -nowindow -zeroram -fsroot <dir>`. `MCP.md` explains the
options, the host path root (`X16EMU_MCP_ROOT`) and how to register it with a client.

## Tests

- `make mcp-test`, or `python3 tests/mcp/run_all.py build/x16emu [-v] [name...]`: 16 scripts, about 760
  checks, about 85 s. Always windowless.
- They need the R49 ROM at `build/rom.bin` (gitignored). `X16EMU` and `X16EMU_ROM` override the emulator and
  ROM paths; `X16EMU_TEST_WINDOW=1` shows a window.
- Helpers are in `tests/mcp/mcp_testlib.py` (`Emu`, `check`, `wait_screen`, ...).
- Every new tool gets checks in a `tests/mcp/test_*.py` and a row in `MCP.md`. Add its name to the exact tool
  lists in `test_queue.py` and `test_tools.py`.
- Fixtures built with cc65 live in `tests/mcp/fixtures/` (`scoped/`, `calls/`). Each has a `build.sh`; the built
  outputs (`.bin`, `.dbg`, `.sym`) are committed, so the tests do not need cc65. New fixture code goes at the
  end, so that existing addresses (which the tests hard-code) do not move.
- Measure with emulated time (`cycles`, the frame counter), never wall-clock time: the host speed varies.
- **Known flaky check:** the boot-determinism check in `test_extras.py` ("identical RAM (except the stack page)"
  and "identical VRAM") fails about once in 20 runs, only when several tests run in parallel. In a bad run, the
  first of the two boots comes up in 40×30 screen mode (`$0261` = 3, `$0386` = `$28`). Ruled out: host
  keyboard input (tests run `-nowindow`), and a reset in the middle of the KERNAL's keyboard read over I2C.
  Remaining suspect: at boot the KERNAL reads its screen-mode profile from the RTC's NVRAM over I2C, and the
  emulated RTC follows host time. Also found: `machine_reset()` does not clear some SMC/I2C state
  (`i2c_data_pos`, `i2c_data[]`, `default_read_state` and `mse_count` in `smc.c`; `device` and `old_i2c_port` in
  `i2c.c`). Not fixed.

## Architecture

All MCP code is in `src/`:

| File | What it holds |
|---|---|
| `mcp.h` | The only MCP header the rest of the emulator includes. Without `MCP` it provides constants and empty inline stubs, so the hooks compile away. New hooks into emulator code go here, each with a stub. |
| `mcp.c` | stdio transport (newline-delimited JSON-RPC), reader thread, job queue, pause and resume, per-call timeouts. |
| `mcp_tools.c` | Every tool and the tool table; argument helpers (`arg_uint`, `arg_addr`, which accepts labels); `host_path()`. |
| `mcp_debug.c` | Breakpoints, watchpoints, stepping, stop state, stop results. |
| `mcp_symbols.c/.h` | Labels (ld65 `-Ln`) and cc65 debug info (`--dbgfile`): scope-path names, scope ranges, source lines, name resolution and ambiguity, the tail-call test. |
| `mcp_json.c`, `mcp_hash.c`, `mcp_paths.c` | JSON parser and writer (no dependencies), SHA-256 (CRC32 from zlib), host path policy. |
| `mcp_internal.h` | Shared declarations inside the MCP code. |
| `profiler.c/.h` | The profiler. It is not MCP code; the MCP tools are its front end. It covers cycles and counts per address, and call tracking. |

Key points:

- **Threading:** tool jobs run on the emulator thread, at the top of the emulator loop (`mcp_service()`), so tools
  read and change emulator state without locks.
- **stdout is reserved for the protocol.** `mcp_enable()` points C's stdout at stderr; never print to fd 1.
- **Hooks in existing code** cost one integer test when unused:
  - `memory.c` (watchpoints);
  - `video.c` (VRAM watchpoints, screenshots, VERA state);
  - `i2c.c` (`mouse_has_room`);
  - `main.c` (`-mcp`, `-nowindow`, the service call, and the profiler step: `if (profiler_on()) profiler_step(); else step6502();`).
- **Call tracking has no hooks in the CPU core.** After each `step6502()` it reads the CPU's `opcode` global. It
  detects interrupts at the next step: the pc moved and the stack dropped by 3 (4 in 65C816 native mode).
- **The disassembler** is the emulator's own `disasm.c`, reused.

## Behaviour that must be preserved

These were decided deliberately. `MCP.md` documents them for clients.

- **Run control:**
  - The emulator is running or paused, and a pause falls between instructions.
  - Tool calls take no emulated time.
  - `run_frames` stops right after the instruction that completes the frame.
  - A timed-out call leaves the emulator paused.
  - `cycles` is an exact 64-bit count.
- **Determinism:** `-zeroram` makes launches identical. `reset {zero_ram}` zeroes RAM and reseeds so that VRAM
  matches a fresh start, and it keeps breakpoints, watchpoints and counters.
- **Breakpoints:**
  - They are checked before the instruction at the pc runs.
  - On resume, a breakpoint is skipped once if the pc hasn't changed since the stop.
  - A bank applies only from `$A000` up.
  - At most 64.
- **Watchpoints:**
  - **CPU:** read, write or both. The stop comes after the accessing instruction, which is reported as `accessing_pc`. Instruction fetches count as reads unless `exclude_fetch` is set, which is a documented heuristic.
  - **VRAM:** writes only.
  - At most 32 in total.
- **Host files:** every host path is limited to the host path root. A drive root is refused, and symbolic links
  are not examined (documented).
- **Symbols:**
  - Names are scope paths from debug info (`proc::label`).
  - As input, a bare name works only if it is unique; an ambiguous one is an error listing the candidates.
  - `name@$XXXX` picks one label.
  - A name that is still shared is shown as `name@$XXXX`.
- **Source positions:**
  - `file`/`line` is the line as written (where the macro is used; for `.repeat`, the `.endrepeat` line).
  - `macro_file`/`macro_line` is the innermost body line.
  - For C compiled by cc65, `file`/`line` is the C line.
- **Call tracking:**
  - A frame closes only on RTS, RTI, RTL, TXS or TCS, once the stack pointer is above the frame's return address.
  - Interrupt time is left out of the routine that was interrupted.
  - A recursive routine's time counts its outermost call only.
  - A tail call is a JMP or branch to the start of a `.proc` that lies outside the jumping code's outermost scope.
  - `self_cycles` comes from the address view (scope ownership), so it can exceed a routine's inclusive cycles.
- **Transport** is stdio only (HTTP was considered and not done).

## Rules for changing the code

- Keep everything MCP-related behind `ENABLE_MCP`/`MCP` (and the profiler behind `PROFILER`) using the stub
  header pattern, so that a build without them is identical to upstream in the hot paths.
- Portability (CI builds Linux x86_64/ARM, Windows, macOS, WebAssembly):
  - CI predefines `_XOPEN_SOURCE=600` on Linux. Never `#define` a different value, and guard any such define with `#ifndef`.
  - `-Werror` is on, and gcc 10's `-Wformat-truncation` is strict, so size `snprintf` buffers generously.
  - Stay compatible with SDL 2.0.10 (don't use `SDL_GetTicks64`).
  - The WebAssembly build must keep linking.
- Prefer rich, structured results over prose: the clients are agents.
- Say what was measured and what was not; say what is untested.
- `.github/workflows/build.yml` contains fork-only changes (documentation PDFs fetched from the fork's docs
  repository, the 32-bit Windows job removed) that must not go into an upstream pull request.
- Write to the fork (`jburks/x16-emu`) only, never to the upstream X16Community repositories. With the `gh`
  CLI, pass `-R jburks/x16-emu` to every command that writes, or it may resolve to upstream.
- Commit or push only when the maintainer asks. `CMakeLists.txt` may carry the maintainer's own uncommitted SDL2
  change: never stage it (`git add -p`).

## Backlog

### Done recently

All on the `mcp` branch. Check `git log` for what is pushed and released:

- cc65 debug info, with scope-path names and the ambiguity rule;
- `profile_report` `by: "scope"`;
- source positions (`file`, `line`, `macro_file`, `macro_line`);
- call tracking (`profile_start calls`, `by: "routine"`);
- `by: "line"`.

These were requested by the main user of the server, the OtterVeX graphics library's agent. Its write-ups are in
`SCOPED-SYMBOLS.md` and `MCP-SUGGESTIONS.md` in this folder (untracked).

### Deferred, in priority order

Ask the maintainer which to take next.

1. **Code coverage.** It builds on what exists. The profiler's per-address counts already mark code that never
   ran, and the debug info gives code segments, scope ranges and object modules (`mod`/`lib` records). Proposed:
   - an option for `by: "scope"` that also lists routines that never ran, with `size`, `executed_bytes` and a percentage (executed bytes counted from the instruction lengths of executed instructions);
   - a `by: "module"` view per object module and library, which is the actionable one for size, because ld65 links whole object modules, never single routines;
   - later, merging the counts of several programs.

   Caveats: a count of 0 means "not run in this test", not dead. Data in the CODE segment and self-modifying
   code look unexecuted.

   Finding that motivated it: OtterVeX's library is linked from a single object module, so every program gets
   all of it. Consider asking the OtterVeX agent which views it needs before building.
2. **ROM label files.** The plan is parked, and four decisions are open: ask before implementing.
   - **Findings for ROM r49:**
     - The ROM build writes a `.sym` per bank, with no bank in the addresses.
     - Each has its 16 KB `.bin` beside it, so the bank can be detected by comparing with the running ROM.
     - `$A000-$BFFF` labels in ROM files are RAM bank 0 variables.
     - 218 names exist in several banks, as KERNAL-call trampolines.
     - About 1,600 cheap locals (`@...`).
   - **Design:**
     - `load_symbols` gets `rom_bank` and `ram_bank` arguments, with the bank detected from the `.bin` when neither is given.
     - Loading a whole directory.
     - A name in several banks is an error unless `bank` is given.
     - `@` labels are never used to name an address.
     - Time the profiler's naming with every ROM file loaded.
   - **Open decisions (recommended answer first):**
     - Accept a bank that is at least 90% equal, with a warning (the alternative is exact only).
     - An ambiguous name is an error.
     - `@` labels are never used for naming.
     - Load ROM labels below `$A000` by default.
3. **`run_cycles`:** run until `cycles` reaches N, stopping at the first instruction boundary at or past N and
   reporting the overshoot (it cannot be exact).
4. **Exact `exclude_fetch`:** mark the bytes the CPU core really fetched, instead of the 4-byte-window
   heuristic. This touches the hot read path, so measure the cost first.
5. **Reset determinism** (low priority, though the OtterVeX side values determinism). After `reset {zero_ram}`,
   a few dead stack bytes differ from run to run, and the cycles from reset to the first BASIC idle depend on
   history: some device state survives `machine_reset()`. Related: the flaky test and the SMC/I2C state listed
   under Tests. Diff the device state (VIA timers, I2C/SMC, audio, RTC) after a reset against a fresh start.
6. **Smaller follow-ups:**
   - Decode `jsrfar` in call tracking: KERNAL calls through it show as calls of `jsrfar`.
   - Source positions in `disassemble`.
   - A non-MCP front end for the profiler (for example `-profile <file>` written at exit), so it would be useful upstream and need not require MCP.
7. **Open items from `MCP-SUGGESTIONS.md`:**
   - `load_prg` paths resolve against the host path root, not the emulated drive. Document this more clearly or offer both.
   - The cycle count at the first request differs between launches.
   - Optionally prefer exported labels over inner labels when naming.
   - Document how a prerelease ROM (`$FF80` = `$CF`) is treated by `load_prg`.
8. **Upstream bug, for a separate pull request** (not mixed into MCP work), read from the source and not
   reproduced:
   - `mouse_send_state()` in `src/i2c.c` loops `while (diff_x != 0 && diff_y != 0)`. It should be `||`, because one-axis moves over 255 leave movement behind.
   - `mouse_send()` drops packets when its buffer is full, while the caller still subtracts the movement.

   The MCP tools avoid both by checking `mouse_has_room()`.
9. **Untested so far:**
   - Windows path handling;
   - running the macOS, Windows and ARM builds;
   - MCP clients other than Claude Code;
   - the speed cost of the watchpoint hooks.
10. **Possible upstream pull requests:**
    - the CI commits that set the Release build type and bundle `libz-1.dll` for Win64;
    - the `macos-15` runner;
    - the WebAssembly template's `&lt;=` fix;
    - conditional compilation of the MCP server (upstream would likely want it off by default).

### Rejected

- **Save and restore state.** The emulator has no snapshot support, and a partial one would drift. Booting takes
  about 0.7 s, and `reset` with `set_warp` covers the need.
- **VRAM read watchpoints.** VERA's prefetch makes them misleading. A bounded version (data-port reads only,
  reporting the prefetch source address) only if a real need appears.

## Releases

Releases are pre-releases on the fork, tagged `mcp-vX.Y.Z` (annotated). The last one is `mcp-v0.5.1`, which is
older than everything under "Done recently". The process:

1. Push the branch and wait for CI to pass.
2. Push the tag; the tag's CI run builds the artifacts.
3. Create a draft release (`-R jburks/x16-emu`).
4. The maintainer attaches the zipped artifacts and publishes.

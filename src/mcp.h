// Commander X16 Emulator
// MCP (Model Context Protocol) server: JSON-RPC 2.0 over stdin/stdout.
// All rights reserved. License: 2-clause BSD

// This is the only MCP header the rest of the emulator includes. The server is
// optional: it is compiled in when MCP is defined (CMake option ENABLE_MCP).
// Without it, every function below is an empty inline stub and mcp_enabled is
// the constant false, so the hooks in the emulator compile to nothing and the
// call sites need no #ifdef.

#ifndef MCP_H
#define MCP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef MCP

extern bool mcp_enabled;

// Call as soon as -mcp is parsed. Reserves the process's real stdout for
// protocol messages and redirects everything else that is written to stdout
// (banners, printf debugging) to stderr, so it can never corrupt the stream.
void mcp_enable(void);

// Call once SDL is initialized, right before the emulator loop starts.
// Starts the thread that reads requests from stdin.
void mcp_start(void);

// Call at the top of every emulator loop pass, on the emulator thread. Runs
// queued tool calls. Costs one atomic read when there is nothing to do.
void mcp_service(void);

// Pause state. The tools run on the emulator thread, so these are only used from
// there. While paused, emulator_loop does not execute instructions, but still
// calls mcp_service().
bool mcp_is_paused(void);
void mcp_set_paused(bool paused);
// Called by emulator_loop instead of running an instruction while paused.
// Sleeps briefly and pumps window events. Returns false if the user closed the
// window (or the client disconnected), meaning the loop should exit.
bool mcp_idle_while_paused(void);

// Watchpoint hooks. memory.c and video.c test mcp_cpu_watching() /
// mcp_vram_watching() on every access, so they must stay one integer test.
extern int mcp_watch_active;  // number of CPU-space watchpoints
extern int mcp_vwatch_active; // number of VRAM watchpoints
static inline bool mcp_cpu_watching(void) { return mcp_watch_active != 0; }
static inline bool mcp_vram_watching(void) { return mcp_vwatch_active != 0; }
// Called from read6502/write6502 only when mcp_cpu_watching().
void mcp_watch_hit(uint16_t address, bool write, uint8_t value);
// Called from video.c after the CPU stores a byte in VRAM through a data port
// (port 0 or 1), only when mcp_vram_watching(). value is the byte now in VRAM.
void mcp_vram_watch_hit(uint32_t address, uint8_t value, int port);

#else // !MCP: the server is not compiled in

#include <stdio.h>
#include <stdlib.h>

#define mcp_enabled false

static inline void
mcp_enable(void)
{
	fprintf(stderr, "-mcp: this emulator was built without the MCP server (CMake option ENABLE_MCP)\n");
	exit(1);
}
static inline void mcp_start(void) {}
static inline void mcp_service(void) {}
static inline bool mcp_is_paused(void) { return false; }
static inline bool mcp_idle_while_paused(void) { return true; }
static inline bool mcp_cpu_watching(void) { return false; }
static inline bool mcp_vram_watching(void) { return false; }
static inline void mcp_watch_hit(uint16_t address, bool write, uint8_t value) { (void)address; (void)write; (void)value; }
static inline void mcp_vram_watch_hit(uint32_t address, uint8_t value, int port) { (void)address; (void)value; (void)port; }

#endif // MCP

#endif

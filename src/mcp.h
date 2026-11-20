// Commander X16 Emulator
// MCP (Model Context Protocol) server: JSON-RPC 2.0 over stdin/stdout.
// All rights reserved. License: 2-clause BSD

#ifndef MCP_H
#define MCP_H

#include <stdbool.h>

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

#endif

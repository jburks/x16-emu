// Commander X16 Emulator
// MCP server internals shared between the protocol/queue code (mcp.c) and the
// tool implementations (mcp_tools.c).
// All rights reserved. License: 2-clause BSD

#ifndef MCP_INTERNAL_H
#define MCP_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include "mcp_json.h"

// A tool call in flight. Created by the reader thread, run on the EMULATOR
// thread (so tools may touch emulator state freely, without locks), then
// completed back to the reader thread.
typedef struct mcp_job {
	struct mcp_job *next;
	const struct mcp_tool *tool;
	json_node *msg;         // the whole request; the job owns it
	const json_node *args;  // the "arguments" object inside msg, or NULL
	unsigned polls;         // 0 on the first call of run(), then counts up
	uint64_t scratch[2];    // free for the tool's use between polls

	// Result, filled in by run():
	bool is_error;          // tool execution error (the model sees the text)
	json_buf text;          // result text

	// Completion handshake, protected by the queue lock in mcp.c
	bool done;
	bool abandoned;         // the reader gave up waiting (timeout)
} mcp_job;

typedef struct mcp_tool {
	const char *name;
	const char *description;
	const char *input_schema;   // a complete JSON Schema object, as JSON text
	// Called on the emulator thread: once when the job starts, then again every
	// pass of the emulator loop until it returns true. Return true when the
	// result in job->text / job->is_error is final. A tool that is not done
	// must return quickly: the emulator is not running while it executes.
	bool (*run)(mcp_job *job);
	unsigned timeout_ms;        // 0 = default (X16EMU_MCP_TIMEOUT_MS, else 30000)
} mcp_tool;

// mcp_tools.c: the registry. Returns a NULL-terminated array.
const mcp_tool *const *mcp_tool_table(void);

// Helpers for tool implementations
void mcp_job_fail(mcp_job *job, const char *message);  // sets is_error and the text

#endif

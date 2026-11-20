// Commander X16 Emulator
// MCP server internals shared between the protocol/queue code (mcp.c) and the
// tool implementations (mcp_tools.c).
// All rights reserved. License: 2-clause BSD

#ifndef MCP_INTERNAL_H
#define MCP_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
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
	json_buf text;          // result text (may be empty if there is an image)
	uint8_t *image;         // optional image result, malloc'd; the job frees it
	size_t image_len;
	const char *image_mime; // e.g. "image/png"

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
	unsigned timeout_ms;        // 0 = default (X16EMU_MCP_TIMEOUT_MS, else 30000); a call may
	                            // override it with a numeric "timeout_ms" argument
	// Optional. Called on the emulator thread when the reader gave up waiting
	// (timeout) on a job that had started but not finished. Use it to leave the
	// emulator in a sane state. The job is dropped afterwards.
	void (*cancel)(mcp_job *job);
	// By default a job that is not finished is polled once per video frame while the emulator runs (and on
	// every idle pass while it is paused), which keeps the cost of waiting negligible. A tool that needs to
	// react between two instructions sets this to be polled on every pass instead.
	bool poll_every_pass;
} mcp_tool;

// mcp_tools.c: the registry. Returns a NULL-terminated array.
const mcp_tool *const *mcp_tool_table(void);

// ---- mcp_debug.c: breakpoints and stop state (emulator thread only) ----

typedef enum {
	STOP_NONE,        // never stopped, or running since the last resume
	STOP_PAUSE,       // paused by the pause tool or by a cancelled/timed-out run
	STOP_BREAKPOINT,
	STOP_FRAMES,      // run_frames completed
	STOP_STEP,        // step completed
	STOP_WATCHPOINT
} mcp_stop_reason;

typedef struct {
	mcp_stop_reason reason;
	int breakpoint_id;  // valid for STOP_BREAKPOINT
	int watch_id;       // valid for STOP_WATCHPOINT
} mcp_stop_info;

extern mcp_stop_info mcp_last_stop;
extern int mcp_bp_active; // number of breakpoints
// True while anything needs mcp_debug_check() each pass (breakpoints, a step in progress, a pending watch hit).
extern bool mcp_debug_armed;
// Number of watchpoints; memory.c checks it on every CPU access so it must stay a plain int.
extern int mcp_watch_active;  // number of CPU-space watchpoints
extern int mcp_vwatch_active; // number of VRAM watchpoints (video.c checks this on every VRAM write)

#define MCP_MAX_BREAKPOINTS 64
int mcp_bp_add(uint16_t address, int bank, bool *existed); // bank -1 = any; returns id, or -1 if full
bool mcp_bp_remove(int id);
void mcp_bp_clear(void);
// Iterates breakpoints: pass index 0..; returns false past the end.
bool mcp_bp_get(int index, int *id, uint16_t *address, int *bank, unsigned *hits);
// Watchpoints on CPU accesses (instruction fetches count as reads). A hit pauses
// the emulator after the accessing instruction completes.
#define MCP_MAX_WATCHPOINTS 32
typedef enum { WATCH_READ = 1, WATCH_WRITE = 2, WATCH_RW = 3 } mcp_watch_access_t;
typedef enum { WATCH_SPACE_CPU = 0, WATCH_SPACE_VRAM = 1 } mcp_watch_space_t;
// exclude_fetch (CPU reads only): ignore reads of the bytes of the instruction being executed.
int mcp_watch_add(int space, uint32_t address, uint32_t length, int bank, int access, bool exclude_fetch, bool *existed); // -1 if full
bool mcp_watch_remove(int id);
void mcp_watch_clear(void);
int mcp_watch_total(void);
bool mcp_watch_get(int index, int *id, int *space, uint32_t *address, uint32_t *length, int *bank, int *access,
                   bool *exclude_fetch, unsigned *hits);
// Called from read6502/write6502 only when mcp_watch_active != 0.
void mcp_watch_hit(uint16_t address, bool write, uint8_t value);
// Called from video.c after the CPU stores a byte in VRAM through a data port (port 0 or 1), only when
// mcp_vwatch_active != 0. value is the byte now in VRAM.
void mcp_vram_watch_hit(uint32_t address, uint8_t value, int port);
// Set while a tool performs its own memory accesses, so they do not trigger watchpoints.
extern bool mcp_watch_suppress;
// Starts executing `n` instructions from the paused state, then pauses (STOP_STEP).
void mcp_step_begin(uint32_t n);
// Called every emulator pass while running and breakpoints exist: pauses on a hit.
void mcp_debug_check(void);
// Resumes a paused emulator without re-hitting a breakpoint at the current pc.
void mcp_resume(void);
// Marks the emulator paused with a reason.
void mcp_stop(mcp_stop_reason reason);
// "running", "paused", "breakpoint" or "frames"
const char *mcp_stop_name(void);
// {"stopped":"...","pc":"0x....",...} describing the current stop state.
void mcp_describe_stop(json_buf *out);

// Exact number of CPU clock cycles executed since start (not just as of the last frame).
uint64_t mcp_total_cycles(void);

// Helpers for tool implementations
void mcp_job_fail(mcp_job *job, const char *message);  // sets is_error and the text

#endif

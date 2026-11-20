// Commander X16 Emulator
// MCP tool implementations. Every run() executes on the emulator thread.
// All rights reserved. License: 2-clause BSD

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <SDL.h>
#include "glue.h"
#include "memory.h"
#include "video.h"
#include "keyboard.h"
#include "i2c.h"
#include "cpu/fake6502.h"
#include "mcp.h"
#include "mcp_internal.h"

// timing.c: counted once per completed video frame
extern uint32_t frames;
extern int64_t cpu_ticks;
extern uint32_t sdlTicks_base; // wall-clock origin of the speed limiter

void
mcp_job_fail(mcp_job *job, const char *message)
{
	job->is_error = true;
	job->text.len = 0;
	if (job->text.data) {
		job->text.data[0] = 0;
	}
	json_buf_append(&job->text, message);
}

// Reads an unsigned integer argument given either as a JSON number or as a
// string ("1234", "0x9F20", "$9F20"). Returns false if absent or invalid.
static bool
arg_uint(const mcp_job *job, const char *name, uint32_t *out)
{
	const json_node *n = json_get(job->args, name);
	if (!n) {
		return false;
	}
	if (n->type == JSON_NUMBER) {
		if (n->number < 0 || n->number > 4294967295.0 || n->number != (double)(uint32_t)n->number) {
			return false;
		}
		*out = (uint32_t)n->number;
		return true;
	}
	if (n->type == JSON_STRING && n->string[0]) {
		const char *t = n->string;
		int base = 10;
		if (t[0] == '$') {
			t++;
			base = 16;
		} else if (t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) {
			t += 2;
			base = 16;
		}
		char *end;
		unsigned long v = strtoul(t, &end, base);
		if (*t == 0 || *end != 0 || *t == '-' || v > 0xFFFFFFFFul) {
			return false;
		}
		*out = (uint32_t)v;
		return true;
	}
	return false;
}

// ---- get_registers ----

static bool
run_get_registers(mcp_job *job)
{
	char tmp[256];
	snprintf(tmp, sizeof(tmp),
		"{\"pc\":\"0x%04X\",\"a\":\"0x%02X\",\"x\":\"0x%02X\",\"y\":\"0x%02X\","
		"\"sp\":\"0x%04X\",\"status\":\"0x%02X\",\"ram_bank\":%u,\"rom_bank\":%u,\"paused\":%s}",
		regs.pc, regs.a, regs.xl, regs.yl, regs.sp, regs.status,
		memory_get_ram_bank(), memory_get_rom_bank(), mcp_is_paused() ? "true" : "false");
	json_buf_append(&job->text, tmp);
	return true;
}

static const mcp_tool tool_get_registers = {
	"get_registers",
	"Returns the 65C02 registers (pc, a, x, y, sp, status), the current RAM and ROM bank, and whether the emulator is paused.",
	"{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
	run_get_registers,
	0
};

// ---- get_status ----

static bool
run_get_status(mcp_job *job)
{
	char tmp[320];
	// How far the emulation is behind wall-clock time, as the speed limiter sees it.
	// Near 0 or negative: keeping up (and being throttled). Growing: the host cannot
	// run the emulator at full speed.
	long long lag_ms = (long long)(SDL_GetTicks() - sdlTicks_base) - cpu_ticks / MHZ / 1000;
	snprintf(tmp, sizeof(tmp),
		"{\"frames\":%u,\"cpu_cycles_at_last_frame\":%lld,\"mhz\":%u,\"warp\":%s,\"lag_ms\":%lld,\"paused\":%s,"
		"\"state\":\"%s\",\"breakpoints\":%d}",
		frames, (long long)cpu_ticks, (unsigned)MHZ, warp_mode ? "true" : "false", lag_ms,
		mcp_is_paused() ? "true" : "false", mcp_stop_name(), mcp_bp_active);
	json_buf_append(&job->text, tmp);
	return true;
}

static const mcp_tool tool_get_status = {
	"get_status",
	"Returns emulator progress: the number of video frames completed since start (about 60 per emulated second), "
	"the CPU cycle count at the last completed frame, the emulated clock in MHz, warp mode, lag_ms (how far emulation is behind wall-clock time; it grows if the host cannot keep up), whether paused, the state (running, paused, breakpoint, watchpoint, step or frames: why it last stopped) and the number of breakpoints.",
	"{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
	run_get_status,
	0
};

// ---- read_memory ----

#define VRAM_SIZE 0x20000u
#define MAX_READ 0x20000u

static bool
run_read_memory(mcp_job *job)
{
	const json_node *sp = json_get(job->args, "space");
	const char *space = sp ? json_as_string(sp) : "cpu";
	if (!space || (strcmp(space, "cpu") && strcmp(space, "vram"))) {
		mcp_job_fail(job, "space must be \"cpu\" or \"vram\"");
		return true;
	}
	bool vram = !strcmp(space, "vram");
	uint32_t address, count = 16, bank = 0;
	if (!arg_uint(job, "address", &address)) {
		mcp_job_fail(job, "address is required: a non-negative integer or a string like \"0x9F20\" or \"$9F20\"");
		return true;
	}
	if (json_get(job->args, "count") && !arg_uint(job, "count", &count)) {
		mcp_job_fail(job, "count must be a non-negative integer");
		return true;
	}
	bool have_bank = json_get(job->args, "bank") != NULL;
	if (have_bank && (!arg_uint(job, "bank", &bank) || bank > 255)) {
		mcp_job_fail(job, "bank must be an integer from 0 to 255");
		return true;
	}
	if (have_bank && vram) {
		mcp_job_fail(job, "bank only applies to space \"cpu\"");
		return true;
	}
	uint32_t limit = vram ? VRAM_SIZE : 0x10000u;
	if (count == 0 || count > MAX_READ) {
		mcp_job_fail(job, "count must be between 1 and 131072");
		return true;
	}
	if (address >= limit || count > limit - address) {
		char msg[96];
		snprintf(msg, sizeof(msg), "range exceeds the %s address space (0x%X bytes)", space, limit);
		mcp_job_fail(job, msg);
		return true;
	}

	json_buf *t = &job->text;
	char head[160];
	snprintf(head, sizeof(head), "{\"space\":\"%s\",\"address\":\"0x%0*X\",\"count\":%u,",
	         space, vram ? 5 : 4, address, count);
	json_buf_append(t, head);
	if (!vram) {
		snprintf(head, sizeof(head), "\"ram_bank\":%u,\"rom_bank\":%u,",
		         have_bank ? bank : memory_get_ram_bank(), have_bank ? bank : memory_get_rom_bank());
		json_buf_append(t, head);
	}
	json_buf_append(t, "\"hex\":\"");
	static const char hexd[] = "0123456789abcdef";
	for (uint32_t i = 0; i < count; i++) {
		uint8_t v = vram ? video_space_read(address + i)
		                 : debug_read6502((uint16_t)(address + i), 0, have_bank ? (int16_t)bank : USE_CURRENT_X16_BANK);
		char two[2] = { hexd[v >> 4], hexd[v & 15] };
		json_buf_append_n(t, two, 2);
	}
	json_buf_append(t, "\"}");
	return true;
}

static const mcp_tool tool_read_memory = {
	"read_memory",
	"Reads bytes without side effects (I/O registers are not triggered). space \"cpu\" (default) is the 64 KB CPU "
	"address space as the CPU sees it: $A000-$BFFF is banked RAM and $C000-$FFFF is banked ROM, using the current "
	"banks unless `bank` is given (it selects both the RAM and the ROM bank). space \"vram\" is the 128 KB VERA "
	"address space (0x00000-0x1FFFF), including the palette at 0x1FA00 and sprite attributes at 0x1FC00. "
	"Returns JSON with the bytes as one lowercase hex string.",
	"{\"type\":\"object\",\"properties\":{"
		"\"space\":{\"type\":\"string\",\"enum\":[\"cpu\",\"vram\"],\"default\":\"cpu\"},"
		"\"address\":{\"type\":[\"integer\",\"string\"],\"description\":\"integer or string such as \\\"0x9F20\\\" or \\\"$9F20\\\"\"},"
		"\"count\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":131072,\"default\":16},"
		"\"bank\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255,\"description\":\"cpu space only\"}},"
	 "\"required\":[\"address\"],\"additionalProperties\":false}",
	run_read_memory,
	0
};

// ---- get_screen ----

static bool
run_get_screen(mcp_job *job)
{
	int len = 0;
	uint8_t *png = video_get_png(&len);
	if (!png || len <= 0) {
		free(png);
		mcp_job_fail(job, "could not encode the screen");
		return true;
	}
	job->image = png;
	job->image_len = (size_t)len;
	job->image_mime = "image/png";
	json_buf_append(&job->text, "640x480 screenshot of the emulator's current framebuffer");
	return true;
}

static const mcp_tool tool_get_screen = {
	"get_screen",
	"Returns a 640x480 PNG screenshot of the emulator's current framebuffer. If the emulator is running, the image "
	"may show a frame that is still being drawn; pause first for a stable image.",
	"{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
	run_get_screen,
	0
};

// ---- pause / continue ----

static bool
run_pause(mcp_job *job)
{
	bool was = mcp_is_paused();
	if (!was) {
		mcp_stop(STOP_PAUSE);
	}
	char tmp[96];
	snprintf(tmp, sizeof(tmp), "%s at pc=0x%04X", was ? "already paused" : "paused", regs.pc);
	json_buf_append(&job->text, tmp);
	return true;
}

static const mcp_tool tool_pause = {
	"pause",
	"Pauses the emulator between two instructions. Memory, registers and the screen can then be inspected without "
	"them changing. The window stays open.",
	"{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
	run_pause,
	0
};

static void
cancel_run(mcp_job *job)
{
	(void)job;
	mcp_stop(STOP_PAUSE); // the client gave up: leave the emulator paused and inspectable
}

static bool
run_continue(mcp_job *job)
{
	const json_node *w = json_get(job->args, "wait");
	bool wait = w && w->type == JSON_BOOL && w->boolean;
	if (job->polls == 0) {
		if (w && w->type != JSON_BOOL) {
			mcp_job_fail(job, "wait must be true or false");
			return true;
		}
		if (wait && mcp_bp_active == 0 && mcp_watch_active == 0) {
			mcp_job_fail(job, "wait=true needs at least one breakpoint or watchpoint, otherwise it would never return");
			return true;
		}
		bool was = mcp_is_paused();
		if (was) {
			mcp_resume();
		}
		if (!wait) {
			json_buf_append(&job->text, was ? "running" : "was not paused; running");
			return true;
		}
		return false;
	}
	if (mcp_is_paused()) { // a breakpoint stopped it
		mcp_describe_stop(&job->text);
		return true;
	}
	return false;
}

static const mcp_tool tool_continue = {
	"continue",
	"Resumes a paused emulator at normal speed. With wait=true it does not return until a breakpoint stops the "
	"emulator (breakpoint or watchpoint; returns where it stopped); on timeout the emulator is paused. Optional timeout_ms (default 30000, "
	"max 600000) applies to wait=true.",
	"{\"type\":\"object\",\"properties\":{\"wait\":{\"type\":\"boolean\",\"default\":false},"
	"\"timeout_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":600000}},\"additionalProperties\":false}",
	run_continue,
	0,
	cancel_run
};

// ---- run_frames ----

static bool
run_run_frames(mcp_job *job)
{
	if (job->polls == 0) {
		uint32_t n = 1;
		if (json_get(job->args, "frames") && (!arg_uint(job, "frames", &n) || n < 1 || n > 1000000)) {
			mcp_job_fail(job, "frames must be an integer from 1 to 1000000");
			return true;
		}
		job->scratch[0] = (uint32_t)(frames + n);
		if (mcp_is_paused()) {
			mcp_resume();
		} else {
			mcp_last_stop.reason = STOP_NONE;
		}
		return false;
	}
	if (mcp_is_paused()) { // a breakpoint stopped it before the frames were up
		mcp_describe_stop(&job->text);
		return true;
	}
	if ((int32_t)(frames - (uint32_t)job->scratch[0]) >= 0) {
		mcp_stop(STOP_FRAMES);
		mcp_describe_stop(&job->text);
		return true;
	}
	return false;
}

static const mcp_tool tool_run_frames = {
	"run_frames",
	"Runs the emulator for exactly `frames` video frames (about 1/60 s each; default 1) and then pauses it, so the "
	"stop point is deterministic: right after the instruction that completed the last frame. Works from paused or "
	"running. Stops early, still paused, if a breakpoint is hit. Returns where it stopped (stopped is \"frames\" or "
	"\"breakpoint\"). At roughly 40-60 frames/s, pass a larger timeout_ms (max 600000) for long runs; on timeout the "
	"emulator is left paused.",
	"{\"type\":\"object\",\"properties\":{\"frames\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":1000000,\"default\":1},"
	"\"timeout_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":600000}},\"additionalProperties\":false}",
	run_run_frames,
	0,
	cancel_run
};

// ---- breakpoints ----

static bool
run_set_breakpoint(mcp_job *job)
{
	uint32_t address, bank = 0;
	if (!arg_uint(job, "address", &address) || address > 0xFFFF) {
		mcp_job_fail(job, "address is required: 0 to 0xFFFF as an integer or a string like \"0xC000\" or \"$C000\"");
		return true;
	}
	int b = -1;
	if (json_get(job->args, "bank")) {
		if (!arg_uint(job, "bank", &bank) || bank > 255) {
			mcp_job_fail(job, "bank must be an integer from 0 to 255");
			return true;
		}
		if (address < 0xA000) {
			mcp_job_fail(job, "bank only applies to addresses from $A000 up (RAM bank for $A000-$BFFF, ROM bank for $C000-$FFFF)");
			return true;
		}
		b = (int)bank;
	}
	bool existed;
	int id = mcp_bp_add((uint16_t)address, b, &existed);
	if (id < 0) {
		mcp_job_fail(job, "too many breakpoints (64 maximum)");
		return true;
	}
	char bankstr[16] = "null";
	if (b >= 0) {
		snprintf(bankstr, sizeof(bankstr), "%d", b);
	}
	char tmp[160];
	snprintf(tmp, sizeof(tmp), "{\"id\":%d,\"address\":\"0x%04X\",\"bank\":%s,\"already_set\":%s}",
	         id, address, bankstr, existed ? "true" : "false");
	json_buf_append(&job->text, tmp);
	return true;
}

static const mcp_tool tool_set_breakpoint = {
	"set_breakpoint",
	"Sets an execution breakpoint: the emulator pauses just before the instruction at `address` executes. For "
	"addresses from $A000 up, an optional `bank` restricts it to that RAM bank ($A000-$BFFF) or ROM bank "
	"($C000-$FFFF); without it the breakpoint matches in any bank. Resuming from a breakpoint steps over it once. "
	"Setting the same address and bank again returns the existing breakpoint. Returns its id.",
	"{\"type\":\"object\",\"properties\":{"
		"\"address\":{\"type\":[\"integer\",\"string\"],\"description\":\"integer or string such as \\\"0xC000\\\" or \\\"$C000\\\"\"},"
		"\"bank\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255}},"
	 "\"required\":[\"address\"],\"additionalProperties\":false}",
	run_set_breakpoint,
	0,
	NULL
};

static bool
run_clear_breakpoint(mcp_job *job)
{
	const json_node *all = json_get(job->args, "all");
	uint32_t id;
	bool have_id = json_get(job->args, "id") != NULL;
	if (all && all->type == JSON_BOOL && all->boolean) {
		if (have_id) {
			mcp_job_fail(job, "pass either id or all, not both");
			return true;
		}
		int n = mcp_bp_active;
		mcp_bp_clear();
		char tmp[48];
		snprintf(tmp, sizeof(tmp), "{\"removed\":%d}", n);
		json_buf_append(&job->text, tmp);
		return true;
	}
	if (!have_id || !arg_uint(job, "id", &id)) {
		mcp_job_fail(job, "pass id (an integer from set_breakpoint or list_breakpoints) or all=true");
		return true;
	}
	if (!mcp_bp_remove((int)id)) {
		mcp_job_fail(job, "no such breakpoint id");
		return true;
	}
	json_buf_append(&job->text, "{\"removed\":1}");
	return true;
}

static const mcp_tool tool_clear_breakpoint = {
	"clear_breakpoint",
	"Removes the breakpoint with the given id, or all breakpoints with all=true.",
	"{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\"},\"all\":{\"type\":\"boolean\"}},\"additionalProperties\":false}",
	run_clear_breakpoint,
	0,
	NULL
};

static bool
run_list_breakpoints(mcp_job *job)
{
	json_buf_append(&job->text, "[");
	int id, bank, i = 0;
	uint16_t address;
	unsigned hits;
	while (mcp_bp_get(i, &id, &address, &bank, &hits)) {
		char tmp[128];
		if (bank < 0) {
			snprintf(tmp, sizeof(tmp), "%s{\"id\":%d,\"address\":\"0x%04X\",\"bank\":null,\"hits\":%u}",
			         i ? "," : "", id, address, hits);
		} else {
			snprintf(tmp, sizeof(tmp), "%s{\"id\":%d,\"address\":\"0x%04X\",\"bank\":%d,\"hits\":%u}",
			         i ? "," : "", id, address, bank, hits);
		}
		json_buf_append(&job->text, tmp);
		i++;
	}
	json_buf_append(&job->text, "]");
	return true;
}

static const mcp_tool tool_list_breakpoints = {
	"list_breakpoints",
	"Lists the breakpoints with their ids, addresses, banks (null = any) and how often each was hit.",
	"{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
	run_list_breakpoints,
	0,
	NULL
};

// ---- step ----

static bool
run_step(mcp_job *job)
{
	if (job->polls == 0) {
		uint32_t n = 1;
		if (json_get(job->args, "count") && (!arg_uint(job, "count", &n) || n < 1 || n > 1000000)) {
			mcp_job_fail(job, "count must be an integer from 1 to 1000000");
			return true;
		}
		if (!mcp_is_paused()) {
			mcp_job_fail(job, "the emulator must be paused to step (call pause first)");
			return true;
		}
		mcp_step_begin(n);
		return false;
	}
	if (mcp_is_paused()) { // done, or a breakpoint/watchpoint came first
		mcp_describe_stop(&job->text);
		return true;
	}
	return false;
}

static const mcp_tool tool_step = {
	"step",
	"Executes `count` CPU instructions (default 1) from the paused state and pauses again. Returns where it stopped "
	"and the registers; stopped is \"step\", or \"breakpoint\"/\"watchpoint\" if one came first. A WAI/STP that is "
	"waiting for an interrupt is stepped over: execution continues to the next real instruction (usually the IRQ "
	"handler). Interrupt handlers are not skipped; they step like any other code.",
	"{\"type\":\"object\",\"properties\":{\"count\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":1000000,\"default\":1},"
	"\"timeout_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":600000}},\"additionalProperties\":false}",
	run_step,
	0,
	cancel_run
};

// ---- watchpoints ----

static const char *
access_name(int a)
{
	return a == WATCH_READ ? "read" : a == WATCH_WRITE ? "write" : "readwrite";
}

static bool
run_set_watchpoint(mcp_job *job)
{
	uint32_t address, length = 1, bank = 0;
	if (!arg_uint(job, "address", &address) || address > 0xFFFF) {
		mcp_job_fail(job, "address is required: 0 to 0xFFFF as an integer or a string like \"0x9F23\" or \"$9F23\"");
		return true;
	}
	if (json_get(job->args, "length") && (!arg_uint(job, "length", &length) || length < 1 || length > 0x10000
	                                       || address + length > 0x10000)) {
		mcp_job_fail(job, "length must be at least 1 and the range must stay within $0000-$FFFF");
		return true;
	}
	int access = WATCH_WRITE;
	const json_node *an = json_get(job->args, "access");
	if (an) {
		const char *a = json_as_string(an);
		if (a && !strcmp(a, "read")) access = WATCH_READ;
		else if (a && !strcmp(a, "write")) access = WATCH_WRITE;
		else if (a && !strcmp(a, "readwrite")) access = WATCH_RW;
		else {
			mcp_job_fail(job, "access must be \"read\", \"write\" or \"readwrite\"");
			return true;
		}
	}
	int b = -1;
	if (json_get(job->args, "bank")) {
		if (!arg_uint(job, "bank", &bank) || bank > 255) {
			mcp_job_fail(job, "bank must be an integer from 0 to 255");
			return true;
		}
		b = (int)bank;
	}
	bool existed;
	int id = mcp_watch_add((uint16_t)address, length, b, access, &existed);
	if (id < 0) {
		mcp_job_fail(job, "too many watchpoints (32 maximum)");
		return true;
	}
	char bankstr[16] = "null";
	if (b >= 0) {
		snprintf(bankstr, sizeof(bankstr), "%d", b);
	}
	char tmp[200];
	snprintf(tmp, sizeof(tmp), "{\"id\":%d,\"address\":\"0x%04X\",\"length\":%u,\"access\":\"%s\",\"bank\":%s,\"already_set\":%s}",
	         id, address, length, access_name(access), bankstr, existed ? "true" : "false");
	json_buf_append(&job->text, tmp);
	return true;
}

static const mcp_tool tool_set_watchpoint = {
	"set_watchpoint",
	"Pauses the emulator when the CPU reads and/or writes an address range (default: writes to one byte). The "
	"emulator stops right after the accessing instruction completes; the result reports watchpoint, access, address, "
	"value and accessing_pc. Instruction fetches count as reads, so a read watch on code fires when it executes. "
	"Only CPU accesses are seen: VRAM writes through the VERA data ports show up as accesses to $9F23/$9F24, not as "
	"VRAM addresses, and reads by tools are not reported. Addresses from $A000 up can be restricted to a RAM bank "
	"($A000-$BFFF) or ROM bank ($C000-$FFFF). Resuming after a hit does not re-trigger it. Max 32.",
	"{\"type\":\"object\",\"properties\":{"
		"\"address\":{\"type\":[\"integer\",\"string\"]},"
		"\"length\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":65536,\"default\":1},"
		"\"access\":{\"type\":\"string\",\"enum\":[\"read\",\"write\",\"readwrite\"],\"default\":\"write\"},"
		"\"bank\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255}},"
	 "\"required\":[\"address\"],\"additionalProperties\":false}",
	run_set_watchpoint,
	0,
	NULL
};

static bool
run_clear_watchpoint(mcp_job *job)
{
	const json_node *all = json_get(job->args, "all");
	uint32_t id;
	bool have_id = json_get(job->args, "id") != NULL;
	if (all && all->type == JSON_BOOL && all->boolean) {
		if (have_id) {
			mcp_job_fail(job, "pass either id or all, not both");
			return true;
		}
		int n = mcp_watch_active;
		mcp_watch_clear();
		char tmp[48];
		snprintf(tmp, sizeof(tmp), "{\"removed\":%d}", n);
		json_buf_append(&job->text, tmp);
		return true;
	}
	if (!have_id || !arg_uint(job, "id", &id)) {
		mcp_job_fail(job, "pass id (an integer from set_watchpoint or list_watchpoints) or all=true");
		return true;
	}
	if (!mcp_watch_remove((int)id)) {
		mcp_job_fail(job, "no such watchpoint id");
		return true;
	}
	json_buf_append(&job->text, "{\"removed\":1}");
	return true;
}

static const mcp_tool tool_clear_watchpoint = {
	"clear_watchpoint",
	"Removes the watchpoint with the given id, or all watchpoints with all=true.",
	"{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\"},\"all\":{\"type\":\"boolean\"}},\"additionalProperties\":false}",
	run_clear_watchpoint,
	0,
	NULL
};

static bool
run_list_watchpoints(mcp_job *job)
{
	json_buf_append(&job->text, "[");
	int id, bank, access, i = 0;
	uint16_t address;
	uint32_t length;
	unsigned hits;
	while (mcp_watch_get(i, &id, &address, &length, &bank, &access, &hits)) {
		char bankstr[16] = "null";
		if (bank >= 0) {
			snprintf(bankstr, sizeof(bankstr), "%d", bank);
		}
		char tmp[200];
		snprintf(tmp, sizeof(tmp), "%s{\"id\":%d,\"address\":\"0x%04X\",\"length\":%u,\"access\":\"%s\",\"bank\":%s,\"hits\":%u}",
		         i ? "," : "", id, address, length, access_name(access), bankstr, hits);
		json_buf_append(&job->text, tmp);
		i++;
	}
	json_buf_append(&job->text, "]");
	return true;
}

static const mcp_tool tool_list_watchpoints = {
	"list_watchpoints",
	"Lists the watchpoints with their ids, ranges, access types, banks (null = any) and hit counts.",
	"{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
	run_list_watchpoints,
	0,
	NULL
};

// ---- write_memory ----

// Decodes hex digits (whitespace allowed between bytes). Returns the byte count, or -1.
static int
decode_hex(const char *s, uint8_t *out, size_t max)
{
	size_t n = 0;
	int hi = -1;
	for (; *s; s++) {
		int v;
		if (*s >= '0' && *s <= '9') v = *s - '0';
		else if (*s >= 'a' && *s <= 'f') v = *s - 'a' + 10;
		else if (*s >= 'A' && *s <= 'F') v = *s - 'A' + 10;
		else if (*s == ' ' || *s == '\n' || *s == '\t' || *s == '\r') {
			if (hi >= 0) return -1; // whitespace inside a byte
			continue;
		} else return -1;
		if (hi < 0) {
			hi = v;
		} else {
			if (n >= max) return -1;
			out[n++] = (uint8_t)(hi << 4 | v);
			hi = -1;
		}
	}
	return hi >= 0 ? -1 : (int)n;
}

static bool
run_write_memory(mcp_job *job)
{
	const json_node *sp = json_get(job->args, "space");
	const char *space = sp ? json_as_string(sp) : "cpu";
	if (!space || (strcmp(space, "cpu") && strcmp(space, "vram"))) {
		mcp_job_fail(job, "space must be \"cpu\" or \"vram\"");
		return true;
	}
	bool vram = !strcmp(space, "vram");
	uint32_t address, bank = 0;
	if (!arg_uint(job, "address", &address)) {
		mcp_job_fail(job, "address is required: a non-negative integer or a string like \"0x0400\" or \"$0400\"");
		return true;
	}
	const char *hex = json_as_string(json_get(job->args, "hex"));
	if (!hex) {
		mcp_job_fail(job, "hex is required: the bytes as a hex string such as \"A9 01 8D 00 04\"");
		return true;
	}
	bool have_bank = json_get(job->args, "bank") != NULL;
	if (have_bank && (!arg_uint(job, "bank", &bank) || bank >= num_ram_banks)) {
		mcp_job_fail(job, "bank must be a valid RAM bank number");
		return true;
	}
	if (have_bank && vram) {
		mcp_job_fail(job, "bank only applies to space \"cpu\"");
		return true;
	}
	uint8_t *data = malloc(MAX_READ);
	if (!data) {
		mcp_job_fail(job, "out of memory");
		return true;
	}
	int n = decode_hex(hex, data, MAX_READ);
	if (n <= 0) {
		free(data);
		mcp_job_fail(job, "hex must contain 1 to 131072 bytes as pairs of hex digits");
		return true;
	}
	uint32_t limit = vram ? VRAM_SIZE : 0xC000u; // CPU ROM is not writable
	if (address >= limit || (uint32_t)n > limit - address) {
		free(data);
		char msg[112];
		snprintf(msg, sizeof(msg), vram ? "range exceeds VRAM (0x20000 bytes)"
		                                : "range must lie within $0000-$BFFF (ROM at $C000 and up is read-only)");
		mcp_job_fail(job, msg);
		return true;
	}

	mcp_watch_suppress = true;
	int64_t saved_ticks = clockticks6502;
	for (int i = 0; i < n; i++) {
		uint32_t a = address + (uint32_t)i;
		if (vram) {
			video_space_write(a, data[i]);
		} else if (have_bank && a >= 0xA000) {
			BRAM[((size_t)bank << 13) + a - 0xA000] = data[i];
		} else {
			write6502((uint16_t)a, 0, data[i]); // I/O and the CPU port keep their side effects
		}
	}
	clockticks6502 = (uint32_t)saved_ticks; // slow I/O writes must not disturb timing
	mcp_watch_suppress = false;
	free(data);

	char tmp[96];
	snprintf(tmp, sizeof(tmp), "{\"written\":%d,\"space\":\"%s\",\"address\":\"0x%0*X\"}", n, space, vram ? 5 : 4, address);
	json_buf_append(&job->text, tmp);
	return true;
}

static const mcp_tool tool_write_memory = {
	"write_memory",
	"Writes bytes given as a hex string. space \"cpu\" (default) writes through the normal CPU write path, so "
	"I/O registers such as VERA ($9F20-$9F3F) have their usual side effects, but it takes no emulated time and does "
	"not trigger watchpoints; ROM ($C000+) is rejected. For $A000-$BFFF, `bank` selects the RAM bank to write "
	"(default: the current one). space \"vram\" writes the 128 KB VERA address space directly (palette, sprite "
	"attributes and PSG registers included). Works while paused or running; pause first if the program could be "
	"touching the same memory.",
	"{\"type\":\"object\",\"properties\":{"
		"\"space\":{\"type\":\"string\",\"enum\":[\"cpu\",\"vram\"],\"default\":\"cpu\"},"
		"\"address\":{\"type\":[\"integer\",\"string\"]},"
		"\"hex\":{\"type\":\"string\",\"description\":\"bytes as hex digit pairs, spaces allowed\"},"
		"\"bank\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255,\"description\":\"RAM bank for $A000-$BFFF, cpu space only\"}},"
	 "\"required\":[\"address\",\"hex\"],\"additionalProperties\":false}",
	run_write_memory,
	0,
	NULL
};

// ---- set_registers ----

static bool
run_set_registers(mcp_job *job)
{
	if (!mcp_is_paused()) {
		mcp_job_fail(job, "the emulator must be paused to change registers (call pause first)");
		return true;
	}
	static const struct { const char *name; uint32_t max; } fields[] = {
		{ "pc", 0xFFFF }, { "a", 0xFF }, { "x", 0xFF }, { "y", 0xFF }, { "sp", 0x1FF },
		{ "status", 0xFF }, { "ram_bank", 0xFF }, { "rom_bank", 0xFF }
	};
	uint32_t v[8];
	bool have[8] = { false };
	bool any = false;
	for (const struct json_node *m = job->args ? job->args->first : NULL; m; m = m->next) {
		bool known = false;
		for (int i = 0; i < 8; i++) {
			if (!strcmp(m->key, fields[i].name)) known = true;
		}
		if (!known) {
			mcp_job_fail(job, "unknown register; use pc, a, x, y, sp, status, ram_bank, rom_bank");
			return true;
		}
	}
	for (int i = 0; i < 8; i++) {
		if (!json_get(job->args, fields[i].name)) {
			continue;
		}
		if (!arg_uint(job, fields[i].name, &v[i]) || v[i] > fields[i].max) {
			char msg[96];
			snprintf(msg, sizeof(msg), "%s must be an integer from 0 to 0x%X", fields[i].name, fields[i].max);
			mcp_job_fail(job, msg);
			return true;
		}
		have[i] = true;
		any = true;
	}
	if (!any) {
		mcp_job_fail(job, "pass at least one of pc, a, x, y, sp, status, ram_bank, rom_bank");
		return true;
	}
	if (have[0]) { regs.pc = (uint16_t)v[0]; waiting = 0; }
	if (have[1]) regs.a = (uint8_t)v[1];
	if (have[2]) regs.xl = (uint8_t)v[2];
	if (have[3]) regs.yl = (uint8_t)v[3];
	if (have[4]) regs.sp = (uint16_t)((regs.sp & 0xFF00) | (v[4] & 0xFF));
	if (have[5]) regs.status = (uint8_t)v[5];
	if (have[6]) memory_set_ram_bank((uint8_t)v[6]);
	if (have[7]) memory_set_rom_bank((uint8_t)v[7]);
	run_get_registers(job);
	return true;
}

static const mcp_tool tool_set_registers = {
	"set_registers",
	"Sets any of pc, a, x, y, sp (low byte), status, ram_bank, rom_bank while the emulator is paused, and returns "
	"the resulting registers. Setting pc also ends a WAI/STP wait. A breakpoint at the new pc will fire when you "
	"resume.",
	"{\"type\":\"object\",\"properties\":{"
		"\"pc\":{\"type\":[\"integer\",\"string\"]},\"a\":{\"type\":[\"integer\",\"string\"]},"
		"\"x\":{\"type\":[\"integer\",\"string\"]},\"y\":{\"type\":[\"integer\",\"string\"]},"
		"\"sp\":{\"type\":[\"integer\",\"string\"]},\"status\":{\"type\":[\"integer\",\"string\"]},"
		"\"ram_bank\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255},"
		"\"rom_bank\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255}},"
	 "\"additionalProperties\":false}",
	run_set_registers,
	0,
	NULL
};

// ---- send_key / type_text ----

extern bool pasting_bas;
extern char *paste_text;

static void
cancel_key(mcp_job *job)
{
	handle_keyboard(false, SDLK_UNKNOWN, (SDL_Scancode)job->scratch[1]); // never leave a key stuck down
}

static bool
run_send_key(mcp_job *job)
{
	if (job->polls == 0) {
		const char *name = json_as_string(json_get(job->args, "key"));
		if (!name) {
			mcp_job_fail(job, "key is required: an SDL key name such as \"Return\", \"A\", \"Space\", \"Left\", \"F5\", \"Left Shift\"");
			return true;
		}
		SDL_Scancode sc = SDL_GetScancodeFromName(name);
		if (sc == SDL_SCANCODE_UNKNOWN) {
			mcp_job_fail(job, "unknown key name (SDL names: letters, digits, \"Return\", \"Space\", \"Escape\", \"Left\", \"Up\", \"F1\"..\"F12\", \"Left Shift\", ...)");
			return true;
		}
		const json_node *an = json_get(job->args, "action");
		const char *action = an ? json_as_string(an) : "tap";
		if (!action || (strcmp(action, "tap") && strcmp(action, "down") && strcmp(action, "up"))) {
			mcp_job_fail(job, "action must be \"tap\", \"down\" or \"up\"");
			return true;
		}
		uint32_t hold = 0;
		if (json_get(job->args, "hold_frames") && (!arg_uint(job, "hold_frames", &hold) || hold > 100000)) {
			mcp_job_fail(job, "hold_frames must be an integer from 0 to 100000");
			return true;
		}
		job->scratch[1] = (uint64_t)sc;
		if (!strcmp(action, "up")) {
			handle_keyboard(false, SDLK_UNKNOWN, sc);
		} else {
			handle_keyboard(true, SDLK_UNKNOWN, sc);
			if (!strcmp(action, "tap")) {
				if (hold == 0) {
					handle_keyboard(false, SDLK_UNKNOWN, sc);
				} else {
					if (mcp_is_paused()) {
						handle_keyboard(false, SDLK_UNKNOWN, sc);
						mcp_job_fail(job, "hold_frames needs the emulator running (time does not pass while paused)");
						return true;
					}
					job->scratch[0] = (uint32_t)(frames + hold);
					return false;
				}
			}
		}
		char tmp[64];
		snprintf(tmp, sizeof(tmp), "{\"key\":\"%s\",\"action\":\"%s\"}", SDL_GetScancodeName(sc), action);
		json_buf_append(&job->text, tmp);
		return true;
	}
	if ((int32_t)(frames - (uint32_t)job->scratch[0]) >= 0) {
		handle_keyboard(false, SDLK_UNKNOWN, (SDL_Scancode)job->scratch[1]);
		json_buf_append(&job->text, "{\"action\":\"tap\"}");
		return true;
	}
	return false;
}

static const mcp_tool tool_send_key = {
	"send_key",
	"Sends a key press to the emulated PS/2 keyboard (it reaches the program through the KERNAL or a game's own "
	"keyboard scan). `key` is an SDL key name such as \"Return\", \"A\", \"Space\", \"Left\", \"F5\" or \"Left Shift\". "
	"`action` is \"tap\" (default: down then up), \"down\" or \"up\" (to hold modifiers or movement keys). "
	"hold_frames (tap only, default 0) keeps the key down for that many video frames while the emulator runs; "
	"use it for programs that poll key state. Keys are queued even while paused and are delivered when running. "
	"For text, use type_text.",
	"{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\"},"
	"\"action\":{\"type\":\"string\",\"enum\":[\"tap\",\"down\",\"up\"],\"default\":\"tap\"},"
	"\"hold_frames\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100000},"
	"\"timeout_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":600000}},\"required\":[\"key\"],\"additionalProperties\":false}",
	run_send_key,
	0,
	cancel_key
};

// Queues text to be typed into the KERNAL keyboard buffer. Returns NULL on success, else an error message.
static const char *
queue_typed(const char *text, size_t len)
{
	if (pasting_bas || paste_text) {
		return "a previous text is still being typed";
	}
	char *copy = SDL_malloc(len + 1);
	if (!copy) {
		return "out of memory";
	}
	for (size_t i = 0; i <= len; i++) {
		copy[i] = text[i] == '\n' ? '\r' : text[i]; // the X16 Enter key is CR
	}
	machine_paste(copy, true); // typed into the KERNAL keyboard buffer as the program runs
	return NULL;
}

static bool
run_type_text(mcp_job *job)
{
	if (job->polls == 0) {
		const json_node *tn = json_get(job->args, "text");
		const char *text = json_as_string(tn);
		if (!text || !text[0] || tn->string_len != strlen(text) || tn->string_len > 60000) {
			mcp_job_fail(job, "text is required: 1 to 60000 characters, no NUL");
			return true;
		}
		const json_node *w = json_get(job->args, "wait");
		bool wait = w && w->type == JSON_BOOL && w->boolean;
		if (wait && mcp_is_paused() && !pasting_bas) {
			mcp_job_fail(job, "wait=true needs the emulator running (typing only progresses while it runs)");
			return true;
		}
		const char *err = queue_typed(text, tn->string_len);
		if (err) {
			mcp_job_fail(job, err);
			return true;
		}
		if (!wait) {
			json_buf_append(&job->text, "{\"queued\":true}");
			return true;
		}
		return false;
	}
	if (!pasting_bas) {
		json_buf_append(&job->text, "{\"typed\":true}");
		return true;
	}
	return false;
}

static const mcp_tool tool_type_text = {
	"type_text",
	"Types text into the KERNAL keyboard buffer as if typed, e.g. a BASIC line or command; use \"\\n\" for Enter. "
	"Characters are ISO-8859-15, and \"\\\\Xhh\" inserts a raw PETSCII byte. Typing only progresses while the "
	"emulator runs (and interrupts are enabled), so with the emulator paused the text is queued and typed after "
	"continue. With wait=true it returns once everything has been typed (not once the program it started has "
	"finished). The KERNAL buffer is bypassed by programs that read the keyboard directly; use send_key for those.",
	"{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"},\"wait\":{\"type\":\"boolean\",\"default\":false},"
	"\"timeout_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":600000}},\"required\":[\"text\"],\"additionalProperties\":false}",
	run_type_text,
	0,
	NULL
};

// ---- load_prg ----

// Where BASIC keeps its program-end pointers, per KERNAL release (the byte at $FF80). Real LOAD sets all
// three to the end address; without that, BASIC would put variables on top of the loaded program.
typedef struct { int8_t release; uint16_t txttab, vartab, arytab, strend; } basic_layout_t;
static const basic_layout_t basic_layouts[] = {
	{ 49, 0x00DF, 0x03E1, 0x03E3, 0x03E5 },
};

static bool
run_load_prg(mcp_job *job)
{
	const char *path = json_as_string(json_get(job->args, "path"));
	if (!path || !path[0]) {
		mcp_job_fail(job, "path is required: the PRG file on the host (an absolute path is safest)");
		return true;
	}
	const json_node *rawn = json_get(job->args, "raw");
	const json_node *runn = json_get(job->args, "run");
	if ((rawn && rawn->type != JSON_BOOL) || (runn && runn->type != JSON_BOOL)) {
		mcp_job_fail(job, "raw and run must be true or false");
		return true;
	}
	bool raw = rawn && rawn->boolean, run = runn && runn->boolean;
	uint32_t override = 0, bank = 0;
	bool have_addr = json_get(job->args, "address") != NULL;
	bool have_bank = json_get(job->args, "bank") != NULL;
	if (have_addr && (!arg_uint(job, "address", &override) || override > 0xFFFF)) {
		mcp_job_fail(job, "address must be 0 to 0xFFFF as an integer or a string like \"0x0801\"");
		return true;
	}
	if (raw && !have_addr) {
		mcp_job_fail(job, "raw files have no load address in them: pass address");
		return true;
	}
	if (have_bank && (!arg_uint(job, "bank", &bank) || bank >= num_ram_banks)) {
		mcp_job_fail(job, "bank must be a valid RAM bank number");
		return true;
	}

	FILE *f = fopen(path, "rb");
	if (!f) {
		char msg[600];
		snprintf(msg, sizeof(msg), "cannot open %.400s: %s", path, strerror(errno));
		mcp_job_fail(job, msg);
		return true;
	}
	enum { MAX_PRG = 0x100000 };
	uint8_t *file = malloc(MAX_PRG + 1);
	size_t n = file ? fread(file, 1, MAX_PRG + 1, f) : 0;
	bool read_err = ferror(f);
	fclose(f);
	const char *fail = NULL;
	if (!file) fail = "out of memory";
	else if (read_err) fail = "error reading the file (is it a directory?)";
	else if (n > MAX_PRG) fail = "file is larger than 1 MB";
	if (fail) {
		free(file);
		mcp_job_fail(job, fail);
		return true;
	}

	const uint8_t *data = file;
	size_t len = n;
	uint32_t start;
	if (raw) {
		start = override;
	} else {
		if (n < 3) {
			free(file);
			mcp_job_fail(job, "not a PRG: it needs a 2-byte load address followed by at least one byte of data");
			return true;
		}
		start = have_addr ? override : (uint32_t)(file[0] | file[1] << 8);
		data += 2;
		len -= 2;
	}
	if (len == 0) {
		free(file);
		mcp_job_fail(job, "the file has no data");
		return true;
	}
	if (have_bank && start < 0xA000) {
		free(file);
		mcp_job_fail(job, "bank only applies to load addresses from $A000 up");
		return true;
	}

	// Check the whole destination before touching anything. Low RAM ends at $9EFF; banked RAM is
	// $A000-$BFFF and, as with the KERNAL's LOAD, continues in the next bank after $BFFF.
	uint32_t a = start, b = have_bank ? bank : (uint32_t)memory_get_ram_bank();
	for (size_t i = 0; i < len; i++) {
		if (a >= 0xA000 && a < 0xC000) {
			if (b >= num_ram_banks) {
				fail = "the data runs past the last RAM bank";
				break;
			}
		} else if (a >= 0x9F00) {
			fail = a < 0xA000 ? "the data would overwrite the I/O area ($9F00-$9FFF)"
			                  : "the data would run into ROM ($C000 and up)";
			break;
		}
		if (++a == 0xC000 && start >= 0xA000) {
			a = 0xA000;
			b++;
		}
	}
	if (fail) {
		free(file);
		mcp_job_fail(job, fail);
		return true;
	}
	uint32_t end = a & 0xFFFF; // exclusive; after a bank wrap this is the address in the last bank

	// BASIC program? Then BASIC's pointers must move with it, as the LOAD command does.
	bool basic = start == 0x0801 && !have_bank;
	int rel = debug_read6502(0xFF80, 0, 0);
	const char *ptr_status = "n/a";
	int layout = -1;
	if (basic) {
		for (size_t i = 0; i < sizeof(basic_layouts) / sizeof(basic_layouts[0]); i++) {
			if (basic_layouts[i].release == (int8_t)rel) layout = (int)i;
		}
		if (layout >= 0 && (RAM[basic_layouts[layout].txttab] | RAM[basic_layouts[layout].txttab + 1] << 8) != 0x0801) {
			layout = -1; // BASIC's start-of-program pointer is not where we expect it
		}
		ptr_status = layout >= 0 ? "set" : "not set (unknown ROM release)";
	}
	if (run && basic && layout < 0) {
		free(file);
		mcp_job_fail(job, "cannot run a BASIC program on this ROM release: BASIC's pointers are unknown, so variables "
		                  "would overwrite the program. Load without run and start it yourself");
		return true;
	}
	char cmd[32] = "";
	if (run) {
		snprintf(cmd, sizeof(cmd), basic ? "RUN\n" : "SYS $%04X\n", start);
		const char *err = queue_typed(cmd, strlen(cmd));
		if (err) {
			free(file);
			mcp_job_fail(job, err);
			return true;
		}
	}

	a = start;
	b = have_bank ? bank : (uint32_t)memory_get_ram_bank();
	for (size_t i = 0; i < len; i++) {
		if (a < 0x9F00) {
			RAM[a] = data[i];
		} else {
			BRAM[((size_t)b << 13) + a - 0xA000] = data[i];
		}
		if (++a == 0xC000 && start >= 0xA000) {
			a = 0xA000;
			b++;
		}
	}
	free(file);
	if (layout >= 0) {
		const basic_layout_t *l = &basic_layouts[layout];
		uint16_t ptrs[3] = { l->vartab, l->arytab, l->strend };
		for (int i = 0; i < 3; i++) {
			RAM[ptrs[i]] = (uint8_t)end;
			RAM[ptrs[i] + 1] = (uint8_t)(end >> 8);
		}
	}

	char tmp[320];
	snprintf(tmp, sizeof(tmp),
	         "{\"start\":\"0x%04X\",\"end\":\"0x%04X\",\"bytes\":%zu,\"basic\":%s,\"basic_pointers\":\"%s\","
	         "\"started\":%s,\"start_command\":\"",
	         start, end, len, basic ? "true" : "false", ptr_status, run ? "true" : "false");
	json_buf_append(&job->text, tmp);
	if (basic) {
		json_buf_append(&job->text, "RUN");
	} else {
		snprintf(tmp, sizeof(tmp), "SYS $%04X", start);
		json_buf_append(&job->text, tmp);
	}
	json_buf_append(&job->text, "\"}");
	return true;
}

static const mcp_tool tool_load_prg = {
	"load_prg",
	"Loads a PRG file from the host into emulated memory, instantly and without emulated time (nothing else is "
	"disturbed, so runs stay deterministic). The first two bytes of the file are the load address, as with the "
	"KERNAL's LOAD; `address` overrides it, and `raw` treats the whole file as data (then `address` is required). "
	"`bank` selects the RAM bank for loads at $A000 and up (default: the current one); like the KERNAL, data that "
	"runs past $BFFF continues in the next bank. A load at $0801 is taken to be a BASIC program, and BASIC's "
	"program-end pointers are set as the LOAD command would (supported ROM releases only; the result says whether "
	"they were set). With run=true the emulator is told to start it by typing RUN (BASIC) or SYS <address> (machine "
	"code), which needs the emulator running at the BASIC prompt. Do not load while another program is running "
	"unless you mean to overwrite it. Returns the start, the end (exclusive) and the size.",
	"{\"type\":\"object\",\"properties\":{"
		"\"path\":{\"type\":\"string\",\"description\":\"host file path\"},"
		"\"address\":{\"type\":[\"integer\",\"string\"],\"description\":\"load address, overriding the file's\"},"
		"\"raw\":{\"type\":\"boolean\",\"default\":false,\"description\":\"file has no 2-byte load address\"},"
		"\"bank\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255},"
		"\"run\":{\"type\":\"boolean\",\"default\":false}},"
	 "\"required\":[\"path\"],\"additionalProperties\":false}",
	run_load_prg,
	0,
	NULL
};

// ---- mouse ----
// The X16 mouse is a PS/2 mouse: it reports relative movement, and the KERNAL
// keeps the pointer position (when a program enables it, e.g. BASIC "MOUSE 1").

static int32_t pk_lo(uint64_t v) { return (int32_t)(uint32_t)v; }
static int32_t pk_hi(uint64_t v) { return (int32_t)(uint32_t)(v >> 32); }
static uint64_t pk(int32_t a, int32_t b) { return (uint64_t)(uint32_t)a | (uint64_t)(uint32_t)b << 32; }

static int32_t
clamp255(int32_t v)
{
	return v > 255 ? 255 : v < -255 ? -255 : v;
}

// Reads a signed integer argument; returns false if absent or not an integer in [lo, hi].
static bool
arg_int(const mcp_job *job, const char *name, int32_t lo, int32_t hi, int32_t *out)
{
	const json_node *n = json_get(job->args, name);
	if (!n || n->type != JSON_NUMBER || n->number != (double)(int32_t)n->number
	    || n->number < lo || n->number > hi) {
		return false;
	}
	*out = (int32_t)n->number;
	return true;
}

// Jobs run one at a time, so the mouse tools can share this: the frame at which they last made progress.
static uint32_t mouse_progress;
#define MOUSE_STALL_FRAMES 90
static bool
mouse_stalled(void)
{
	return (int32_t)(frames - mouse_progress) > MOUSE_STALL_FRAMES;
}
static const char mouse_stall_msg[] =
	"the program is not consuming mouse data (is the mouse enabled? in BASIC: MOUSE 1); part of it may have been delivered";
// Waits (only while running) until the program has read everything queued, so that on return the
// KERNAL's pointer position and button state are up to date. Returns true when it is time to answer.
static bool
mouse_drained(void)
{
	return mcp_is_paused() || i2c_mse_buffer_count() == 0 || mouse_stalled();
}

static bool
run_mouse_move(mcp_job *job)
{
	if (job->polls == 0) {
		mouse_progress = frames;
		int32_t dx = 0, dy = 0, x = 0, y = 0;
		bool have_d = json_get(job->args, "dx") || json_get(job->args, "dy");
		bool have_abs = json_get(job->args, "x") || json_get(job->args, "y");
		if (have_d == have_abs) {
			mcp_job_fail(job, "pass either dx and/or dy (relative), or x and y (absolute), not both and not neither");
			return true;
		}
		if (have_d) {
			if ((json_get(job->args, "dx") && !arg_int(job, "dx", -4096, 4096, &dx))
			    || (json_get(job->args, "dy") && !arg_int(job, "dy", -4096, 4096, &dy))) {
				mcp_job_fail(job, "dx and dy must be integers from -4096 to 4096");
				return true;
			}
			job->scratch[0] = pk(dx, dy);
			job->scratch[1] = 0;
		} else {
			if (!arg_int(job, "x", 0, 1023, &x) || !arg_int(job, "y", 0, 1023, &y)) {
				mcp_job_fail(job, "absolute moves need both x and y, integers from 0 to 1023");
				return true;
			}
			// Drive the pointer into the top-left corner (the KERNAL clamps it there), then out to x,y.
			job->scratch[0] = pk(-1024, -1024);
			job->scratch[1] = pk(x, y);
		}
	}
	int32_t dx = pk_lo(job->scratch[0]), dy = pk_hi(job->scratch[0]);
	for (;;) {
		if (dx == 0 && dy == 0) {
			if (job->scratch[1] == 0) {
				if (!mouse_drained()) {
					job->scratch[0] = 0;
					return false;
				}
				json_buf_append(&job->text, i2c_mse_buffer_count() ? "{\"moved\":true,\"consumed\":false}" : "{\"moved\":true,\"consumed\":true}");
				return true;
			}
			dx = pk_lo(job->scratch[1]);
			dy = pk_hi(job->scratch[1]);
			job->scratch[1] = 0;
			continue;
		}
		if (!mouse_has_room()) {
			if (mcp_is_paused()) {
				mcp_job_fail(job, "the mouse buffer is full and the emulator is paused, so the program cannot consume "
				                  "it; continue (or run_frames) and repeat. Part of the move was already delivered");
				return true;
			}
			if (mouse_stalled()) {
				mcp_job_fail(job, mouse_stall_msg);
				return true;
			}
			job->scratch[0] = pk(dx, dy);
			return false;
		}
		mouse_progress = frames;
		int32_t cx = clamp255(dx), cy = clamp255(dy);
		mouse_move(cx, cy);
		mouse_send_state();
		dx -= cx;
		dy -= cy;
	}
}

static const mcp_tool tool_mouse_move = {
	"mouse_move",
	"Moves the PS/2 mouse. The X16 mouse reports only relative movement and the KERNAL tracks the pointer, so the "
	"program must have enabled it (in BASIC: MOUSE 1; then MX, MY and MB read it back). Pass dx and/or dy (screen "
	"pixels in mouse units, +x right, +y down) for a relative move, or x and y (0-1023) to go to an absolute "
	"position. An absolute move first drives the pointer into the top-left corner and then out to x,y, which "
	"assumes the KERNAL mouse is enabled with its default scale (1:1) and clamps at the screen edge. Movement is "
	"delivered a packet at a time as the program consumes it, so a long move needs the emulator running; with it "
	"paused only about three packets (roughly 700 units) can be queued.",
	"{\"type\":\"object\",\"properties\":{"
		"\"dx\":{\"type\":\"integer\",\"minimum\":-4096,\"maximum\":4096},"
		"\"dy\":{\"type\":\"integer\",\"minimum\":-4096,\"maximum\":4096},"
		"\"x\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":1023},"
		"\"y\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":1023},"
		"\"timeout_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":600000}},\"additionalProperties\":false}",
	run_mouse_move,
	0,
	NULL
};

// scratch[0] = release deadline frame, scratch[1] = button | phase << 8 | hold_only << 16
// phases: 0 press, 1 hold, 2 release
static void
cancel_mouse_button(mcp_job *job)
{
	mouse_button_up((int)(job->scratch[1] & 0xFF)); // never leave a button stuck down
	if (mouse_has_room()) {
		mouse_send_state();
	}
}

static bool
run_mouse_button(mcp_job *job)
{
	if (job->polls == 0) {
		const char *b = json_as_string(json_get(job->args, "button"));
		int button = !b || !strcmp(b, "left") ? 0 : !strcmp(b, "right") ? 1 : !strcmp(b, "middle") ? 2 : -1;
		if (button < 0) {
			mcp_job_fail(job, "button must be \"left\", \"right\" or \"middle\"");
			return true;
		}
		const json_node *an = json_get(job->args, "action");
		const char *action = an ? json_as_string(an) : "tap";
		int act = !action ? -1 : !strcmp(action, "tap") ? 0 : !strcmp(action, "down") ? 1 : !strcmp(action, "up") ? 2 : -1;
		if (act < 0) {
			mcp_job_fail(job, "action must be \"tap\", \"down\" or \"up\"");
			return true;
		}
		uint32_t hold = mcp_is_paused() ? 0 : 2; // a program polling once per frame needs the button to span a frame
		if (json_get(job->args, "hold_frames") && (!arg_uint(job, "hold_frames", &hold) || hold > 100000)) {
			mcp_job_fail(job, "hold_frames must be an integer from 0 to 100000");
			return true;
		}
		if (act == 0 && hold > 0 && mcp_is_paused()) {
			mcp_job_fail(job, "hold_frames needs the emulator running (time does not pass while paused)");
			return true;
		}
		// the phase to start in, and whether to stop after pressing
		unsigned phase = act == 2 ? 2 : 0;
		job->scratch[1] = (uint64_t)button | (uint64_t)phase << 8 | (uint64_t)(act == 1) << 16;
		job->scratch[0] = hold;
		mouse_progress = frames;
	}
	int button = (int)(job->scratch[1] & 0xFF);
	unsigned phase = (unsigned)(job->scratch[1] >> 8) & 0xFF;
	bool press_only = (job->scratch[1] >> 16) & 1;
	for (;;) {
		if (phase == 3) { // everything is queued; wait for the program to read it
			if (!mouse_drained()) {
				break;
			}
			json_buf_append(&job->text, i2c_mse_buffer_count() ? "{\"queued\":true,\"consumed\":false}" : "{\"queued\":true,\"consumed\":true}");
			return true;
		}
		if (phase == 1) {
			if ((int32_t)(frames - (uint32_t)job->scratch[0]) < 0) {
				break;
			}
			phase = 2;
		}
		if (!mouse_has_room()) {
			if (mcp_is_paused()) {
				if (phase == 2) {
					mouse_button_up(button);
				}
				mcp_job_fail(job, "the mouse buffer is full and the emulator is paused; continue and repeat");
				return true;
			}
			if (mouse_stalled()) {
				if (phase == 2) {
					mouse_button_up(button);
				}
				mcp_job_fail(job, mouse_stall_msg);
				return true;
			}
			break;
		}
		if (phase == 0) {
			mouse_button_down(button);
			mouse_send_state();
			mouse_progress = frames;
			if (press_only) {
				phase = 3;
				continue;
			}
			uint32_t hold = (uint32_t)job->scratch[0];
			job->scratch[0] = (uint32_t)(frames + hold);
			phase = hold ? 1 : 2;
		} else {
			mouse_button_up(button);
			mouse_send_state();
			mouse_progress = frames;
			phase = 3;
		}
	}
	job->scratch[1] = (uint64_t)button | (uint64_t)phase << 8 | (uint64_t)press_only << 16;
	return false;
}

static const mcp_tool tool_mouse_button = {
	"mouse_button",
	"Presses and/or releases a mouse button (left, right, middle). action \"tap\" (default) presses then releases "
	"after hold_frames video frames (default 2 while running so a program that polls once per frame sees it; 0 while "
	"paused, where the press and release are just queued); \"down\" and \"up\" hold and release a button, for "
	"dragging. Position the pointer first with mouse_move. The program must have enabled the mouse.",
	"{\"type\":\"object\",\"properties\":{"
		"\"button\":{\"type\":\"string\",\"enum\":[\"left\",\"right\",\"middle\"],\"default\":\"left\"},"
		"\"action\":{\"type\":\"string\",\"enum\":[\"tap\",\"down\",\"up\"],\"default\":\"tap\"},"
		"\"hold_frames\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100000},"
		"\"timeout_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":600000}},\"additionalProperties\":false}",
	run_mouse_button,
	0,
	cancel_mouse_button
};

static bool
run_mouse_wheel(mcp_job *job)
{
	if (job->polls == 0) {
		int32_t n;
		if (!arg_int(job, "notches", -1000, 1000, &n) || n == 0) {
			mcp_job_fail(job, "notches is required: a non-zero integer from -1000 to 1000 (positive = up, away from the user)");
			return true;
		}
		uint8_t id = mouse_get_device_id();
		if (id != 3 && id != 4) {
			mcp_job_fail(job, "the mouse has no wheel: the program or KERNAL has not enabled wheel mode (PS/2 IntelliMouse)");
			return true;
		}
		job->scratch[0] = pk(n, 0);
		mouse_progress = frames;
	}
	int32_t n = pk_lo(job->scratch[0]);
	while (n != 0) {
		if (!mouse_has_room()) {
			if (mcp_is_paused()) {
				mcp_job_fail(job, "the mouse buffer is full and the emulator is paused; continue and repeat");
				return true;
			}
			if (mouse_stalled()) {
				mcp_job_fail(job, mouse_stall_msg);
				return true;
			}
			job->scratch[0] = pk(n, 0);
			return false;
		}
		mouse_progress = frames;
		int32_t c = n > 7 ? 7 : n < -7 ? -7 : n; // one packet carries -8..7
		mouse_set_wheel((int8_t)c);
		mouse_send_state();
		n -= c;
	}
	if (!mouse_drained()) {
		job->scratch[0] = 0;
		return false;
	}
	json_buf_append(&job->text, i2c_mse_buffer_count() ? "{\"scrolled\":true,\"consumed\":false}" : "{\"scrolled\":true,\"consumed\":true}");
	return true;
}

static const mcp_tool tool_mouse_wheel = {
	"mouse_wheel",
	"Scrolls the mouse wheel by `notches` (positive = up, away from the user). Only works if the program or KERNAL "
	"has enabled wheel mode; otherwise it returns an error. Long scrolls are delivered as the program consumes them.",
	"{\"type\":\"object\",\"properties\":{\"notches\":{\"type\":\"integer\",\"minimum\":-1000,\"maximum\":1000},"
	"\"timeout_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":600000}},\"required\":[\"notches\"],\"additionalProperties\":false}",
	run_mouse_wheel,
	0,
	NULL
};

// ---- debug_stall (only registered when X16EMU_MCP_TEST is set) ----
// Takes `ms` milliseconds to complete while the emulator keeps running; used to
// test that polled jobs do not block the emulator and that timeouts work.

static bool
run_debug_stall(mcp_job *job)
{
	const json_node *ms = json_get(job->args, "ms");
	if (!ms || ms->type != JSON_NUMBER || ms->number < 0 || ms->number > 600000) {
		mcp_job_fail(job, "ms must be a number between 0 and 600000");
		return true;
	}
	if (job->polls == 0) {
		job->scratch[0] = SDL_GetTicks64();
	}
	uint64_t elapsed = SDL_GetTicks64() - job->scratch[0];
	if (elapsed < (uint64_t)ms->number) {
		return false;
	}
	char tmp[96];
	snprintf(tmp, sizeof(tmp), "stalled %.0f ms, polled %u times", ms->number, job->polls);
	json_buf_append(&job->text, tmp);
	return true;
}

static const mcp_tool tool_debug_stall = {
	"debug_stall",
	"Test only: completes after `ms` milliseconds without pausing the emulator.",
	"{\"type\":\"object\",\"properties\":{\"ms\":{\"type\":\"number\"}},\"required\":[\"ms\"],\"additionalProperties\":false}",
	run_debug_stall,
	0
};

const mcp_tool *const *
mcp_tool_table(void)
{
	static const mcp_tool *table[32];
	if (!table[0]) {
		size_t n = 0;
		table[n++] = &tool_get_registers;
		table[n++] = &tool_get_status;
		table[n++] = &tool_read_memory;
		table[n++] = &tool_get_screen;
		table[n++] = &tool_pause;
		table[n++] = &tool_continue;
		table[n++] = &tool_run_frames;
		table[n++] = &tool_set_breakpoint;
		table[n++] = &tool_clear_breakpoint;
		table[n++] = &tool_list_breakpoints;
		table[n++] = &tool_step;
		table[n++] = &tool_set_watchpoint;
		table[n++] = &tool_clear_watchpoint;
		table[n++] = &tool_list_watchpoints;
		table[n++] = &tool_write_memory;
		table[n++] = &tool_set_registers;
		table[n++] = &tool_send_key;
		table[n++] = &tool_type_text;
		table[n++] = &tool_load_prg;
		table[n++] = &tool_mouse_move;
		table[n++] = &tool_mouse_button;
		table[n++] = &tool_mouse_wheel;
		if (getenv("X16EMU_MCP_TEST")) {
			table[n++] = &tool_debug_stall;
		}
		table[n] = NULL;
	}
	return table;
}

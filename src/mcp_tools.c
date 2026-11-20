// Commander X16 Emulator
// MCP tool implementations. Every run() executes on the emulator thread.
// All rights reserved. License: 2-clause BSD

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "glue.h"
#include "memory.h"
#include "video.h"
#include "keyboard.h"
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
		if (pasting_bas || paste_text) {
			mcp_job_fail(job, "a previous text is still being typed");
			return true;
		}
		if (wait && mcp_is_paused()) {
			mcp_job_fail(job, "wait=true needs the emulator running (typing only progresses while it runs)");
			return true;
		}
		char *copy = SDL_malloc(tn->string_len + 1);
		if (!copy) {
			mcp_job_fail(job, "out of memory");
			return true;
		}
		for (size_t i = 0; i <= tn->string_len; i++) {
			copy[i] = text[i] == '\n' ? '\r' : text[i]; // the X16 Enter key is CR
		}
		machine_paste(copy, true); // typed into the KERNAL keyboard buffer as the program runs
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
	static const mcp_tool *table[24];
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
		if (getenv("X16EMU_MCP_TEST")) {
			table[n++] = &tool_debug_stall;
		}
		table[n] = NULL;
	}
	return table;
}

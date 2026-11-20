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
	char tmp[256];
	// How far the emulation is behind wall-clock time, as the speed limiter sees it.
	// Near 0 or negative: keeping up (and being throttled). Growing: the host cannot
	// run the emulator at full speed.
	long long lag_ms = (long long)(SDL_GetTicks() - sdlTicks_base) - cpu_ticks / MHZ / 1000;
	snprintf(tmp, sizeof(tmp),
		"{\"frames\":%u,\"cpu_cycles_at_last_frame\":%lld,\"mhz\":%u,\"warp\":%s,\"lag_ms\":%lld,\"paused\":%s}",
		frames, (long long)cpu_ticks, (unsigned)MHZ, warp_mode ? "true" : "false", lag_ms,
		mcp_is_paused() ? "true" : "false");
	json_buf_append(&job->text, tmp);
	return true;
}

static const mcp_tool tool_get_status = {
	"get_status",
	"Returns emulator progress: the number of video frames completed since start (about 60 per emulated second), "
	"the CPU cycle count at the last completed frame, the emulated clock in MHz, warp mode, lag_ms (how far emulation is behind wall-clock time; it grows if the host cannot keep up) and whether paused.",
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
	mcp_set_paused(true);
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

static bool
run_continue(mcp_job *job)
{
	bool was = mcp_is_paused();
	mcp_set_paused(false);
	json_buf_append(&job->text, was ? "running" : "was not paused; running");
	return true;
}

static const mcp_tool tool_continue = {
	"continue",
	"Resumes a paused emulator at normal speed.",
	"{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
	run_continue,
	0
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
	static const mcp_tool *table[8];
	if (!table[0]) {
		size_t n = 0;
		table[n++] = &tool_get_registers;
		table[n++] = &tool_get_status;
		table[n++] = &tool_read_memory;
		table[n++] = &tool_get_screen;
		table[n++] = &tool_pause;
		table[n++] = &tool_continue;
		if (getenv("X16EMU_MCP_TEST")) {
			table[n++] = &tool_debug_stall;
		}
		table[n] = NULL;
	}
	return table;
}

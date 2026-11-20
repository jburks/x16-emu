// Commander X16 Emulator
// MCP tool implementations. Every run() executes on the emulator thread.
// All rights reserved. License: 2-clause BSD

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "glue.h"
#include "memory.h"
#include "mcp_internal.h"

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

// ---- get_registers ----

static bool
run_get_registers(mcp_job *job)
{
	char tmp[256];
	snprintf(tmp, sizeof(tmp),
		"{\"pc\":\"0x%04X\",\"a\":\"0x%02X\",\"x\":\"0x%02X\",\"y\":\"0x%02X\","
		"\"sp\":\"0x%04X\",\"status\":\"0x%02X\",\"ram_bank\":%u,\"rom_bank\":%u}",
		regs.pc, regs.a, regs.xl, regs.yl, regs.sp, regs.status,
		memory_get_ram_bank(), memory_get_rom_bank());
	json_buf_append(&job->text, tmp);
	return true;
}

static const mcp_tool tool_get_registers = {
	"get_registers",
	"Returns the 65C02 registers (pc, a, x, y, sp, status) and the current RAM and ROM bank.",
	"{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
	run_get_registers,
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
	static const mcp_tool *table[3];
	if (!table[0]) {
		size_t n = 0;
		table[n++] = &tool_get_registers;
		if (getenv("X16EMU_MCP_TEST")) {
			table[n++] = &tool_debug_stall;
		}
		table[n] = NULL;
	}
	return table;
}

// Commander X16 Emulator
// MCP server: breakpoints and stop state. Everything here runs on the emulator
// thread (called from mcp_service() and from tools), so there is no locking.
// All rights reserved. License: 2-clause BSD

#include <stdio.h>
#include <string.h>
#include "glue.h"
#include "memory.h"
#include "cpu/fake6502.h"
#include "mcp.h"
#include "mcp_internal.h"

mcp_stop_info mcp_last_stop = { STOP_NONE, 0 };
int mcp_bp_active = 0;

extern uint32_t frames; // timing.c

typedef struct {
	int id;
	uint16_t address;
	int bank;        // -1 = any bank
	unsigned hits;
} breakpoint_t;

static breakpoint_t bps[MCP_MAX_BREAKPOINTS];
static int next_id = 1;
// Set when resuming so the instruction at the current pc, which may carry the
// breakpoint we just stopped at, is allowed to execute once.
static bool skip_once;

int
mcp_bp_add(uint16_t address, int bank, bool *existed)
{
	for (int i = 0; i < mcp_bp_active; i++) {
		if (bps[i].address == address && bps[i].bank == bank) {
			*existed = true;
			return bps[i].id;
		}
	}
	*existed = false;
	if (mcp_bp_active >= MCP_MAX_BREAKPOINTS) {
		return -1;
	}
	breakpoint_t *b = &bps[mcp_bp_active++];
	b->id = next_id++;
	b->address = address;
	b->bank = bank;
	b->hits = 0;
	return b->id;
}

bool
mcp_bp_remove(int id)
{
	for (int i = 0; i < mcp_bp_active; i++) {
		if (bps[i].id == id) {
			memmove(&bps[i], &bps[i + 1], (size_t)(mcp_bp_active - i - 1) * sizeof(bps[0]));
			mcp_bp_active--;
			return true;
		}
	}
	return false;
}

void
mcp_bp_clear(void)
{
	mcp_bp_active = 0;
}

bool
mcp_bp_get(int index, int *id, uint16_t *address, int *bank, unsigned *hits)
{
	if (index < 0 || index >= mcp_bp_active) {
		return false;
	}
	*id = bps[index].id;
	*address = bps[index].address;
	*bank = bps[index].bank;
	*hits = bps[index].hits;
	return true;
}

// A breakpoint above $A000 can be qualified with a bank: the RAM bank for
// $A000-$BFFF, the ROM bank for $C000-$FFFF.
static bool
matches(const breakpoint_t *b, uint16_t pc)
{
	if (b->address != pc) {
		return false;
	}
	if (b->bank < 0 || pc < 0xA000) {
		return true;
	}
	return b->bank == (pc < 0xC000 ? memory_get_ram_bank() : memory_get_rom_bank());
}

void
mcp_stop(mcp_stop_reason reason)
{
	mcp_last_stop.reason = reason;
	mcp_set_paused(true);
}

void
mcp_resume(void)
{
	mcp_last_stop.reason = STOP_NONE;
	skip_once = true;
	mcp_set_paused(false);
}

// Called at the top of an emulator pass, i.e. before the instruction at regs.pc executes.
void
mcp_debug_check(void)
{
	if (waiting) {
		return; // WAI/STP: the instruction at pc has already executed
	}
	if (skip_once) {
		skip_once = false;
		return;
	}
	uint16_t pc = regs.pc;
	for (int i = 0; i < mcp_bp_active; i++) {
		if (matches(&bps[i], pc)) {
			bps[i].hits++;
			mcp_last_stop.breakpoint_id = bps[i].id;
			mcp_stop(STOP_BREAKPOINT);
			return;
		}
	}
}

const char *
mcp_stop_name(void)
{
	if (!mcp_is_paused()) {
		return "running";
	}
	switch (mcp_last_stop.reason) {
		case STOP_BREAKPOINT: return "breakpoint";
		case STOP_FRAMES:     return "frames";
		default:              return "paused";
	}
}

void
mcp_describe_stop(json_buf *out)
{
	const char *why = mcp_stop_name();
	char tmp[256];
	snprintf(tmp, sizeof(tmp), "{\"stopped\":\"%s\",\"pc\":\"0x%04X\",\"frames\":%u,\"ram_bank\":%u,\"rom_bank\":%u",
	         why, regs.pc, frames, memory_get_ram_bank(), memory_get_rom_bank());
	json_buf_append(out, tmp);
	if (mcp_is_paused() && mcp_last_stop.reason == STOP_BREAKPOINT) {
		snprintf(tmp, sizeof(tmp), ",\"breakpoint\":%d", mcp_last_stop.breakpoint_id);
		json_buf_append(out, tmp);
	}
	json_buf_append(out, "}");
}

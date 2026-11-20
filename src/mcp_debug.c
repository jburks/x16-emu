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
#include "mcp_symbols.h"

mcp_stop_info mcp_last_stop = { STOP_NONE, 0 };
int mcp_bp_active = 0;
bool mcp_debug_armed = false;
int mcp_watch_active = 0;
int mcp_vwatch_active = 0;
bool mcp_watch_suppress = false;

extern uint32_t frames;             // timing.c
extern int64_t cpu_ticks;           // timing.c: cycles up to the last completed frame
extern uint32_t clockticks6502_old; // timing.c: the 32-bit cycle counter at that frame

uint64_t
mcp_total_cycles(void)
{
	// clockticks6502 wraps at 2^32, so only the difference since the last frame is meaningful
	return (uint64_t)cpu_ticks + (uint32_t)(clockticks6502 - clockticks6502_old);
}

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
static uint16_t skip_pc; // pc at the last stop: only an unchanged pc is stepped over

static bool stepping;
static uint32_t step_left;

typedef struct {
	int id;
	int space;
	uint32_t address;
	uint32_t length;
	int bank;
	int access;
	bool exclude_fetch;
	unsigned hits;
} watch_t;
static watch_t watches[MCP_MAX_WATCHPOINTS];
static int watch_total;
static int next_watch_id = 1;
static bool watch_pending;
static struct { int id; int space; uint32_t address; bool write; uint8_t value; uint16_t pc; int port; } watch_hit;

static void
recount_watches(void)
{
	mcp_watch_active = mcp_vwatch_active = 0;
	for (int i = 0; i < watch_total; i++) {
		if (watches[i].space == WATCH_SPACE_VRAM) mcp_vwatch_active++;
		else mcp_watch_active++;
	}
}

static void
rearm(void)
{
	mcp_debug_armed = mcp_bp_active > 0 || stepping || watch_pending;
}

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
	mcp_debug_armed = true;
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
			rearm();
			return true;
		}
	}
	return false;
}

void
mcp_bp_clear(void)
{
	mcp_bp_active = 0;
	rearm();
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
bank_ok(int bank, uint16_t address)
{
	if (bank < 0 || address < 0xA000) {
		return true;
	}
	return bank == (address < 0xC000 ? memory_get_ram_bank() : memory_get_rom_bank());
}

static bool
matches(const breakpoint_t *b, uint16_t pc)
{
	return b->address == pc && bank_ok(b->bank, pc);
}

void
mcp_stop(mcp_stop_reason reason)
{
	if (watch_pending) { // a watchpoint hit by the last instruction is the more useful thing to report
		reason = STOP_WATCHPOINT;
		watch_pending = false;
		mcp_last_stop.watch_id = watch_hit.id;
	}
	stepping = false;
	rearm();
	skip_pc = regs.pc;
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

void
mcp_step_begin(uint32_t n)
{
	stepping = true;
	step_left = n;
	rearm();
	mcp_resume();
}

int
mcp_watch_add(int space, uint32_t address, uint32_t length, int bank, int access, bool exclude_fetch, bool *existed)
{
	for (int i = 0; i < watch_total; i++) {
		if (watches[i].space == space && watches[i].address == address && watches[i].length == length
		    && watches[i].bank == bank && watches[i].access == access && watches[i].exclude_fetch == exclude_fetch) {
			*existed = true;
			return watches[i].id;
		}
	}
	*existed = false;
	if (watch_total >= MCP_MAX_WATCHPOINTS) {
		return -1;
	}
	watch_t *w = &watches[watch_total++];
	w->id = next_watch_id++;
	w->space = space;
	w->address = address;
	w->length = length;
	w->bank = bank;
	w->access = access;
	w->exclude_fetch = exclude_fetch;
	w->hits = 0;
	recount_watches();
	return w->id;
}

bool
mcp_watch_remove(int id)
{
	for (int i = 0; i < watch_total; i++) {
		if (watches[i].id == id) {
			memmove(&watches[i], &watches[i + 1], (size_t)(watch_total - i - 1) * sizeof(watches[0]));
			watch_total--;
			recount_watches();
			return true;
		}
	}
	return false;
}

void
mcp_watch_clear(void)
{
	watch_total = 0;
	recount_watches();
}

int
mcp_watch_total(void)
{
	return watch_total;
}

bool
mcp_watch_get(int index, int *id, int *space, uint32_t *address, uint32_t *length, int *bank, int *access,
              bool *exclude_fetch, unsigned *hits)
{
	if (index < 0 || index >= watch_total) {
		return false;
	}
	*id = watches[index].id;
	*space = watches[index].space;
	*address = watches[index].address;
	*length = watches[index].length;
	*bank = watches[index].bank;
	*access = watches[index].access;
	*exclude_fetch = watches[index].exclude_fetch;
	*hits = watches[index].hits;
	return true;
}

static void
record_hit(const watch_t *w, uint32_t address, bool write, uint8_t value, int port)
{
	if (!watch_pending) {
		watch_pending = true;
		watch_hit.id = w->id;
		watch_hit.space = w->space;
		watch_hit.address = address;
		watch_hit.write = write;
		watch_hit.value = value;
		watch_hit.pc = opcode_addr;
		watch_hit.port = port;
		mcp_debug_armed = true;
	}
}

// Called from inside an instruction. Records the first hit; the emulator stops
// at the top of the next pass, i.e. after the accessing instruction completes.
void
mcp_watch_hit(uint16_t address, bool write, uint8_t value)
{
	if (mcp_watch_suppress || mcp_is_paused()) {
		return;
	}
	int want = write ? WATCH_WRITE : WATCH_READ;
	// A read of one of the bytes of the instruction being executed (its opcode and operands, at most 4 bytes
	// on the 65C816) is an instruction fetch. Only a read of the instruction's own bytes as data is misjudged.
	bool fetch = !write && (uint16_t)(address - opcode_addr) < 4;
	for (int i = 0; i < watch_total; i++) {
		watch_t *w = &watches[i];
		if (w->space != WATCH_SPACE_CPU) {
			continue;
		}
		if ((w->access & want) && (uint32_t)address >= w->address && (uint32_t)address - w->address < w->length
		    && bank_ok(w->bank, address) && !(fetch && w->exclude_fetch)) {
			w->hits++;
			record_hit(w, address, write, value, -1);
			return;
		}
	}
}

void
mcp_vram_watch_hit(uint32_t address, uint8_t value, int port)
{
	if (mcp_watch_suppress || mcp_is_paused()) {
		return;
	}
	for (int i = 0; i < watch_total; i++) {
		watch_t *w = &watches[i];
		if (w->space == WATCH_SPACE_VRAM && address >= w->address && address - w->address < w->length) {
			w->hits++;
			record_hit(w, address, true, value, port);
			return;
		}
	}
}

// Called at the top of an emulator pass, i.e. before the instruction at regs.pc executes.
void
mcp_debug_check(void)
{
	if (watch_pending) {
		mcp_stop(STOP_WATCHPOINT);
		return;
	}
	if (waiting) {
		return; // WAI/STP: the instruction at pc has already executed
	}
	bool skip = skip_once && regs.pc == skip_pc;
	skip_once = false;
	if (stepping) {
		if (step_left == 0) {
			mcp_stop(STOP_STEP);
			return;
		}
		step_left--;
	}
	if (skip) {
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

void
mcp_append_symbol(json_buf *b, const char *key, uint16_t address)
{
	char sym[320];
	if (mcp_sym_describe(address, sym, sizeof(sym))) {
		json_buf_append(b, ",\"");
		json_buf_append(b, key);
		json_buf_append(b, "\":");
		json_buf_append_string(b, sym, strlen(sym));
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
		case STOP_STEP:       return "step";
		case STOP_WATCHPOINT: return "watchpoint";
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
	snprintf(tmp, sizeof(tmp), ",\"cycles\":%llu", (unsigned long long)mcp_total_cycles());
	json_buf_append(out, tmp);
	snprintf(tmp, sizeof(tmp), ",\"a\":\"0x%02X\",\"x\":\"0x%02X\",\"y\":\"0x%02X\",\"sp\":\"0x%04X\",\"status\":\"0x%02X\"",
	         regs.a & 0xFF, regs.xl, regs.yl, regs.sp, regs.status);
	json_buf_append(out, tmp);
	if (mcp_is_paused() && mcp_last_stop.reason == STOP_BREAKPOINT) {
		snprintf(tmp, sizeof(tmp), ",\"breakpoint\":%d", mcp_last_stop.breakpoint_id);
		json_buf_append(out, tmp);
	}
	if (mcp_is_paused() && mcp_last_stop.reason == STOP_WATCHPOINT) {
		if (watch_hit.space == WATCH_SPACE_VRAM) {
			snprintf(tmp, sizeof(tmp), ",\"watchpoint\":%d,\"space\":\"vram\",\"access\":\"write\",\"address\":\"0x%05X\","
			         "\"value\":\"0x%02X\",\"data_port\":%d,\"accessing_pc\":\"0x%04X\"", watch_hit.id,
			         watch_hit.address, watch_hit.value, watch_hit.port, watch_hit.pc);
		} else {
			snprintf(tmp, sizeof(tmp), ",\"watchpoint\":%d,\"space\":\"cpu\",\"access\":\"%s\",\"address\":\"0x%04X\","
			         "\"value\":\"0x%02X\",\"accessing_pc\":\"0x%04X\"", watch_hit.id, watch_hit.write ? "write" : "read",
			         watch_hit.address, watch_hit.value, watch_hit.pc);
		}
		json_buf_append(out, tmp);
		mcp_append_symbol(out, "accessing_pc_symbol", watch_hit.pc);
	}
	mcp_append_symbol(out, "symbol", regs.pc);
	json_buf_append(out, "}");
}

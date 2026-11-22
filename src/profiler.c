// Commander X16 Emulator
// Profiler: cycles and execution counts per instruction address. See profiler.h.
// All rights reserved. License: 2-clause BSD

#include <stdlib.h>
#include <string.h>
#include "glue.h"
#include "memory.h"
#include "cpu/fake6502.h"
#include "profiler.h"

typedef struct {
	uint64_t cycles;
	uint64_t count;
} slot_t;

#define LOW_SIZE 0xA000 // $0000-$9FFF
#define RAM_WINDOW 0x2000 // $A000-$BFFF, per RAM bank
#define ROM_WINDOW 0x4000 // $C000-$FFFF, per ROM bank

bool profiler_active = false;

// Low memory is allocated when profiling first starts; a bank's table when code first runs in that bank.
static slot_t *low;
static slot_t *ram_banks[256];
static slot_t *rom_banks[256];
static profiler_totals totals;

static slot_t *
slot_for(uint16_t pc, uint8_t ram_bank, uint8_t rom_bank)
{
	if (pc < 0xA000) {
		return &low[pc];
	}
	slot_t **table = pc < 0xC000 ? &ram_banks[ram_bank] : &rom_banks[rom_bank];
	if (!*table) {
		*table = calloc(pc < 0xC000 ? RAM_WINDOW : ROM_WINDOW, sizeof(slot_t));
		if (!*table) {
			return NULL;
		}
	}
	return &(*table)[pc < 0xC000 ? pc - 0xA000 : pc - 0xC000];
}

void
profiler_step(void)
{
	uint32_t before = clockticks6502;
	if (waiting) { // WAI: no instruction runs, the CPU waits a cycle
		step6502();
		totals.wait_cycles += (uint32_t)(clockticks6502 - before);
		return;
	}
	// The address and banks the instruction is fetched from, before it runs (it may switch banks itself)
	uint16_t pc = regs.pc;
	bool mapped = !(regs.is65c816 && regs.k != 0);
	uint8_t ram_bank = memory_get_ram_bank();
	uint8_t rom_bank = memory_get_rom_bank();
	step6502();
	uint32_t cycles = clockticks6502 - before;
	totals.instructions++;
	slot_t *s = mapped ? slot_for(pc, ram_bank, rom_bank) : NULL;
	if (!s) {
		totals.unmapped_cycles += cycles;
		return;
	}
	s->cycles += cycles;
	s->count++;
	totals.instruction_cycles += cycles;
}

bool
profiler_start(bool reset)
{
	if (reset) {
		profiler_reset();
	}
	if (!low) {
		low = calloc(LOW_SIZE, sizeof(slot_t));
		if (!low) {
			return false;
		}
	}
	profiler_active = true;
	return true;
}

void
profiler_stop(void)
{
	profiler_active = false;
}

void
profiler_reset(void)
{
	if (low) {
		memset(low, 0, LOW_SIZE * sizeof(slot_t));
	}
	for (int i = 0; i < 256; i++) {
		free(ram_banks[i]);
		ram_banks[i] = NULL;
		free(rom_banks[i]);
		rom_banks[i] = NULL;
	}
	memset(&totals, 0, sizeof(totals));
}

void
profiler_get_totals(profiler_totals *out)
{
	*out = totals;
}

static void
visit_table(const slot_t *t, uint32_t size, uint16_t base, int bank, profiler_visit_fn fn, void *ctx)
{
	if (!t) {
		return;
	}
	for (uint32_t i = 0; i < size; i++) {
		if (t[i].count) {
			fn(ctx, (uint16_t)(base + i), bank, t[i].cycles, t[i].count);
		}
	}
}

void
profiler_visit(profiler_visit_fn fn, void *ctx)
{
	visit_table(low, LOW_SIZE, 0x0000, -1, fn, ctx);
	for (int b = 0; b < 256; b++) {
		visit_table(ram_banks[b], RAM_WINDOW, 0xA000, b, fn, ctx);
	}
	for (int b = 0; b < 256; b++) {
		visit_table(rom_banks[b], ROM_WINDOW, 0xC000, b, fn, ctx);
	}
}

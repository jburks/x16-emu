// Commander X16 Emulator
// Profiler: counts, for every instruction address, how many times the instruction ran and how many CPU cycles
// it took. Exact (every instruction is counted, nothing is sampled) and in emulated cycles, so two runs of the
// same code give the same profile.
// All rights reserved. License: 2-clause BSD

// The profiler is optional: it is compiled in when PROFILER is defined (CMake option ENABLE_PROFILER).
// Without it, profiler_on() is the constant false and the other functions are empty inline stubs, so the hook
// in the emulator loop compiles to a plain step6502() and needs no #ifdef.

#ifndef PROFILER_H
#define PROFILER_H

#include <stdbool.h>
#include <stdint.h>

// Cycle totals since the last reset. instruction_cycles are the cycles of the counted instructions (the sum of
// all per-address counts), wait_cycles the cycles spent in WAI waiting for an interrupt, and unmapped_cycles
// those of instructions outside bank 0 in 65C816 native mode, which have no slot. Cycles that pass between
// instructions (interrupt entry, KERNAL file calls done by the host) are in none of them.
typedef struct {
	uint64_t instruction_cycles;
	uint64_t wait_cycles;
	uint64_t unmapped_cycles;
	uint64_t instructions;
} profiler_totals;

// One counted address. bank is -1 below $A000, the RAM bank for $A000-$BFFF and the ROM bank from $C000 up.
typedef void (*profiler_visit_fn)(void *ctx, uint16_t address, int bank, uint64_t cycles, uint64_t count);

#ifdef PROFILER

extern bool profiler_active;
static inline bool profiler_on(void) { return profiler_active; }

// Runs one step6502() and counts it. Call instead of step6502() while profiler_on().
void profiler_step(void);

// Starts counting; with reset, the counts start from zero. Returns false if the memory could not be allocated.
bool profiler_start(bool reset);
// Stops counting; the counts are kept.
void profiler_stop(void);
// Clears the counts (and frees the per-bank tables).
void profiler_reset(void);

void profiler_get_totals(profiler_totals *out);
// Calls fn for every address with a non-zero count: low memory first, then each RAM bank's $A000-$BFFF, then
// each ROM bank's $C000-$FFFF, in address order.
void profiler_visit(profiler_visit_fn fn, void *ctx);

#else // !PROFILER

static inline bool profiler_on(void) { return false; }
static inline void profiler_step(void) {}

#endif // PROFILER

#endif

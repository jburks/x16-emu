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

// Call tracking (profile_start with calls): totals.
typedef struct {
	uint64_t calls;            // JSR/JSL calls and tail calls seen
	uint64_t tail_calls;       // of them, jumps to the start of another routine (see profiler_tail_fn)
	uint64_t interrupts;       // interrupt frames (IRQ, NMI, BRK, COP)
	uint64_t interrupt_cycles; // cycles inside interrupts, entry included; left out of every routine's cycles
	uint64_t depth_overflows;  // calls not tracked because the frame stack was full
	unsigned max_depth;
	unsigned open_frames;      // frames on the stack now
	bool out_of_memory;        // some calls could not be recorded
} profiler_call_totals;

// One routine: an address that was called (JSR/JSL) or tail-called. cycles: from each call until its return,
// including everything it called, but not interrupts; a recursive routine counts its outermost call only. Calls
// still running count up to now. min and max: the shortest and longest call, of those that have returned.
typedef struct {
	uint16_t address;
	int bank;                  // -1 below $A000, else the RAM or ROM bank the routine ran in
	uint64_t calls, returned, cycles, min, max;
} profiler_routine;
typedef void (*profiler_routine_fn)(void *ctx, const profiler_routine *r);

typedef enum { PROFILER_CALLER_ROUTINE, PROFILER_CALLER_NONE, PROFILER_CALLER_INTERRUPT } profiler_caller_kind;
// One caller -> callee pair: how often the caller called it, and the callee's cycles for those calls.
typedef struct {
	profiler_caller_kind caller_kind; // NONE: called outside every tracked routine
	uint16_t caller_address;
	int caller_bank;
	uint16_t callee_address;
	int callee_bank;
	uint64_t calls, cycles;
} profiler_edge;
typedef void (*profiler_edge_fn)(void *ctx, const profiler_edge *e);

// Whether a jump (JMP or a taken branch) from -> to is a tail call: a call of the routine at `to` that will return
// to the caller of the routine that jumps. bank: the bank `to` runs in (-1 below $A000).
typedef bool (*profiler_tail_fn)(uint16_t from, uint16_t to, int bank);

// One counted address. bank is -1 below $A000, the RAM bank for $A000-$BFFF and the ROM bank from $C000 up.
typedef void (*profiler_visit_fn)(void *ctx, uint16_t address, int bank, uint64_t cycles, uint64_t count);

#ifdef PROFILER

extern bool profiler_active;
static inline bool profiler_on(void) { return profiler_active; }

// Runs one step6502() and counts it. Call instead of step6502() while profiler_on().
void profiler_step(void);

// Starts counting; with reset, the counts start from zero. Returns false if the memory could not be allocated.
// With calls, also tracks subroutine calls (JSR/JSL, returns matched by the stack pointer, interrupts as frames of
// their own, and tail calls as tail says; tail may be NULL). Calls made before the start are not known: returns
// from them are ignored.
bool profiler_start(bool reset, bool calls, profiler_tail_fn tail);
// Stops counting; the counts are kept. Calls still running are ended here.
void profiler_stop(void);
// Clears the counts (and frees the per-bank tables).
void profiler_reset(void);

void profiler_get_totals(profiler_totals *out);
// Calls fn for every address with a non-zero count: low memory first, then each RAM bank's $A000-$BFFF, then
// each ROM bank's $C000-$FFFF, in address order.
void profiler_visit(profiler_visit_fn fn, void *ctx);

// Whether calls have been tracked since the last reset.
bool profiler_has_calls(void);
void profiler_get_call_totals(profiler_call_totals *out);
void profiler_visit_routines(profiler_routine_fn fn, void *ctx);
void profiler_visit_edges(profiler_edge_fn fn, void *ctx);

#else // !PROFILER

static inline bool profiler_on(void) { return false; }
static inline void profiler_step(void) {}

#endif // PROFILER

#endif

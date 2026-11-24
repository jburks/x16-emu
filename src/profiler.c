// Commander X16 Emulator
// Profiler: cycles and execution counts per instruction address. See profiler.h.
// All rights reserved. License: 2-clause BSD

#include <stdlib.h>
#include <string.h>
#include "glue.h"
#include "memory.h"
#include "cpu/fake6502.h"
#include "profiler.h"

extern uint8_t opcode; // the opcode step6502() last ran (cpu/fake6502.c)

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

// ---- call tracking ----

#define MAX_DEPTH 4096
#define MAX_LEVEL 8      // interrupt nesting levels told apart; deeper ones count as the deepest
#define CALLER_NONE (-1) // a call made outside every frame
#define CALLER_INT (-2)  // a call made by an interrupt handler (outside every routine frame inside it)
#define FRAME_INT (-2)   // a frame for an interrupt

typedef struct {
	uint16_t address;
	int16_t bank;
	uint64_t calls, cycles, min, max;
	uint32_t active;              // frames of it on the stack (recursion)
	uint64_t outer_now, outer_excl;
	uint8_t outer_level;
} routine_t;

typedef struct {
	int32_t caller, callee;       // caller: a routine index, CALLER_NONE or CALLER_INT
	uint64_t calls, cycles;
	uint32_t active;
	uint64_t outer_now, outer_excl;
	uint8_t outer_level;
} edge_t;

typedef struct {
	int32_t routine;              // FRAME_INT for an interrupt
	int32_t edge;
	uint16_t sp;                  // the stack pointer after the call pushed its return address
	uint8_t level;                // interrupt frames below this one
	uint64_t start_now, start_excl;
} frame_t;

static bool calls_on;             // track calls while profiling
static bool calls_seen;           // call data exists (since the last reset)
static profiler_tail_fn tail_fn;
static routine_t *routines;
static size_t routine_count, routine_cap;
static int32_t *routine_hash;     // open addressing: index + 1, 0 = empty
static size_t routine_hash_size;
static edge_t *edges;
static size_t edge_count, edge_cap;
static int32_t *edge_hash;
static size_t edge_hash_size;
static frame_t frames[MAX_DEPTH];
static int depth;
static int int_depth;             // interrupt frames on the stack
static uint64_t now;              // cycles since the call tracking began (instructions, waits and interrupt entries)
static uint64_t excl[MAX_LEVEL + 2]; // excl[d]: cycles spent with at least d interrupt frames on the stack
static uint32_t last_clock;
static uint16_t expect_pc, expect_sp;
static bool expect_valid;
static profiler_call_totals ctotals;

static uint64_t
mix(uint64_t k)
{
	k ^= k >> 33;
	k *= 0xff51afd7ed558ccdULL;
	k ^= k >> 33;
	return k;
}

// Finds or adds an entry in an open-addressing table of indexes. key_of(i) gives entry i's key. Returns the index,
// or -1 (out of memory).
static int32_t
hash_find(int32_t **table, size_t *size, uint64_t key, uint64_t (*key_of)(size_t), size_t count, bool *found)
{
	if (*size == 0 || count * 2 >= *size) { // grow (and rehash)
		size_t ns = *size ? *size * 2 : 1024;
		int32_t *nt = calloc(ns, sizeof(*nt));
		if (!nt) return -1;
		for (size_t i = 0; i < count; i++) {
			size_t h = mix(key_of(i)) & (ns - 1);
			while (nt[h]) h = (h + 1) & (ns - 1);
			nt[h] = (int32_t)i + 1;
		}
		free(*table);
		*table = nt;
		*size = ns;
	}
	size_t h = mix(key) & (*size - 1);
	while ((*table)[h]) {
		if (key_of((size_t)(*table)[h] - 1) == key) {
			*found = true;
			return (*table)[h] - 1;
		}
		h = (h + 1) & (*size - 1);
	}
	*found = false;
	(*table)[h] = (int32_t)count + 1;
	return (int32_t)count;
}

static uint64_t routine_key_of(size_t i) { return routines[i].address | (uint64_t)(routines[i].bank + 1) << 16; }
static uint64_t edge_key_of(size_t i) { return (uint64_t)(uint32_t)(edges[i].caller + 2) << 32 | (uint32_t)edges[i].callee; }

static int32_t
routine_index(uint16_t address, int bank)
{
	if (routine_count == routine_cap) { // room for a new one first, so a failure leaves the table consistent
		size_t nc = routine_cap ? routine_cap * 2 : 256;
		routine_t *nr = realloc(routines, nc * sizeof(*nr));
		if (!nr) return -1;
		routines = nr;
		routine_cap = nc;
	}
	bool found;
	int32_t i = hash_find(&routine_hash, &routine_hash_size, address | (uint64_t)(bank + 1) << 16, routine_key_of,
	                      routine_count, &found);
	if (i < 0 || found) return i;
	routine_t *r = &routines[routine_count++];
	memset(r, 0, sizeof(*r));
	r->address = address;
	r->bank = (int16_t)bank;
	r->min = UINT64_MAX;
	return i;
}

static int32_t
edge_index(int32_t caller, int32_t callee)
{
	if (edge_count == edge_cap) {
		size_t nc = edge_cap ? edge_cap * 2 : 512;
		edge_t *ne = realloc(edges, nc * sizeof(*ne));
		if (!ne) return -1;
		edges = ne;
		edge_cap = nc;
	}
	bool found;
	int32_t i = hash_find(&edge_hash, &edge_hash_size, (uint64_t)(uint32_t)(caller + 2) << 32 | (uint32_t)callee,
	                      edge_key_of, edge_count, &found);
	if (i < 0 || found) return i;
	edge_t *e = &edges[edge_count++];
	memset(e, 0, sizeof(*e));
	e->caller = caller;
	e->callee = callee;
	return i;
}

static uint8_t
level_now(void)
{
	return (uint8_t)(int_depth < MAX_LEVEL ? int_depth : MAX_LEVEL);
}

// Cycles since (start_now, start_excl), less those spent in interrupts nested deeper than level.
static uint64_t
elapsed(uint64_t start_now, uint64_t start_excl, uint8_t level)
{
	return (now - start_now) - (excl[level + 1] - start_excl);
}

static void
add_cycles(uint64_t c)
{
	now += c;
	for (int d = 1; d <= int_depth && d <= MAX_LEVEL + 1; d++) excl[d] += c;
}

static void
push_frame(int32_t routine, int32_t edge, uint16_t sp)
{
	if (depth == MAX_DEPTH) {
		ctotals.depth_overflows++;
		return;
	}
	uint8_t level = level_now();
	frame_t *f = &frames[depth++];
	f->routine = routine;
	f->edge = edge;
	f->sp = sp;
	f->level = level;
	f->start_now = now;
	f->start_excl = excl[level + 1];
	if ((unsigned)depth > ctotals.max_depth) ctotals.max_depth = (unsigned)depth;
	if (routine == FRAME_INT) {
		int_depth++;
		ctotals.interrupts++;
		return;
	}
	routine_t *r = &routines[routine];
	r->calls++;
	if (r->active++ == 0) {
		r->outer_now = now;
		r->outer_excl = f->start_excl;
		r->outer_level = level;
	}
	if (edge >= 0) {
		edge_t *e = &edges[edge];
		e->calls++;
		if (e->active++ == 0) {
			e->outer_now = now;
			e->outer_excl = f->start_excl;
			e->outer_level = level;
		}
	}
	ctotals.calls++;
}

static void
pop_frame(void)
{
	frame_t *f = &frames[--depth];
	if (f->routine == FRAME_INT) {
		int_depth--;
		return;
	}
	routine_t *r = &routines[f->routine];
	uint64_t t = elapsed(f->start_now, f->start_excl, f->level);
	if (t < r->min) r->min = t;
	if (t > r->max) r->max = t;
	if (--r->active == 0) r->cycles += elapsed(r->outer_now, r->outer_excl, r->outer_level);
	if (f->edge >= 0) {
		edge_t *e = &edges[f->edge];
		if (--e->active == 0) e->cycles += elapsed(e->outer_now, e->outer_excl, e->outer_level);
	}
}

// A call (or tail call) of the routine at the current pc.
static void
enter_routine(uint16_t sp, bool tail)
{
	uint16_t pc = regs.pc;
	int bank = pc < 0xA000 ? -1 : pc < 0xC000 ? memory_get_ram_bank() : memory_get_rom_bank();
	int32_t caller = CALLER_NONE;
	if (tail && depth > 0 && frames[depth - 1].routine != FRAME_INT) { // the callee replaces the jumping routine's frame
		caller = frames[depth - 1].routine;
		sp = frames[depth - 1].sp;
		pop_frame();
		ctotals.tail_calls++;
	} else {
		if (tail) ctotals.tail_calls++;
		for (int i = depth - 1; i >= 0; i--) { // the innermost routine, unless an interrupt came in after it
			caller = frames[i].routine == FRAME_INT ? CALLER_INT : frames[i].routine;
			break;
		}
	}
	int32_t r = routine_index(pc, bank);
	int32_t e = r >= 0 ? edge_index(caller, r) : -1;
	if (r < 0 || e < 0) {
		ctotals.out_of_memory = true;
		return;
	}
	push_frame(r, e, sp);
}

static bool
is_branch(uint8_t op)
{
	if ((op & 0x1F) == 0x10) return true; // BPL BMI BVC BVS BCC BCS BNE BEQ
	if (op == 0x80) return true;          // BRA
	if (regs.is65c816) return op == 0x82; // BRL
	return (op & 0x0F) == 0x0F;           // BBR/BBS (65C02)
}

static bool
is_jump(uint8_t op)
{
	if (op == 0x4C || op == 0x6C || op == 0x7C) return true; // JMP abs, (abs), (abs,X)
	return regs.is65c816 && (op == 0x5C || op == 0xDC);      // JML long, [abs]
}

// Called around each step while calls are tracked: pc and sp before the step, the opcode it ran.
static void
track_calls(uint16_t pc, uint16_t sp, uint8_t op, uint64_t cycles)
{
	bool c816 = regs.is65c816;
	if (op == 0x00 || (c816 && op == 0x02)) { // BRK, COP: a software interrupt
		push_frame(FRAME_INT, -1, regs.sp);
		add_cycles(cycles);
		return;
	}
	add_cycles(cycles);
	if ((op == 0x20 || op == 0xFC || (c816 && op == 0x22)) && regs.sp < sp) { // JSR, JSR (abs,X), JSL
		enter_routine(regs.sp, false);
	} else if (op == 0x60 || op == 0x40 || op == 0x9A || (c816 && (op == 0x6B || op == 0x1B))) {
		// RTS, RTI, TXS, RTL, TCS: frames whose return address is now off the stack are done
		while (depth > 0 && regs.sp > frames[depth - 1].sp) pop_frame();
	} else if (tail_fn && regs.pc != pc && (is_jump(op) || is_branch(op))) {
		int bank = regs.pc < 0xA000 ? -1 : regs.pc < 0xC000 ? memory_get_ram_bank() : memory_get_rom_bank();
		if (tail_fn(pc, regs.pc, bank)) enter_routine(regs.sp, true);
	}
}

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
	if (calls_on) {
		// Cycles that passed since the last step: an interrupt was taken if the pc moved and its return address and
		// status were pushed (4 bytes in 65C816 native mode)
		if (expect_valid && regs.pc != expect_pc && (uint16_t)(expect_sp - regs.sp) == (regs.e ? 3 : 4)) {
			push_frame(FRAME_INT, -1, regs.sp);
		}
		if (expect_valid) add_cycles((uint32_t)(before - last_clock));
	}
	if (waiting) { // WAI: no instruction runs, the CPU waits a cycle
		step6502();
		uint32_t w = (uint32_t)(clockticks6502 - before);
		totals.wait_cycles += w;
		if (calls_on) {
			add_cycles(w);
			last_clock = clockticks6502;
			expect_pc = regs.pc;
			expect_sp = regs.sp;
			expect_valid = true;
		}
		return;
	}
	// The address and banks the instruction is fetched from, before it runs (it may switch banks itself)
	uint16_t pc = regs.pc;
	uint16_t sp = regs.sp;
	bool mapped = !(regs.is65c816 && regs.k != 0);
	uint8_t ram_bank = memory_get_ram_bank();
	uint8_t rom_bank = memory_get_rom_bank();
	step6502();
	uint32_t cycles = clockticks6502 - before;
	totals.instructions++;
	if (calls_on) {
		track_calls(pc, sp, opcode, cycles);
		last_clock = clockticks6502;
		expect_pc = regs.pc;
		expect_sp = regs.sp;
		expect_valid = true;
	}
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
profiler_start(bool reset, bool calls, profiler_tail_fn tail)
{
	if (reset) {
		profiler_reset();
	}
	if (!calls) {
		while (depth > 0) pop_frame();
	}
	calls_on = calls;
	tail_fn = tail;
	if (calls) {
		calls_seen = true;
		expect_valid = false; // the stack as it is now is unknown: returns past it are ignored
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
	while (depth > 0) pop_frame(); // routines still running count up to here
	calls_on = false;
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
	free(routines); free(routine_hash); free(edges); free(edge_hash);
	routines = NULL; routine_hash = NULL; edges = NULL; edge_hash = NULL;
	routine_count = routine_cap = routine_hash_size = edge_count = edge_cap = edge_hash_size = 0;
	depth = int_depth = 0;
	now = 0;
	memset(excl, 0, sizeof(excl));
	memset(&ctotals, 0, sizeof(ctotals));
	calls_seen = calls_on && profiler_active;
	expect_valid = false;
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

bool profiler_has_calls(void) { return calls_seen; }

void
profiler_get_call_totals(profiler_call_totals *out)
{
	*out = ctotals;
	out->interrupt_cycles = excl[1];
	out->open_frames = (unsigned)depth;
}

// A routine's cycles including the frames still open (counted up to now).
static uint64_t
routine_cycles(const routine_t *r)
{
	return r->cycles + (r->active ? elapsed(r->outer_now, r->outer_excl, r->outer_level) : 0);
}

void
profiler_visit_routines(profiler_routine_fn fn, void *ctx)
{
	for (size_t i = 0; i < routine_count; i++) {
		const routine_t *r = &routines[i];
		profiler_routine pr;
		pr.address = r->address;
		pr.bank = r->bank;
		pr.calls = r->calls;
		pr.returned = r->calls - r->active;
		pr.cycles = routine_cycles(r);
		pr.min = r->min == UINT64_MAX ? 0 : r->min;
		pr.max = r->max;
		fn(ctx, &pr);
	}
}

void
profiler_visit_edges(profiler_edge_fn fn, void *ctx)
{
	for (size_t i = 0; i < edge_count; i++) {
		const edge_t *e = &edges[i];
		profiler_edge pe;
		pe.caller_kind = e->caller == CALLER_NONE ? PROFILER_CALLER_NONE : e->caller == CALLER_INT ? PROFILER_CALLER_INTERRUPT
		                 : PROFILER_CALLER_ROUTINE;
		pe.caller_address = e->caller >= 0 ? routines[e->caller].address : 0;
		pe.caller_bank = e->caller >= 0 ? routines[e->caller].bank : -1;
		pe.callee_address = routines[e->callee].address;
		pe.callee_bank = routines[e->callee].bank;
		pe.calls = e->calls;
		pe.cycles = e->cycles + (e->active ? elapsed(e->outer_now, e->outer_excl, e->outer_level) : 0);
		fn(ctx, &pe);
	}
}

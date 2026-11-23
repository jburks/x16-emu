// Commander X16 Emulator
// Symbols for the MCP server: labels loaded from a ca65/ld65 label file ("ld65 -Ln"), so that addresses
// can be given by name and stops can be reported by name.
// All rights reserved. License: 2-clause BSD

#ifndef MCP_SYMBOLS_H
#define MCP_SYMBOLS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
	size_t added;      // labels read from the file
	size_t skipped;    // lines (label file) or symbol records (debug info) that were not labels
	size_t merged;     // labels dropped because the same label (address, bank, name) was already there
	size_t scopes;     // address ranges of named scopes (debug info only)
	bool debug_info;   // the file was cc65 debug info, not a label file
} mcp_sym_load_info;

// Loads labels from either kind of file, told apart by content:
// - a VICE-format label file (ld65 -Ln): lines "al <hex address> .<name>" (an optional "C:" before the address is
//   accepted). The address is read as bank:address when it has more than four hex digits (ld65 for the X16 puts
//   the bank in the upper byte). Other lines are skipped.
// - cc65 debug info (ld65 --dbgfile, version 2.x): every label, and every constant that is an address (has a
//   segment) or is exported (the set ld65 -Ln writes), named with its scope path ("tslope::sign"); plus the address ranges of named scopes.
// With replace, the existing symbols are dropped first. A label already loaded (same address, bank and name) is
// kept once, preferring the scoped name, so a program's label file and debug info can both be loaded. Returns false
// with a message in err if the file cannot be read or has no labels (the loaded symbols are then unchanged).
bool mcp_sym_load(const char *path, bool replace, mcp_sym_load_info *info, char *err, size_t err_size);

size_t mcp_sym_count(void);

// Resolves "name", "name+N" or "name-N" (N decimal, 0x or $ hex). name is a full name with its scope path
// ("tslope::sign") or, if no full name matches, a label without one ("sign"); "name@$XXXX" picks the label of
// that name at that address. A name that several labels have is an error listing them. *bank is the symbol's
// bank, or -1 if it has none (addresses below $A000, and labels without a bank). On failure err says why.
bool mcp_sym_find(const char *expr, uint16_t *address, int *bank, char *err, size_t err_size);

// Describes an address as "name" or "name+0xN" (name: the scope path and label, with @$XXXX added when other
// labels have the same name) using the nearest label at or below it, if there is one
// within 4096 bytes, in the same memory region, and (from $A000 up) in the current RAM or ROM bank. Linker
// symbols such as __X_SIZE__ are not offered. Returns false if there is no such label.
bool mcp_sym_describe(uint16_t address, char *out, size_t out_size);

typedef struct {
	const char *name;  // as shown (see mcp_sym_describe); valid until the symbols are next loaded
	uint16_t address;
	int bank;          // -1: none
} mcp_sym_entry;

// The label mcp_sym_describe would use, but for an explicit bank instead of the current one: from $A000 up,
// only labels for `bank` (or without a bank) are considered. Returns false if there is none.
bool mcp_sym_nearest(uint16_t address, int bank, mcp_sym_entry *out);

// Lists symbols for the list_symbols tool. A symbol matches if its name starts with prefix (if not NULL) and
// contains `contains` (if not NULL); both are case-sensitive. Linker symbols (names that start and end with
// "__") are left out unless include_linker. Matches are ordered by name, or by address (then name) with
// by_address. Skips `offset` matches and stores up to `limit` in out. Returns the number stored; the number of
// matches in total, not just the stored ones, goes to *matching.
size_t mcp_sym_list(const char *prefix, const char *contains, bool include_linker, bool by_address,
                    size_t offset, size_t limit, mcp_sym_entry *out, size_t *matching);

// Number of address ranges of named scopes loaded from debug info.
size_t mcp_sym_scope_count(void);

// The named scope (.proc or .scope, from debug info) whose address range holds address: the innermost one, or
// with outermost the outermost. *path is its scope path (valid until the symbols are next loaded) and *start the
// start of its range. Ranges above $FFFF (with a bank) only match that bank. False if no scope holds it.
bool mcp_sym_scope_at(uint16_t address, int bank, bool outermost, const char **path, uint16_t *start);

// Whether path is the full path of a loaded scope.
bool mcp_sym_is_scope(const char *path);
// Whether address lies in the scope path, or in a scope nested in it.
bool mcp_sym_in_scope(uint16_t address, int bank, const char *path);

// The source position of the code at address, from debug info. *file and *line: the line as written where the
// code is: for code from a macro (or .repeat), the line that uses the macro (for .repeat, ca65 records the
// .endrepeat line); for C compiled by cc65, the C source line. macro_file and macro_line: for code from a macro or
// .repeat, the line inside its body that produced the code (the innermost body when macros are nested), else
// NULL and 0. File names are as the debug info gives them (as passed to the assembler) and stay valid until the
// symbols are next loaded with replace. Ranges above $FFFF (with a bank) only match that bank. False if no
// loaded source line covers address.
typedef struct {
	const char *file;
	unsigned line;
	const char *macro_file;
	unsigned macro_line;
} mcp_sym_source;
bool mcp_sym_source_at(uint16_t address, int bank, mcp_sym_source *out);

// Number of address ranges of source lines loaded from debug info.
size_t mcp_sym_line_count(void);

// The bank the CPU sees at address now: -1 below $A000, else the current RAM or ROM bank.
int mcp_sym_current_bank(uint16_t address);

#endif

// Commander X16 Emulator
// Symbols for the MCP server: labels loaded from a ca65/ld65 label file ("ld65 -Ln"), so that addresses
// can be given by name and stops can be reported by name.
// All rights reserved. License: 2-clause BSD

#ifndef MCP_SYMBOLS_H
#define MCP_SYMBOLS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Loads a VICE-format label file: lines "al <hex address> .<name>" (an optional "C:" before the address is
// accepted). The address is read as bank:address when it has more than four hex digits (ld65 for the X16
// puts the bank in the upper byte). Other lines are skipped. With replace, the existing symbols are dropped
// first. Returns false with a message in err if the file cannot be read or has no label lines.
bool mcp_sym_load(const char *path, bool replace, size_t *added, size_t *skipped_lines, char *err, size_t err_size);

size_t mcp_sym_count(void);

// Resolves "name", "name+N" or "name-N" (N decimal, 0x or $ hex). *bank is the symbol's bank, or -1 if it
// has none (addresses below $A000, and labels without a bank). On failure err says why.
bool mcp_sym_find(const char *expr, uint16_t *address, int *bank, char *err, size_t err_size);

// Describes an address as "name" or "name+0xN" using the nearest label at or below it, if there is one
// within 4096 bytes, in the same memory region, and (from $A000 up) in the current RAM or ROM bank. Linker
// symbols such as __X_SIZE__ are not offered. Returns false if there is no such label.
bool mcp_sym_describe(uint16_t address, char *out, size_t out_size);

typedef struct {
	const char *name;  // valid until the symbols are next loaded
	uint16_t address;
	int bank;          // -1: none
} mcp_sym_entry;

// Lists symbols for the list_symbols tool. A symbol matches if its name starts with prefix (if not NULL) and
// contains `contains` (if not NULL); both are case-sensitive. Linker symbols (names that start and end with
// "__") are left out unless include_linker. Matches are ordered by name, or by address (then name) with
// by_address. Skips `offset` matches and stores up to `limit` in out. Returns the number stored; the number of
// matches in total, not just the stored ones, goes to *matching.
size_t mcp_sym_list(const char *prefix, const char *contains, bool include_linker, bool by_address,
                    size_t offset, size_t limit, mcp_sym_entry *out, size_t *matching);

#endif

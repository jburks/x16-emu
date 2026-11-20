// Commander X16 Emulator
// Symbols for the MCP server. See mcp_symbols.h.
// All rights reserved. License: 2-clause BSD

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "memory.h"
#include "mcp_symbols.h"

typedef struct {
	char *name;
	uint16_t address;
	int bank; // -1: none
} symbol_t;

#define MAX_SYMBOLS 200000
#define MAX_FILE (32u * 1024 * 1024)
#define MAX_DESCRIBE_OFFSET 0x1000

static symbol_t *symbols;
static size_t count, capacity;
static symbol_t **by_name; // sorted by name
static symbol_t **by_addr; // sorted by address, then bank

static int
cmp_name(const void *a, const void *b)
{
	return strcmp((*(symbol_t *const *)a)->name, (*(symbol_t *const *)b)->name);
}

static int
cmp_addr(const void *a, const void *b)
{
	const symbol_t *x = *(symbol_t *const *)a, *y = *(symbol_t *const *)b;
	if (x->address != y->address) return x->address < y->address ? -1 : 1;
	return x->bank - y->bank;
}

static void
free_all(void)
{
	for (size_t i = 0; i < count; i++) free(symbols[i].name);
	free(symbols); free(by_name); free(by_addr);
	symbols = NULL; by_name = by_addr = NULL;
	count = capacity = 0;
}

static void
rebuild_indexes(void)
{
	free(by_name); free(by_addr);
	by_name = malloc((count ? count : 1) * sizeof(*by_name));
	by_addr = malloc((count ? count : 1) * sizeof(*by_addr));
	if (!by_name || !by_addr) {
		free_all();
		return;
	}
	for (size_t i = 0; i < count; i++) by_name[i] = by_addr[i] = &symbols[i];
	qsort(by_name, count, sizeof(*by_name), cmp_name);
	qsort(by_addr, count, sizeof(*by_addr), cmp_addr);
}

static bool
add_symbol(const char *name, size_t name_len, uint16_t address, int bank)
{
	if (count >= MAX_SYMBOLS) return false;
	if (count == capacity) {
		size_t nc = capacity ? capacity * 2 : 1024;
		symbol_t *ns = realloc(symbols, nc * sizeof(*ns));
		if (!ns) return false;
		symbols = ns;
		capacity = nc;
	}
	char *n = malloc(name_len + 1);
	if (!n) return false;
	memcpy(n, name, name_len);
	n[name_len] = 0;
	symbols[count].name = n;
	symbols[count].address = address;
	symbols[count].bank = bank;
	count++;
	return true;
}

// Parses one line. Returns 1 if it is a label line (and stores it), 0 if it is something else.
static int
parse_line(const char *line, size_t len)
{
	const char *p = line, *end = line + len;
	if (len < 6 || p[0] != 'a' || p[1] != 'l' || (p[2] != ' ' && p[2] != '\t')) return 0;
	p += 3;
	while (p < end && (*p == ' ' || *p == '\t')) p++;
	if (p + 1 < end && p[1] == ':') p += 2; // VICE memory space prefix, "C:"
	uint32_t v = 0;
	int digits = 0;
	while (p < end && ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))) {
		v = v << 4 | (uint32_t)(*p <= '9' ? *p - '0' : (*p | 0x20) - 'a' + 10);
		digits++;
		p++;
	}
	if (digits == 0 || digits > 8) return 0;
	while (p < end && (*p == ' ' || *p == '\t')) p++;
	if (p >= end || *p != '.') return 0;
	p++;
	const char *name = p;
	while (p < end && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
	size_t nlen = (size_t)(p - name);
	if (nlen == 0 || nlen > 255) return 0;
	uint16_t address = (uint16_t)(v & 0xFFFF);
	int bank = (address >= 0xA000 && (v >> 16) != 0) ? (int)((v >> 16) & 0xFF) : -1;
	return add_symbol(name, nlen, address, bank) ? 1 : 0;
}

bool
mcp_sym_load(const char *path, bool replace, size_t *added, size_t *skipped_lines, char *err, size_t err_size)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		snprintf(err, err_size, "cannot open %.400s", path);
		return false;
	}
	char *text = malloc(MAX_FILE + 1);
	size_t n = text ? fread(text, 1, MAX_FILE + 1, f) : 0;
	bool bad = !text || ferror(f) || n > MAX_FILE;
	fclose(f);
	if (bad) {
		free(text);
		snprintf(err, err_size, "cannot read %.400s (a directory, or larger than 32 MB)", path);
		return false;
	}
	text[n] = 0;
	// With replace the new labels are collected on the side, so a file without labels leaves the old ones alone.
	symbol_t *bk_syms = symbols;
	symbol_t **bk_name = by_name, **bk_addr = by_addr;
	size_t bk_count = count, bk_cap = capacity;
	if (replace) {
		symbols = NULL; by_name = by_addr = NULL;
		count = capacity = 0;
	}
	size_t ok = 0, skipped = 0;
	for (size_t i = 0; i < n;) {
		size_t j = i;
		while (j < n && text[j] != '\n') j++;
		if (j > i) {
			if (parse_line(text + i, j - i)) ok++;
			else skipped++;
		}
		i = j + 1;
	}
	free(text);
	if (ok == 0) {
		if (replace) {
			free_all();
			symbols = bk_syms; by_name = bk_name; by_addr = bk_addr; count = bk_count; capacity = bk_cap;
		}
		snprintf(err, err_size, "no label lines found in %.400s: expected a ca65/ld65 label file (ld65 -Ln), "
		         "lines like \"al 000801 .main\"", path);
		return false;
	}
	if (replace) {
		for (size_t i = 0; i < bk_count; i++) free(bk_syms[i].name);
		free(bk_syms); free(bk_name); free(bk_addr);
	}
	rebuild_indexes();
	*added = ok;
	*skipped_lines = skipped;
	return true;
}

size_t mcp_sym_count(void) { return count; }

static bool
parse_offset(const char *s, long *out)
{
	if (!*s) return false;
	int base = 10;
	if (s[0] == '$') { s++; base = 16; }
	else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; base = 16; }
	char *end;
	long v = strtol(s, &end, base);
	if (end == s || *end || v < 0 || v > 0xFFFF) return false;
	*out = v;
	return true;
}

static symbol_t *
find_exact(const char *name)
{
	if (!count) return NULL;
	symbol_t key = { (char *)name, 0, 0 };
	symbol_t *kp = &key;
	symbol_t **hit = bsearch(&kp, by_name, count, sizeof(*by_name), cmp_name);
	if (!hit) return NULL;
	// first of equal names (duplicates keep file order only approximately; any is fine)
	size_t i = (size_t)(hit - by_name);
	while (i > 0 && strcmp(by_name[i - 1]->name, name) == 0) i--;
	return by_name[i];
}

bool
mcp_sym_find(const char *expr, uint16_t *address, int *bank, char *err, size_t err_size)
{
	if (!count) {
		snprintf(err, err_size, "unknown symbol '%.100s' (no symbols are loaded; load_symbols reads a ca65 label file)", expr);
		return false;
	}
	symbol_t *s = find_exact(expr);
	long off = 0;
	int sign = 1;
	if (!s) {
		const char *op = strpbrk(expr, "+-");
		if (op && op > expr) {
			char name[300];
			size_t nl = (size_t)(op - expr);
			if (nl < sizeof(name)) {
				memcpy(name, expr, nl);
				name[nl] = 0;
				s = find_exact(name);
				sign = *op == '-' ? -1 : 1;
				if (s && !parse_offset(op + 1, &off)) {
					snprintf(err, err_size, "bad offset in '%.100s' (use name+N or name-N, N decimal or 0x/$ hex)", expr);
					return false;
				}
			}
		}
	}
	if (!s) {
		snprintf(err, err_size, "unknown symbol '%.100s' (%zu symbols are loaded)", expr, count);
		return false;
	}
	long a = (long)s->address + sign * off;
	if (a < 0 || a > 0xFFFF) {
		snprintf(err, err_size, "'%.100s' is outside the address space", expr);
		return false;
	}
	*address = (uint16_t)a;
	*bank = s->bank;
	return true;
}

static bool
is_linker_symbol(const char *n)
{
	size_t l = strlen(n);
	return l > 4 && n[0] == '_' && n[1] == '_' && n[l - 1] == '_' && n[l - 2] == '_';
}

bool
mcp_sym_describe(uint16_t address, char *out, size_t out_size)
{
	if (!count) return false;
	// the last entry with address <= target
	size_t lo = 0, hi = count;
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		if (by_addr[mid]->address <= address) lo = mid + 1; else hi = mid;
	}
	int cur_bank = address < 0xA000 ? -1 : address < 0xC000 ? memory_get_ram_bank() : memory_get_rom_bank();
	for (size_t i = lo, tries = 0; i > 0 && tries < 4096; i--, tries++) {
		const symbol_t *s = by_addr[i - 1];
		if ((unsigned)address - s->address > MAX_DESCRIBE_OFFSET) break;
		if ((s->address < 0xA000) != (address < 0xA000)) continue;
		if (address >= 0xA000 && s->bank >= 0 && s->bank != cur_bank) continue;
		if (is_linker_symbol(s->name)) continue;
		unsigned off = (unsigned)address - s->address;
		if (off) snprintf(out, out_size, "%s+0x%X", s->name, off);
		else snprintf(out, out_size, "%s", s->name);
		return true;
	}
	return false;
}

size_t
mcp_sym_list(const char *prefix, const char *contains, bool include_linker, bool by_address,
             size_t offset, size_t limit, mcp_sym_entry *out, size_t *matching)
{
	size_t total = 0, stored = 0;
	size_t plen = prefix ? strlen(prefix) : 0;
	symbol_t **order = by_address ? by_addr : by_name;
	for (size_t i = 0; i < count; i++) {
		const symbol_t *sy = order[i];
		if (prefix && strncmp(sy->name, prefix, plen) != 0) continue;
		if (contains && !strstr(sy->name, contains)) continue;
		if (!include_linker && is_linker_symbol(sy->name)) continue;
		if (total >= offset && stored < limit) {
			out[stored].name = sy->name;
			out[stored].address = sy->address;
			out[stored].bank = sy->bank;
			stored++;
		}
		total++;
	}
	*matching = total;
	return stored;
}

// Commander X16 Emulator
// Symbols for the MCP server. See mcp_symbols.h.
// All rights reserved. License: 2-clause BSD

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "memory.h"
#include "mcp_symbols.h"

typedef struct {
	char *name;             // full name: scope path and label ("tslope::sign"), or just the label
	const char *short_name; // the label without its scope path (points into name)
	char *display;          // how the label is shown: name, or name@$XXXX when other labels have the same name
	uint16_t address;
	int bank;               // -1: none
} symbol_t;

// An address range that belongs to a named scope (a .proc or .scope), from debug info.
typedef struct {
	char *path;
	uint32_t start; // with the bank in bits 16-23, as in the label files
	uint32_t size;
} scope_range_t;

#define MAX_SYMBOLS 200000
#define MAX_FILE (32u * 1024 * 1024)
#define MAX_DESCRIBE_OFFSET 0x1000
#define MAX_NAME 1000

static symbol_t *symbols;
static size_t count;
static symbol_t **by_name;  // sorted by full name
static symbol_t **by_short; // sorted by short name
static symbol_t **by_addr;  // sorted by address, then bank
static scope_range_t *scopes;
static size_t scope_count;

// ---- growable arrays used while a file is read ----

typedef struct {
	symbol_t *v;
	size_t n, cap;
} symvec;

typedef struct {
	scope_range_t *v;
	size_t n, cap;
} scopevec;

static const char *
short_of(const char *name)
{
	const char *s = name;
	for (const char *p = name; (p = strstr(p, "::")) != NULL; p += 2) s = p + 2;
	return s;
}

// Takes ownership of name.
static bool
symvec_push(symvec *s, char *name, uint16_t address, int bank)
{
	if (s->n >= MAX_SYMBOLS) {
		free(name);
		return false;
	}
	if (s->n == s->cap) {
		size_t nc = s->cap ? s->cap * 2 : 1024;
		symbol_t *nv = realloc(s->v, nc * sizeof(*nv));
		if (!nv) {
			free(name);
			return false;
		}
		s->v = nv;
		s->cap = nc;
	}
	symbol_t *sy = &s->v[s->n++];
	sy->name = name;
	sy->short_name = short_of(name);
	sy->display = NULL;
	sy->address = address;
	sy->bank = bank;
	return true;
}

// Takes ownership of path.
static bool
scopevec_push(scopevec *s, char *path, uint32_t start, uint32_t size)
{
	if (s->n == s->cap) {
		size_t nc = s->cap ? s->cap * 2 : 256;
		scope_range_t *nv = realloc(s->v, nc * sizeof(*nv));
		if (!nv) {
			free(path);
			return false;
		}
		s->v = nv;
		s->cap = nc;
	}
	s->v[s->n].path = path;
	s->v[s->n].start = start;
	s->v[s->n].size = size;
	s->n++;
	return true;
}

static char *
dup_n(const char *s, size_t n)
{
	char *d = malloc(n + 1);
	if (d) {
		memcpy(d, s, n);
		d[n] = 0;
	}
	return d;
}

static void
free_display(symbol_t *s)
{
	if (s->display && s->display != s->name) free(s->display);
	s->display = NULL;
}

static void
free_all(void)
{
	for (size_t i = 0; i < count; i++) {
		free_display(&symbols[i]);
		free(symbols[i].name);
	}
	for (size_t i = 0; i < scope_count; i++) free(scopes[i].path);
	free(symbols); free(by_name); free(by_short); free(by_addr); free(scopes);
	symbols = NULL; by_name = by_short = by_addr = NULL; scopes = NULL;
	count = scope_count = 0;
}

// ---- indexes ----

static int
cmp_name(const void *a, const void *b)
{
	return strcmp((*(symbol_t *const *)a)->name, (*(symbol_t *const *)b)->name);
}

static int
cmp_short(const void *a, const void *b)
{
	return strcmp((*(symbol_t *const *)a)->short_name, (*(symbol_t *const *)b)->short_name);
}

static int
cmp_addr(const void *a, const void *b)
{
	const symbol_t *x = *(symbol_t *const *)a, *y = *(symbol_t *const *)b;
	if (x->address != y->address) return x->address < y->address ? -1 : 1;
	return x->bank - y->bank;
}

// The same label twice: same address, bank and short name. The one with the longer (scoped) name sorts first.
static int
cmp_same_label(const void *a, const void *b)
{
	const symbol_t *x = *(symbol_t *const *)a, *y = *(symbol_t *const *)b;
	int c = cmp_addr(a, b);
	if (c) return c;
	c = strcmp(x->short_name, y->short_name);
	if (c) return c;
	size_t lx = strlen(x->name), ly = strlen(y->name);
	if (lx != ly) return lx > ly ? -1 : 1;
	return strcmp(x->name, y->name);
}

static int
cmp_scope(const void *a, const void *b)
{
	const scope_range_t *x = a, *y = b;
	if (x->start != y->start) return x->start < y->start ? -1 : 1;
	return x->size > y->size ? -1 : x->size < y->size ? 1 : 0;
}

static bool
same_label(const symbol_t *x, const symbol_t *y)
{
	return x->address == y->address && x->bank == y->bank && !strcmp(x->short_name, y->short_name);
}

// Drops repeated labels (keeping the scoped one), then builds the sorted indexes and the display names.
// Returns how many repeated labels were dropped.
static size_t
rebuild_indexes(void)
{
	free(by_name); free(by_short); free(by_addr);
	by_name = by_short = by_addr = NULL;
	size_t merged = 0;
	symbol_t **order = malloc((count ? count : 1) * sizeof(*order));
	if (!order) {
		free_all();
		return 0;
	}
	for (size_t i = 0; i < count; i++) order[i] = &symbols[i];
	qsort(order, count, sizeof(*order), cmp_same_label);
	bool *drop = calloc(count ? count : 1, sizeof(*drop));
	if (!drop) {
		free(order);
		free_all();
		return 0;
	}
	for (size_t i = 1; i < count; i++) {
		if (same_label(order[i - 1], order[i])) { // the first of each group (the most scoped name) stays
			drop[order[i] - symbols] = true;
		}
	}
	free(order);
	size_t n = 0;
	for (size_t i = 0; i < count; i++) {
		if (drop[i]) {
			free_display(&symbols[i]);
			free(symbols[i].name);
			merged++;
		} else {
			symbols[n++] = symbols[i];
		}
	}
	free(drop);
	count = n;

	by_name = malloc((count ? count : 1) * sizeof(*by_name));
	by_short = malloc((count ? count : 1) * sizeof(*by_short));
	by_addr = malloc((count ? count : 1) * sizeof(*by_addr));
	if (!by_name || !by_short || !by_addr) {
		free_all();
		return 0;
	}
	for (size_t i = 0; i < count; i++) by_name[i] = by_short[i] = by_addr[i] = &symbols[i];
	qsort(by_name, count, sizeof(*by_name), cmp_name);
	qsort(by_short, count, sizeof(*by_short), cmp_short);
	qsort(by_addr, count, sizeof(*by_addr), cmp_addr);
	if (scope_count) qsort(scopes, scope_count, sizeof(*scopes), cmp_scope);

	// A name that several labels share is shown with the address: next@$1A24
	for (size_t i = 0; i < count;) {
		size_t j = i + 1;
		while (j < count && !strcmp(by_name[j]->name, by_name[i]->name)) j++;
		for (size_t k = i; k < j; k++) {
			symbol_t *s = by_name[k];
			free_display(s);
			if (j - i > 1) {
				size_t len = strlen(s->name) + 8;
				s->display = malloc(len);
				if (s->display) snprintf(s->display, len, "%s@$%04X", s->name, s->address);
			}
			if (!s->display) s->display = s->name;
		}
		i = j;
	}
	return merged;
}

// Adds the read labels and scopes (taking them over), after dropping the old ones with replace.
static size_t
commit(symvec *sv, scopevec *cv, bool replace)
{
	if (replace) free_all();
	if (sv->n) {
		symbol_t *ns = realloc(symbols, (count + sv->n) * sizeof(*ns));
		if (!ns) return 0;
		symbols = ns;
		memcpy(symbols + count, sv->v, sv->n * sizeof(*ns));
		count += sv->n;
	}
	if (cv->n) {
		scope_range_t *nc = realloc(scopes, (scope_count + cv->n) * sizeof(*nc));
		if (nc) {
			scopes = nc;
			memcpy(scopes + scope_count, cv->v, cv->n * sizeof(*nc));
			scope_count += cv->n;
		} else {
			for (size_t i = 0; i < cv->n; i++) free(cv->v[i].path);
		}
	}
	free(sv->v);
	free(cv->v);
	return rebuild_indexes();
}

static void
discard(symvec *sv, scopevec *cv)
{
	for (size_t i = 0; i < sv->n; i++) free(sv->v[i].name);
	for (size_t i = 0; i < cv->n; i++) free(cv->v[i].path);
	free(sv->v);
	free(cv->v);
}

// The address and bank of a label value: more than 16 bits carry the bank (kept only from $A000 up).
static void
split_value(uint32_t v, uint16_t *address, int *bank)
{
	*address = (uint16_t)(v & 0xFFFF);
	*bank = (*address >= 0xA000 && (v >> 16) != 0) ? (int)((v >> 16) & 0xFF) : -1;
}

// ---- VICE label files (ld65 -Ln) ----

// Parses one line. Returns 1 if it is a label line (and stores it), 0 if it is something else.
static int
parse_vice_line(symvec *sv, const char *line, size_t len)
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
	uint16_t address;
	int bank;
	split_value(v, &address, &bank);
	char *n = dup_n(name, nlen);
	return n && symvec_push(sv, n, address, bank) ? 1 : 0;
}

static size_t
parse_vice(symvec *sv, const char *text, size_t n, size_t *skipped)
{
	size_t ok = 0;
	*skipped = 0;
	for (size_t i = 0; i < n;) {
		size_t j = i;
		while (j < n && text[j] != '\n') j++;
		if (j > i) {
			if (parse_vice_line(sv, text + i, j - i)) ok++;
			else ++*skipped;
		}
		i = j + 1;
	}
	return ok;
}

// ---- cc65 debug info (ld65 --dbgfile) ----

#define MAX_FIELDS 24

typedef struct {
	const char *key[MAX_FIELDS], *val[MAX_FIELDS];
	size_t key_len[MAX_FIELDS], val_len[MAX_FIELDS];
	int n;
} fields_t;

// Splits "key=value,key=\"value\",..." (one record, after the record type and tab).
static void
split_fields(const char *p, const char *end, fields_t *f)
{
	f->n = 0;
	while (p < end && f->n < MAX_FIELDS) {
		const char *k = p;
		while (p < end && *p != '=' && *p != ',') p++;
		if (p >= end || *p != '=') break;
		f->key[f->n] = k;
		f->key_len[f->n] = (size_t)(p - k);
		p++;
		const char *v;
		if (p < end && *p == '"') {
			v = ++p;
			while (p < end && *p != '"') p++;
			f->val[f->n] = v;
			f->val_len[f->n] = (size_t)(p - v);
			if (p < end) p++;
		} else {
			v = p;
			while (p < end && *p != ',') p++;
			f->val[f->n] = v;
			f->val_len[f->n] = (size_t)(p - v);
		}
		f->n++;
		if (p < end && *p == ',') p++;
	}
}

static bool
field(const fields_t *f, const char *key, const char **val, size_t *len)
{
	size_t kl = strlen(key);
	for (int i = 0; i < f->n; i++) {
		if (f->key_len[i] == kl && !memcmp(f->key[i], key, kl)) {
			*val = f->val[i];
			*len = f->val_len[i];
			return true;
		}
	}
	return false;
}

// A numeric field (decimal or 0x hex). Returns def if absent or invalid.
static long
field_num(const fields_t *f, const char *key, long def)
{
	const char *v;
	size_t l;
	char buf[32];
	if (!field(f, key, &v, &l) || l == 0 || l >= sizeof(buf)) return def;
	memcpy(buf, v, l);
	buf[l] = 0;
	char *e;
	long r = strtol(buf, &e, 0);
	return *e ? def : r;
}

typedef struct {
	char *name;  // NULL: no name (a module's top level)
	long parent; // -1: none
	char *spans; // "3+4+5", or NULL
	char *path;  // built on demand
	bool path_done;
} dbg_scope;

typedef struct {
	long seg, start, size;
} dbg_span;

// Grows an array of elements of `size` bytes so that index id is valid (new elements zeroed).
static bool
grow_to(void **arr, size_t *cap, long id, size_t size)
{
	if (id < 0 || id > 10000000) return false;
	if ((size_t)id < *cap) return true;
	size_t nc = *cap ? *cap : 64;
	while (nc <= (size_t)id) nc *= 2;
	void *na = realloc(*arr, nc * size);
	if (!na) return false;
	memset((char *)na + *cap * size, 0, (nc - *cap) * size);
	*arr = na;
	*cap = nc;
	return true;
}

static const char *
scope_path(dbg_scope *sc, size_t nsc, long id, int depth)
{
	if (id < 0 || (size_t)id >= nsc || depth > 64) return "";
	dbg_scope *s = &sc[id];
	if (!s->path_done) {
		s->path_done = true;
		const char *pp = scope_path(sc, nsc, s->parent, depth + 1);
		if (s->name && s->name[0]) {
			size_t len = strlen(pp) + strlen(s->name) + 3;
			s->path = malloc(len);
			if (s->path) snprintf(s->path, len, "%s%s%s", pp, pp[0] ? "::" : "", s->name);
		} else if (pp[0]) {
			s->path = dup_n(pp, strlen(pp));
		}
	}
	return s->path ? s->path : "";
}

typedef struct {
	char *name;
	long scope;
	uint32_t value;
	bool has_value;
	bool label;    // type=lab
	bool constant; // type=equ: used if it is an address (has a segment) or exported, as ld65 -Ln does
	bool in_seg;
	long exp;      // an import (type=imp): the id of the symbol it imports, else -1
	bool exported;
} dbg_sym;

static bool
parse_dbg(symvec *sv, scopevec *cv, const char *text, size_t n, size_t *added, size_t *skipped, char *err, size_t err_size)
{
	dbg_scope *sc = NULL;
	dbg_span *sp = NULL;
	long *seg = NULL;
	dbg_sym *sy = NULL;
	size_t nsc = 0, nsp = 0, nseg = 0, nsy = 0;
	bool ok = true;
	*added = *skipped = 0;
	for (size_t i = 0; i < n && ok;) {
		size_t j = i;
		while (j < n && text[j] != '\n') j++;
		const char *line = text + i, *end = text + j;
		if (end > line && end[-1] == '\r') end--;
		i = j + 1;
		const char *tab = memchr(line, '\t', (size_t)(end - line));
		if (!tab) continue;
		size_t tl = (size_t)(tab - line);
		fields_t f;
		split_fields(tab + 1, end, &f);
		long id = field_num(&f, "id", -1);
		const char *v;
		size_t vl;
		if (tl == 3 && !memcmp(line, "seg", 3)) {
			if (!(ok = grow_to((void **)&seg, &nseg, id, sizeof(*seg)))) break;
			seg[id] = field_num(&f, "start", 0);
		} else if (tl == 4 && !memcmp(line, "span", 4)) {
			if (!(ok = grow_to((void **)&sp, &nsp, id, sizeof(*sp)))) break;
			sp[id].seg = field_num(&f, "seg", -1);
			sp[id].start = field_num(&f, "start", 0);
			sp[id].size = field_num(&f, "size", 0);
		} else if (tl == 5 && !memcmp(line, "scope", 5)) {
			if (!(ok = grow_to((void **)&sc, &nsc, id, sizeof(*sc)))) break;
			sc[id].parent = field_num(&f, "parent", -1);
			if (field(&f, "name", &v, &vl) && vl) sc[id].name = dup_n(v, vl);
			if (field(&f, "span", &v, &vl) && vl) sc[id].spans = dup_n(v, vl);
		} else if (tl == 3 && !memcmp(line, "sym", 3)) {
			if (!(ok = grow_to((void **)&sy, &nsy, id, sizeof(*sy)))) break;
			const char *ty = "";
			size_t tyl = 0;
			field(&f, "type", &ty, &tyl);
			sy[id].label = tyl == 3 && !memcmp(ty, "lab", 3);
			sy[id].constant = tyl == 3 && !memcmp(ty, "equ", 3);
			sy[id].exp = tyl == 3 && !memcmp(ty, "imp", 3) ? field_num(&f, "exp", -1) : -1;
			sy[id].in_seg = field(&f, "seg", &v, &vl);
			sy[id].has_value = field(&f, "val", &v, &vl);
			sy[id].value = (uint32_t)field_num(&f, "val", 0);
			sy[id].scope = field_num(&f, "scope", -1);
			if (field(&f, "name", &v, &vl) && vl) sy[id].name = dup_n(v, vl);
		}
	}
	if (!ok) {
		snprintf(err, err_size, "the debug info has a record id that is out of range or too many records");
	}
	// Exported symbols are the ones an import refers to
	for (size_t i = 0; ok && i < nsy; i++) {
		if (sy[i].exp >= 0 && (size_t)sy[i].exp < nsy) sy[sy[i].exp].exported = true;
	}
	// Labels and exported constants, named by their scope path
	for (size_t i = 0; ok && i < nsy; i++) {
		bool use = sy[i].has_value && sy[i].name && (sy[i].label || (sy[i].constant && (sy[i].in_seg || sy[i].exported)));
		if (!use) {
			if (sy[i].name) ++*skipped;
			continue;
		}
		const char *pp = scope_path(sc, nsc, sy[i].scope, 0);
		size_t len = strlen(pp) + strlen(sy[i].name) + 3;
		if (len > MAX_NAME) continue;
		char *full = malloc(len);
		if (!full) {
			ok = false;
			break;
		}
		snprintf(full, len, "%s%s%s", pp, pp[0] ? "::" : "", sy[i].name);
		uint16_t address;
		int bank;
		split_value(sy[i].value, &address, &bank);
		if (!symvec_push(sv, full, address, bank)) {
			ok = false;
			break;
		}
		++*added;
	}
	// Address ranges of named scopes
	for (size_t i = 0; ok && i < nsc; i++) {
		if (!sc[i].name || !sc[i].spans) continue;
		const char *path = scope_path(sc, nsc, (long)i, 0);
		for (const char *p = sc[i].spans; *p;) {
			char *e;
			long s = strtol(p, &e, 10);
			if (e == p) break;
			p = *e == '+' ? e + 1 : e;
			if (s < 0 || (size_t)s >= nsp || sp[s].size <= 0 || sp[s].seg < 0 || (size_t)sp[s].seg >= nseg) continue;
			char *pcopy = dup_n(path, strlen(path));
			if (!pcopy || !scopevec_push(cv, pcopy, (uint32_t)(seg[sp[s].seg] + sp[s].start), (uint32_t)sp[s].size)) {
				ok = false;
				break;
			}
		}
	}
	for (size_t i = 0; i < nsc; i++) {
		free(sc[i].name);
		free(sc[i].spans);
		free(sc[i].path);
	}
	for (size_t i = 0; i < nsy; i++) free(sy[i].name);
	free(sc); free(sp); free(seg); free(sy);
	if (!ok && !err[0]) snprintf(err, err_size, "out of memory reading the debug info");
	return ok;
}

// ---- loading ----

bool
mcp_sym_load(const char *path, bool replace, mcp_sym_load_info *info, char *err, size_t err_size)
{
	memset(info, 0, sizeof(*info));
	err[0] = 0;
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
	symvec sv = { 0 };
	scopevec cv = { 0 };
	size_t ok;
	if (!strncmp(text, "version\tmajor=", 14)) {
		info->debug_info = true;
		if (strncmp(text + 14, "2,", 2)) {
			free(text);
			snprintf(err, err_size, "%.400s: unsupported cc65 debug info version (major 2 is read)", path);
			return false;
		}
		bool good = parse_dbg(&sv, &cv, text, n, &ok, &info->skipped, err, err_size);
		free(text);
		if (!good) {
			discard(&sv, &cv);
			return false;
		}
		if (ok == 0) {
			discard(&sv, &cv);
			snprintf(err, err_size, "no labels found in the debug info %.400s", path);
			return false;
		}
	} else {
		ok = parse_vice(&sv, text, n, &info->skipped);
		free(text);
		if (ok == 0) {
			discard(&sv, &cv);
			snprintf(err, err_size, "no label lines found in %.400s: expected a ca65/ld65 label file (ld65 -Ln), "
			         "lines like \"al 000801 .main\", or cc65 debug info (ld65 --dbgfile)", path);
			return false;
		}
	}
	info->added = ok;
	info->scopes = cv.n;
	info->merged = commit(&sv, &cv, replace);
	return true;
}

size_t mcp_sym_count(void) { return count; }

// ---- lookups ----

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

// The range of entries in a sorted index whose key (full or short name) equals name.
static size_t
equal_range(symbol_t **idx, bool short_key, const char *name, size_t *first)
{
	size_t lo = 0, hi = count;
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		const char *k = short_key ? idx[mid]->short_name : idx[mid]->name;
		if (strcmp(k, name) < 0) lo = mid + 1; else hi = mid;
	}
	size_t e = lo;
	while (e < count && !strcmp(short_key ? idx[e]->short_name : idx[e]->name, name)) e++;
	*first = lo;
	return e - lo;
}

// Finds the one label a name means: a full name ("tslope::sign") first, then a short name ("sign"), optionally
// narrowed to one address (have_at). Fails, saying why, if none or several match.
static const symbol_t *
resolve(const char *name, bool have_at, uint16_t at, const char *expr, char *err, size_t err_size)
{
	size_t first, n = equal_range(by_name, false, name, &first);
	symbol_t **idx = by_name;
	if (n == 0) {
		n = equal_range(by_short, true, name, &first);
		idx = by_short;
	}
	const symbol_t *found = NULL;
	size_t matches = 0;
	for (size_t i = first; i < first + n; i++) {
		if (have_at && idx[i]->address != at) continue;
		if (!found) found = idx[i];
		matches++;
	}
	if (matches == 1) return found;
	if (matches == 0) {
		if (have_at && n) snprintf(err, err_size, "no label '%.100s' at $%04X (in '%.100s')", name, at, expr);
		else snprintf(err, err_size, "unknown symbol '%.100s' (%zu symbols are loaded)", expr, count);
		return NULL;
	}
	int w = snprintf(err, err_size, "'%.100s' is ambiguous: %zu labels have that name:", name, matches);
	size_t shown = 0;
	for (size_t i = first; i < first + n && w > 0 && (size_t)w < err_size; i++) {
		if (have_at && idx[i]->address != at) continue;
		if (shown == 8) {
			w += snprintf(err + w, err_size - (size_t)w, ", ...");
			break;
		}
		if (idx[i]->bank >= 0) {
			w += snprintf(err + w, err_size - (size_t)w, "%s %.120s ($%04X bank %d)", shown ? "," : "", idx[i]->display,
			              idx[i]->address, idx[i]->bank);
		} else {
			w += snprintf(err + w, err_size - (size_t)w, "%s %.120s", shown ? "," : "", idx[i]->display);
		}
		shown++;
	}
	if (w > 0 && (size_t)w < err_size) {
		snprintf(err + w, err_size - (size_t)w, " (give the scope path, proc::label, or name@$XXXX)");
	}
	return NULL;
}

bool
mcp_sym_find(const char *expr, uint16_t *address, int *bank, char *err, size_t err_size)
{
	if (!count) {
		snprintf(err, err_size, "unknown symbol '%.100s' (no symbols are loaded; load_symbols reads a ca65 label file)", expr);
		return false;
	}
	char name[MAX_NAME + 1];
	size_t len = strlen(expr);
	// name, then an optional @$XXXX (which label), then an optional +N or -N
	const char *op = strpbrk(expr, "+-");
	size_t nl = op && op > expr ? (size_t)(op - expr) : len;
	if (nl == 0 || nl > MAX_NAME) {
		snprintf(err, err_size, "unknown symbol '%.100s' (%zu symbols are loaded)", expr, count);
		return false;
	}
	memcpy(name, expr, nl);
	name[nl] = 0;
	long off = 0;
	int sign = 1;
	if (nl < len) {
		sign = *op == '-' ? -1 : 1;
		if (!parse_offset(op + 1, &off)) {
			snprintf(err, err_size, "bad offset in '%.100s' (use name+N or name-N, N decimal or 0x/$ hex)", expr);
			return false;
		}
	}
	bool have_at = false;
	uint16_t at = 0;
	char *q = strchr(name, '@');
	while (q && !(q[1] == '$' || (q[1] == '0' && (q[2] == 'x' || q[2] == 'X')))) q = strchr(q + 1, '@'); // "@loop" is a name
	if (q && q > name) {
		long a;
		if (!parse_offset(q + 1, &a)) {
			snprintf(err, err_size, "bad address in '%.100s' (use name@$XXXX)", expr);
			return false;
		}
		*q = 0;
		have_at = true;
		at = (uint16_t)a;
	}
	const symbol_t *s = resolve(name, have_at, at, expr, err, err_size);
	if (!s) return false;
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

// The nearest label at or below address (see mcp_sym_describe), looking from $A000 up only at labels for `bank`
// (or without a bank). NULL if there is none.
static const symbol_t *
nearest(uint16_t address, int bank)
{
	if (!count) return NULL;
	// the last entry with address <= target
	size_t lo = 0, hi = count;
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		if (by_addr[mid]->address <= address) lo = mid + 1; else hi = mid;
	}
	for (size_t i = lo, tries = 0; i > 0 && tries < 4096; i--, tries++) {
		const symbol_t *s = by_addr[i - 1];
		if ((unsigned)address - s->address > MAX_DESCRIBE_OFFSET) break;
		if ((s->address < 0xA000) != (address < 0xA000)) continue;
		if (address >= 0xA000 && s->bank >= 0 && s->bank != bank) continue;
		if (is_linker_symbol(s->name)) continue;
		return s;
	}
	return NULL;
}

bool
mcp_sym_describe(uint16_t address, char *out, size_t out_size)
{
	int cur_bank = address < 0xA000 ? -1 : address < 0xC000 ? memory_get_ram_bank() : memory_get_rom_bank();
	const symbol_t *s = nearest(address, cur_bank);
	if (!s) return false;
	unsigned off = (unsigned)address - s->address;
	if (off) snprintf(out, out_size, "%s+0x%X", s->display, off);
	else snprintf(out, out_size, "%s", s->display);
	return true;
}

bool
mcp_sym_nearest(uint16_t address, int bank, mcp_sym_entry *out)
{
	const symbol_t *s = nearest(address, bank);
	if (!s) return false;
	out->name = s->display;
	out->address = s->address;
	out->bank = s->bank;
	return true;
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
			out[stored].name = sy->display;
			out[stored].address = sy->address;
			out[stored].bank = sy->bank;
			stored++;
		}
		total++;
	}
	*matching = total;
	return stored;
}

size_t mcp_sym_scope_count(void) { return scope_count; }

bool
mcp_sym_scope_at(uint16_t address, int bank, bool outermost, const char **path, uint16_t *start)
{
	const scope_range_t *best = NULL;
	// scopes are sorted by start; every range starting at or below address is a candidate
	for (size_t i = 0; i < scope_count && (scopes[i].start & 0xFFFF) <= address; i++) {
		const scope_range_t *r = &scopes[i];
		if (r->start > 0xFFFF && (address < 0xA000 || (int)(r->start >> 16) != bank)) continue;
		if ((uint32_t)address - (r->start & 0xFFFF) >= r->size) continue;
		if (!best || (outermost ? r->size > best->size : r->size < best->size)) best = r;
	}
	if (!best) return false;
	*path = best->path;
	*start = (uint16_t)(best->start & 0xFFFF);
	return true;
}

bool
mcp_sym_is_scope(const char *path)
{
	for (size_t i = 0; i < scope_count; i++) {
		if (!strcmp(scopes[i].path, path)) return true;
	}
	return false;
}

bool
mcp_sym_in_scope(uint16_t address, int bank, const char *path)
{
	for (size_t i = 0; i < scope_count && (scopes[i].start & 0xFFFF) <= address; i++) {
		const scope_range_t *r = &scopes[i];
		if (r->start > 0xFFFF && (address < 0xA000 || (int)(r->start >> 16) != bank)) continue;
		if ((uint32_t)address - (r->start & 0xFFFF) >= r->size) continue;
		if (!strcmp(r->path, path) || (!strncmp(r->path, path, strlen(path)) && !strncmp(r->path + strlen(path), "::", 2))) {
			return true; // that scope, or one nested in it
		}
	}
	return false;
}

// Commander X16 Emulator
// Minimal JSON parser/writer for the MCP server. No external dependencies.
// All rights reserved. License: 2-clause BSD

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "mcp_json.h"

#define MAX_DEPTH 64

typedef struct {
	const char *p;
	const char *err;
} parser;

static json_node *parse_value(parser *ps, int depth);

static json_node *
new_node(json_type type)
{
	json_node *n = calloc(1, sizeof(json_node));
	if (n) {
		n->type = type;
	}
	return n;
}

void
json_free(json_node *node)
{
	while (node) {
		json_node *next = node->next;
		json_free(node->first);
		free(node->string);
		free(node->key);
		free(node);
		node = next;
	}
}

static void
skip_ws(parser *ps)
{
	while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r') {
		ps->p++;
	}
}

static json_node *
fail(parser *ps, const char *msg)
{
	if (!ps->err) {
		ps->err = msg;
	}
	return NULL;
}

static int
hex4(const char *s)
{
	int v = 0;
	for (int i = 0; i < 4; i++) {
		char c = s[i];
		v <<= 4;
		if (c >= '0' && c <= '9') v |= c - '0';
		else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
		else return -1;
	}
	return v;
}

static size_t
put_utf8(char *out, unsigned cp)
{
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	} else if (cp < 0x800) {
		out[0] = (char)(0xC0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	} else if (cp < 0x10000) {
		out[0] = (char)(0xE0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	out[0] = (char)(0xF0 | (cp >> 18));
	out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
	out[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

// Parses a string starting at the opening quote. Returns a malloc'd NUL
// terminated buffer and stores the length (excluding the NUL) in *len.
static char *
parse_string_raw(parser *ps, size_t *len)
{
	const char *s = ps->p;
	if (*s != '"') {
		fail(ps, "expected string");
		return NULL;
	}
	s++;
	// the decoded string is never longer than the encoded one
	const char *e = s;
	while (*e && *e != '"') {
		if (*e == '\\' && e[1]) e++;
		e++;
	}
	if (*e != '"') {
		fail(ps, "unterminated string");
		return NULL;
	}
	char *out = malloc((size_t)(e - s) + 1);
	if (!out) {
		fail(ps, "out of memory");
		return NULL;
	}
	size_t n = 0;
	while (s < e) {
		unsigned char c = (unsigned char)*s;
		if (c < 0x20) {
			free(out);
			fail(ps, "control character in string");
			return NULL;
		}
		if (c != '\\') {
			out[n++] = (char)c;
			s++;
			continue;
		}
		s++;
		switch (*s) {
			case '"':  out[n++] = '"';  s++; break;
			case '\\': out[n++] = '\\'; s++; break;
			case '/':  out[n++] = '/';  s++; break;
			case 'b':  out[n++] = '\b'; s++; break;
			case 'f':  out[n++] = '\f'; s++; break;
			case 'n':  out[n++] = '\n'; s++; break;
			case 'r':  out[n++] = '\r'; s++; break;
			case 't':  out[n++] = '\t'; s++; break;
			case 'u': {
				if (e - s < 5) {
					free(out);
					fail(ps, "bad \\u escape");
					return NULL;
				}
				int cp = hex4(s + 1);
				if (cp < 0) {
					free(out);
					fail(ps, "bad \\u escape");
					return NULL;
				}
				s += 5;
				if (cp >= 0xD800 && cp <= 0xDBFF) {
					// high surrogate: must be followed by \uDC00-\uDFFF
					int lo = -1;
					if (e - s >= 6 && s[0] == '\\' && s[1] == 'u') {
						lo = hex4(s + 2);
					}
					if (lo < 0xDC00 || lo > 0xDFFF) {
						free(out);
						fail(ps, "unpaired surrogate");
						return NULL;
					}
					s += 6;
					cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
				} else if (cp >= 0xDC00 && cp <= 0xDFFF) {
					free(out);
					fail(ps, "unpaired surrogate");
					return NULL;
				}
				n += put_utf8(out + n, (unsigned)cp);
				break;
			}
			default:
				free(out);
				fail(ps, "bad escape");
				return NULL;
		}
	}
	out[n] = 0;
	*len = n;
	ps->p = e + 1;
	return out;
}

static json_node *
parse_number(parser *ps)
{
	const char *s = ps->p;
	const char *q = s;
	if (*q == '-') q++;
	if (*q == '0') {
		q++;
	} else if (*q >= '1' && *q <= '9') {
		while (*q >= '0' && *q <= '9') q++;
	} else {
		return fail(ps, "bad number");
	}
	if (*q == '.') {
		q++;
		if (!(*q >= '0' && *q <= '9')) return fail(ps, "bad number");
		while (*q >= '0' && *q <= '9') q++;
	}
	if (*q == 'e' || *q == 'E') {
		q++;
		if (*q == '+' || *q == '-') q++;
		if (!(*q >= '0' && *q <= '9')) return fail(ps, "bad number");
		while (*q >= '0' && *q <= '9') q++;
	}
	size_t len = (size_t)(q - s);
	char *tmp = malloc(len + 1);
	if (!tmp) return fail(ps, "out of memory");
	memcpy(tmp, s, len);
	tmp[len] = 0;
	json_node *n = new_node(JSON_NUMBER);
	if (!n) {
		free(tmp);
		return fail(ps, "out of memory");
	}
	n->number = strtod(tmp, NULL);
	free(tmp);
	ps->p = q;
	return n;
}

static json_node *
parse_literal(parser *ps, const char *word, json_type type, bool value)
{
	size_t len = strlen(word);
	if (strncmp(ps->p, word, len) != 0) {
		return fail(ps, "invalid literal");
	}
	json_node *n = new_node(type);
	if (!n) return fail(ps, "out of memory");
	n->boolean = value;
	ps->p += len;
	return n;
}

static json_node *
parse_array(parser *ps, int depth)
{
	json_node *arr = new_node(JSON_ARRAY);
	if (!arr) return fail(ps, "out of memory");
	ps->p++; // [
	skip_ws(ps);
	if (*ps->p == ']') {
		ps->p++;
		return arr;
	}
	json_node *tail = NULL;
	for (;;) {
		json_node *v = parse_value(ps, depth + 1);
		if (!v) {
			json_free(arr);
			return NULL;
		}
		if (tail) tail->next = v; else arr->first = v;
		tail = v;
		skip_ws(ps);
		if (*ps->p == ',') {
			ps->p++;
		} else if (*ps->p == ']') {
			ps->p++;
			return arr;
		} else {
			json_free(arr);
			return fail(ps, "expected ',' or ']'");
		}
	}
}

static json_node *
parse_object(parser *ps, int depth)
{
	json_node *obj = new_node(JSON_OBJECT);
	if (!obj) return fail(ps, "out of memory");
	ps->p++; // {
	skip_ws(ps);
	if (*ps->p == '}') {
		ps->p++;
		return obj;
	}
	json_node *tail = NULL;
	for (;;) {
		skip_ws(ps);
		size_t klen;
		char *key = parse_string_raw(ps, &klen);
		if (!key) {
			json_free(obj);
			return NULL;
		}
		skip_ws(ps);
		if (*ps->p != ':') {
			free(key);
			json_free(obj);
			return fail(ps, "expected ':'");
		}
		ps->p++;
		json_node *v = parse_value(ps, depth + 1);
		if (!v) {
			free(key);
			json_free(obj);
			return NULL;
		}
		v->key = key;
		if (tail) tail->next = v; else obj->first = v;
		tail = v;
		skip_ws(ps);
		if (*ps->p == ',') {
			ps->p++;
		} else if (*ps->p == '}') {
			ps->p++;
			return obj;
		} else {
			json_free(obj);
			return fail(ps, "expected ',' or '}'");
		}
	}
}

static json_node *
parse_value(parser *ps, int depth)
{
	if (depth > MAX_DEPTH) {
		return fail(ps, "nesting too deep");
	}
	skip_ws(ps);
	switch (*ps->p) {
		case '{': return parse_object(ps, depth);
		case '[': return parse_array(ps, depth);
		case '"': {
			size_t len;
			char *s = parse_string_raw(ps, &len);
			if (!s) return NULL;
			json_node *n = new_node(JSON_STRING);
			if (!n) {
				free(s);
				return fail(ps, "out of memory");
			}
			n->string = s;
			n->string_len = len;
			return n;
		}
		case 't': return parse_literal(ps, "true", JSON_BOOL, true);
		case 'f': return parse_literal(ps, "false", JSON_BOOL, false);
		case 'n': return parse_literal(ps, "null", JSON_NULL, false);
		case 0:   return fail(ps, "unexpected end of input");
		default:
			if (*ps->p == '-' || (*ps->p >= '0' && *ps->p <= '9')) {
				return parse_number(ps);
			}
			return fail(ps, "unexpected character");
	}
}

json_node *
json_parse(const char *text, const char **err)
{
	parser ps = { text, NULL };
	json_node *n = parse_value(&ps, 0);
	if (n) {
		skip_ws(&ps);
		if (*ps.p != 0) {
			json_free(n);
			n = NULL;
			ps.err = "trailing characters after JSON value";
		}
	}
	if (!n && err) {
		*err = ps.err ? ps.err : "parse error";
	}
	return n;
}

const json_node *
json_get(const json_node *object, const char *key)
{
	if (!object || object->type != JSON_OBJECT) {
		return NULL;
	}
	for (const json_node *c = object->first; c; c = c->next) {
		if (c->key && !strcmp(c->key, key)) {
			return c;
		}
	}
	return NULL;
}

const char *
json_as_string(const json_node *node)
{
	return (node && node->type == JSON_STRING) ? node->string : NULL;
}

// ---- writer ----

void
json_buf_init(json_buf *b)
{
	b->data = NULL;
	b->len = 0;
	b->cap = 0;
}

void
json_buf_free(json_buf *b)
{
	free(b->data);
	json_buf_init(b);
}

static void
buf_reserve(json_buf *b, size_t extra)
{
	if (b->len + extra + 1 <= b->cap) {
		return;
	}
	size_t cap = b->cap ? b->cap : 256;
	while (cap < b->len + extra + 1) {
		cap *= 2;
	}
	char *d = realloc(b->data, cap);
	if (!d) {
		fprintf(stderr, "mcp: out of memory\n");
		exit(1);
	}
	b->data = d;
	b->cap = cap;
}

void
json_buf_append_n(json_buf *b, const char *s, size_t n)
{
	buf_reserve(b, n);
	memcpy(b->data + b->len, s, n);
	b->len += n;
	b->data[b->len] = 0;
}

void
json_buf_append(json_buf *b, const char *s)
{
	json_buf_append_n(b, s, strlen(s));
}

void
json_buf_append_string(json_buf *b, const char *s, size_t n)
{
	json_buf_append(b, "\"");
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		switch (c) {
			case '"':  json_buf_append(b, "\\\""); break;
			case '\\': json_buf_append(b, "\\\\"); break;
			case '\n': json_buf_append(b, "\\n"); break;
			case '\r': json_buf_append(b, "\\r"); break;
			case '\t': json_buf_append(b, "\\t"); break;
			case '\b': json_buf_append(b, "\\b"); break;
			case '\f': json_buf_append(b, "\\f"); break;
			default:
				if (c < 0x20) {
					char tmp[8];
					snprintf(tmp, sizeof(tmp), "\\u%04x", c);
					json_buf_append(b, tmp);
				} else {
					json_buf_append_n(b, (const char *)&c, 1);
				}
		}
	}
	json_buf_append(b, "\"");
}

void
json_buf_append_scalar(json_buf *b, const json_node *node)
{
	if (!node) {
		json_buf_append(b, "null");
		return;
	}
	switch (node->type) {
		case JSON_BOOL:
			json_buf_append(b, node->boolean ? "true" : "false");
			break;
		case JSON_NUMBER: {
			char tmp[40];
			if (node->number == floor(node->number) && fabs(node->number) < 9007199254740992.0) {
				snprintf(tmp, sizeof(tmp), "%.0f", node->number);
			} else {
				snprintf(tmp, sizeof(tmp), "%.17g", node->number);
			}
			json_buf_append(b, tmp);
			break;
		}
		case JSON_STRING:
			json_buf_append_string(b, node->string, node->string_len);
			break;
		default:
			json_buf_append(b, "null");
	}
}

void
json_buf_append_base64(json_buf *b, const unsigned char *data, size_t n)
{
	static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	buf_reserve(b, (n + 2) / 3 * 4);
	char *o = b->data + b->len;
	size_t i = 0;
	for (; i + 2 < n; i += 3) {
		unsigned v = (unsigned)data[i] << 16 | (unsigned)data[i + 1] << 8 | data[i + 2];
		*o++ = tbl[v >> 18]; *o++ = tbl[(v >> 12) & 63]; *o++ = tbl[(v >> 6) & 63]; *o++ = tbl[v & 63];
	}
	if (n - i == 1) {
		unsigned v = (unsigned)data[i] << 16;
		*o++ = tbl[v >> 18]; *o++ = tbl[(v >> 12) & 63]; *o++ = '='; *o++ = '=';
	} else if (n - i == 2) {
		unsigned v = (unsigned)data[i] << 16 | (unsigned)data[i + 1] << 8;
		*o++ = tbl[v >> 18]; *o++ = tbl[(v >> 12) & 63]; *o++ = tbl[(v >> 6) & 63]; *o++ = '=';
	}
	b->len = (size_t)(o - b->data);
	b->data[b->len] = 0;
}

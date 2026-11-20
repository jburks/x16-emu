// Commander X16 Emulator
// Minimal JSON parser/writer for the MCP server. No external dependencies.
// All rights reserved. License: 2-clause BSD

#ifndef MCP_JSON_H
#define MCP_JSON_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
	JSON_NULL,
	JSON_BOOL,
	JSON_NUMBER,
	JSON_STRING,
	JSON_ARRAY,
	JSON_OBJECT
} json_type;

typedef struct json_node {
	json_type type;
	bool boolean;               // JSON_BOOL
	double number;              // JSON_NUMBER
	char *string;               // JSON_STRING: decoded UTF-8, NUL terminated
	size_t string_len;          // JSON_STRING: length in bytes (strings may contain "\u0000")
	char *key;                  // member name when this node is an object member, else NULL
	struct json_node *first;    // first child (ARRAY and OBJECT)
	struct json_node *next;     // next sibling
} json_node;

// Parses one complete JSON document. Returns NULL on a syntax error, and then
// (if err is not NULL) stores a static description in *err. Free with json_free().
json_node *json_parse(const char *text, const char **err);
void json_free(json_node *node);

// Object member lookup (first match). Returns NULL if node is not an object or has no such key.
const json_node *json_get(const json_node *object, const char *key);
// Returns the string value, or NULL if node is NULL or not a string.
const char *json_as_string(const json_node *node);

// Growable output buffer.
typedef struct {
	char *data;
	size_t len;
	size_t cap;
} json_buf;

void json_buf_init(json_buf *b);
void json_buf_free(json_buf *b);
void json_buf_append(json_buf *b, const char *s);
void json_buf_append_n(json_buf *b, const char *s, size_t n);
// Appends s as a quoted, escaped JSON string.
void json_buf_append_string(json_buf *b, const char *s, size_t n);
// Appends the standard base64 encoding (with padding) of data.
void json_buf_append_base64(json_buf *b, const unsigned char *data, size_t n);
// Appends a scalar node (null/bool/number/string) as JSON. Other types append "null".
void json_buf_append_scalar(json_buf *b, const json_node *node);

#endif

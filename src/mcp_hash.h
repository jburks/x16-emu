// Commander X16 Emulator
// SHA-256 for the MCP server's hash_memory tool. No external dependencies.
// All rights reserved. License: 2-clause BSD

#ifndef MCP_HASH_H
#define MCP_HASH_H

#include <stddef.h>
#include <stdint.h>

// Computes the SHA-256 digest of data[0..len) into out[32].
void mcp_sha256(const uint8_t *data, size_t len, uint8_t out[32]);

#endif

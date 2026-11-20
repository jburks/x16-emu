// Commander X16 Emulator
// Host path policy for the MCP server: which host files the tools that read or write files
// (load_prg, and the "path" argument of read_memory and get_screen) may touch.
// All rights reserved. License: 2-clause BSD

#ifndef MCP_PATHS_H
#define MCP_PATHS_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
	MCP_PATHS_RESTRICTED,   // paths must lie inside the root folder
	MCP_PATHS_UNRESTRICTED, // X16EMU_MCP_UNRESTRICTED is set: any path
	MCP_PATHS_DISABLED      // the root is a drive root, or could not be determined: no host paths at all
} mcp_paths_mode;

// Decides the policy once, at startup. The root is, in order of preference: $X16EMU_MCP_ROOT, the -fsroot
// folder (pass it as fsroot, or NULL), or the current working directory. $X16EMU_MCP_UNRESTRICTED
// (set and not "0") turns the restriction off. Prints one line about the outcome to stderr.
void mcp_paths_init(const char *fsroot);

mcp_paths_mode mcp_paths_get_mode(void);
// The root folder (absolute), or NULL when unrestricted or disabled.
const char *mcp_paths_root(void);

// Turns a path given by a client into the path to open. Relative paths are taken relative to the root.
// Paths are judged by their text only: "." and ".." are resolved, but symbolic links are NOT examined,
// so a link inside the root that points outside it is followed. Returns false with a message in err if the
// path is not allowed.
bool mcp_path_resolve(const char *path, char *out, size_t out_size, char *err, size_t err_size);

// For tests: the lexical normalisation and containment check, without the environment.
bool mcp_path_is_inside(const char *root, const char *path, char *out, size_t out_size);

#endif

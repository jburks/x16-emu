// Commander X16 Emulator
// Host path policy for the MCP server. See mcp_paths.h.
// All rights reserved. License: 2-clause BSD

#ifndef __APPLE__
#define _XOPEN_SOURCE 700
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#ifdef _WIN32
#include <ctype.h>
#include <direct.h>
#define getcwd _getcwd
#else
#include <unistd.h>
#endif
#include "mcp_paths.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static mcp_paths_mode mode = MCP_PATHS_DISABLED;
static char root_canonical[PATH_MAX]; // the root with symbolic links resolved
static char root_given[PATH_MAX];     // the root as configured, made absolute (may differ through a link)
static char disabled_reason[640] = "host paths are not configured";

#ifdef _WIN32
static bool is_sep(char c) { return c == '/' || c == '\\'; }
static int cmp_char(char a, char b) { return tolower((unsigned char)a) - tolower((unsigned char)b); }
#else
static bool is_sep(char c) { return c == '/'; }
static int cmp_char(char a, char b) { return (unsigned char)a - (unsigned char)b; }
#endif

// Length of the non-component prefix: "/" on POSIX; "C:", "C:\" or "\\server\share\" on Windows.
static size_t prefix_len(const char *p)
{
#ifdef _WIN32
	if (is_sep(p[0]) && is_sep(p[1])) { // UNC: \\server\share\...
		size_t i = 2;
		int parts = 0;
		while (p[i] && parts < 2) {
			if (is_sep(p[i])) parts++;
			if (parts < 2) i++;
		}
		return is_sep(p[i]) ? i + 1 : i;
	}
	if (p[0] && p[1] == ':') {
		return is_sep(p[2]) ? 3 : 2;
	}
#endif
	return is_sep(p[0]) ? 1 : 0;
}

static bool is_absolute(const char *p)
{
	size_t n = prefix_len(p);
#ifdef _WIN32
	return n >= 3 || (n >= 2 && is_sep(p[0])); // "C:\" or UNC; "C:foo" and "\foo" are drive-relative
#else
	return n > 0;
#endif
}

// Normalises an absolute path in place: collapses "." and ".." and repeated separators, uses one separator
// style, and removes a trailing separator. Returns the length of the prefix (the part ".." can never remove).
static size_t normalise(char *path, size_t cap)
{
	char tmp[PATH_MAX];
	size_t plen = prefix_len(path);
	if (plen >= sizeof(tmp)) { path[0] = 0; return 0; }
	for (size_t i = 0; i < plen; i++) tmp[i] = is_sep(path[i]) ? '/' : path[i];
	size_t out = plen;
	size_t floor_len = plen; // ".." stops here
	const char *c = path + plen;
	while (*c) {
		while (is_sep(*c)) c++;
		const char *e = c;
		while (*e && !is_sep(*e)) e++;
		size_t n = (size_t)(e - c);
		if (n == 0) break;
		if (n == 1 && c[0] == '.') {
			// skip
		} else if (n == 2 && c[0] == '.' && c[1] == '.') {
			if (out > floor_len) { // drop the last component and its separator
				size_t k = out;
				while (k > floor_len && tmp[k - 1] == '/') k--;
				while (k > floor_len && tmp[k - 1] != '/') k--;
				out = k;
			}
		} else {
			if (out > floor_len && tmp[out - 1] != '/') tmp[out++] = '/';
			if (out + n + 1 >= sizeof(tmp)) { path[0] = 0; return 0; }
			memcpy(tmp + out, c, n);
			out += n;
		}
		c = e;
	}
	while (out > floor_len && tmp[out - 1] == '/') out--;
	tmp[out] = 0;
	if (out >= cap) { path[0] = 0; return 0; }
	memcpy(path, tmp, out + 1);
	return plen;
}

static bool same_prefix_and_start(const char *root, const char *path)
{
	size_t rl = strlen(root);
	if (strlen(path) < rl) return false;
	for (size_t i = 0; i < rl; i++) {
		if (cmp_char(root[i], path[i]) != 0) return false;
	}
	// the root must end exactly at a component boundary of the path
	return path[rl] == 0 || path[rl] == '/' || (rl > 0 && root[rl - 1] == '/');
}

bool mcp_path_is_inside(const char *root, const char *path, char *out, size_t out_size)
{
	char r[PATH_MAX], p[PATH_MAX];
	if (strlen(root) >= sizeof(r) || strlen(path) >= sizeof(p)) return false;
	strcpy(r, root);
	normalise(r, sizeof(r));
	if (is_absolute(path)) {
		strcpy(p, path);
	} else {
		size_t rl = strlen(r), pl = strlen(path);
		if (rl + pl + 2 > sizeof(p)) return false;
		memcpy(p, r, rl);
		p[rl] = '/';
		memcpy(p + rl + 1, path, pl + 1);
	}
	normalise(p, sizeof(p));
	if (!p[0] || !r[0] || !same_prefix_and_start(r, p)) return false;
	if (strlen(p) >= out_size) return false;
	strcpy(out, p);
	return true;
}

static bool absolute_of(const char *path, char *out, size_t cap)
{
	if (is_absolute(path)) {
		if (strlen(path) >= cap) return false;
		strcpy(out, path);
	} else {
		char cwd[PATH_MAX];
		if (!getcwd(cwd, sizeof(cwd))) return false;
		size_t cl = strlen(cwd), pl = strlen(path);
		if (cl + pl + 2 > cap) return false;
		memcpy(out, cwd, cl);
		out[cl] = '/';
		memcpy(out + cl + 1, path, pl + 1);
	}
	normalise(out, cap);
	return out[0] != 0;
}

static bool is_drive_root(const char *abs)
{
	return strlen(abs) == prefix_len(abs); // nothing after the prefix: "/" or "C:/"
}

static bool resolve_links(const char *abs, char *out, size_t cap)
{
#ifdef _WIN32
	return _fullpath(out, abs, cap) != NULL;
#else
	char buf[PATH_MAX];
	if (!realpath(abs, buf) || strlen(buf) >= cap) return false;
	strcpy(out, buf);
	return true;
#endif
}

static const char *env_nonempty(const char *name)
{
	const char *v = getenv(name);
	return v && v[0] ? v : NULL;
}

void mcp_paths_init(const char *fsroot)
{
	const char *unrestricted = env_nonempty("X16EMU_MCP_UNRESTRICTED");
	if (unrestricted && strcmp(unrestricted, "0")) {
		mode = MCP_PATHS_UNRESTRICTED;
		fprintf(stderr, "mcp: host file paths are UNRESTRICTED (X16EMU_MCP_UNRESTRICTED is set)\n");
		return;
	}
	const char *configured = env_nonempty("X16EMU_MCP_ROOT");
	const char *source = "X16EMU_MCP_ROOT";
	if (!configured && fsroot && fsroot[0]) {
		configured = fsroot; // main.c has already defaulted this to the working directory when there was no -fsroot
		source = "the emulated drive folder (-fsroot, else the current directory)";
	}
	if (!configured) {
		configured = ".";
		source = "the current directory";
	}
	char abs[PATH_MAX];
	if (!absolute_of(configured, abs, sizeof(abs))) {
		snprintf(disabled_reason, sizeof(disabled_reason), "the host path root (%.100s) could not be determined", configured);
		mode = MCP_PATHS_DISABLED;
		fprintf(stderr, "mcp: host file paths are disabled: %s\n", disabled_reason);
		return;
	}
	strcpy(root_given, abs);
	if (!resolve_links(abs, root_canonical, sizeof(root_canonical))) {
		strcpy(root_canonical, abs); // the folder may not exist (yet): fall back to the text
	} else {
		normalise(root_canonical, sizeof(root_canonical));
	}
	if (is_drive_root(root_canonical) || is_drive_root(root_given)) {
		snprintf(disabled_reason, sizeof(disabled_reason),
		         "the host path root (from %.80s) is a drive root (%.100s), which would not restrict anything; "
		         "pass -fsroot or set X16EMU_MCP_ROOT to a project folder, or set X16EMU_MCP_UNRESTRICTED=1 to allow every path",
		         source, root_canonical);
		mode = MCP_PATHS_DISABLED;
		fprintf(stderr, "mcp: host file paths are disabled: %s\n", disabled_reason);
		return;
	}
	mode = MCP_PATHS_RESTRICTED;
	fprintf(stderr, "mcp: host file paths are limited to %s (from %s; X16EMU_MCP_ROOT changes it, "
	        "X16EMU_MCP_UNRESTRICTED=1 turns the restriction off)\n", root_canonical, source);
	const char *home = env_nonempty("HOME");
#ifdef _WIN32
	if (!home) home = env_nonempty("USERPROFILE");
#endif
	if (home) {
		char h[PATH_MAX];
		if (absolute_of(home, h, sizeof(h)) && (strcmp(h, root_canonical) == 0 || strcmp(h, root_given) == 0)) {
			fprintf(stderr, "mcp: warning: the host path root is your home directory; consider -fsroot with a project folder\n");
		}
	}
}

mcp_paths_mode mcp_paths_get_mode(void) { return mode; }

const char *mcp_paths_root(void) { return mode == MCP_PATHS_RESTRICTED ? root_canonical : NULL; }

bool mcp_path_resolve(const char *path, char *out, size_t out_size, char *err, size_t err_size)
{
	if (mode == MCP_PATHS_UNRESTRICTED) {
		if (strlen(path) >= out_size) {
			snprintf(err, err_size, "path is too long");
			return false;
		}
		strcpy(out, path);
		return true;
	}
	if (mode == MCP_PATHS_DISABLED) {
		snprintf(err, err_size, "host file paths are disabled: %s", disabled_reason);
		return false;
	}
	if (mcp_path_is_inside(root_canonical, path, out, out_size) || mcp_path_is_inside(root_given, path, out, out_size)) {
		return true;
	}
	snprintf(err, err_size,
	         "%.200s is outside the folder the MCP server may use for host files (%.300s). Paths may be relative to that "
	         "folder. The folder comes from -fsroot (else the current directory) or X16EMU_MCP_ROOT, and "
	         "X16EMU_MCP_UNRESTRICTED=1 turns the restriction off",
	         path, root_canonical);
	return false;
}

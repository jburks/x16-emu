// Commander X16 Emulator
// MCP tool implementations. Every run() executes on the emulator thread.
// All rights reserved. License: 2-clause BSD

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <SDL.h>
#include "glue.h"
#include "memory.h"
#include "video.h"
#include "keyboard.h"
#include "mcp_hash.h"
#include "mcp_paths.h"
#include <zlib.h>
#include "i2c.h"
#include "cpu/fake6502.h"
#include "mcp.h"
#include "mcp_internal.h"

// timing.c: counted once per completed video frame
extern uint32_t frames;
extern int64_t cpu_ticks;
extern uint32_t sdlTicks_base; // wall-clock origin of the speed limiter

void
mcp_job_fail(mcp_job *job, const char *message)
{
	job->is_error = true;
	job->text.len = 0;
	if (job->text.data) {
		job->text.data[0] = 0;
	}
	json_buf_append(&job->text, message);
}

// Reads an unsigned integer argument given either as a JSON number or as a
// string ("1234", "0x9F20", "$9F20"). Returns false if absent or invalid.
static bool
arg_uint(const mcp_job *job, const char *name, uint32_t *out)
{
	const json_node *n = json_get(job->args, name);
	if (!n) {
		return false;
	}
	if (n->type == JSON_NUMBER) {
		if (n->number < 0 || n->number > 4294967295.0 || n->number != (double)(uint32_t)n->number) {
			return false;
		}
		*out = (uint32_t)n->number;
		return true;
	}
	if (n->type == JSON_STRING && n->string[0]) {
		const char *t = n->string;
		int base = 10;
		if (t[0] == '$') {
			t++;
			base = 16;
		} else if (t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) {
			t += 2;
			base = 16;
		}
		char *end;
		unsigned long v = strtoul(t, &end, base);
		if (*t == 0 || *end != 0 || *t == '-' || v > 0xFFFFFFFFul) {
			return false;
		}
		*out = (uint32_t)v;
		return true;
	}
	return false;
}

// ---- get_registers ----

static bool
run_get_registers(mcp_job *job)
{
	char tmp[320];
	snprintf(tmp, sizeof(tmp),
		"{\"pc\":\"0x%04X\",\"a\":\"0x%02X\",\"x\":\"0x%02X\",\"y\":\"0x%02X\","
		"\"sp\":\"0x%04X\",\"status\":\"0x%02X\",\"ram_bank\":%u,\"rom_bank\":%u,\"paused\":%s,\"cycles\":%llu}",
		regs.pc, regs.a, regs.xl, regs.yl, regs.sp, regs.status,
		memory_get_ram_bank(), memory_get_rom_bank(), mcp_is_paused() ? "true" : "false",
		(unsigned long long)mcp_total_cycles());
	json_buf_append(&job->text, tmp);
	return true;
}

static const mcp_tool tool_get_registers = {
	"get_registers",
	"Returns the 65C02 registers (pc, a, x, y, sp, status), the current RAM and ROM bank, whether the emulator is paused, and cycles: the exact number of CPU clock cycles executed since start. Subtract two values to measure what a routine costs.",
	"{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
	run_get_registers,
	0
};

// ---- get_status ----

static bool
run_get_status(mcp_job *job)
{
	char tmp[400];
	// How far the emulation is behind wall-clock time, as the speed limiter sees it.
	// Near 0 or negative: keeping up (and being throttled). Growing: the host cannot
	// run the emulator at full speed.
	long long lag_ms = (long long)(SDL_GetTicks() - sdlTicks_base) - cpu_ticks / MHZ / 1000;
	snprintf(tmp, sizeof(tmp),
		"{\"frames\":%u,\"cycles\":%llu,\"cpu_cycles_at_last_frame\":%lld,\"mhz\":%u,\"warp\":%s,\"lag_ms\":%lld,\"paused\":%s,"
		"\"state\":\"%s\",\"breakpoints\":%d,\"host_paths\":\"%s\",\"host_root\":",
		frames, (unsigned long long)mcp_total_cycles(), (long long)cpu_ticks, (unsigned)MHZ, warp_mode ? "true" : "false", lag_ms,
		mcp_is_paused() ? "true" : "false", mcp_stop_name(), mcp_bp_active,
		mcp_paths_get_mode() == MCP_PATHS_RESTRICTED ? "restricted" :
		mcp_paths_get_mode() == MCP_PATHS_UNRESTRICTED ? "unrestricted" : "disabled");
	json_buf_append(&job->text, tmp);
	const char *root = mcp_paths_root();
	if (root) {
		json_buf_append_string(&job->text, root, strlen(root));
	} else {
		json_buf_append(&job->text, "null");
	}
	json_buf_append(&job->text, "}");
	return true;
}

static const mcp_tool tool_get_status = {
	"get_status",
	"Returns emulator progress: the number of video frames completed since start (about 60 per emulated second), "
	"the CPU cycle count at the last completed frame, the emulated clock in MHz, warp mode, lag_ms (how far emulation is behind wall-clock time; it grows if the host cannot keep up), whether paused, the state (running, paused, breakpoint, watchpoint, step or frames: why it last stopped) and the number of breakpoints. Also host_paths (\"restricted\", \"unrestricted\" or \"disabled\") and host_root: the folder that load_prg and the path arguments of read_memory and get_screen must stay inside (null when unrestricted or disabled).",
	"{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
	run_get_status,
	0
};

// ---- read_memory ----

#define VRAM_SIZE 0x20000u
#define MAX_READ 0x20000u

// A validated memory range, shared by read_memory, hash_memory and the file dumps.
typedef struct {
	bool vram;
	bool have_bank;
	uint32_t address, count, bank;
} mem_range;

// Parses and validates space/address/count/bank. On failure the job is failed and false is returned.
// count_default 0 means count is required.
static bool
parse_mem_range(mcp_job *job, mem_range *r, uint32_t count_default)
{
	const json_node *sp = json_get(job->args, "space");
	const char *space = sp ? json_as_string(sp) : "cpu";
	if (!space || (strcmp(space, "cpu") && strcmp(space, "vram"))) {
		mcp_job_fail(job, "space must be \"cpu\" or \"vram\"");
		return false;
	}
	r->vram = !strcmp(space, "vram");
	r->bank = 0;
	r->count = count_default;
	if (!arg_uint(job, "address", &r->address)) {
		mcp_job_fail(job, "address is required: a non-negative integer or a string like \"0x9F20\" or \"$9F20\"");
		return false;
	}
	if (json_get(job->args, "count") ? !arg_uint(job, "count", &r->count) : count_default == 0) {
		mcp_job_fail(job, count_default ? "count must be a non-negative integer" : "count is required");
		return false;
	}
	r->have_bank = json_get(job->args, "bank") != NULL;
	if (r->have_bank && (!arg_uint(job, "bank", &r->bank) || r->bank > 255)) {
		mcp_job_fail(job, "bank must be an integer from 0 to 255");
		return false;
	}
	if (r->have_bank && r->vram) {
		mcp_job_fail(job, "bank only applies to space \"cpu\"");
		return false;
	}
	uint32_t limit = r->vram ? VRAM_SIZE : 0x10000u;
	if (r->count == 0 || r->count > MAX_READ) {
		mcp_job_fail(job, "count must be between 1 and 131072");
		return false;
	}
	if (r->address >= limit || r->count > limit - r->address) {
		char msg[96];
		snprintf(msg, sizeof(msg), "range exceeds the %s address space (0x%X bytes)", space, limit);
		mcp_job_fail(job, msg);
		return false;
	}
	return true;
}

// Reads the range without side effects into out[count].
static void
fetch_mem_range(const mem_range *r, uint8_t *out)
{
	for (uint32_t i = 0; i < r->count; i++) {
		out[i] = r->vram ? video_space_read(r->address + i)
		                 : debug_read6502((uint16_t)(r->address + i), 0, r->have_bank ? (int16_t)r->bank : USE_CURRENT_X16_BANK);
	}
}

// Optional "path" and "overwrite" arguments: where to dump raw bytes instead of returning them.
// Returns false (job failed) on a bad argument; *path is NULL if no path was given.
static bool
parse_dump_args(mcp_job *job, const char **path, bool *overwrite)
{
	const json_node *pn = json_get(job->args, "path");
	const json_node *ow = json_get(job->args, "overwrite");
	*path = NULL;
	*overwrite = false;
	if (ow && ow->type != JSON_BOOL) {
		mcp_job_fail(job, "overwrite must be true or false");
		return false;
	}
	*overwrite = ow && ow->boolean;
	if (pn) {
		*path = json_as_string(pn);
		if (!*path || !(*path)[0] || pn->string_len != strlen(*path)) {
			mcp_job_fail(job, "path must be a non-empty string");
			return false;
		}
	} else if (*overwrite) {
		mcp_job_fail(job, "overwrite only applies together with path");
		return false;
	}
	return true;
}

// Writes data to a host file. An existing file is only replaced if overwrite is set. Returns false
// (job failed) on error.
static bool
dump_to_file(mcp_job *job, const char *path, const uint8_t *data, size_t len, bool overwrite)
{
	char msg[700];
	FILE *probe = fopen(path, "rb");
	if (probe) {
		fclose(probe);
		if (!overwrite) {
			snprintf(msg, sizeof(msg), "%.400s already exists (pass overwrite: true to replace it)", path);
			mcp_job_fail(job, msg);
			return false;
		}
	}
	FILE *f = fopen(path, "wb");
	if (!f) {
		snprintf(msg, sizeof(msg), "cannot create %.400s: %s", path, strerror(errno));
		mcp_job_fail(job, msg);
		return false;
	}
	bool ok = fwrite(data, 1, len, f) == len;
	ok = fclose(f) == 0 && ok;
	if (!ok) {
		snprintf(msg, sizeof(msg), "error writing %.400s: %s", path, strerror(errno));
		mcp_job_fail(job, msg);
		return false;
	}
	return true;
}

// Applies the host path policy (see mcp_paths.h): turns the client's path into the one to open, or fails the job.
static bool
host_path(mcp_job *job, const char *path, char *out, size_t out_size)
{
	char err[900];
	if (!mcp_path_resolve(path, out, out_size, err, sizeof(err))) {
		mcp_job_fail(job, err);
		return false;
	}
	return true;
}

static bool
run_read_memory(mcp_job *job)
{
	mem_range r;
	const char *path;
	bool overwrite;
	if (!parse_mem_range(job, &r, 16) || !parse_dump_args(job, &path, &overwrite)) {
		return true;
	}
	uint8_t *buf = malloc(r.count);
	if (!buf) {
		mcp_job_fail(job, "out of memory");
		return true;
	}
	fetch_mem_range(&r, buf);
	char resolved[4200];
	if (path && (!host_path(job, path, resolved, sizeof(resolved)) || !dump_to_file(job, resolved, buf, r.count, overwrite))) {
		free(buf);
		return true;
	}
	if (path) {
		path = resolved; // report where it really went
	}

	json_buf *t = &job->text;
	char head[160];
	snprintf(head, sizeof(head), "{\"space\":\"%s\",\"address\":\"0x%0*X\",\"count\":%u,",
	         r.vram ? "vram" : "cpu", r.vram ? 5 : 4, r.address, r.count);
	json_buf_append(t, head);
	if (!r.vram) {
		snprintf(head, sizeof(head), "\"ram_bank\":%u,\"rom_bank\":%u,",
		         r.have_bank ? r.bank : memory_get_ram_bank(), r.have_bank ? r.bank : memory_get_rom_bank());
		json_buf_append(t, head);
	}
	if (path) {
		json_buf_append(t, "\"path\":");
		json_buf_append_string(t, path, strlen(path));
		snprintf(head, sizeof(head), ",\"bytes_written\":%u}", r.count);
		json_buf_append(t, head);
	} else {
		json_buf_append(t, "\"hex\":\"");
		static const char hexd[] = "0123456789abcdef";
		for (uint32_t i = 0; i < r.count; i++) {
			char two[2] = { hexd[buf[i] >> 4], hexd[buf[i] & 15] };
			json_buf_append_n(t, two, 2);
		}
		json_buf_append(t, "\"}");
	}
	free(buf);
	return true;
}

static const mcp_tool tool_read_memory = {
	"read_memory",
	"Reads bytes without side effects (I/O registers are not triggered). space \"cpu\" (default) is the 64 KB CPU "
	"address space as the CPU sees it: $A000-$BFFF is banked RAM and $C000-$FFFF is banked ROM, using the current "
	"banks unless `bank` is given (it selects both the RAM and the ROM bank). space \"vram\" is the 128 KB VERA "
	"address space (0x00000-0x1FFFF), including the palette at 0x1FA00 and sprite attributes at 0x1FC00. "
	"Returns JSON with the bytes as one lowercase hex string. With `path` the raw bytes are written to that host "
	"file instead and only the byte count is returned, which keeps large buffers out of the conversation; an "
	"existing file is only replaced if `overwrite` is true. Host paths must lie inside the folder shown as host_root "
	"by get_status; relative paths are relative to it.",
	"{\"type\":\"object\",\"properties\":{"
		"\"space\":{\"type\":\"string\",\"enum\":[\"cpu\",\"vram\"],\"default\":\"cpu\"},"
		"\"address\":{\"type\":[\"integer\",\"string\"],\"description\":\"integer or string such as \\\"0x9F20\\\" or \\\"$9F20\\\"\"},"
		"\"count\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":131072,\"default\":16},"
		"\"bank\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255,\"description\":\"cpu space only\"},"
		"\"path\":{\"type\":\"string\",\"description\":\"write the raw bytes to this host file instead of returning hex\"},"
		"\"overwrite\":{\"type\":\"boolean\",\"default\":false}},"
	 "\"required\":[\"address\"],\"additionalProperties\":false}",
	run_read_memory,
	0
};

// ---- hash_memory ----

static bool
run_hash_memory(mcp_job *job)
{
	mem_range r;
	if (!parse_mem_range(job, &r, 0)) {
		return true;
	}
	const json_node *an = json_get(job->args, "algorithm");
	const char *alg = an ? json_as_string(an) : "both";
	if (!alg || (strcmp(alg, "both") && strcmp(alg, "crc32") && strcmp(alg, "sha256"))) {
		mcp_job_fail(job, "algorithm must be \"crc32\", \"sha256\" or \"both\"");
		return true;
	}
	uint8_t *buf = malloc(r.count);
	if (!buf) {
		mcp_job_fail(job, "out of memory");
		return true;
	}
	fetch_mem_range(&r, buf);
	char tmp[160];
	snprintf(tmp, sizeof(tmp), "{\"space\":\"%s\",\"address\":\"0x%0*X\",\"count\":%u", r.vram ? "vram" : "cpu",
	         r.vram ? 5 : 4, r.address, r.count);
	json_buf_append(&job->text, tmp);
	if (strcmp(alg, "sha256")) {
		snprintf(tmp, sizeof(tmp), ",\"crc32\":\"%08lx\"", (unsigned long)crc32(0, buf, (uInt)r.count));
		json_buf_append(&job->text, tmp);
	}
	if (strcmp(alg, "crc32")) {
		uint8_t d[32];
		mcp_sha256(buf, r.count, d);
		json_buf_append(&job->text, ",\"sha256\":\"");
		for (int i = 0; i < 32; i++) {
			snprintf(tmp, sizeof(tmp), "%02x", d[i]);
			json_buf_append(&job->text, tmp);
		}
		json_buf_append(&job->text, "\"");
	}
	json_buf_append(&job->text, "}");
	free(buf);
	return true;
}

static const mcp_tool tool_hash_memory = {
	"hash_memory",
	"Returns the CRC32 and/or SHA-256 of a memory range, read without side effects exactly as read_memory does. "
	"Use it to check a buffer (for example a VRAM frame buffer) against a known value without transferring it. "
	"`count` is required (1 to 131072). `algorithm` is \"crc32\", \"sha256\" or \"both\" (default). Digests are "
	"lowercase hex; the CRC32 is the standard one (the same as zlib or Python's zlib.crc32).",
	"{\"type\":\"object\",\"properties\":{"
		"\"space\":{\"type\":\"string\",\"enum\":[\"cpu\",\"vram\"],\"default\":\"cpu\"},"
		"\"address\":{\"type\":[\"integer\",\"string\"]},"
		"\"count\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":131072},"
		"\"bank\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255,\"description\":\"cpu space only\"},"
		"\"algorithm\":{\"type\":\"string\",\"enum\":[\"crc32\",\"sha256\",\"both\"],\"default\":\"both\"}},"
	 "\"required\":[\"address\",\"count\"],\"additionalProperties\":false}",
	run_hash_memory,
	0
};

// ---- get_screen ----

static bool
run_get_screen(mcp_job *job)
{
	const char *path;
	bool overwrite;
	if (!parse_dump_args(job, &path, &overwrite)) {
		return true;
	}
	int len = 0;
	uint8_t *png = video_get_png(&len);
	if (!png || len <= 0) {
		free(png);
		mcp_job_fail(job, "could not encode the screen");
		return true;
	}
	if (path) {
		char resolved[4200];
		bool ok = host_path(job, path, resolved, sizeof(resolved)) && dump_to_file(job, resolved, png, (size_t)len, overwrite);
		free(png);
		if (ok) {
			json_buf_append(&job->text, "{\"path\":");
			json_buf_append_string(&job->text, resolved, strlen(resolved));
			char tmp[48];
			snprintf(tmp, sizeof(tmp), ",\"bytes_written\":%d}", len);
			json_buf_append(&job->text, tmp);
		}
		return true;
	}
	job->image = png;
	job->image_len = (size_t)len;
	job->image_mime = "image/png";
	json_buf_append(&job->text, "640x480 screenshot of the emulator's current framebuffer");
	return true;
}

static const mcp_tool tool_get_screen = {
	"get_screen",
	"Returns a 640x480 PNG screenshot of the emulator's current framebuffer. If the emulator is running, the image "
	"may show a frame that is still being drawn; pause first for a stable image. With `path` the PNG is written to "
	"that host file instead of being returned; an existing file is only replaced if `overwrite` is true. The path must "
	"lie inside the folder shown as host_root by get_status; relative paths are relative to it.",
	"{\"type\":\"object\",\"properties\":{"
		"\"path\":{\"type\":\"string\",\"description\":\"write the PNG to this host file instead of returning it\"},"
		"\"overwrite\":{\"type\":\"boolean\",\"default\":false}},\"additionalProperties\":false}",
	run_get_screen,
	0
};

// ---- pause / continue ----

static bool
run_pause(mcp_job *job)
{
	bool was = mcp_is_paused();
	if (!was) {
		mcp_stop(STOP_PAUSE);
	}
	char tmp[96];
	snprintf(tmp, sizeof(tmp), "%s at pc=0x%04X", was ? "already paused" : "paused", regs.pc);
	json_buf_append(&job->text, tmp);
	return true;
}

static const mcp_tool tool_pause = {
	"pause",
	"Pauses the emulator between two instructions. Memory, registers and the screen can then be inspected without "
	"them changing. The window stays open.",
	"{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
	run_pause,
	0
};

static void
cancel_run(mcp_job *job)
{
	(void)job;
	mcp_stop(STOP_PAUSE); // the client gave up: leave the emulator paused and inspectable
}

static bool
run_continue(mcp_job *job)
{
	const json_node *w = json_get(job->args, "wait");
	bool wait = w && w->type == JSON_BOOL && w->boolean;
	if (job->polls == 0) {
		if (w && w->type != JSON_BOOL) {
			mcp_job_fail(job, "wait must be true or false");
			return true;
		}
		if (wait && mcp_bp_active == 0 && mcp_watch_total() == 0) {
			mcp_job_fail(job, "wait=true needs at least one breakpoint or watchpoint, otherwise it would never return");
			return true;
		}
		bool was = mcp_is_paused();
		if (was) {
			mcp_resume();
		}
		if (!wait) {
			json_buf_append(&job->text, was ? "running" : "was not paused; running");
			return true;
		}
		return false;
	}
	if (mcp_is_paused()) { // a breakpoint stopped it
		mcp_describe_stop(&job->text);
		return true;
	}
	return false;
}

static const mcp_tool tool_continue = {
	"continue",
	"Resumes a paused emulator at normal speed. With wait=true it does not return until a breakpoint stops the "
	"emulator (breakpoint or watchpoint; returns where it stopped); on timeout the emulator is paused. Optional timeout_ms (default 30000, "
	"max 600000) applies to wait=true.",
	"{\"type\":\"object\",\"properties\":{\"wait\":{\"type\":\"boolean\",\"default\":false},"
	"\"timeout_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":600000}},\"additionalProperties\":false}",
	run_continue,
	0,
	cancel_run
};

// ---- run_frames ----

static bool
run_run_frames(mcp_job *job)
{
	if (job->polls == 0) {
		uint32_t n = 1;
		if (json_get(job->args, "frames") && (!arg_uint(job, "frames", &n) || n < 1 || n > 1000000)) {
			mcp_job_fail(job, "frames must be an integer from 1 to 1000000");
			return true;
		}
		job->scratch[0] = (uint32_t)(frames + n);
		if (mcp_is_paused()) {
			mcp_resume();
		} else {
			mcp_last_stop.reason = STOP_NONE;
		}
		return false;
	}
	if (mcp_is_paused()) { // a breakpoint stopped it before the frames were up
		mcp_describe_stop(&job->text);
		return true;
	}
	if ((int32_t)(frames - (uint32_t)job->scratch[0]) >= 0) {
		mcp_stop(STOP_FRAMES);
		mcp_describe_stop(&job->text);
		return true;
	}
	return false;
}

static const mcp_tool tool_run_frames = {
	"run_frames",
	"Runs the emulator for exactly `frames` video frames (about 1/60 s each; default 1) and then pauses it, so the "
	"stop point is deterministic: right after the instruction that completed the last frame. Works from paused or "
	"running. Stops early, still paused, if a breakpoint is hit. Returns where it stopped (stopped is \"frames\" or "
	"\"breakpoint\"). At roughly 40-60 frames/s, pass a larger timeout_ms (max 600000) for long runs; on timeout the "
	"emulator is left paused.",
	"{\"type\":\"object\",\"properties\":{\"frames\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":1000000,\"default\":1},"
	"\"timeout_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":600000}},\"additionalProperties\":false}",
	run_run_frames,
	0,
	cancel_run
};

// ---- breakpoints ----

static bool
run_set_breakpoint(mcp_job *job)
{
	uint32_t address, bank = 0;
	if (!arg_uint(job, "address", &address) || address > 0xFFFF) {
		mcp_job_fail(job, "address is required: 0 to 0xFFFF as an integer or a string like \"0xC000\" or \"$C000\"");
		return true;
	}
	int b = -1;
	if (json_get(job->args, "bank")) {
		if (!arg_uint(job, "bank", &bank) || bank > 255) {
			mcp_job_fail(job, "bank must be an integer from 0 to 255");
			return true;
		}
		if (address < 0xA000) {
			mcp_job_fail(job, "bank only applies to addresses from $A000 up (RAM bank for $A000-$BFFF, ROM bank for $C000-$FFFF)");
			return true;
		}
		b = (int)bank;
	}
	bool existed;
	int id = mcp_bp_add((uint16_t)address, b, &existed);
	if (id < 0) {
		mcp_job_fail(job, "too many breakpoints (64 maximum)");
		return true;
	}
	char bankstr[16] = "null";
	if (b >= 0) {
		snprintf(bankstr, sizeof(bankstr), "%d", b);
	}
	char tmp[160];
	snprintf(tmp, sizeof(tmp), "{\"id\":%d,\"address\":\"0x%04X\",\"bank\":%s,\"already_set\":%s}",
	         id, address, bankstr, existed ? "true" : "false");
	json_buf_append(&job->text, tmp);
	return true;
}

static const mcp_tool tool_set_breakpoint = {
	"set_breakpoint",
	"Sets an execution breakpoint: the emulator pauses just before the instruction at `address` executes. For "
	"addresses from $A000 up, an optional `bank` restricts it to that RAM bank ($A000-$BFFF) or ROM bank "
	"($C000-$FFFF); without it the breakpoint matches in any bank. Resuming from a breakpoint steps over it once. "
	"Setting the same address and bank again returns the existing breakpoint. Returns its id.",
	"{\"type\":\"object\",\"properties\":{"
		"\"address\":{\"type\":[\"integer\",\"string\"],\"description\":\"integer or string such as \\\"0xC000\\\" or \\\"$C000\\\"\"},"
		"\"bank\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255}},"
	 "\"required\":[\"address\"],\"additionalProperties\":false}",
	run_set_breakpoint,
	0,
	NULL
};

static bool
run_clear_breakpoint(mcp_job *job)
{
	const json_node *all = json_get(job->args, "all");
	uint32_t id;
	bool have_id = json_get(job->args, "id") != NULL;
	if (all && all->type == JSON_BOOL && all->boolean) {
		if (have_id) {
			mcp_job_fail(job, "pass either id or all, not both");
			return true;
		}
		int n = mcp_bp_active;
		mcp_bp_clear();
		char tmp[48];
		snprintf(tmp, sizeof(tmp), "{\"removed\":%d}", n);
		json_buf_append(&job->text, tmp);
		return true;
	}
	if (!have_id || !arg_uint(job, "id", &id)) {
		mcp_job_fail(job, "pass id (an integer from set_breakpoint or list_breakpoints) or all=true");
		return true;
	}
	if (!mcp_bp_remove((int)id)) {
		mcp_job_fail(job, "no such breakpoint id");
		return true;
	}
	json_buf_append(&job->text, "{\"removed\":1}");
	return true;
}

static const mcp_tool tool_clear_breakpoint = {
	"clear_breakpoint",
	"Removes the breakpoint with the given id, or all breakpoints with all=true.",
	"{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\"},\"all\":{\"type\":\"boolean\"}},\"additionalProperties\":false}",
	run_clear_breakpoint,
	0,
	NULL
};

static bool
run_list_breakpoints(mcp_job *job)
{
	json_buf_append(&job->text, "[");
	int id, bank, i = 0;
	uint16_t address;
	unsigned hits;
	while (mcp_bp_get(i, &id, &address, &bank, &hits)) {
		char tmp[128];
		if (bank < 0) {
			snprintf(tmp, sizeof(tmp), "%s{\"id\":%d,\"address\":\"0x%04X\",\"bank\":null,\"hits\":%u}",
			         i ? "," : "", id, address, hits);
		} else {
			snprintf(tmp, sizeof(tmp), "%s{\"id\":%d,\"address\":\"0x%04X\",\"bank\":%d,\"hits\":%u}",
			         i ? "," : "", id, address, bank, hits);
		}
		json_buf_append(&job->text, tmp);
		i++;
	}
	json_buf_append(&job->text, "]");
	return true;
}

static const mcp_tool tool_list_breakpoints = {
	"list_breakpoints",
	"Lists the breakpoints with their ids, addresses, banks (null = any) and how often each was hit.",
	"{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
	run_list_breakpoints,
	0,
	NULL
};

// ---- step ----

static bool
run_step(mcp_job *job)
{
	if (job->polls == 0) {
		uint32_t n = 1;
		if (json_get(job->args, "count") && (!arg_uint(job, "count", &n) || n < 1 || n > 1000000)) {
			mcp_job_fail(job, "count must be an integer from 1 to 1000000");
			return true;
		}
		if (!mcp_is_paused()) {
			mcp_job_fail(job, "the emulator must be paused to step (call pause first)");
			return true;
		}
		mcp_step_begin(n);
		return false;
	}
	if (mcp_is_paused()) { // done, or a breakpoint/watchpoint came first
		mcp_describe_stop(&job->text);
		return true;
	}
	return false;
}

static const mcp_tool tool_step = {
	"step",
	"Executes `count` CPU instructions (default 1) from the paused state and pauses again. Returns where it stopped "
	"and the registers; stopped is \"step\", or \"breakpoint\"/\"watchpoint\" if one came first. A WAI/STP that is "
	"waiting for an interrupt is stepped over: execution continues to the next real instruction (usually the IRQ "
	"handler). Interrupt handlers are not skipped; they step like any other code.",
	"{\"type\":\"object\",\"properties\":{\"count\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":1000000,\"default\":1},"
	"\"timeout_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":600000}},\"additionalProperties\":false}",
	run_step,
	0,
	cancel_run
};

// ---- watchpoints ----

static const char *
access_name(int a)
{
	return a == WATCH_READ ? "read" : a == WATCH_WRITE ? "write" : "readwrite";
}

static bool
run_set_watchpoint(mcp_job *job)
{
	const json_node *spn = json_get(job->args, "space");
	const char *space = spn ? json_as_string(spn) : "cpu";
	if (!space || (strcmp(space, "cpu") && strcmp(space, "vram"))) {
		mcp_job_fail(job, "space must be \"cpu\" or \"vram\"");
		return true;
	}
	bool vram = !strcmp(space, "vram");
	uint32_t limit = vram ? 0x20000u : 0x10000u;
	uint32_t address, length = 1, bank = 0;
	if (!arg_uint(job, "address", &address) || address >= limit) {
		mcp_job_fail(job, vram ? "address is required: 0 to 0x1FFFF as an integer or a string like \"0x0A000\""
		                       : "address is required: 0 to 0xFFFF as an integer or a string like \"0x9F23\" or \"$9F23\"");
		return true;
	}
	if (json_get(job->args, "length") && (!arg_uint(job, "length", &length) || length < 1 || length > limit
	                                       || address + length > limit)) {
		mcp_job_fail(job, vram ? "length must be at least 1 and the range must stay within VRAM (0x00000-0x1FFFF)"
		                       : "length must be at least 1 and the range must stay within $0000-$FFFF");
		return true;
	}
	int access = WATCH_WRITE;
	const json_node *an = json_get(job->args, "access");
	if (an) {
		const char *a = json_as_string(an);
		if (a && !strcmp(a, "read")) access = WATCH_READ;
		else if (a && !strcmp(a, "write")) access = WATCH_WRITE;
		else if (a && !strcmp(a, "readwrite")) access = WATCH_RW;
		else {
			mcp_job_fail(job, "access must be \"read\", \"write\" or \"readwrite\"");
			return true;
		}
	}
	int b = -1;
	if (json_get(job->args, "bank")) {
		if (vram) {
			mcp_job_fail(job, "bank only applies to space \"cpu\"");
			return true;
		}
		if (!arg_uint(job, "bank", &bank) || bank > 255) {
			mcp_job_fail(job, "bank must be an integer from 0 to 255");
			return true;
		}
		b = (int)bank;
	}
	if (vram && access != WATCH_WRITE) {
		mcp_job_fail(job, "VRAM watchpoints see writes only. VRAM reads are not supported: VERA prefetches the byte "
		                  "when an address is set, so a read event would not correspond to a CPU access");
		return true;
	}
	bool existed;
	int id = mcp_watch_add(vram ? WATCH_SPACE_VRAM : WATCH_SPACE_CPU, address, length, b, access, &existed);
	if (id < 0) {
		mcp_job_fail(job, "too many watchpoints (32 maximum)");
		return true;
	}
	char bankstr[16] = "null";
	if (b >= 0) {
		snprintf(bankstr, sizeof(bankstr), "%d", b);
	}
	char tmp[260];
	snprintf(tmp, sizeof(tmp),
	         "{\"id\":%d,\"space\":\"%s\",\"address\":\"0x%0*X\",\"length\":%u,\"access\":\"%s\",\"bank\":%s,"
	         "\"already_set\":%s}",
	         id, space, vram ? 5 : 4, address, length, access_name(access), bankstr, existed ? "true" : "false");
	json_buf_append(&job->text, tmp);
	return true;
}

static const mcp_tool tool_set_watchpoint = {
	"set_watchpoint",
	"Pauses the emulator when something accesses an address range. The emulator stops right after the accessing "
	"instruction completes, and the result reports watchpoint, space, access, address, value and accessing_pc. "
	"space \"cpu\" (default) watches CPU accesses, by default writes to one byte; `access` can be read, write or "
	"readwrite. Instruction fetches count as reads, so a read watch on code fires when it executes. "
	"Addresses from $A000 up can be restricted to a RAM bank ($A000-$BFFF) or ROM bank ($C000-$FFFF). "
	"space \"vram\" watches writes (only) to VERA memory, 0x00000-0x1FFFF, through either "
	"data port: it reports the VRAM address of the first byte written in your range, the byte now stored there, "
	"data_port (0 or 1) and accessing_pc. One CPU store can write several bytes (FX cache writes store up to four, "
	"and a 4-bit write changes half a byte): a hit is reported if any byte it writes lies in the range, and a store "
	"that FX transparency or a nibble mask skips is not a write. Writes the tools make do not trigger "
	"watchpoints. Resuming after a hit does not re-trigger it. Max 32 watchpoints in all.",
	"{\"type\":\"object\",\"properties\":{"
		"\"space\":{\"type\":\"string\",\"enum\":[\"cpu\",\"vram\"],\"default\":\"cpu\"},"
		"\"address\":{\"type\":[\"integer\",\"string\"]},"
		"\"length\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":131072,\"default\":1},"
		"\"access\":{\"type\":\"string\",\"enum\":[\"read\",\"write\",\"readwrite\"],\"default\":\"write\",\"description\":\"vram: write only\"},"
		"\"bank\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255,\"description\":\"cpu space only\"}},"
	 "\"required\":[\"address\"],\"additionalProperties\":false}",
	run_set_watchpoint,
	0,
	NULL
};

static bool
run_clear_watchpoint(mcp_job *job)
{
	const json_node *all = json_get(job->args, "all");
	uint32_t id;
	bool have_id = json_get(job->args, "id") != NULL;
	if (all && all->type == JSON_BOOL && all->boolean) {
		if (have_id) {
			mcp_job_fail(job, "pass either id or all, not both");
			return true;
		}
		int n = mcp_watch_total();
		mcp_watch_clear();
		char tmp[48];
		snprintf(tmp, sizeof(tmp), "{\"removed\":%d}", n);
		json_buf_append(&job->text, tmp);
		return true;
	}
	if (!have_id || !arg_uint(job, "id", &id)) {
		mcp_job_fail(job, "pass id (an integer from set_watchpoint or list_watchpoints) or all=true");
		return true;
	}
	if (!mcp_watch_remove((int)id)) {
		mcp_job_fail(job, "no such watchpoint id");
		return true;
	}
	json_buf_append(&job->text, "{\"removed\":1}");
	return true;
}

static const mcp_tool tool_clear_watchpoint = {
	"clear_watchpoint",
	"Removes the watchpoint with the given id, or all watchpoints with all=true.",
	"{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\"},\"all\":{\"type\":\"boolean\"}},\"additionalProperties\":false}",
	run_clear_watchpoint,
	0,
	NULL
};

static bool
run_list_watchpoints(mcp_job *job)
{
	json_buf_append(&job->text, "[");
	int id, space, bank, access, i = 0;
	uint32_t address, length;
	unsigned hits;
	while (mcp_watch_get(i, &id, &space, &address, &length, &bank, &access, &hits)) {
		char bankstr[16] = "null";
		if (bank >= 0) {
			snprintf(bankstr, sizeof(bankstr), "%d", bank);
		}
		char tmp[260];
		snprintf(tmp, sizeof(tmp),
		         "%s{\"id\":%d,\"space\":\"%s\",\"address\":\"0x%0*X\",\"length\":%u,\"access\":\"%s\",\"bank\":%s,"
		         "\"hits\":%u}",
		         i ? "," : "", id, space == WATCH_SPACE_VRAM ? "vram" : "cpu", space == WATCH_SPACE_VRAM ? 5 : 4, address,
		         length, access_name(access), bankstr, hits);
		json_buf_append(&job->text, tmp);
		i++;
	}
	json_buf_append(&job->text, "]");
	return true;
}

static const mcp_tool tool_list_watchpoints = {
	"list_watchpoints",
	"Lists the watchpoints with their ids, spaces (cpu or vram), ranges, access types, banks (null = any) and hit counts.",
	"{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
	run_list_watchpoints,
	0,
	NULL
};

// ---- write_memory ----

// Decodes hex digits (whitespace allowed between bytes). Returns the byte count, or -1.
static int
decode_hex(const char *s, uint8_t *out, size_t max)
{
	size_t n = 0;
	int hi = -1;
	for (; *s; s++) {
		int v;
		if (*s >= '0' && *s <= '9') v = *s - '0';
		else if (*s >= 'a' && *s <= 'f') v = *s - 'a' + 10;
		else if (*s >= 'A' && *s <= 'F') v = *s - 'A' + 10;
		else if (*s == ' ' || *s == '\n' || *s == '\t' || *s == '\r') {
			if (hi >= 0) return -1; // whitespace inside a byte
			continue;
		} else return -1;
		if (hi < 0) {
			hi = v;
		} else {
			if (n >= max) return -1;
			out[n++] = (uint8_t)(hi << 4 | v);
			hi = -1;
		}
	}
	return hi >= 0 ? -1 : (int)n;
}

static bool
run_write_memory(mcp_job *job)
{
	const json_node *sp = json_get(job->args, "space");
	const char *space = sp ? json_as_string(sp) : "cpu";
	if (!space || (strcmp(space, "cpu") && strcmp(space, "vram"))) {
		mcp_job_fail(job, "space must be \"cpu\" or \"vram\"");
		return true;
	}
	bool vram = !strcmp(space, "vram");
	uint32_t address, bank = 0;
	if (!arg_uint(job, "address", &address)) {
		mcp_job_fail(job, "address is required: a non-negative integer or a string like \"0x0400\" or \"$0400\"");
		return true;
	}
	const char *hex = json_as_string(json_get(job->args, "hex"));
	if (!hex) {
		mcp_job_fail(job, "hex is required: the bytes as a hex string such as \"A9 01 8D 00 04\"");
		return true;
	}
	bool have_bank = json_get(job->args, "bank") != NULL;
	if (have_bank && (!arg_uint(job, "bank", &bank) || bank >= num_ram_banks)) {
		mcp_job_fail(job, "bank must be a valid RAM bank number");
		return true;
	}
	if (have_bank && vram) {
		mcp_job_fail(job, "bank only applies to space \"cpu\"");
		return true;
	}
	uint8_t *data = malloc(MAX_READ);
	if (!data) {
		mcp_job_fail(job, "out of memory");
		return true;
	}
	int n = decode_hex(hex, data, MAX_READ);
	if (n <= 0) {
		free(data);
		mcp_job_fail(job, "hex must contain 1 to 131072 bytes as pairs of hex digits");
		return true;
	}
	uint32_t limit = vram ? VRAM_SIZE : 0xC000u; // CPU ROM is not writable
	if (address >= limit || (uint32_t)n > limit - address) {
		free(data);
		char msg[112];
		snprintf(msg, sizeof(msg), vram ? "range exceeds VRAM (0x20000 bytes)"
		                                : "range must lie within $0000-$BFFF (ROM at $C000 and up is read-only)");
		mcp_job_fail(job, msg);
		return true;
	}

	mcp_watch_suppress = true;
	int64_t saved_ticks = clockticks6502;
	for (int i = 0; i < n; i++) {
		uint32_t a = address + (uint32_t)i;
		if (vram) {
			video_space_write(a, data[i]);
		} else if (have_bank && a >= 0xA000) {
			BRAM[((size_t)bank << 13) + a - 0xA000] = data[i];
		} else {
			write6502((uint16_t)a, 0, data[i]); // I/O and the CPU port keep their side effects
		}
	}
	clockticks6502 = (uint32_t)saved_ticks; // slow I/O writes must not disturb timing
	mcp_watch_suppress = false;
	free(data);

	char tmp[96];
	snprintf(tmp, sizeof(tmp), "{\"written\":%d,\"space\":\"%s\",\"address\":\"0x%0*X\"}", n, space, vram ? 5 : 4, address);
	json_buf_append(&job->text, tmp);
	return true;
}

static const mcp_tool tool_write_memory = {
	"write_memory",
	"Writes bytes given as a hex string. space \"cpu\" (default) writes through the normal CPU write path, so "
	"I/O registers such as VERA ($9F20-$9F3F) have their usual side effects, but it takes no emulated time and does "
	"not trigger watchpoints; ROM ($C000+) is rejected. For $A000-$BFFF, `bank` selects the RAM bank to write "
	"(default: the current one). space \"vram\" writes the 128 KB VERA address space directly (palette, sprite "
	"attributes and PSG registers included). Works while paused or running; pause first if the program could be "
	"touching the same memory.",
	"{\"type\":\"object\",\"properties\":{"
		"\"space\":{\"type\":\"string\",\"enum\":[\"cpu\",\"vram\"],\"default\":\"cpu\"},"
		"\"address\":{\"type\":[\"integer\",\"string\"]},"
		"\"hex\":{\"type\":\"string\",\"description\":\"bytes as hex digit pairs, spaces allowed\"},"
		"\"bank\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255,\"description\":\"RAM bank for $A000-$BFFF, cpu space only\"}},"
	 "\"required\":[\"address\",\"hex\"],\"additionalProperties\":false}",
	run_write_memory,
	0,
	NULL
};

// ---- set_registers ----

static bool
run_set_registers(mcp_job *job)
{
	if (!mcp_is_paused()) {
		mcp_job_fail(job, "the emulator must be paused to change registers (call pause first)");
		return true;
	}
	static const struct { const char *name; uint32_t max; } fields[] = {
		{ "pc", 0xFFFF }, { "a", 0xFF }, { "x", 0xFF }, { "y", 0xFF }, { "sp", 0x1FF },
		{ "status", 0xFF }, { "ram_bank", 0xFF }, { "rom_bank", 0xFF }
	};
	uint32_t v[8];
	bool have[8] = { false };
	bool any = false;
	for (const struct json_node *m = job->args ? job->args->first : NULL; m; m = m->next) {
		bool known = false;
		for (int i = 0; i < 8; i++) {
			if (!strcmp(m->key, fields[i].name)) known = true;
		}
		if (!known) {
			mcp_job_fail(job, "unknown register; use pc, a, x, y, sp, status, ram_bank, rom_bank");
			return true;
		}
	}
	for (int i = 0; i < 8; i++) {
		if (!json_get(job->args, fields[i].name)) {
			continue;
		}
		if (!arg_uint(job, fields[i].name, &v[i]) || v[i] > fields[i].max) {
			char msg[96];
			snprintf(msg, sizeof(msg), "%s must be an integer from 0 to 0x%X", fields[i].name, fields[i].max);
			mcp_job_fail(job, msg);
			return true;
		}
		have[i] = true;
		any = true;
	}
	if (!any) {
		mcp_job_fail(job, "pass at least one of pc, a, x, y, sp, status, ram_bank, rom_bank");
		return true;
	}
	if (have[0]) { regs.pc = (uint16_t)v[0]; waiting = 0; }
	if (have[1]) regs.a = (uint8_t)v[1];
	if (have[2]) regs.xl = (uint8_t)v[2];
	if (have[3]) regs.yl = (uint8_t)v[3];
	if (have[4]) regs.sp = (uint16_t)((regs.sp & 0xFF00) | (v[4] & 0xFF));
	if (have[5]) regs.status = (uint8_t)v[5];
	if (have[6]) memory_set_ram_bank((uint8_t)v[6]);
	if (have[7]) memory_set_rom_bank((uint8_t)v[7]);
	run_get_registers(job);
	return true;
}

static const mcp_tool tool_set_registers = {
	"set_registers",
	"Sets any of pc, a, x, y, sp (low byte), status, ram_bank, rom_bank while the emulator is paused, and returns "
	"the resulting registers. Setting pc also ends a WAI/STP wait. A breakpoint at the new pc will fire when you "
	"resume.",
	"{\"type\":\"object\",\"properties\":{"
		"\"pc\":{\"type\":[\"integer\",\"string\"]},\"a\":{\"type\":[\"integer\",\"string\"]},"
		"\"x\":{\"type\":[\"integer\",\"string\"]},\"y\":{\"type\":[\"integer\",\"string\"]},"
		"\"sp\":{\"type\":[\"integer\",\"string\"]},\"status\":{\"type\":[\"integer\",\"string\"]},"
		"\"ram_bank\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255},"
		"\"rom_bank\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255}},"
	 "\"additionalProperties\":false}",
	run_set_registers,
	0,
	NULL
};

// ---- send_key / type_text ----

extern bool pasting_bas;
extern char *paste_text;

static void
cancel_key(mcp_job *job)
{
	handle_keyboard(false, SDLK_UNKNOWN, (SDL_Scancode)job->scratch[1]); // never leave a key stuck down
}

static bool
run_send_key(mcp_job *job)
{
	if (job->polls == 0) {
		const char *name = json_as_string(json_get(job->args, "key"));
		if (!name) {
			mcp_job_fail(job, "key is required: an SDL key name such as \"Return\", \"A\", \"Space\", \"Left\", \"F5\", \"Left Shift\"");
			return true;
		}
		SDL_Scancode sc = SDL_GetScancodeFromName(name);
		if (sc == SDL_SCANCODE_UNKNOWN) {
			mcp_job_fail(job, "unknown key name (SDL names: letters, digits, \"Return\", \"Space\", \"Escape\", \"Left\", \"Up\", \"F1\"..\"F12\", \"Left Shift\", ...)");
			return true;
		}
		const json_node *an = json_get(job->args, "action");
		const char *action = an ? json_as_string(an) : "tap";
		if (!action || (strcmp(action, "tap") && strcmp(action, "down") && strcmp(action, "up"))) {
			mcp_job_fail(job, "action must be \"tap\", \"down\" or \"up\"");
			return true;
		}
		uint32_t hold = 0;
		if (json_get(job->args, "hold_frames") && (!arg_uint(job, "hold_frames", &hold) || hold > 100000)) {
			mcp_job_fail(job, "hold_frames must be an integer from 0 to 100000");
			return true;
		}
		job->scratch[1] = (uint64_t)sc;
		if (!strcmp(action, "up")) {
			handle_keyboard(false, SDLK_UNKNOWN, sc);
		} else {
			handle_keyboard(true, SDLK_UNKNOWN, sc);
			if (!strcmp(action, "tap")) {
				if (hold == 0) {
					handle_keyboard(false, SDLK_UNKNOWN, sc);
				} else {
					if (mcp_is_paused()) {
						handle_keyboard(false, SDLK_UNKNOWN, sc);
						mcp_job_fail(job, "hold_frames needs the emulator running (time does not pass while paused)");
						return true;
					}
					job->scratch[0] = (uint32_t)(frames + hold);
					return false;
				}
			}
		}
		char tmp[64];
		snprintf(tmp, sizeof(tmp), "{\"key\":\"%s\",\"action\":\"%s\"}", SDL_GetScancodeName(sc), action);
		json_buf_append(&job->text, tmp);
		return true;
	}
	if ((int32_t)(frames - (uint32_t)job->scratch[0]) >= 0) {
		handle_keyboard(false, SDLK_UNKNOWN, (SDL_Scancode)job->scratch[1]);
		json_buf_append(&job->text, "{\"action\":\"tap\"}");
		return true;
	}
	return false;
}

static const mcp_tool tool_send_key = {
	"send_key",
	"Sends a key press to the emulated PS/2 keyboard (it reaches the program through the KERNAL or a game's own "
	"keyboard scan). `key` is an SDL key name such as \"Return\", \"A\", \"Space\", \"Left\", \"F5\" or \"Left Shift\". "
	"`action` is \"tap\" (default: down then up), \"down\" or \"up\" (to hold modifiers or movement keys). "
	"hold_frames (tap only, default 0) keeps the key down for that many video frames while the emulator runs; "
	"use it for programs that poll key state. Keys are queued even while paused and are delivered when running. "
	"For text, use type_text.",
	"{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\"},"
	"\"action\":{\"type\":\"string\",\"enum\":[\"tap\",\"down\",\"up\"],\"default\":\"tap\"},"
	"\"hold_frames\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100000},"
	"\"timeout_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":600000}},\"required\":[\"key\"],\"additionalProperties\":false}",
	run_send_key,
	0,
	cancel_key
};

// Queues text to be typed into the KERNAL keyboard buffer. Returns NULL on success, else an error message.
static const char *
queue_typed(const char *text, size_t len)
{
	if (pasting_bas || paste_text) {
		return "a previous text is still being typed";
	}
	char *copy = SDL_malloc(len + 1);
	if (!copy) {
		return "out of memory";
	}
	for (size_t i = 0; i <= len; i++) {
		copy[i] = text[i] == '\n' ? '\r' : text[i]; // the X16 Enter key is CR
	}
	machine_paste(copy, true); // typed into the KERNAL keyboard buffer as the program runs
	return NULL;
}

static bool
run_type_text(mcp_job *job)
{
	if (job->polls == 0) {
		const json_node *tn = json_get(job->args, "text");
		const char *text = json_as_string(tn);
		if (!text || !text[0] || tn->string_len != strlen(text) || tn->string_len > 60000) {
			mcp_job_fail(job, "text is required: 1 to 60000 characters, no NUL");
			return true;
		}
		const json_node *w = json_get(job->args, "wait");
		bool wait = w && w->type == JSON_BOOL && w->boolean;
		if (wait && mcp_is_paused() && !pasting_bas) {
			mcp_job_fail(job, "wait=true needs the emulator running (typing only progresses while it runs)");
			return true;
		}
		const char *err = queue_typed(text, tn->string_len);
		if (err) {
			mcp_job_fail(job, err);
			return true;
		}
		if (!wait) {
			json_buf_append(&job->text, "{\"queued\":true}");
			return true;
		}
		return false;
	}
	if (!pasting_bas) {
		json_buf_append(&job->text, "{\"typed\":true}");
		return true;
	}
	return false;
}

static const mcp_tool tool_type_text = {
	"type_text",
	"Types text into the KERNAL keyboard buffer as if typed, e.g. a BASIC line or command; use \"\\n\" for Enter. "
	"Characters are ISO-8859-15, and \"\\\\Xhh\" inserts a raw PETSCII byte. Typing only progresses while the "
	"emulator runs (and interrupts are enabled), so with the emulator paused the text is queued and typed after "
	"continue. With wait=true it returns once everything has been typed (not once the program it started has "
	"finished). The KERNAL buffer is bypassed by programs that read the keyboard directly; use send_key for those.",
	"{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"},\"wait\":{\"type\":\"boolean\",\"default\":false},"
	"\"timeout_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":600000}},\"required\":[\"text\"],\"additionalProperties\":false}",
	run_type_text,
	0,
	NULL
};

// ---- get_vera_state ----

static void
vera_fixed(json_buf *b, const char *name, uint32_t raw)
{
	char tmp[96];
	snprintf(tmp, sizeof(tmp), "\"%s\":%.6f,\"%s_raw\":\"0x%08X\",", name, (double)(int32_t)raw / 65536.0, name, raw);
	json_buf_append(b, tmp);
}

static bool
run_get_vera_state(mcp_job *job)
{
	struct video_state v;
	video_get_state(&v);
	json_buf *b = &job->text;
	char tmp[1024]; // the largest snprintf below is about 450 bytes
	#define BOOL(x) ((x) ? "true" : "false")

	snprintf(tmp, sizeof(tmp), "{\"addrsel\":%u,\"dcsel\":%u,\"data_ports\":[", v.addrsel, v.dcsel);
	json_buf_append(b, tmp);
	for (int i = 0; i < 2; i++) {
		snprintf(tmp, sizeof(tmp),
		         "%s{\"port\":%d,\"address\":\"0x%05X\",\"step\":%d,\"increment_field\":%u,\"prefetched_byte\":\"0x%02X\","
		         "\"nibble_bit\":%s,\"nibble_increment\":%s}",
		         i ? "," : "", i, v.data[i].address, v.data[i].step, v.data[i].inc_raw, v.data[i].prefetch,
		         BOOL(v.data[i].nibble_bit), BOOL(v.data[i].nibble_incr));
		json_buf_append(b, tmp);
	}
	snprintf(tmp, sizeof(tmp),
	         "],\"irq\":{\"enable\":\"0x%X\",\"status\":\"0x%02X\",\"line_compare\":%u,\"irq_asserted\":%s},"
	         "\"video\":{\"scanline\":%u,\"ntsc\":%s},",
	         v.ien, v.isr, v.irq_line, BOOL(v.irq_out), v.scanline, BOOL(v.ntsc));
	json_buf_append(b, tmp);

	snprintf(tmp, sizeof(tmp),
	         "\"fx\":{\"addr1_mode\":%u,\"four_bit_mode\":%s,\"sixteen_bit_hop\":%s,\"cache_byte_cycling\":%s,"
	         "\"cache_fill\":%s,\"cache_write\":%s,\"transparent_writes\":%s,\"two_bit_polygon\":%s,\"two_bit_poking\":%s,"
	         "\"affine_clip\":%s,\"multiplier\":%s,\"subtract\":%s,\"cache_increment_mode\":%s,\"cache_nibble_index\":%s,"
	         "\"cache_byte_index\":%u,\"hop_align\":%u,\"affine_map_size\":%u,",
	         v.fx_addr1_mode, BOOL(v.fx_4bit_mode), BOOL(v.fx_16bit_hop), BOOL(v.fx_cache_byte_cycling),
	         BOOL(v.fx_cache_fill), BOOL(v.fx_cache_write), BOOL(v.fx_trans_writes), BOOL(v.fx_2bit_poly),
	         BOOL(v.fx_2bit_poking), BOOL(v.fx_affine_clip), BOOL(v.fx_multiplier), BOOL(v.fx_subtract),
	         BOOL(v.fx_cache_increment_mode), BOOL(v.fx_cache_nibble_index), v.fx_cache_byte_index,
	         v.fx_16bit_hop_align, v.fx_affine_map_size);
	json_buf_append(b, tmp);
	vera_fixed(b, "x_pixel_increment", v.fx_x_pixel_increment);
	vera_fixed(b, "y_pixel_increment", v.fx_y_pixel_increment);
	vera_fixed(b, "x_pixel_position", v.fx_x_pixel_position);
	vera_fixed(b, "y_pixel_position", v.fx_y_pixel_position);
	snprintf(tmp, sizeof(tmp),
	         "\"poly_fill_length\":%u,\"affine_tile_base\":\"0x%05X\",\"affine_map_base\":\"0x%05X\","
	         "\"cache\":\"%02x%02x%02x%02x\",\"multiplier_accumulator\":%d}}",
	         v.fx_poly_fill_length, v.fx_affine_tile_base, v.fx_affine_map_base,
	         v.fx_cache[0], v.fx_cache[1], v.fx_cache[2], v.fx_cache[3], v.fx_mult_accumulator);
	json_buf_append(b, tmp);
	#undef BOOL
	return true;
}

static const mcp_tool tool_get_vera_state = {
	"get_vera_state",
	"Returns VERA's internal state, which the CPU cannot see through the registers at $9F20-$9F3F: both data ports "
	"at once (address, the resulting signed step, increment field, prefetched byte, nibble flags), ADDRSEL and DCSEL, "
	"the IRQ enable and status bits, the line-compare value and whether IRQ is asserted, the current raster line, "
	"and the FX state (mode, flags, the four cache bytes as hex in order, pixel increments and positions, polygon "
	"length, affine bases, multiplier accumulator). FX pixel values are 16.16 fixed point as the emulator stores them: "
	"each comes as a decimal and as _raw hex. Reading has no side effects. Pause first for a stable snapshot; the "
	"raster line is where the beam is at this instant.",
	"{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
	run_get_vera_state,
	0
};

// ---- reset / set_warp ----

extern char *clipboard_buffer; // main.c: text queued for typing, owned by the paste machinery

static bool
run_reset(mcp_job *job)
{
	const json_node *z = json_get(job->args, "zero_ram");
	if (z && z->type != JSON_BOOL) {
		mcp_job_fail(job, "zero_ram must be true or false");
		return true;
	}
	bool zero = z && z->boolean;
	if (pasting_bas || paste_text) { // drop text still waiting to be typed
		pasting_bas = false;
		paste_text = NULL;
		if (clipboard_buffer) {
			SDL_free(clipboard_buffer);
			clipboard_buffer = NULL;
		}
	}
	if (zero) {
		// Match a fresh "-zeroram" start: zeroed RAM, and VRAM filled from the start of the (unseeded) random sequence.
		memset(RAM, 0, RAM_SIZE);
		memset(BRAM, 0, BRAM_SIZE);
		srand(1);
	}
	machine_reset(); // a hard reset, like the SMC's reset line; the frame and cycle counters keep running
	run_get_registers(job);
	return true;
}

static const mcp_tool tool_reset = {
	"reset",
	"Resets the machine as if the reset button were pressed: the CPU restarts at the reset vector, VERA, the VIAs and "
	"the input queues are reset and any text still waiting to be typed is dropped. RAM is NOT cleared by a real "
	"reset, so by default it keeps its contents; with zero_ram=true RAM and banked RAM are zeroed first and VRAM is "
	"refilled as at a fresh `-zeroram` start, which makes repeated runs deterministic. A paused emulator stays "
	"paused (at the reset vector), a running one keeps running. Breakpoints and watchpoints are kept; the frame and "
	"cycle counters keep counting. With zero_ram, VRAM, banked RAM and low RAM come out identical from run to run, "
	"apart from a few stale bytes below the stack pointer. Returns the registers after the reset.",
	"{\"type\":\"object\",\"properties\":{\"zero_ram\":{\"type\":\"boolean\",\"default\":false}},\"additionalProperties\":false}",
	run_reset,
	0
};

static bool
run_set_warp(mcp_job *job)
{
	const json_node *e = json_get(job->args, "enabled");
	if (!e || e->type != JSON_BOOL) {
		mcp_job_fail(job, "enabled is required: true or false");
		return true;
	}
	warp_mode = e->boolean;
	// Forget any speed-limiter debt, so leaving warp mode does not make the emulator race to "catch up".
	// (The window's own warp toggle restarts the frame counter, which would break run_frames; this does not.)
	sdlTicks_base = SDL_GetTicks() - (uint32_t)(cpu_ticks / MHZ / 1000);
	json_buf_append(&job->text, warp_mode ? "{\"warp\":true}" : "{\"warp\":false}");
	return true;
}

static const mcp_tool tool_set_warp = {
	"set_warp",
	"Turns warp mode on or off. In warp mode the emulator is not held to real time, so frames pass as fast as the "
	"host can emulate them: use it to fast-forward (boot, long run_frames) when wall-clock time matters more than "
	"pacing. Emulated behaviour is unchanged; only the speed limiter is bypassed. get_status shows the current setting.",
	"{\"type\":\"object\",\"properties\":{\"enabled\":{\"type\":\"boolean\"}},\"required\":[\"enabled\"],\"additionalProperties\":false}",
	run_set_warp,
	0
};

// ---- load_prg ----

// Where BASIC keeps its start-of-program and program-end pointers in release 49 (the layout is assumed to
// hold for later releases too). Real LOAD sets all three end pointers to the end address; without that, BASIC
// would put variables on top of the loaded program.
#define BASIC_TXTTAB 0x00DF
#define BASIC_VARTAB 0x03E1
#define BASIC_ARYTAB 0x03E3
#define BASIC_STREND 0x03E5
#define BASIC_LAYOUT_RELEASE 49

static bool
run_load_prg(mcp_job *job)
{
	const char *path = json_as_string(json_get(job->args, "path"));
	if (!path || !path[0]) {
		mcp_job_fail(job, "path is required: the PRG file on the host (an absolute path is safest)");
		return true;
	}
	const json_node *rawn = json_get(job->args, "raw");
	const json_node *runn = json_get(job->args, "run");
	if ((rawn && rawn->type != JSON_BOOL) || (runn && runn->type != JSON_BOOL)) {
		mcp_job_fail(job, "raw and run must be true or false");
		return true;
	}
	bool raw = rawn && rawn->boolean, run = runn && runn->boolean;
	uint32_t override = 0, bank = 0;
	bool have_addr = json_get(job->args, "address") != NULL;
	bool have_bank = json_get(job->args, "bank") != NULL;
	if (have_addr && (!arg_uint(job, "address", &override) || override > 0xFFFF)) {
		mcp_job_fail(job, "address must be 0 to 0xFFFF as an integer or a string like \"0x0801\"");
		return true;
	}
	if (raw && !have_addr) {
		mcp_job_fail(job, "raw files have no load address in them: pass address");
		return true;
	}
	if (have_bank && (!arg_uint(job, "bank", &bank) || bank >= num_ram_banks)) {
		mcp_job_fail(job, "bank must be a valid RAM bank number");
		return true;
	}

	char resolved[4200];
	if (!host_path(job, path, resolved, sizeof(resolved))) {
		return true;
	}
	FILE *f = fopen(resolved, "rb");
	if (!f) {
		char msg[600];
		snprintf(msg, sizeof(msg), "cannot open %.400s: %s", resolved, strerror(errno));
		mcp_job_fail(job, msg);
		return true;
	}
	enum { MAX_PRG = 0x100000 };
	uint8_t *file = malloc(MAX_PRG + 1);
	size_t n = file ? fread(file, 1, MAX_PRG + 1, f) : 0;
	bool read_err = ferror(f);
	fclose(f);
	const char *fail = NULL;
	if (!file) fail = "out of memory";
	else if (read_err) fail = "error reading the file (is it a directory?)";
	else if (n > MAX_PRG) fail = "file is larger than 1 MB";
	if (fail) {
		free(file);
		mcp_job_fail(job, fail);
		return true;
	}

	const uint8_t *data = file;
	size_t len = n;
	uint32_t start;
	if (raw) {
		start = override;
	} else {
		if (n < 3) {
			free(file);
			mcp_job_fail(job, "not a PRG: it needs a 2-byte load address followed by at least one byte of data");
			return true;
		}
		start = have_addr ? override : (uint32_t)(file[0] | file[1] << 8);
		data += 2;
		len -= 2;
	}
	if (len == 0) {
		free(file);
		mcp_job_fail(job, "the file has no data");
		return true;
	}
	if (have_bank && start < 0xA000) {
		free(file);
		mcp_job_fail(job, "bank only applies to load addresses from $A000 up");
		return true;
	}

	// Check the whole destination before touching anything. Low RAM ends at $9EFF; banked RAM is
	// $A000-$BFFF and, as with the KERNAL's LOAD, continues in the next bank after $BFFF.
	uint32_t a = start, b = have_bank ? bank : (uint32_t)memory_get_ram_bank();
	for (size_t i = 0; i < len; i++) {
		if (a >= 0xA000 && a < 0xC000) {
			if (b >= num_ram_banks) {
				fail = "the data runs past the last RAM bank";
				break;
			}
		} else if (a >= 0x9F00) {
			fail = a < 0xA000 ? "the data would overwrite the I/O area ($9F00-$9FFF)"
			                  : "the data would run into ROM ($C000 and up)";
			break;
		}
		if (++a == 0xC000 && start >= 0xA000) {
			a = 0xA000;
			b++;
		}
	}
	if (fail) {
		free(file);
		mcp_job_fail(job, fail);
		return true;
	}
	uint32_t end = a & 0xFFFF; // exclusive; after a bank wrap this is the address in the last bank

	// BASIC program? Then BASIC's pointers must move with it, as the LOAD command does.
	bool basic = start == 0x0801 && !have_bank;
	// The KERNAL release is the byte at $FF80: positive = release, $FF = custom build, other negative = prerelease.
	int8_t rel = (int8_t)debug_read6502(0xFF80, 0, 0);
	char rom_release[24];
	bool older = false;
	if (rel > 0) {
		snprintf(rom_release, sizeof(rom_release), "r%d", rel);
		older = rel < BASIC_LAYOUT_RELEASE;
	} else if (rel == -1) {
		snprintf(rom_release, sizeof(rom_release), "custom build");
	} else {
		snprintf(rom_release, sizeof(rom_release), "prerelease %d", -rel);
		older = -rel <= BASIC_LAYOUT_RELEASE; // a prerelease of r49 or earlier
	}
	const char *ptr_status = "n/a";
	bool set_pointers = false;
	if (basic) {
		// BASIC's start-of-program pointer is where we expect it only if the ROM is booted and has this layout
		set_pointers = (RAM[BASIC_TXTTAB] | RAM[BASIC_TXTTAB + 1] << 8) == 0x0801;
		ptr_status = set_pointers ? "set" : "not set (BASIC's start pointer is not at the expected place: ROM not booted yet, or a different layout)";
	}
	if (run && basic && !set_pointers) {
		free(file);
		mcp_job_fail(job, "cannot run a BASIC program: BASIC's pointers could not be located (has the ROM finished "
		                  "booting? is it a different release?), so variables would overwrite the program. Load without "
		                  "run, or wait for the READY. prompt and try again");
		return true;
	}
	char cmd[32] = "";
	if (run) {
		snprintf(cmd, sizeof(cmd), basic ? "RUN\n" : "SYS $%04X\n", start);
		const char *err = queue_typed(cmd, strlen(cmd));
		if (err) {
			free(file);
			mcp_job_fail(job, err);
			return true;
		}
	}

	a = start;
	b = have_bank ? bank : (uint32_t)memory_get_ram_bank();
	for (size_t i = 0; i < len; i++) {
		if (a < 0x9F00) {
			RAM[a] = data[i];
		} else {
			BRAM[((size_t)b << 13) + a - 0xA000] = data[i];
		}
		if (++a == 0xC000 && start >= 0xA000) {
			a = 0xA000;
			b++;
		}
	}
	free(file);
	if (set_pointers) {
		uint16_t ptrs[3] = { BASIC_VARTAB, BASIC_ARYTAB, BASIC_STREND };
		for (int i = 0; i < 3; i++) {
			RAM[ptrs[i]] = (uint8_t)end;
			RAM[ptrs[i] + 1] = (uint8_t)(end >> 8);
		}
	}

	char tmp[480];
	snprintf(tmp, sizeof(tmp),
	         "{\"start\":\"0x%04X\",\"end\":\"0x%04X\",\"bytes\":%zu,\"basic\":%s,\"basic_pointers\":\"%s\","
	         "\"rom_release\":\"%s\",\"started\":%s,\"start_command\":\"",
	         start, end, len, basic ? "true" : "false", ptr_status, rom_release, run ? "true" : "false");
	json_buf_append(&job->text, tmp);
	if (basic) {
		json_buf_append(&job->text, "RUN");
	} else {
		snprintf(tmp, sizeof(tmp), "SYS $%04X", start);
		json_buf_append(&job->text, tmp);
	}
	json_buf_append(&job->text, "\"");
	if (basic && older) {
		snprintf(tmp, sizeof(tmp),
		         ",\"warning\":\"This ROM (%s) is older than r%d. BASIC's pointers were written at the r%d addresses "
		         "($%04X, $%04X, $%04X), which may be different in this release; if the program misbehaves, load it with the "
		         "LOAD command instead.\"",
		         rom_release, BASIC_LAYOUT_RELEASE, BASIC_LAYOUT_RELEASE, BASIC_VARTAB, BASIC_ARYTAB, BASIC_STREND);
		json_buf_append(&job->text, tmp);
	}
	json_buf_append(&job->text, "}");
	return true;
}

static const mcp_tool tool_load_prg = {
	"load_prg",
	"Loads a PRG file from the host into emulated memory, instantly and without emulated time (nothing else is "
	"disturbed, so runs stay deterministic). The first two bytes of the file are the load address, as with the "
	"KERNAL's LOAD; `address` overrides it, and `raw` treats the whole file as data (then `address` is required). "
	"`bank` selects the RAM bank for loads at $A000 and up (default: the current one); like the KERNAL, data that "
	"runs past $BFFF continues in the next bank. A load at $0801 is taken to be a BASIC program, and BASIC's "
	"program-end pointers are set as the LOAD command would, using the release 49 layout (the result says whether "
	"they were set, names the ROM release, and warns if the ROM is detectably older than r49). With run=true the emulator is told to start it by typing RUN (BASIC) or SYS <address> (machine "
	"code), which needs the emulator running at the BASIC prompt. The path must lie inside the folder shown as "
	"host_root by get_status; relative paths are relative to it. Do not load while another program is running "
	"unless you mean to overwrite it. Returns the start, the end (exclusive) and the size.",
	"{\"type\":\"object\",\"properties\":{"
		"\"path\":{\"type\":\"string\",\"description\":\"host file path\"},"
		"\"address\":{\"type\":[\"integer\",\"string\"],\"description\":\"load address, overriding the file's\"},"
		"\"raw\":{\"type\":\"boolean\",\"default\":false,\"description\":\"file has no 2-byte load address\"},"
		"\"bank\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255},"
		"\"run\":{\"type\":\"boolean\",\"default\":false}},"
	 "\"required\":[\"path\"],\"additionalProperties\":false}",
	run_load_prg,
	0,
	NULL
};

// ---- mouse ----
// The X16 mouse is a PS/2 mouse: it reports relative movement, and the KERNAL
// keeps the pointer position (when a program enables it, e.g. BASIC "MOUSE 1").

static int32_t pk_lo(uint64_t v) { return (int32_t)(uint32_t)v; }
static int32_t pk_hi(uint64_t v) { return (int32_t)(uint32_t)(v >> 32); }
static uint64_t pk(int32_t a, int32_t b) { return (uint64_t)(uint32_t)a | (uint64_t)(uint32_t)b << 32; }

static int32_t
clamp255(int32_t v)
{
	return v > 255 ? 255 : v < -255 ? -255 : v;
}

// Reads a signed integer argument; returns false if absent or not an integer in [lo, hi].
static bool
arg_int(const mcp_job *job, const char *name, int32_t lo, int32_t hi, int32_t *out)
{
	const json_node *n = json_get(job->args, name);
	if (!n || n->type != JSON_NUMBER || n->number != (double)(int32_t)n->number
	    || n->number < lo || n->number > hi) {
		return false;
	}
	*out = (int32_t)n->number;
	return true;
}

// Jobs run one at a time, so the mouse tools can share this: the frame at which they last made progress.
static uint32_t mouse_progress;
#define MOUSE_STALL_FRAMES 90
static bool
mouse_stalled(void)
{
	return (int32_t)(frames - mouse_progress) > MOUSE_STALL_FRAMES;
}
static const char mouse_stall_msg[] =
	"the program is not consuming mouse data (is the mouse enabled? in BASIC: MOUSE 1); part of it may have been delivered";
// Waits (only while running) until the program has read everything queued, so that on return the
// KERNAL's pointer position and button state are up to date. Returns true when it is time to answer.
static bool
mouse_drained(void)
{
	return mcp_is_paused() || i2c_mse_buffer_count() == 0 || mouse_stalled();
}

static bool
run_mouse_move(mcp_job *job)
{
	if (job->polls == 0) {
		mouse_progress = frames;
		int32_t dx = 0, dy = 0, x = 0, y = 0;
		bool have_d = json_get(job->args, "dx") || json_get(job->args, "dy");
		bool have_abs = json_get(job->args, "x") || json_get(job->args, "y");
		if (have_d == have_abs) {
			mcp_job_fail(job, "pass either dx and/or dy (relative), or x and y (absolute), not both and not neither");
			return true;
		}
		if (have_d) {
			if ((json_get(job->args, "dx") && !arg_int(job, "dx", -4096, 4096, &dx))
			    || (json_get(job->args, "dy") && !arg_int(job, "dy", -4096, 4096, &dy))) {
				mcp_job_fail(job, "dx and dy must be integers from -4096 to 4096");
				return true;
			}
			job->scratch[0] = pk(dx, dy);
			job->scratch[1] = 0;
		} else {
			if (!arg_int(job, "x", 0, 1023, &x) || !arg_int(job, "y", 0, 1023, &y)) {
				mcp_job_fail(job, "absolute moves need both x and y, integers from 0 to 1023");
				return true;
			}
			// Drive the pointer into the top-left corner (the KERNAL clamps it there), then out to x,y.
			job->scratch[0] = pk(-1024, -1024);
			job->scratch[1] = pk(x, y);
		}
	}
	int32_t dx = pk_lo(job->scratch[0]), dy = pk_hi(job->scratch[0]);
	for (;;) {
		if (dx == 0 && dy == 0) {
			if (job->scratch[1] == 0) {
				if (!mouse_drained()) {
					job->scratch[0] = 0;
					return false;
				}
				json_buf_append(&job->text, i2c_mse_buffer_count() ? "{\"moved\":true,\"consumed\":false}" : "{\"moved\":true,\"consumed\":true}");
				return true;
			}
			dx = pk_lo(job->scratch[1]);
			dy = pk_hi(job->scratch[1]);
			job->scratch[1] = 0;
			continue;
		}
		if (!mouse_has_room()) {
			if (mcp_is_paused()) {
				mcp_job_fail(job, "the mouse buffer is full and the emulator is paused, so the program cannot consume "
				                  "it; continue (or run_frames) and repeat. Part of the move was already delivered");
				return true;
			}
			if (mouse_stalled()) {
				mcp_job_fail(job, mouse_stall_msg);
				return true;
			}
			job->scratch[0] = pk(dx, dy);
			return false;
		}
		mouse_progress = frames;
		int32_t cx = clamp255(dx), cy = clamp255(dy);
		mouse_move(cx, cy);
		mouse_send_state();
		dx -= cx;
		dy -= cy;
	}
}

static const mcp_tool tool_mouse_move = {
	"mouse_move",
	"Moves the PS/2 mouse. The X16 mouse reports only relative movement and the KERNAL tracks the pointer, so the "
	"program must have enabled it (in BASIC: MOUSE 1; then MX, MY and MB read it back). Pass dx and/or dy (screen "
	"pixels in mouse units, +x right, +y down) for a relative move, or x and y (0-1023) to go to an absolute "
	"position. An absolute move first drives the pointer into the top-left corner and then out to x,y, which "
	"assumes the KERNAL mouse is enabled with its default scale (1:1) and clamps at the screen edge. Movement is "
	"delivered a packet at a time as the program consumes it, so a long move needs the emulator running; with it "
	"paused only about three packets (roughly 700 units) can be queued.",
	"{\"type\":\"object\",\"properties\":{"
		"\"dx\":{\"type\":\"integer\",\"minimum\":-4096,\"maximum\":4096},"
		"\"dy\":{\"type\":\"integer\",\"minimum\":-4096,\"maximum\":4096},"
		"\"x\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":1023},"
		"\"y\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":1023},"
		"\"timeout_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":600000}},\"additionalProperties\":false}",
	run_mouse_move,
	0,
	NULL
};

// scratch[0] = release deadline frame, scratch[1] = button | phase << 8 | hold_only << 16
// phases: 0 press, 1 hold, 2 release
static void
cancel_mouse_button(mcp_job *job)
{
	mouse_button_up((int)(job->scratch[1] & 0xFF)); // never leave a button stuck down
	if (mouse_has_room()) {
		mouse_send_state();
	}
}

static bool
run_mouse_button(mcp_job *job)
{
	if (job->polls == 0) {
		const char *b = json_as_string(json_get(job->args, "button"));
		int button = !b || !strcmp(b, "left") ? 0 : !strcmp(b, "right") ? 1 : !strcmp(b, "middle") ? 2 : -1;
		if (button < 0) {
			mcp_job_fail(job, "button must be \"left\", \"right\" or \"middle\"");
			return true;
		}
		const json_node *an = json_get(job->args, "action");
		const char *action = an ? json_as_string(an) : "tap";
		int act = !action ? -1 : !strcmp(action, "tap") ? 0 : !strcmp(action, "down") ? 1 : !strcmp(action, "up") ? 2 : -1;
		if (act < 0) {
			mcp_job_fail(job, "action must be \"tap\", \"down\" or \"up\"");
			return true;
		}
		uint32_t hold = mcp_is_paused() ? 0 : 2; // a program polling once per frame needs the button to span a frame
		if (json_get(job->args, "hold_frames") && (!arg_uint(job, "hold_frames", &hold) || hold > 100000)) {
			mcp_job_fail(job, "hold_frames must be an integer from 0 to 100000");
			return true;
		}
		if (act == 0 && hold > 0 && mcp_is_paused()) {
			mcp_job_fail(job, "hold_frames needs the emulator running (time does not pass while paused)");
			return true;
		}
		// the phase to start in, and whether to stop after pressing
		unsigned phase = act == 2 ? 2 : 0;
		job->scratch[1] = (uint64_t)button | (uint64_t)phase << 8 | (uint64_t)(act == 1) << 16;
		job->scratch[0] = hold;
		mouse_progress = frames;
	}
	int button = (int)(job->scratch[1] & 0xFF);
	unsigned phase = (unsigned)(job->scratch[1] >> 8) & 0xFF;
	bool press_only = (job->scratch[1] >> 16) & 1;
	for (;;) {
		if (phase == 3) { // everything is queued; wait for the program to read it
			if (!mouse_drained()) {
				break;
			}
			json_buf_append(&job->text, i2c_mse_buffer_count() ? "{\"queued\":true,\"consumed\":false}" : "{\"queued\":true,\"consumed\":true}");
			return true;
		}
		if (phase == 1) {
			if ((int32_t)(frames - (uint32_t)job->scratch[0]) < 0) {
				break;
			}
			phase = 2;
		}
		if (!mouse_has_room()) {
			if (mcp_is_paused()) {
				if (phase == 2) {
					mouse_button_up(button);
				}
				mcp_job_fail(job, "the mouse buffer is full and the emulator is paused; continue and repeat");
				return true;
			}
			if (mouse_stalled()) {
				if (phase == 2) {
					mouse_button_up(button);
				}
				mcp_job_fail(job, mouse_stall_msg);
				return true;
			}
			break;
		}
		if (phase == 0) {
			mouse_button_down(button);
			mouse_send_state();
			mouse_progress = frames;
			if (press_only) {
				phase = 3;
				continue;
			}
			uint32_t hold = (uint32_t)job->scratch[0];
			job->scratch[0] = (uint32_t)(frames + hold);
			phase = hold ? 1 : 2;
		} else {
			mouse_button_up(button);
			mouse_send_state();
			mouse_progress = frames;
			phase = 3;
		}
	}
	job->scratch[1] = (uint64_t)button | (uint64_t)phase << 8 | (uint64_t)press_only << 16;
	return false;
}

static const mcp_tool tool_mouse_button = {
	"mouse_button",
	"Presses and/or releases a mouse button (left, right, middle). action \"tap\" (default) presses then releases "
	"after hold_frames video frames (default 2 while running so a program that polls once per frame sees it; 0 while "
	"paused, where the press and release are just queued); \"down\" and \"up\" hold and release a button, for "
	"dragging. Position the pointer first with mouse_move. The program must have enabled the mouse.",
	"{\"type\":\"object\",\"properties\":{"
		"\"button\":{\"type\":\"string\",\"enum\":[\"left\",\"right\",\"middle\"],\"default\":\"left\"},"
		"\"action\":{\"type\":\"string\",\"enum\":[\"tap\",\"down\",\"up\"],\"default\":\"tap\"},"
		"\"hold_frames\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100000},"
		"\"timeout_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":600000}},\"additionalProperties\":false}",
	run_mouse_button,
	0,
	cancel_mouse_button
};

static bool
run_mouse_wheel(mcp_job *job)
{
	if (job->polls == 0) {
		int32_t n;
		if (!arg_int(job, "notches", -1000, 1000, &n) || n == 0) {
			mcp_job_fail(job, "notches is required: a non-zero integer from -1000 to 1000 (positive = up, away from the user)");
			return true;
		}
		uint8_t id = mouse_get_device_id();
		if (id != 3 && id != 4) {
			mcp_job_fail(job, "the mouse has no wheel: the program or KERNAL has not enabled wheel mode (PS/2 IntelliMouse)");
			return true;
		}
		job->scratch[0] = pk(n, 0);
		mouse_progress = frames;
	}
	int32_t n = pk_lo(job->scratch[0]);
	while (n != 0) {
		if (!mouse_has_room()) {
			if (mcp_is_paused()) {
				mcp_job_fail(job, "the mouse buffer is full and the emulator is paused; continue and repeat");
				return true;
			}
			if (mouse_stalled()) {
				mcp_job_fail(job, mouse_stall_msg);
				return true;
			}
			job->scratch[0] = pk(n, 0);
			return false;
		}
		mouse_progress = frames;
		int32_t c = n > 7 ? 7 : n < -7 ? -7 : n; // one packet carries -8..7
		mouse_set_wheel((int8_t)c);
		mouse_send_state();
		n -= c;
	}
	if (!mouse_drained()) {
		job->scratch[0] = 0;
		return false;
	}
	json_buf_append(&job->text, i2c_mse_buffer_count() ? "{\"scrolled\":true,\"consumed\":false}" : "{\"scrolled\":true,\"consumed\":true}");
	return true;
}

static const mcp_tool tool_mouse_wheel = {
	"mouse_wheel",
	"Scrolls the mouse wheel by `notches` (positive = up, away from the user). Only works if the program or KERNAL "
	"has enabled wheel mode; otherwise it returns an error. Long scrolls are delivered as the program consumes them.",
	"{\"type\":\"object\",\"properties\":{\"notches\":{\"type\":\"integer\",\"minimum\":-1000,\"maximum\":1000},"
	"\"timeout_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":600000}},\"required\":[\"notches\"],\"additionalProperties\":false}",
	run_mouse_wheel,
	0,
	NULL
};

// ---- debug_stall (only registered when X16EMU_MCP_TEST is set) ----
// Takes `ms` milliseconds to complete while the emulator keeps running; used to
// test that polled jobs do not block the emulator and that timeouts work.

static bool
run_debug_stall(mcp_job *job)
{
	const json_node *ms = json_get(job->args, "ms");
	if (!ms || ms->type != JSON_NUMBER || ms->number < 0 || ms->number > 600000) {
		mcp_job_fail(job, "ms must be a number between 0 and 600000");
		return true;
	}
	if (job->polls == 0) {
		job->scratch[0] = SDL_GetTicks();
	}
	uint32_t elapsed = SDL_GetTicks() - (uint32_t)job->scratch[0];
	if (elapsed < (uint64_t)ms->number) {
		return false;
	}
	char tmp[96];
	snprintf(tmp, sizeof(tmp), "stalled %.0f ms, polled %u times", ms->number, job->polls);
	json_buf_append(&job->text, tmp);
	return true;
}

static const mcp_tool tool_debug_stall = {
	"debug_stall",
	"Test only: completes after `ms` milliseconds without pausing the emulator.",
	"{\"type\":\"object\",\"properties\":{\"ms\":{\"type\":\"number\"}},\"required\":[\"ms\"],\"additionalProperties\":false}",
	run_debug_stall,
	0,
	NULL,
	true // measures how often it is polled
};

const mcp_tool *const *
mcp_tool_table(void)
{
	static const mcp_tool *table[44];
	if (!table[0]) {
		size_t n = 0;
		table[n++] = &tool_get_registers;
		table[n++] = &tool_get_status;
		table[n++] = &tool_read_memory;
		table[n++] = &tool_hash_memory;
		table[n++] = &tool_get_screen;
		table[n++] = &tool_get_vera_state;
		table[n++] = &tool_pause;
		table[n++] = &tool_continue;
		table[n++] = &tool_run_frames;
		table[n++] = &tool_set_breakpoint;
		table[n++] = &tool_clear_breakpoint;
		table[n++] = &tool_list_breakpoints;
		table[n++] = &tool_step;
		table[n++] = &tool_set_watchpoint;
		table[n++] = &tool_clear_watchpoint;
		table[n++] = &tool_list_watchpoints;
		table[n++] = &tool_write_memory;
		table[n++] = &tool_set_registers;
		table[n++] = &tool_reset;
		table[n++] = &tool_set_warp;
		table[n++] = &tool_send_key;
		table[n++] = &tool_type_text;
		table[n++] = &tool_load_prg;
		table[n++] = &tool_mouse_move;
		table[n++] = &tool_mouse_button;
		table[n++] = &tool_mouse_wheel;
		if (getenv("X16EMU_MCP_TEST")) {
			table[n++] = &tool_debug_stall;
		}
		table[n] = NULL;
	}
	return table;
}

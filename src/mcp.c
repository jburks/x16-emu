// Commander X16 Emulator
// MCP (Model Context Protocol) server: JSON-RPC 2.0 over stdin/stdout.
// All rights reserved. License: 2-clause BSD
//
// Transport: stdio, one JSON message per line (newline delimited, no embedded
// newlines).
//
// Threading: a reader thread parses requests and answers the protocol-level
// ones itself. A tools/call becomes a job that is queued for the EMULATOR
// thread; emulator_loop calls mcp_service() every pass, which runs the job
// there. Tools therefore touch emulator state without any locking. The reader
// thread waits for the job with a timeout, so a stuck or paused emulator can
// never hang the client.
//
// Timeout semantics: if the job has not started when the timeout expires it is
// dropped and never runs. A job that has started but is not finished (a polled
// job such as run_frames) is cancelled via its tool's cancel hook and dropped.
// A job that completes in a single pass cannot time out once it has started.

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L // fdopen
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#define dup _dup
#define dup2 _dup2
#define fdopen _fdopen
#else
#include <unistd.h>
#endif
#include "mcp.h"
#include "mcp_internal.h"
#include "mcp_json.h"
#include "version.h"

#define MAX_LINE (16 * 1024 * 1024)
#define DEFAULT_TIMEOUT_MS 30000

// JSON-RPC error codes
#define ERR_PARSE            (-32700)
#define ERR_INVALID_REQUEST  (-32600)
#define ERR_METHOD_NOT_FOUND (-32601)
#define ERR_INVALID_PARAMS   (-32602)

bool mcp_enabled = false;

static FILE *mcp_out;
static SDL_mutex *out_lock;
static bool log_traffic;

// Job queue. pending_* and the done/abandoned flags of jobs are protected by
// queue_lock. The active list is only touched by the emulator thread.
static SDL_mutex *queue_lock;
static SDL_cond *queue_cond;
static mcp_job *pending_head, *pending_tail;
static mcp_job *active_head, *active_tail;
// Bit 0: pending or active jobs may exist. Bit 1: a new job is waiting to be taken over. Set by the reader
// thread and cleared by the emulator thread, both under queue_lock; the emulator thread also reads it unlocked.
#define WORK_JOBS 1
#define WORK_NEW  2
static SDL_atomic_t work_flag;
extern uint32_t frames;               // timing.c
static uint32_t last_poll_frame;      // frame counter at the last service_jobs() (emulator thread only)
static bool poll_every_pass;          // some active job asked for per-pass polling (emulator thread only)
static bool paused;            // emulator thread only

// Newest first; the client's version is echoed if we know it.
static const char *const supported_versions[] = {
	"2025-11-25",
	"2025-06-18",
	"2025-03-26",
	"2024-11-05",
};
#define NUM_VERSIONS (sizeof(supported_versions) / sizeof(supported_versions[0]))

void
mcp_enable(void)
{
	mcp_enabled = true;
	fflush(stdout);
	int out = dup(1);
	if (out < 0) {
		fprintf(stderr, "mcp: cannot duplicate stdout\n");
		exit(1);
	}
	// Anything else that prints to stdout now goes to stderr.
	if (dup2(2, 1) < 0) {
		fprintf(stderr, "mcp: cannot redirect stdout\n");
		exit(1);
	}
#ifdef _WIN32
	_setmode(out, _O_BINARY);
#endif
	mcp_out = fdopen(out, "wb");
	if (!mcp_out) {
		fprintf(stderr, "mcp: cannot open protocol stream\n");
		exit(1);
	}
	log_traffic = getenv("X16EMU_MCP_LOG") != NULL;
}

// ---- output ----

// Sends one complete message (without trailing newline).
static void
send_message(const json_buf *msg)
{
	if (log_traffic) {
		fprintf(stderr, "mcp: -> %s\n", msg->data);
	}
	SDL_LockMutex(out_lock);
	fwrite(msg->data, 1, msg->len, mcp_out);
	fputc('\n', mcp_out);
	fflush(mcp_out);
	SDL_UnlockMutex(out_lock);
}

// Starts {"jsonrpc":"2.0","id":<id>, ; id_json is already-rendered JSON.
static void
begin_reply(json_buf *b, const char *id_json)
{
	json_buf_append(b, "{\"jsonrpc\":\"2.0\",\"id\":");
	json_buf_append(b, id_json);
	json_buf_append(b, ",");
}

static void
send_error(const char *id_json, int code, const char *message)
{
	json_buf b;
	json_buf_init(&b);
	begin_reply(&b, id_json);
	char tmp[48];
	snprintf(tmp, sizeof(tmp), "\"error\":{\"code\":%d,\"message\":", code);
	json_buf_append(&b, tmp);
	json_buf_append_string(&b, message, strlen(message));
	json_buf_append(&b, "}}");
	send_message(&b);
	json_buf_free(&b);
}

// result_json must be a complete JSON value.
static void
send_result(const char *id_json, const char *result_json)
{
	json_buf b;
	json_buf_init(&b);
	begin_reply(&b, id_json);
	json_buf_append(&b, "\"result\":");
	json_buf_append(&b, result_json);
	json_buf_append(&b, "}");
	send_message(&b);
	json_buf_free(&b);
}

// A tools/call result: an optional text item, then an optional image item.
static void
send_tool_result(const char *id_json, const char *text, size_t len, bool is_error,
                 const uint8_t *image, size_t image_len, const char *image_mime)
{
	json_buf r;
	json_buf_init(&r);
	json_buf_append(&r, "{\"content\":[");
	bool any = false;
	if (len > 0 || !image) {
		json_buf_append(&r, "{\"type\":\"text\",\"text\":");
		json_buf_append_string(&r, text, len);
		json_buf_append(&r, "}");
		any = true;
	}
	if (image) {
		json_buf_append(&r, any ? ",{\"type\":\"image\",\"data\":\"" : "{\"type\":\"image\",\"data\":\"");
		json_buf_append_base64(&r, image, image_len);
		json_buf_append(&r, "\",\"mimeType\":");
		json_buf_append_string(&r, image_mime, strlen(image_mime));
		json_buf_append(&r, "}");
	}
	json_buf_append(&r, is_error ? "],\"isError\":true}" : "],\"isError\":false}");
	send_result(id_json, r.data);
	json_buf_free(&r);
}

// ---- job queue ----

static void
job_free(mcp_job *job)
{
	json_buf_free(&job->text);
	free(job->image);
	json_free(job->msg);
	free(job);
}

// Emulator thread: runs queued jobs. Called at the top of every emulator loop
// pass, so the fast path must stay a single atomic read.
static void service_jobs(void);

void
mcp_service(void)
{
	int work = SDL_AtomicGet(&work_flag);
	if (work) {
		// Unfinished jobs only need a look when something can have changed for them: a new job arrived, a
		// video frame completed, or the emulator is paused (no frames pass then). Polling every instruction
		// would roughly halve the speed of a long run_frames.
		if ((work & WORK_NEW) || poll_every_pass || paused || frames != last_poll_frame) {
			service_jobs();
		}
	}
	if (mcp_debug_armed && !paused) {
		mcp_debug_check();
	}
}

static void
service_jobs(void)
{

	// Take over the pending jobs.
	SDL_LockMutex(queue_lock);
	mcp_job *fresh = pending_head;
	pending_head = pending_tail = NULL;
	SDL_UnlockMutex(queue_lock);

	while (fresh) {
		mcp_job *job = fresh;
		fresh = job->next;
		job->next = NULL;
		SDL_LockMutex(queue_lock);
		bool dropped = job->abandoned; // timed out before it started: never run
		SDL_UnlockMutex(queue_lock);
		if (dropped) {
			job_free(job);
			continue;
		}
		if (active_tail) active_tail->next = job; else active_head = job;
		active_tail = job;
	}

	// Run every active job once; finished ones leave the list.
	mcp_job *prev = NULL;
	mcp_job *job = active_head;
	while (job) {
		mcp_job *next = job->next;
		SDL_LockMutex(queue_lock);
		bool gave_up = job->abandoned;
		SDL_UnlockMutex(queue_lock);
		if (gave_up) {
			// The reader timed out. Leave the emulator in a sane state and drop the job.
			if (job->tool->cancel) {
				job->tool->cancel(job);
			}
			if (prev) prev->next = next; else active_head = next;
			if (active_tail == job) active_tail = prev;
			job_free(job);
			job = next;
			continue;
		}
		bool finished = job->tool->run(job);
		job->polls++;
		if (!finished) {
			prev = job;
			job = next;
			continue;
		}
		if (prev) prev->next = next; else active_head = next;
		if (active_tail == job) active_tail = prev;
		job->next = NULL;

		SDL_LockMutex(queue_lock);
		if (job->abandoned) {
			SDL_UnlockMutex(queue_lock);
			job_free(job);
		} else {
			job->done = true;
			SDL_CondBroadcast(queue_cond);
			SDL_UnlockMutex(queue_lock);
			// the reader thread owns the job from here on
		}
		job = next;
	}

	last_poll_frame = frames;
	poll_every_pass = false;
	for (mcp_job *j = active_head; j; j = j->next) {
		if (j->tool->poll_every_pass) {
			poll_every_pass = true;
		}
	}
	SDL_LockMutex(queue_lock);
	if (!pending_head && !active_head) {
		SDL_AtomicSet(&work_flag, 0);
	} else if (!pending_head) {
		SDL_AtomicSet(&work_flag, WORK_JOBS);
	}
	SDL_UnlockMutex(queue_lock);
}

// ---- pause ----

extern uint32_t sdlTicks_base; // timing.c: wall-clock origin of the speed limiter

static uint32_t pause_started;

bool
mcp_is_paused(void)
{
	return paused;
}

void
mcp_set_paused(bool p)
{
	if (p == paused) {
		return;
	}
	if (p) {
		pause_started = SDL_GetTicks();
	} else {
		// The speed limiter compares emulated time with wall-clock time since
		// start. Move the origin forward by the time we were paused, or the
		// emulator would run flat out until it had "caught up".
		sdlTicks_base += SDL_GetTicks() - pause_started;
	}
	paused = p;
}

bool
mcp_idle_while_paused(void)
{
	SDL_Delay(2);
	SDL_PumpEvents();
	SDL_Event ev;
	return SDL_PeepEvents(&ev, 1, SDL_GETEVENT, SDL_QUIT, SDL_QUIT) == 0;
}

static unsigned
default_timeout_ms(void)
{
	const char *e = getenv("X16EMU_MCP_TIMEOUT_MS");
	if (e && atoi(e) > 0) {
		return (unsigned)atoi(e);
	}
	return DEFAULT_TIMEOUT_MS;
}

// Reader thread: queues the job, waits for it, sends the reply. Takes
// ownership of msg (the job frees it).
static void
run_tool(const char *id_json, const mcp_tool *tool, json_node *msg)
{
	mcp_job *job = calloc(1, sizeof(mcp_job));
	if (!job) {
		json_free(msg);
		send_error(id_json, ERR_INVALID_REQUEST, "out of memory");
		return;
	}
	job->tool = tool;
	job->msg = msg;
	job->args = json_get(json_get(msg, "params"), "arguments");
	json_buf_init(&job->text);

	unsigned timeout = tool->timeout_ms ? tool->timeout_ms : default_timeout_ms();
	const json_node *to = json_get(job->args, "timeout_ms");
	if (to && to->type == JSON_NUMBER && to->number >= 1 && to->number <= 600000) {
		timeout = (unsigned)to->number;
	}
	// SDL_GetTicks64 needs SDL 2.0.18; older distributions (Raspberry Pi OS, Ubuntu 20.04) ship less.
	// The 32-bit counter wraps after 49 days, so measure elapsed time by subtraction.
	Uint32 started = SDL_GetTicks();

	SDL_LockMutex(queue_lock);
	if (pending_tail) pending_tail->next = job; else pending_head = job;
	pending_tail = job;
	SDL_AtomicSet(&work_flag, WORK_JOBS | WORK_NEW);
	while (!job->done) {
		Uint32 elapsed = SDL_GetTicks() - started;
		if (elapsed >= timeout) {
			break;
		}
		SDL_CondWaitTimeout(queue_cond, queue_lock, timeout - elapsed);
	}
	if (!job->done) {
		job->abandoned = true; // the emulator thread frees it from here on
		SDL_UnlockMutex(queue_lock);
		char text[256];
		snprintf(text, sizeof(text), "tool %.80s timed out after %u ms (pass a larger timeout_ms if it legitimately needs longer)",
		         tool->name, timeout);
		send_tool_result(id_json, text, strlen(text), true, NULL, 0, NULL);
		return;
	}
	SDL_UnlockMutex(queue_lock);

	send_tool_result(id_json, job->text.data ? job->text.data : "", job->text.len, job->is_error,
	                 job->image, job->image_len, job->image_mime);
	job_free(job);
}

// ---- protocol ----

static void
handle_initialize(const char *id_json, const json_node *params)
{
	const char *wanted = json_as_string(json_get(params, "protocolVersion"));
	const char *version = supported_versions[0];
	for (size_t i = 0; wanted && i < NUM_VERSIONS; i++) {
		if (!strcmp(wanted, supported_versions[i])) {
			version = supported_versions[i];
			break;
		}
	}
	json_buf b;
	json_buf_init(&b);
	json_buf_append(&b, "{\"protocolVersion\":");
	json_buf_append_string(&b, version, strlen(version));
	json_buf_append(&b, ",\"capabilities\":{\"tools\":{\"listChanged\":false}},"
	                    "\"serverInfo\":{\"name\":\"x16emu\",\"version\":");
	json_buf_append_string(&b, VER, strlen(VER));
	json_buf_append(&b, "},\"instructions\":");
	static const char instructions[] =
		"Controls a running Commander X16 emulator. Pause before inspecting memory or the screen; "
		"run_frames(n) advances exactly n video frames and leaves the emulator paused (deterministic), "
		"continue resumes at normal speed. Breakpoints and watchpoints pause the emulator; use "
		"continue{wait:true} to block until one hits. step needs the emulator paused. Tools that wait "
		"accept timeout_ms and leave the emulator paused if it expires. Addresses may be integers or "
		"strings such as \"$9F20\". The emulator boots to BASIC in about 3-5 s: wait for READY. "
		"(text layer at VRAM $1B000, 256 bytes per row, character at every other byte) before typing. "
		"Start with -zeroram for bit-identical runs. Full documentation: MCP.md in the emulator repository.";
	json_buf_append_string(&b, instructions, sizeof(instructions) - 1);
	json_buf_append(&b, "}");
	send_result(id_json, b.data);
	json_buf_free(&b);
}

static void
handle_tools_list(const char *id_json)
{
	json_buf b;
	json_buf_init(&b);
	json_buf_append(&b, "{\"tools\":[");
	for (const mcp_tool *const *t = mcp_tool_table(); *t; t++) {
		if (t != mcp_tool_table()) {
			json_buf_append(&b, ",");
		}
		json_buf_append(&b, "{\"name\":");
		json_buf_append_string(&b, (*t)->name, strlen((*t)->name));
		json_buf_append(&b, ",\"description\":");
		json_buf_append_string(&b, (*t)->description, strlen((*t)->description));
		json_buf_append(&b, ",\"inputSchema\":");
		json_buf_append(&b, (*t)->input_schema);
		json_buf_append(&b, "}");
	}
	json_buf_append(&b, "]}");
	send_result(id_json, b.data);
	json_buf_free(&b);
}

// Returns true if it took ownership of msg.
static bool
handle_tools_call(const char *id_json, json_node *msg)
{
	const char *name = json_as_string(json_get(json_get(msg, "params"), "name"));
	if (!name) {
		send_error(id_json, ERR_INVALID_PARAMS, "missing tool name");
		return false;
	}
	for (const mcp_tool *const *t = mcp_tool_table(); *t; t++) {
		if (!strcmp((*t)->name, name)) {
			run_tool(id_json, *t, msg);
			return true;
		}
	}
	char text[256];
	snprintf(text, sizeof(text), "unknown tool: %.200s", name);
	send_error(id_json, ERR_INVALID_PARAMS, text);
	return false;
}

static void
handle_line(const char *line)
{
	if (log_traffic) {
		fprintf(stderr, "mcp: <- %s\n", line);
	}
	const char *perr = NULL;
	json_node *msg = json_parse(line, &perr);
	if (!msg) {
		send_error("null", ERR_PARSE, perr);
		return;
	}
	if (msg->type != JSON_OBJECT) {
		// includes JSON-RPC batches, which MCP does not use
		send_error("null", ERR_INVALID_REQUEST, "request must be a JSON object");
		json_free(msg);
		return;
	}

	const json_node *id = json_get(msg, "id");
	const char *method = json_as_string(json_get(msg, "method"));
	const json_node *params = json_get(msg, "params");

	if (!method) {
		// A response to something we sent (we send no requests yet) or garbage.
		if (!json_get(msg, "result") && !json_get(msg, "error")) {
			send_error("null", ERR_INVALID_REQUEST, "missing method");
		}
		json_free(msg);
		return;
	}

	if (!id) {
		// Notification: never answered, even if it is unknown.
		if (strcmp(method, "notifications/initialized") &&
		    strcmp(method, "notifications/cancelled")) {
			fprintf(stderr, "mcp: ignoring notification %.100s\n", method);
		}
		json_free(msg);
		return;
	}
	if (id->type != JSON_STRING && id->type != JSON_NUMBER) {
		send_error("null", ERR_INVALID_REQUEST, "id must be a string or number");
		json_free(msg);
		return;
	}

	// Render the id now: a tool job takes the message with it and may free it
	// at any time after a timeout.
	json_buf idb;
	json_buf_init(&idb);
	json_buf_append_scalar(&idb, id);
	const char *id_json = idb.data;

	bool owned = false;
	if (!strcmp(method, "initialize")) {
		handle_initialize(id_json, params);
	} else if (!strcmp(method, "ping")) {
		send_result(id_json, "{}");
	} else if (!strcmp(method, "tools/list")) {
		handle_tools_list(id_json);
	} else if (!strcmp(method, "tools/call")) {
		owned = handle_tools_call(id_json, msg);
	} else {
		char text[256];
		snprintf(text, sizeof(text), "method not found: %.200s", method);
		send_error(id_json, ERR_METHOD_NOT_FOUND, text);
	}
	json_buf_free(&idb);
	if (!owned) {
		json_free(msg);
	}
}

static int
reader_thread(void *unused)
{
	(void)unused;
	size_t cap = 4096;
	size_t len = 0;
	char *line = malloc(cap);
	if (!line) {
		fprintf(stderr, "mcp: out of memory\n");
		exit(1);
	}
	bool overflow = false;
	for (;;) {
		if (!fgets(line + len, (int)(cap - len), stdin)) {
			break; // EOF or read error: the client went away
		}
		len += strlen(line + len);
		if (len == 0 || line[len - 1] != '\n') {
			// Partial line: grow the buffer and keep reading
			if (len + 1 >= cap) {
				if (cap >= MAX_LINE) {
					// Discard the rest of an oversized message
					overflow = true;
					len = 0;
				} else {
					cap *= 2;
					line = realloc(line, cap);
					if (!line) {
						fprintf(stderr, "mcp: out of memory\n");
						exit(1);
					}
				}
			}
			continue;
		}
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
			line[--len] = 0;
		}
		if (overflow) {
			overflow = false;
			send_error("null", ERR_INVALID_REQUEST, "message too large");
		} else if (len > 0) {
			handle_line(line);
		}
		len = 0;
	}
	free(line);
	fprintf(stderr, "mcp: stdin closed, exiting\n");
	SDL_Event quit;
	memset(&quit, 0, sizeof(quit));
	quit.type = SDL_QUIT;
	SDL_PushEvent(&quit);
	return 0;
}

void
mcp_start(void)
{
	out_lock = SDL_CreateMutex();
	queue_lock = SDL_CreateMutex();
	queue_cond = SDL_CreateCond();
	SDL_Thread *t = SDL_CreateThread(reader_thread, "mcp", NULL);
	if (!out_lock || !queue_lock || !queue_cond || !t) {
		fprintf(stderr, "mcp: cannot start server thread: %s\n", SDL_GetError());
		exit(1);
	}
	SDL_DetachThread(t);
}

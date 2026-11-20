// Personal folders written short in what players share (log_privacy.c): the game log, stderr, the bug capture's files
// and the crash report. C:\Users\<name>\AppData\Local\NX2Saria\game becomes %LOCALAPPDATA%\NX2Saria\game,
// /home/<name>/Games becomes ~/Games, the account's name alone inside a path becomes %USERNAME% or $USER, and the
// computer's name %COMPUTERNAME%. On by
// default; log_short_paths = 0 writes them in full. Paths handed to other programs (the tracker API's save_dir, the
// files the game opens) are never changed: only the text written for people to read.
#pragma once
#include <stddef.h>

void log_privacy_init(void);    // main.c main_run, first: the folders from the environment; no allocation
void log_privacy_start(void);   // after the settings are read (mem_init): the switch, the folders' resolved forms, a line
int log_privacy_on(void);

// Where the first personal folder starts in p (n bytes), or n when it holds none or the switch is off. Counts the texts
// that hold one (log_privacy_count).
size_t log_privacy_find(const char *p, size_t n);
// p (n bytes) from *at on into out (cap >= 64 bytes) with each folder short; the bytes written. *at moves past the bytes
// read: a caller loops until *at == n (log_privacy_core.h priv_rewrite).
size_t log_privacy_rewrite(const char *p, size_t n, size_t *at, char *out, size_t cap);
// A whole string short into out (cap bytes), for one value in a file the game writes for people (the bug capture);
// returns out, or s itself when it has nothing to shorten or the switch is off. Cut at cap - 1 bytes.
const char *log_privacy_str(const char *s, char *out, size_t cap);
// The same with the switch off too, for text that leaves the PC (session_summary.c: the session summary the launcher
// may send): log_short_paths = 0 is for one's own logs, never for what goes to a server.
const char *log_privacy_str_always(const char *s, char *out, size_t cap);
// A malloc'd copy of p (n bytes) with each folder short, *out_n its length; NULL when it has nothing to shorten, the
// switch is off or memory runs out (the caller keeps p). Not for the crash report: it allocates.
char *log_privacy_dup(const char *p, size_t n, size_t *out_n);
// The lines and values written short so far.
unsigned long long log_privacy_count(void);

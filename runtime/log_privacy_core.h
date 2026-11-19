// Personal folders written short (log_privacy.c): the part that makes no system call, so that
// tests/log_privacy_check.c builds it on any system with Windows and Linux tables alike.
//
// A table holds the folders to write short (prefixes, such as C:\Users\<name>\AppData\Local) and the account's name.
// priv_find says whether a text holds any of them; priv_rewrite copies the text with each one replaced by its marker
// (%LOCALAPPDATA%, %USERPROFILE%, ~, %USERNAME%). The rest of each path is kept: it tells where a file was.
//
// Matching, in one pass over the bytes (priv_first marks the bytes a pattern can start with, so most bytes cost one
// table read):
// - a prefix matches where the byte before it is not part of a name (a letter, digit, '_', '.', '-' or a byte of a
//   UTF-8 sequence) and the byte after it is not either: /home/ana does not match in /home/anabel nor in /var/home/ana,
//   and \\?\C:\Users\ana\x matches. The longest prefix that matches at a place wins (%LOCALAPPDATA% before
//   %USERPROFILE%).
// - the account's name, three bytes or longer, matches as a word of its own (no letter, digit, '_' or UTF-8 byte on
//   either side) inside a path: the run of non-blank bytes around it holds a '/' or '\'. "Steam Deck" stays as it is for
//   the account deck; /tmp/run-deck/x and D:\deck\saves become /tmp/run-%USERNAME%/x and D:\%USERNAME%\saves.
// - a table with fold = 1 (Windows) compares letters without case (ASCII), takes '/' and '\' as the same byte, and a
//   run of separators as one (C:\\Users\\ana, a path as an escaped string writes it).
// - a '.' or '-' right after a folder ends it when no name byte follows (the end of a sentence: "saved in /home/ana.").
// - the computer's name, three bytes or longer, matches as a word of its own anywhere (no path needed: a UNC path
//   \\<host>\share, a network line), without case on every system (host names have none). maria-pc.local becomes
//   %COMPUTERNAME%.local; names a fresh install gives every machine (localhost, steamdeck, ...; priv_common_host) are
//   not looked for.
// - names too common to be the account's alone (user, admin, deck, ...; priv_common) are not looked for alone: a path
//   under their home still matches as a folder, and /run/user/1000 stays as it is.
//
// No allocation and no lock: the crash report writes its lines through here.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define PRIV_MAX 24           // patterns in a table
#define PRIV_PAT_MAX 520      // bytes of one pattern (a Windows path of MAX_PATH UTF-16 units is up to 3x that; longer
                              // folders are skipped, see priv_add)
#define PRIV_MARK_MAX 16      // bytes of one marker
#define PRIV_NAME_MIN 3       // an account name shorter than this is not looked for alone

enum { PRIV_PREFIX = 0, PRIV_NAME = 1, PRIV_HOST = 2 };

typedef struct {
    char pat[PRIV_PAT_MAX];   // folded when the table folds (lower case, '/' for '\')
    char mark[PRIV_MARK_MAX];
    uint16_t len, mlen;
    uint8_t kind;
} PrivPat;

typedef struct {
    PrivPat p[PRIV_MAX];
    int n;
    int fold;                 // 1: Windows paths (no case, either separator)
    unsigned char first[256]; // 1 where a pattern can start with that byte (both cases when folding)
} PrivTable;

static inline unsigned char priv_fold(const PrivTable *t, unsigned char c) {
    if (!t->fold) return c;
    if (c == '\\') return '/';
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + 32) : c;
}

static inline int priv_name_byte(unsigned char c) {   // a byte that continues a word
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c >= 0x80;
}

static inline int priv_path_byte(unsigned char c) {   // a byte that continues a folder's name
    return priv_name_byte(c) || c == '.' || c == '-';
}

static inline int priv_sep(unsigned char c) { return c == '/' || c == '\\'; }

static inline int priv_blank(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0; }

// Account names that are words of their own in paths that are not the account's (/run/user, "User Data").
static inline int priv_common(const char *s, size_t n) {
    static const char *const names[] = {"user", "users", "admin", "administrator", "root", "guest", "deck", "steamuser",
                                        "owner", "default", "public", "home", "local", "data", "game", "games"};
    for (size_t k = 0; k < sizeof names / sizeof *names; k++) {
        size_t m = strlen(names[k]), i = 0;
        if (m != n) continue;
        while (i < n && (s[i] | 0x20) == names[k][i]) i++;
        if (i == n) return 1;
    }
    return 0;
}

// Computer names a system gives every machine of its kind: not the person's.
static inline int priv_common_host(const char *s, size_t n) {
    static const char *const names[] = {"localhost", "steamdeck", "ubuntu", "debian", "fedora", "archlinux", "linux",
                                        "desktop", "laptop", "computer", "workgroup", "mint", "pop-os", "manjaro",
                                        "nobara", "bazzite", "cachyos", "endeavouros", "opensuse", "localdomain"};
    for (size_t k = 0; k < sizeof names / sizeof *names; k++) {
        size_t m = strlen(names[k]), i = 0;
        if (m != n) continue;
        while (i < n && (s[i] | 0x20) == names[k][i]) i++;
        if (i == n) return 1;
    }
    return 0;
}

static inline unsigned char priv_lower(unsigned char c) { return c >= 'A' && c <= 'Z' ? (unsigned char)(c + 32) : c; }

static inline void priv_clear(PrivTable *t, int fold) {
    memset(t, 0, sizeof *t);
    t->fold = fold;
}

// Adds a pattern. A prefix loses its trailing separators; one that is empty, a root ("/", "C:\", "\\") or too long is
// not added, nor a name shorter than PRIV_NAME_MIN, nor a pattern the table holds already. Kept longest first, so the
// longest match at a place is found first. 1 when added.
static inline int priv_add(PrivTable *t, const char *s, int kind, const char *mark) {
    if (!s || !mark || t->n >= PRIV_MAX) return 0;
    size_t n = strlen(s), m = strlen(mark);
    if (kind == PRIV_PREFIX)
        while (n && priv_sep((unsigned char)s[n - 1])) n--;
    if (!n || n >= PRIV_PAT_MAX || !m || m >= PRIV_MARK_MAX) return 0;
    if (kind == PRIV_NAME || kind == PRIV_HOST) {
        if (n < PRIV_NAME_MIN || (kind == PRIV_NAME ? priv_common(s, n) : priv_common_host(s, n))) return 0;
        for (size_t i = 0; i < n; i++)
            if (priv_sep((unsigned char)s[i])) return 0;
    } else {
        // a root, or a drive's root ("C:"), would write every path short
        size_t seps = 0;
        for (size_t i = 0; i < n; i++) seps += priv_sep((unsigned char)s[i]);
        if (!seps || s[n - 1] == ':') return 0;
    }
    PrivPat q;
    memset(&q, 0, sizeof q);
    for (size_t i = 0; i < n; i++)
        q.pat[i] = (char)(kind == PRIV_HOST ? priv_lower((unsigned char)s[i]) : priv_fold(t, (unsigned char)s[i]));
    memcpy(q.mark, mark, m);
    q.len = (uint16_t)n, q.mlen = (uint16_t)m, q.kind = (uint8_t)kind;
    for (int i = 0; i < t->n; i++)
        if (t->p[i].kind == q.kind && t->p[i].len == q.len && !memcmp(t->p[i].pat, q.pat, n)) return 0;
    int at = t->n;
    while (at > 0 && t->p[at - 1].len < q.len) at--;
    memmove(&t->p[at + 1], &t->p[at], (size_t)(t->n - at) * sizeof q);
    t->p[at] = q;
    t->n++;
    unsigned char c = (unsigned char)q.pat[0];
    t->first[c] = 1;
    if (t->fold || kind == PRIV_HOST) {
        if (c >= 'a' && c <= 'z') t->first[c - 32] = 1;
        if (c == '/') t->first['\\'] = 1;
    }
    return 1;
}

// The byte after a folder ends it: none, a byte that is not part of a name, or a '.' or '-' with none after it.
static inline int priv_folder_end(const unsigned char *u, size_t n, size_t e) {
    if (e >= n || !priv_path_byte(u[e])) return 1;
    return (u[e] == '.' || u[e] == '-') && (e + 1 >= n || !priv_path_byte(u[e + 1]));
}

// The pattern that matches at s[i] (s holds n bytes), or NULL; *len gets the bytes of s it covers (more than the
// pattern's when a folded table took a run of separators for one).
static inline const PrivPat *priv_match(const PrivTable *t, const char *s, size_t n, size_t i, size_t *len) {
    const unsigned char *u = (const unsigned char *)s;
    for (int k = 0; k < t->n; k++) {
        const PrivPat *q = &t->p[k];
        if (q->len > n - i) continue;
        size_t j = 0, e = i;
        while (j < q->len && e < n &&
               (q->kind == PRIV_HOST ? priv_lower(u[e]) : priv_fold(t, u[e])) == (unsigned char)q->pat[j]) {
            if (t->fold && q->pat[j] == '/' && j > 0)   // C:\\Users: the run of separators is one
                while (e + 1 < n && priv_sep(u[e + 1])) e++;
            j++, e++;
        }
        if (j < q->len) continue;
        if (q->kind == PRIV_PREFIX) {
            if (i && priv_path_byte(u[i - 1])) continue;
            if (!priv_folder_end(u, n, e)) continue;
            *len = e - i;
            return q;
        }
        if ((i && priv_name_byte(u[i - 1])) || (e < n && priv_name_byte(u[e]))) continue;
        if (q->kind == PRIV_HOST) {
            *len = e - i;
            return q;
        }
        int path = 0;   // a separator in the non-blank run around the name
        for (size_t a = i; a > 0 && !priv_blank(u[a - 1]) && !path; a--) path = priv_sep(u[a - 1]);
        for (size_t b = e; b < n && !priv_blank(u[b]) && !path; b++) path = priv_sep(u[b]);
        if (path) {
            *len = e - i;
            return q;
        }
    }
    return NULL;
}

// Where the first pattern starts in s, or n when s holds none.
static inline size_t priv_find(const PrivTable *t, const char *s, size_t n) {
    if (!t->n) return n;
    const unsigned char *u = (const unsigned char *)s;
    for (size_t i = 0; i < n; i++)
        if (t->first[u[i]] && priv_match(t, s, n, i, &(size_t){0})) return i;
    return n;
}

// Copies s (n bytes) from *at on into out (cap bytes) with each pattern replaced by its marker, and returns the bytes
// written; *at moves past the bytes read. It stops before a piece that would not fit, so a caller with more to copy
// writes out and calls again; with cap >= PRIV_MARK_MAX every call reads at least one byte. The whole text is passed
// each time so that the byte before *at still counts for the match there. *hits, when given, is increased by the
// patterns replaced.
static inline size_t priv_rewrite(const PrivTable *t, const char *s, size_t n, size_t *at, char *out, size_t cap,
                                  unsigned *hits) {
    size_t i = *at, o = 0;
    while (i < n) {
        size_t len = 0;
        const PrivPat *q = t->first[(unsigned char)s[i]] ? priv_match(t, s, n, i, &len) : NULL;
        if (q) {
            if (o + q->mlen > cap) break;
            memcpy(out + o, q->mark, q->mlen);
            o += q->mlen, i += len;
            if (hits) ++*hits;
        } else {
            if (o + 1 > cap) break;
            out[o++] = s[i++];
        }
    }
    *at = i;
    return o;
}

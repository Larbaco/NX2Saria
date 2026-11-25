#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define PRIV_MAX 24
#define PRIV_PAT_MAX 520
#define PRIV_MARK_MAX 16
#define PRIV_NAME_MIN 3

enum { PRIV_PREFIX = 0, PRIV_NAME = 1, PRIV_HOST = 2 };

typedef struct {
    char pat[PRIV_PAT_MAX];
    char mark[PRIV_MARK_MAX];
    uint16_t len, mlen;
    uint8_t kind;
} PrivPat;

typedef struct {
    PrivPat p[PRIV_MAX];
    int n;
    int fold;
    unsigned char first[256];
} PrivTable;

static inline unsigned char priv_fold(const PrivTable *t, unsigned char c) {
    if (!t->fold) return c;
    if (c == '\\') return '/';
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + 32) : c;
}

static inline int priv_name_byte(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c >= 0x80;
}

static inline int priv_path_byte(unsigned char c) {
    return priv_name_byte(c) || c == '.' || c == '-';
}

static inline int priv_sep(unsigned char c) { return c == '/' || c == '\\'; }

static inline int priv_blank(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0; }

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

static inline int priv_folder_end(const unsigned char *u, size_t n, size_t e) {
    if (e >= n || !priv_path_byte(u[e])) return 1;
    return (u[e] == '.' || u[e] == '-') && (e + 1 >= n || !priv_path_byte(u[e + 1]));
}

static inline const PrivPat *priv_match(const PrivTable *t, const char *s, size_t n, size_t i, size_t *len) {
    const unsigned char *u = (const unsigned char *)s;
    for (int k = 0; k < t->n; k++) {
        const PrivPat *q = &t->p[k];
        if (q->len > n - i) continue;
        size_t j = 0, e = i;
        while (j < q->len && e < n &&
               (q->kind == PRIV_HOST ? priv_lower(u[e]) : priv_fold(t, u[e])) == (unsigned char)q->pat[j]) {
            if (t->fold && q->pat[j] == '/' && j > 0)
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
        int path = 0;
        for (size_t a = i; a > 0 && !priv_blank(u[a - 1]) && !path; a--) path = priv_sep(u[a - 1]);
        for (size_t b = e; b < n && !priv_blank(u[b]) && !path; b++) path = priv_sep(u[b]);
        if (path) {
            *len = e - i;
            return q;
        }
    }
    return NULL;
}

static inline size_t priv_find(const PrivTable *t, const char *s, size_t n) {
    if (!t->n) return n;
    const unsigned char *u = (const unsigned char *)s;
    for (size_t i = 0; i < n; i++)
        if (t->first[u[i]] && priv_match(t, s, n, i, &(size_t){0})) return i;
    return n;
}

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

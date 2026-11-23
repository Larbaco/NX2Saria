#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "debug_tools.h"
#include "fs.h"
#include "host.h"
#include "layout.h"
#include "mods.h"
#include "platform/platform.h"

#define TRACKER_API_VERSION 1
#define TITLE_ID "0400486024DEC000"
#define PORT_NAME "nx2saria"
#define HEAP_POINTER_FLOOR 0x1000000000ull
#define LINE_CAP 4096
#define LINE_MS 5000
#define READ_MAX (256u << 10)
#define PATTERN_MAX 64
#define HITS_MAX 64
#define HITS_DEFAULT 16
#define BUDGET_MAX_MS 3000
#define BUDGET_DEFAULT_MS 1000
#define PIECE (1u << 20)
#define PAGE 0x1000ull
#define SEARCH_BYTES_PER_MS (4u << 20)
#ifndef SEARCH_BYTES_PER_MIN
#define SEARCH_BYTES_PER_MIN (8ull << 30)
#endif
#define CLIENTS_MAX 4
#define IDLE_MS 60000
#define SEND_MS 2000
#define RETRY_LISTEN_MS 5000
#define ACCEPT_ERRORS_MAX 50
#ifndef CLIENT_LOGS
#define CLIENT_LOGS 20
#endif
#define SUMMARY_MS 600000
#define SUMMARY_LOGS 36

enum { M_STATUS, M_READ, M_SEARCH, M_STATE, M_GAMEDATA, M_OTHER, M_N };
static const char *const method_name[M_N] = {"status", "read_memory", "search_memory", "nx2saria.state",
                                             "nx2saria.game_data", "other"};
enum { E_PARAMS, E_UNREADABLE, E_METHOD, E_NOT_YET, E_UNSUPPORTED, E_KINDS };

static int port;
static char build_id[65];
static char methods_text[160];
static PlatMutex search_lock = PLAT_MUTEX_INIT;
static uint64_t search_tokens = SEARCH_BYTES_PER_MIN, search_tokens_ms;
static int32_t open_clients, client_seq, client_logs;
static uint64_t win_req[M_N], win_errors, win_bytes, win_checks;
static int32_t said_first[M_N], said_refusal[M_N][E_KINDS], said_hit, said_check, said_bytes, said_accept;

static int once(int32_t *flag) { return __atomic_exchange_n(flag, 1, __ATOMIC_ACQ_REL) == 0; }

static int client_log_slot(void) {
    int n = __atomic_add_fetch(&client_logs, 1, __ATOMIC_ACQ_REL);
    if (n == CLIENT_LOGS + 1)
        rt_log("tracker api: %d client lines logged; later clients are counted in the summaries only", CLIENT_LOGS);
    return n <= CLIENT_LOGS;
}

static void shown(char *out, size_t cap, const char *p, size_t n) {
    size_t k = 0;
    for (size_t i = 0; i < n && k + 1 < cap; i++)
        out[k++] = (unsigned char)p[i] >= 0x20 && (unsigned char)p[i] < 0x7f ? p[i] : '.';
    out[k] = 0;
}

typedef struct { char key[24]; int is_str, has_num, cut; char str[160]; double num; } Field;
typedef struct {
    char id[64];
    char method[40];
    int method_cut;
    Field params[12];
    int nparams;
} Req;
typedef struct { const char *p, *e; } Js;

static void ws(Js *j) {
    while (j->p < j->e && (*j->p == ' ' || *j->p == '\t' || *j->p == '\r' || *j->p == '\n')) j->p++;
}

static int is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

static int str(Js *j, char *out, size_t cap, int *cut) {
    size_t n = 0;
    if (cut) *cut = 0;
    if (j->p >= j->e || *j->p != '"') return 0;
    for (j->p++; j->p < j->e; j->p++) {
        char c = *j->p;
        if (c == '"') {
            j->p++;
            if (cap) out[n] = 0;
            return 1;
        }
        if ((unsigned char)c < 0x20) return 0;
        if (c == '\\') {
            if (++j->p >= j->e) return 0;
            c = *j->p;
            if (c == 'u') {
                for (int k = 0; k < 4; k++)
                    if (++j->p >= j->e || !is_hex(*j->p)) return 0;
                c = '?';
            } else {
                static const char from[] = "\"\\/bfnrt", to[] = "\"\\/\b\f\n\r\t";
                const char *at = c ? memchr(from, c, sizeof from - 1) : NULL;
                if (!at) return 0;
                c = to[at - from];
            }
        }
        if (cap && n + 1 < cap) out[n++] = c;
        else if (cut) *cut = 1;
    }
    return 0;
}

static int number(Js *j, double *v) {
    const char *s = j->p, *p = s, *e = j->e;
    if (p < e && *p == '-') p++;
    if (p >= e || *p < '0' || *p > '9') return 0;
    if (*p == '0') p++;
    else while (p < e && *p >= '0' && *p <= '9') p++;
    if (p < e && *p == '.') {
        if (++p >= e || *p < '0' || *p > '9') return 0;
        while (p < e && *p >= '0' && *p <= '9') p++;
    }
    if (p < e && (*p == 'e' || *p == 'E')) {
        if (++p < e && (*p == '+' || *p == '-')) p++;
        if (p >= e || *p < '0' || *p > '9') return 0;
        while (p < e && *p >= '0' && *p <= '9') p++;
    }
    char buf[48];
    if ((size_t)(p - s) >= sizeof buf) return 0;
    memcpy(buf, s, (size_t)(p - s));
    buf[p - s] = 0;
    *v = strtod(buf, NULL);
    j->p = p;
    return 1;
}

static int skip_value(Js *j, int depth);

static int skip_container(Js *j, int depth, char close) {
    if (depth > 8) return 0;
    int object = close == '}';
    j->p++;
    ws(j);
    if (j->p < j->e && *j->p == close) return j->p++, 1;
    for (;;) {
        ws(j);
        if (object) {
            if (!str(j, NULL, 0, NULL)) return 0;
            ws(j);
            if (j->p >= j->e || *j->p != ':') return 0;
            j->p++;
            ws(j);
        }
        if (!skip_value(j, depth + 1)) return 0;
        ws(j);
        if (j->p < j->e && *j->p == ',') {
            j->p++;
            continue;
        }
        if (j->p < j->e && *j->p == close) return j->p++, 1;
        return 0;
    }
}

static int skip_value(Js *j, int depth) {
    double v;
    if (j->p >= j->e) return 0;
    char c = *j->p;
    if (c == '"') return str(j, NULL, 0, NULL);
    if (c == '{') return skip_container(j, depth, '}');
    if (c == '[') return skip_container(j, depth, ']');
    static const char *const words[] = {"true", "false", "null"};
    for (int w = 0; w < 3; w++) {
        size_t n = strlen(words[w]);
        if ((size_t)(j->e - j->p) >= n && !memcmp(j->p, words[w], n)) return j->p += n, 1;
    }
    return number(j, &v);
}

static int parse_params(Js *j, Req *r) {
    j->p++;
    ws(j);
    if (j->p < j->e && *j->p == '}') return j->p++, 1;
    for (;;) {
        Field f = {0};
        int key_cut;
        ws(j);
        if (!str(j, f.key, sizeof f.key, &key_cut)) return 0;
        ws(j);
        if (j->p >= j->e || *j->p != ':') return 0;
        j->p++;
        ws(j);
        if (j->p < j->e && *j->p == '"') {
            if (!str(j, f.str, sizeof f.str, &f.cut)) return 0;
            f.is_str = 1;
        } else if (j->p < j->e && (*j->p == '-' || (*j->p >= '0' && *j->p <= '9'))) {
            if (!number(j, &f.num)) return 0;
            f.has_num = 1;
        } else if (!skip_value(j, 1)) {
            return 0;
        }
        if (!key_cut && r->nparams < (int)(sizeof r->params / sizeof *r->params)) r->params[r->nparams++] = f;
        ws(j);
        if (j->p < j->e && *j->p == ',') {
            j->p++;
            continue;
        }
        if (j->p < j->e && *j->p == '}') return j->p++, 1;
        return 0;
    }
}

static int parse_request(const char *line, size_t n, Req *r) {
    Js j = {line, line + n};
    memset(r, 0, sizeof *r);
    ws(&j);
    if (j.p >= j.e || *j.p != '{') return 0;
    j.p++;
    ws(&j);
    if (j.p < j.e && *j.p == '}') {
        j.p++;
    } else {
        for (;;) {
            char key[24];
            int cut;
            ws(&j);
            if (!str(&j, key, sizeof key, &cut)) return 0;
            ws(&j);
            if (j.p >= j.e || *j.p != ':') return 0;
            j.p++;
            ws(&j);
            if (cut) {
                if (!skip_value(&j, 1)) return 0;
            } else if (!strcmp(key, "id") && j.p < j.e && *j.p == '"') {
                const char *s = j.p;
                if (!str(&j, NULL, 0, NULL)) return 0;
                size_t len = (size_t)(j.p - s);
                if (len >= sizeof r->id) return 0;
                memcpy(r->id, s, len);
                r->id[len] = 0;
            } else if (!strcmp(key, "id") && j.p < j.e && *j.p != 'n') {
                double v;
                if (!number(&j, &v) || !isfinite(v)) return 0;
                snprintf(r->id, sizeof r->id, "%.17g", v);
            } else if (!strcmp(key, "method") && j.p < j.e && *j.p == '"') {
                if (!str(&j, r->method, sizeof r->method, &r->method_cut)) return 0;
                for (char *c = r->method; *c; c++)
                    if ((unsigned char)*c < 0x20 || (unsigned char)*c >= 0x7f) *c = '.';
            } else if (!strcmp(key, "params") && j.p < j.e && *j.p == '{') {
                if (!parse_params(&j, r)) return 0;
            } else if (!skip_value(&j, 1)) {
                return 0;
            }
            ws(&j);
            if (j.p < j.e && *j.p == ',') {
                j.p++;
                continue;
            }
            if (j.p < j.e && *j.p == '}') {
                j.p++;
                break;
            }
            return 0;
        }
    }
    ws(&j);
    return j.p == j.e;
}

static const Field *param(const Req *r, const char *key) {
    for (int i = 0; i < r->nparams; i++)
        if (!strcmp(r->params[i].key, key)) return &r->params[i];
    return NULL;
}

static int param_u64(const Req *r, const char *key, uint64_t *out) {
    const Field *f = param(r, key);
    if (!f) return 0;
    if (f->has_num) {
        if (!(f->num >= 0) || f->num >= 18446744073709551616.0 || floor(f->num) != f->num) return 0;
        *out = (uint64_t)f->num;
        return 1;
    }
    if (!f->is_str || f->cut) return 0;
    const char *s = f->str;
    int hex = s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
    if (hex) s += 2;
    size_t n = strlen(s);
    if (!n || n > (hex ? 16u : 19u)) return 0;
    for (size_t i = 0; i < n; i++)
        if (hex ? !is_hex(s[i]) : s[i] < '0' || s[i] > '9') return 0;
    *out = strtoull(s, NULL, hex ? 16 : 10);
    return 1;
}

static const char *param_str(const Req *r, const char *key) {
    const Field *f = param(r, key);
    return f && f->is_str && !f->cut ? f->str : NULL;
}

typedef struct { char *p; size_t n, cap; int bad, refused; } Out;

static void out_put(Out *o, const char *s, size_t n) {
    if (o->bad) return;
    if (o->n + n + 1 > o->cap) {
        size_t cap = o->cap ? o->cap : 256;
        while (o->n + n + 1 > cap) cap *= 2;
        char *p = realloc(o->p, cap);
        if (!p) {
            o->bad = 1;
            return;
        }
        o->p = p, o->cap = cap;
    }
    memcpy(o->p + o->n, s, n);
    o->n += n;
    o->p[o->n] = 0;
}

static void out_f(Out *o, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) out_put(o, buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
}

static void out_text(Out *o, const char *s) {
    out_put(o, "\"", 1);
    for (; *s; s++)
        if ((unsigned char)*s >= 0x20 && (unsigned char)*s < 0x7f && *s != '"' && *s != '\\') out_put(o, s, 1);
    out_put(o, "\"", 1);
}

static void out_json_string(Out *o, const char *s) {
    out_put(o, "\"", 1);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') out_put(o, "\\", 1), out_put(o, s, 1);
        else if (c < 0x20) out_f(o, "\\u%04x", c);
        else out_put(o, s, 1);
    }
    out_put(o, "\"", 1);
}

static void open_answer(Out *o, const Req *r) {
    out_put(o, "{", 1);
    if (r->id[0]) out_f(o, "\"id\":%s,", r->id);
}

static const int error_code[E_KINDS] = {-32602, -32001, -32601, -32002, -32003};

static void error_answer(Out *o, const Req *r, int kind, int m, const char *fmt, ...) {
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    open_answer(o, r);
    out_f(o, "\"error\":{\"code\":%d,\"message\":", error_code[kind]);
    out_text(o, msg);
    out_put(o, "}}\n", 3);
    o->refused = 1;
    __atomic_add_fetch(&win_errors, 1, __ATOMIC_RELAXED);
    if (once(&said_refusal[m][kind]))
        rt_log("tracker api: %s refused: %s (the first such answer; the closing lines count the others)",
               r->method[0] ? r->method : "a request", msg);
}

static int game_running(void) { return frame_step_presents() > 0; }

static const char *game_version(void) {
    switch (rt_game_version) {
    case 110: return "1.1.0";
    case 111: return "1.1.1";
    case 121: return "1.2.1";
    case 143: return "1.4.3";
    default: return "unknown";
    }
}

static void do_status(Out *o, const Req *r) {
    int on = game_running();
    open_answer(o, r);
    out_f(o, "\"result\":{\"powered_on\":%s,\"program_id\":\"" TITLE_ID "\",\"emulator\":\"" PORT_NAME "\","
             "\"build_id\":\"%s\",\"game_version\":\"%s\",\"port_version\":",
          on ? "true" : "false", build_id, game_version());
    out_text(o, rt_port_version);
    out_f(o, ",\"api_version\":%d,\"read_only\":true,\"methods\":[", TRACKER_API_VERSION);
    for (int m = 0; m < M_OTHER; m++)
        if (m != M_GAMEDATA || RT_V111) out_f(o, "%s\"%s\"", m ? "," : "", method_name[m]);
    const char *saves = fs_save_dir();
    out_put(o, "],\"save_dir\":", 13);
    if (*saves) out_json_string(o, saves);
    else out_put(o, "null", 4);
    out_put(o, "}}\n", 3);
    if (once(&said_first[M_STATUS]))
        rt_log("tracker api: first status answered: powered_on %d, game %s, build %.16s, saves in %s", on, game_version(),
               build_id, *saves ? saves : "(not made yet)");
}

static void hex_bytes(Out *o, const uint8_t *b, size_t n) {
    static const char digits[] = "0123456789abcdef";
    char buf[1024];
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        buf[k++] = digits[b[i] >> 4], buf[k++] = digits[b[i] & 15];
        if (k == sizeof buf) out_put(o, buf, k), k = 0;
    }
    out_put(o, buf, k);
}

static int guest_regions(const char **name, RtRange *range, int max) {
    const char *all_names[24];
    RtRange all[24];
    int n = mem_guest_ranges(all_names, all, 24), k = 0;
    for (int i = 0; i < n && k < max; i++)
        if (strcmp(all_names[i], "hostfn")) name[k] = all_names[i], range[k++] = all[i];
    return k;
}

static int guest_copy(uint64_t a, void *to, size_t n) {
    const char *name[24];
    RtRange g[24];
    int ng = guest_regions(name, g, 24), inside = 0;
    for (int i = 0; i < ng && !inside; i++) inside = a >= g[i].base && n <= g[i].size && a - g[i].base <= g[i].size - n;
    if (!n || !inside || !mem_readable(a, n)) return 0;
    mem_kernel_access(a, n);
    return plat_mem_read(a, to, n);
}

static void do_read(Out *o, const Req *r) {
    uint64_t a = 0, n = 0;
    const char *as = param_str(r, "as");
    if (!param_u64(r, "address", &a) || !param_u64(r, "size", &n) || !n || n > READ_MAX) {
        error_answer(o, r, E_PARAMS, M_READ, "read_memory takes an address and a size of 1 to %u bytes", READ_MAX);
        return;
    }
    if (param(r, "as") && (!as || strcmp(as, "hex"))) {
        error_answer(o, r, E_PARAMS, M_READ, "read_memory answers as hex only");
        return;
    }
    uint8_t *b = malloc((size_t)n);
    if (!b) {
        error_answer(o, r, E_NOT_YET, M_READ, "read_memory has no memory for %llu bytes now", (unsigned long long)n);
        return;
    }
    if (!guest_copy(a, b, (size_t)n)) {
        free(b);
        error_answer(o, r, E_UNREADABLE, M_READ, "%llu bytes at %#llx are not memory the game can read now",
                     (unsigned long long)n, (unsigned long long)a);
        return;
    }
    open_answer(o, r);
    out_put(o, "\"result\":{\"hex\":\"", 17);
    hex_bytes(o, b, (size_t)n);
    out_put(o, "\"}}\n", 4);
    free(b);
    if (once(&said_first[M_READ]))
        rt_log("tracker api: first read_memory answered: %llu bytes at %#llx", (unsigned long long)n,
               (unsigned long long)a);
}

typedef struct {
    const uint8_t *pat;
    size_t len;
    uint8_t *buf;
    uint64_t lo, hi;
    uint64_t hits[HITS_MAX];
    int nhits, max_hits;
    int resident_only;
    uint64_t start_ms, deadline_ms, scanned;
    int out_of_time, out_of_bytes;
} Search;

static void scan(Search *s, const uint8_t *p, size_t n, uint64_t at) {
    const uint8_t *e = p + n;
    for (const uint8_t *q = p; q + s->len <= e && (q = memchr(q, s->pat[0], (size_t)(e - q) - s->len + 1)); q++) {
        if (memcmp(q, s->pat, s->len)) continue;
        s->hits[s->nhits++] = at + (uint64_t)(q - p);
        if (s->nhits == s->max_hits) return;
    }
}

static int take_bytes(uint64_t n) {
    uint64_t now = plat_now_ms();
    if (now > search_tokens_ms) {
        uint64_t add = (now - search_tokens_ms) * (SEARCH_BYTES_PER_MIN / 60000);
        search_tokens = add > SEARCH_BYTES_PER_MIN - search_tokens ? SEARCH_BYTES_PER_MIN : search_tokens + add;
        search_tokens_ms = now;
    }
    if (search_tokens < n) return 0;
    search_tokens -= n;
    return 1;
}

static void copy_scan(Search *s, uint64_t a, uint64_t n) {
    if (!take_bytes(n)) {
        s->out_of_bytes = 1;
        return;
    }
    s->scanned += n;
    if (plat_mem_read(a, s->buf, (size_t)n)) {
        scan(s, s->buf, (size_t)n, a);
        return;
    }
    for (uint64_t pg = a; pg < a + n && s->nhits < s->max_hits; pg = (pg | (PAGE - 1)) + 1) {
        uint64_t m = ((pg | (PAGE - 1)) + 1 < a + n ? (pg | (PAGE - 1)) + 1 : a + n) - pg;
        if (m >= s->len && plat_mem_read(pg, s->buf, (size_t)m)) scan(s, s->buf, (size_t)m, pg);
    }
}

static void resident_scan(Search *s, uint64_t a, uint64_t n) {
    uint8_t vec[PIECE / PAGE];
    if (!plat_mem_resident(a, n, vec)) return;
    uint64_t first = a & ~(PAGE - 1), end = a + n;
    for (uint64_t i = 0, pages = ((end + PAGE - 1) & ~(PAGE - 1)) / PAGE - first / PAGE;
         i < pages && s->nhits < s->max_hits && !s->out_of_bytes;) {
        if (!vec[i]) {
            i++;
            continue;
        }
        uint64_t k = i;
        while (k < pages && vec[k]) k++;
        uint64_t lo = first + i * PAGE, hi = first + k * PAGE;
        if (lo < a) lo = a;
        if (hi > end) hi = end;
        if (hi - lo >= s->len) copy_scan(s, lo, hi - lo);
        i = k;
    }
}

static uint64_t readable_run(uint64_t a, uint64_t limit, uint64_t *skip) {
    uint64_t run = a, next;
    for (;;) {
        int st = plat_mem_state(run, limit, &next);
        if (st != PLAT_PROT_READ && st != PLAT_PROT_RW) {
            if (run == a) *skip = next > a ? next : limit;
            return run == a ? 0 : run;
        }
        if (next <= run || next >= limit) return limit;
        run = next;
    }
}

static void search_region(Search *s, uint64_t a, uint64_t end) {
    while (a < end && s->nhits < s->max_hits) {
        if (plat_now_ms() >= s->deadline_ms) {
            s->out_of_time = 1;
            return;
        }
        uint64_t piece_end = (a & ~(PAGE - 1)) + PIECE, limit = end < piece_end ? end : piece_end, skip = limit,
                 run = readable_run(a, limit, &skip);
        if (!run) {
            a = skip;
            continue;
        }
        uint64_t n = run - a;
        if (n < s->len) {
            a = run;
            continue;
        }
        if (s->resident_only) resident_scan(s, a, n);
        else copy_scan(s, a, n);
        if (s->out_of_bytes) return;
        a += a + n >= end ? n : n - (s->len - 1);
        uint64_t due = s->start_ms + s->scanned / SEARCH_BYTES_PER_MS, now = plat_now_ms();
        if (due > now) plat_sleep_ms((uint32_t)(due - now));
    }
}

static void search_range(Search *s, uint64_t base, uint64_t size) {
    uint64_t a = base > s->lo ? base : s->lo, end = base + size < s->hi ? base + size : s->hi;
    while (a < end && s->nhits < s->max_hits && !s->out_of_time && !s->out_of_bytes) {
        RtMemoryInfo mi;
        if (mem_query(a, &mi) || !mi.size) return;
        uint64_t stop = mi.base + mi.size < end ? mi.base + mi.size : end;
        if (stop <= a) return;
        if (mi.state && (mi.perm & 1)) search_region(s, a, stop);
        a = stop;
    }
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static void do_search(Out *o, const Req *r) {
    uint64_t t0 = plat_now_ms();
    const char *hex = param_str(r, "pattern"), *regions = param_str(r, "regions");
    uint8_t pat[PATTERN_MAX];
    size_t len = hex ? strlen(hex) : 0;
    if (len > 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) hex += 2, len -= 2;
    int ok = len && !(len & 1) && len / 2 <= PATTERN_MAX;
    for (size_t i = 0; ok && i < len / 2; i++) {
        int h = hex_digit(hex[2 * i]), l = hex_digit(hex[2 * i + 1]);
        ok = h >= 0 && l >= 0;
        if (ok) pat[i] = (uint8_t)(h << 4 | l);
    }
    uint64_t lo = 0, hi = RT_ADDRESS_LIMIT, max_hits = HITS_DEFAULT, budget = BUDGET_DEFAULT_MS;
    if (param(r, "start") && !param_u64(r, "start", &lo)) ok = 0;
    if (param(r, "end") && !param_u64(r, "end", &hi)) ok = 0;
    if (param(r, "max_hits") && !param_u64(r, "max_hits", &max_hits)) ok = 0;
    if (param(r, "budget_ms") && !param_u64(r, "budget_ms", &budget)) ok = 0;
    if (param(r, "regions") && !regions) ok = 0;
    int heap_only = regions && !strcmp(regions, "heap");
    if (regions && !heap_only && strcmp(regions, "all")) ok = 0;
    if (!ok || lo >= hi) {
        error_answer(o, r, E_PARAMS, M_SEARCH, "search_memory takes a pattern of 1 to %d bytes in hex, an optional "
                     "start below end, regions \"all\" or \"heap\", max_hits and budget_ms", PATTERN_MAX);
        return;
    }
    Search s = {.pat = pat, .len = len / 2, .lo = lo, .hi = hi > RT_ADDRESS_LIMIT ? RT_ADDRESS_LIMIT : hi,
                .max_hits = max_hits < 1 ? 1 : max_hits > HITS_MAX ? HITS_MAX : (int)max_hits, .buf = malloc(PIECE),
                .deadline_ms = t0 + (budget < 1 ? 1 : budget > BUDGET_MAX_MS ? BUDGET_MAX_MS : budget)};
    if (!s.buf) {
        error_answer(o, r, E_NOT_YET, M_SEARCH, "search_memory has no memory for its copies now");
        return;
    }
    plat_mutex_lock(&search_lock);
    s.start_ms = plat_now_ms();
    if (s.start_ms >= s.deadline_ms) s.out_of_time = 1;
    const char *name[24];
    RtRange range[24];
    int nr = guest_regions(name, range, 24);
    for (int pass = 0; pass < 3 && s.nhits < s.max_hits && !s.out_of_time && !s.out_of_bytes; pass++) {
        if (pass == 1 && s.nhits && !heap_only) break;
        for (int i = 0; i < nr && s.nhits < s.max_hits && !s.out_of_time && !s.out_of_bytes; i++) {
            int modules = !strcmp(name[i], "modules"), heap = !strcmp(name[i], "heap");
            if (heap_only ? !heap || pass != 1 : (pass == 0) != modules || (pass == 1) != heap) continue;
            s.resident_only = !modules;
            search_range(&s, range[i].base, range[i].size);
        }
    }
    plat_mutex_unlock(&search_lock);
    free(s.buf);
    uint64_t ms = plat_now_ms() - t0;
    open_answer(o, r);
    out_put(o, "\"result\":{\"hits\":[", 18);
    for (int i = 0; i < s.nhits; i++) out_f(o, "%s\"0x%llx\"", i ? "," : "", (unsigned long long)s.hits[i]);
    out_f(o, "],\"complete\":%s}}\n", s.out_of_time || s.out_of_bytes ? "false" : "true");
    char first[40] = "";
    if (s.nhits) snprintf(first, sizeof first, ", the first at %#llx", (unsigned long long)s.hits[0]);
    int first_answer = once(&said_first[M_SEARCH]), first_hit = s.nhits && once(&said_hit);
    if (first_answer || first_hit)
        rt_log("tracker api: %s: a %u-byte pattern in %s, %d hit%s%s in %llu ms (%.1f MiB read%s)",
               first_hit ? "first search_memory that found its pattern" : "first search_memory answered",
               (unsigned)s.len,
               heap_only ? "the heap" : "all regions", s.nhits, s.nhits == 1 ? "" : "s", first, (unsigned long long)ms,
               s.scanned / 1048576.0, s.out_of_time ? ", stopped at its time budget" : "");
    if (s.out_of_bytes && once(&said_bytes))
        rt_log("tracker api: searches read %llu MiB in the last minute, the most they may; a search answers what it "
               "found until the minute's bytes come back (the first such answer)",
               (unsigned long long)(SEARCH_BYTES_PER_MIN >> 20));
}

enum { GD_INLINE, GD_TEXT, GD_TEXTS, GD_KEYS };
typedef struct {
    const char *name;
    int number, how;
    uint32_t stride;
    uint32_t at;
    uint32_t size;
    uint32_t record;
    uint32_t max;
    uint64_t vtable;
} GdKind;
static const GdKind gd_kinds[] = {
    {"Bool", 0, GD_INLINE, 0x18, 0x16, 1, 8, 200000, 0x4c20658},
    {"Int", 2, GD_INLINE, 0x28, 0x1c, 4, 8, 100000, 0x4c20718},
    {"Enum", 6, GD_INLINE, 0x38, 0x1c, 4, 8, 50000, 0},
    {"String64", 16, GD_TEXT, 0x108, 0x68, 0, 68, 10000, 0},
    {"String64Array", 17, GD_TEXTS, 0x90, 0x80, 0, 0, 2000, 0},
    {"Int64", 22, GD_INLINE, 0x30, 0x20, 8, 12, 50000, 0},
    {"Bool64bitKey", 32, GD_KEYS, 16, 0, 0, 16, 200000, 0},
};
#define GD_KINDS (int)(sizeof gd_kinds / sizeof *gd_kinds)
#define GD_FORMAT 1
#define GD_TABLE_MAX 1000000u
#define GD_TEXT_ELEMENTS 64u
#define GD_TEXTS_STRIDE 0x50
#define GD_TEXTS_AT 0x0c
#define GD_KEYS_COUNT 0xc58
#define GD_ANSWER_MAX (8u << 20)
static PlatMutex gd_lock = PLAT_MUTEX_INIT;
static int32_t said_kind[GD_KINDS], said_kind_refused[GD_KINDS][E_KINDS], said_not_loaded;
static const char gd_not_loaded[] = "the game has not loaded its game data yet";

static uint64_t gd_manager(void) {
    uint64_t slot = 0, m = 0;
    if (!RT_V111 || !guest_copy(MOD_MAIN_BASE + DBG_GAME_DATA_SLOT, &slot, 8) || !slot || !guest_copy(slot, &m, 8))
        return 0;
    return m;
}

static int gd_loaded(uint64_t m);
static int gd_ready(void) {
    uint64_t m = gd_manager();
    return m && gd_loaded(m);
}

static int in_main(uint64_t p) { return p >= MOD_MAIN_BASE && p < MOD_MAIN_BASE + MOD_MAIN_SIZE; }

typedef struct { uint32_t hash, index; } GdPair;
static int gd_by_hash(const void *a, const void *b) {
    uint32_t x = ((const GdPair *)a)->hash, y = ((const GdPair *)b)->hash;
    return x < y ? -1 : x > y;
}

static uint64_t fnv1a(uint64_t h, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

typedef struct { uint8_t *rec; size_t n; uint32_t count; const char *why; int err; } GdAnswer;

static int gd_fail(GdAnswer *a, int err, const char *why) {
    free(a->rec);
    a->rec = NULL, a->n = 0, a->count = 0, a->err = err, a->why = why;
    return 0;
}

static void text_record(uint8_t *to, const uint8_t *text) {
    size_t n = 0;
    while (n < 64 && text[n]) n++;
    memcpy(to, text, n);
    memset(to + n, 0, 64 - n);
}

static int gd_keys(uint64_t m, const GdKind *k, int set_only, GdAnswer *a) {
    uint64_t head[2] = {0, 0}, again[2] = {0, 0};
    if (!guest_copy(m + GD_KEYS_COUNT, head, sizeof head)) return gd_fail(a, E_NOT_YET, "the game data cannot be read now");
    head[0] &= 0xffffffffu;
    if (!head[0] || !head[1]) return gd_fail(a, E_NOT_YET, gd_not_loaded);
    if (head[0] > k->max) {
        if (!guest_copy(m + GD_KEYS_COUNT, again, sizeof again) || (again[0] &= 0xffffffffu, memcmp(head, again, sizeof head)))
            return gd_fail(a, E_NOT_YET, "the placed objects' flags changed while they were read");
        return gd_fail(a, E_UNSUPPORTED, "the placed objects' flags are not where version 1.1.1 keeps them");
    }
    uint8_t *t = malloc((size_t)head[0] * 16);
    if (!t) return gd_fail(a, E_NOT_YET, "the port has no memory for the answer now");
    if (!guest_copy(head[1], t, (size_t)head[0] * 16) || !guest_copy(m + GD_KEYS_COUNT, again, sizeof again) ||
        (again[0] &= 0xffffffffu, memcmp(head, again, sizeof head))) {
        free(t);
        return gd_fail(a, E_NOT_YET, "the placed objects' flags cannot be read now, or changed while they were read");
    }
    size_t kept = 0;
    uint64_t prev = 0;
    for (uint64_t i = 0; i < head[0]; i++) {
        uint64_t key;
        uint32_t cls;
        uint8_t set = t[i * 16 + 12] & 1;
        memcpy(&key, t + i * 16, 8), memcpy(&cls, t + i * 16 + 8, 4);
        if (key <= prev) {
            free(t);
            return gd_fail(a, E_NOT_YET, "the placed objects' flags are not sorted as version 1.1.1 keeps them");
        }
        prev = key;
        if (set_only && !set) continue;
        uint8_t *r = t + kept++ * 16;
        memcpy(r, &key, 8), memcpy(r + 8, &cls, 4);
        memset(r + 12, 0, 4);
        r[12] = set;
    }
    a->rec = t, a->n = kept * 16, a->count = (uint32_t)kept;
    return 1;
}

static int gd_fields(uint64_t m, const GdKind *k, uint64_t f[4]) {
    uint8_t sec[0x30];
    uint32_t total = 0, size = 0;
    if (!guest_copy(m + (uint64_t)k->number * 0x60, sec, sizeof sec)) return 0;
    memcpy(&total, sec + 0x10, 4), memcpy(&f[1], sec + 0x18, 8), memcpy(&f[2], sec + 0x20, 8), memcpy(&size, sec + 0x2c, 4);
    f[0] = total, f[3] = size;
    return 1;
}

static int gd_unsupported(uint64_t m, const GdKind *k, const uint64_t f[4], GdAnswer *a, const char *why) {
    uint64_t now[4];
    if (!gd_fields(m, k, now) || memcmp(f, now, sizeof now))
        return gd_fail(a, E_NOT_YET, "this kind changed while it was read");
    return gd_fail(a, E_UNSUPPORTED, why);
}

static int gd_loaded(uint64_t m) {
    uint32_t bools = 0;
    return guest_copy(m + 0x10, &bools, 4) && bools;
}

static int gd_records(uint64_t m, const GdKind *k, int has_hash, uint32_t hash, int set_only, GdAnswer *a) {
    memset(a, 0, sizeof *a);
    if (k->how == GD_KEYS) return gd_keys(m, k, set_only, a);

    uint64_t f[4], again[4];
    if (!gd_fields(m, k, f)) return gd_fail(a, E_NOT_YET, "the game data cannot be read now");
    uint32_t total = (uint32_t)f[0], size = (uint32_t)f[3];
    uint64_t entries = f[1], table = f[2];
    if (!total && k->number && gd_loaded(m)) return 1;
    if (!total || !entries || !table) return gd_fail(a, E_NOT_YET, gd_not_loaded);
    if (total > k->max || size < total || size > GD_TABLE_MAX)
        return gd_unsupported(m, k, f, a, "this kind is not where version 1.1.1 keeps it");

    GdPair *pairs = malloc((size_t)size * sizeof *pairs);
    if (!pairs) return gd_fail(a, E_NOT_YET, "the port has no memory for the answer now");
    if (!guest_copy(table, pairs, (size_t)size * sizeof *pairs)) {
        free(pairs);
        return gd_fail(a, E_NOT_YET, "this kind's table cannot be read now");
    }
    uint32_t np = 0;
    if (has_hash) {
        for (uint32_t i = hash % size, n = 0; n < size && pairs[i].hash; i = i + 1 < size ? i + 1 : 0, n++)
            if (pairs[i].hash == hash) {
                pairs[np++] = pairs[i];
                break;
            }
    } else {
        for (uint32_t i = 0; i < size; i++)
            if (pairs[i].hash) pairs[np++] = pairs[i];
        qsort(pairs, np, sizeof *pairs, gd_by_hash);
    }
    uint8_t *seen = calloc((size_t)total / 8 + 1, 1);
    int odd = np > total || !seen;
    for (uint32_t i = 0; i < np && !odd; i++) {
        odd = pairs[i].index >= total || (seen[pairs[i].index / 8] >> (pairs[i].index % 8) & 1) ||
              (i && pairs[i].hash == pairs[i - 1].hash);
        if (!odd) seen[pairs[i].index / 8] |= (uint8_t)(1u << (pairs[i].index % 8));
    }
    free(seen);
    if (odd) {
        free(pairs);
        return gd_fail(a, E_NOT_YET, "this kind's table changed while it was read, or does not read as version 1.1.1 "
                                     "keeps it");
    }

    size_t span = has_hash ? (size_t)k->stride : (size_t)total * k->stride;
    uint8_t *ent = malloc(span ? span : 1);
    uint64_t vt = k->vtable ? MOD_MAIN_BASE + k->vtable : 0, from = entries;
    if (has_hash && np) from = entries + (uint64_t)pairs[0].index * k->stride;
    if (!ent || (span && !guest_copy(from, ent, span)) || (!vt && !guest_copy(entries, &vt, 8))) {
        free(pairs), free(ent);
        return gd_fail(a, E_NOT_YET, "this kind's values cannot be read now");
    }
    for (size_t o = 0; o < span; o += k->stride) {
        uint64_t v;
        memcpy(&v, ent + o, 8);
        if (v != vt || !in_main(v)) {
            free(pairs), free(ent);
            return v ? gd_unsupported(m, k, f, a, "this kind's entries are not laid out as version 1.1.1 lays them out")
                     : gd_fail(a, E_NOT_YET, "this kind's entries are still being made");
        }
    }
#define ENTRY(i) (ent + (has_hash ? 0 : (size_t)pairs[i].index * k->stride))

    size_t cap = (size_t)np * k->record;
    for (uint32_t i = 0; !k->record && i < np; i++) {
        uint32_t c = 0;
        memcpy(&c, ENTRY(i) + k->at, 4);
        if (c > GD_TEXT_ELEMENTS) {
            free(pairs), free(ent);
            return gd_fail(a, E_NOT_YET, "a String64Array value holds more than 64 texts");
        }
        cap += 8 + (size_t)c * 64;
    }
    if (cap > GD_ANSWER_MAX) {
        free(pairs), free(ent);
        return gd_fail(a, E_NOT_YET, "the answer would be over 8 MiB");
    }
    uint8_t *rec = malloc(cap ? cap : 1), text[GD_TEXT_ELEMENTS * GD_TEXTS_STRIDE];
    size_t w = 0;
    int unread = !rec;
    for (uint32_t i = 0; !unread && i < np; i++) {
        const uint8_t *e = ENTRY(i);
        memcpy(rec + w, &pairs[i].hash, 4);
        if (k->how == GD_INLINE) {
            uint32_t v32 = 0;
            if (k->size == 1) v32 = e[k->at] & 1;
            else if (k->size == 4) memcpy(&v32, e + k->at, 4);
            if (k->size == 8) memcpy(rec + w + 4, e + k->at, 8);
            else memcpy(rec + w + 4, &v32, 4);
            w += k->record;
        } else if (k->how == GD_TEXT) {
            uint64_t p = 0;
            memcpy(&p, e + k->at, 8);
            unread = !p || !guest_copy(p, text, 64);
            if (!unread) text_record(rec + w + 4, text);
            w += k->record;
        } else {
            uint32_t c = 0;
            uint64_t p = 0;
            memcpy(&c, e + k->at, 4), memcpy(&p, e + k->at + 8, 8);
            unread = c && (!p || !guest_copy(p, text, (size_t)c * GD_TEXTS_STRIDE));
            memcpy(rec + w + 4, &c, 4);
            for (uint32_t j = 0; !unread && j < c; j++)
                text_record(rec + w + 8 + (size_t)j * 64, text + (size_t)j * GD_TEXTS_STRIDE + GD_TEXTS_AT);
            w += 8 + (size_t)c * 64;
        }
    }
#undef ENTRY
    free(pairs), free(ent);
    if (unread || !gd_fields(m, k, again) || memcmp(f, again, sizeof again)) {
        const char *why = rec ? "a text of this kind cannot be read now, or the kind changed while it was read"
                              : "the port has no memory for the answer now";
        free(rec);
        return gd_fail(a, E_NOT_YET, why);
    }
    a->rec = rec, a->n = w, a->count = np;
    return 1;
}

static void do_game_data(Out *o, const Req *r) {
    const char *kind = param_str(r, "kind"), *filter = param_str(r, "filter"), *if_digest = param_str(r, "if_digest");
    const GdKind *k = NULL;
    for (int i = 0; kind && i < GD_KINDS && !k; i++)
        if (!strcmp(kind, gd_kinds[i].name)) k = &gd_kinds[i];
    uint64_t h64 = 0;
    int has_hash = param(r, "hash") != NULL;
    if (!k || (has_hash && (!param_u64(r, "hash", &h64) || !h64 || h64 > 0xffffffffull || k->how == GD_KEYS)) ||
        (filter && (k->how != GD_KEYS || (strcmp(filter, "set") && strcmp(filter, "all")))) ||
        (param(r, "filter") && !filter) || (param(r, "if_digest") && !if_digest)) {
        error_answer(o, r, E_PARAMS, M_GAMEDATA, "nx2saria.game_data takes kind (Bool, Int, Enum, String64, "
                     "String64Array, Int64, Bool64bitKey), hash (32 bits; not Bool64bitKey), filter (set, all; "
                     "Bool64bitKey) and if_digest");
        return;
    }
    if (!RT_V111) {
        error_answer(o, r, E_UNSUPPORTED, M_GAMEDATA, "nx2saria.game_data knows version 1.1.1's game data only");
        return;
    }
    uint64_t m = gd_manager(), t0 = plat_now_ns();
    GdAnswer a = {.err = E_NOT_YET, .why = gd_not_loaded};
    int ok = 0;
    if (m) {
        plat_mutex_lock(&gd_lock);
        ok = gd_records(m, k, has_hash, (uint32_t)h64, !filter || !strcmp(filter, "set"), &a);
        plat_mutex_unlock(&gd_lock);
    }
    int i = (int)(k - gd_kinds);
    if (!ok) {
        __atomic_store_n(&said_refusal[M_GAMEDATA][a.err], 1, __ATOMIC_RELEASE);
        if (a.why == gd_not_loaded && once(&said_not_loaded))
            rt_log("tracker api: nx2saria.game_data asked before the game loaded its game data (the first such answer, "
                   "for %s; the closing lines count the others)", k->name);
        else if (a.why != gd_not_loaded && once(&said_kind_refused[i][a.err]))
            rt_log("tracker api: nx2saria.game_data %s could not be answered: %s (error %d; the first such answer for "
                   "this kind and error)", k->name, a.why, error_code[a.err]);
        error_answer(o, r, a.err, M_GAMEDATA, "%s: %s", k->name, a.why);
        return;
    }
    char digest[17];
    snprintf(digest, sizeof digest, "%016llx", (unsigned long long)fnv1a(0xcbf29ce484222325ull, a.rec, a.n));
    open_answer(o, r);
    out_f(o, "\"result\":{\"kind\":\"%s\",\"format\":%d,\"count\":%u,\"record_size\":%u,\"presents\":%llu,"
          "\"digest\":\"%s\",", k->name, GD_FORMAT, a.count, k->record, (unsigned long long)frame_step_presents(),
          digest);
    if (if_digest && !strcmp(if_digest, digest)) {
        out_put(o, "\"unchanged\":true}}\n", 19);
    } else {
        out_put(o, "\"records\":\"", 11);
        hex_bytes(o, a.rec, a.n);
        out_put(o, "\"}}\n", 4);
    }
    free(a.rec);
    if (once(&said_kind[i]))
        rt_log("tracker api: first nx2saria.game_data of %s answered: %u value%s, %zu bytes, in %.2f ms%s", k->name,
               a.count, a.count == 1 ? "" : "s", a.n, (double)(plat_now_ns() - t0) / 1e6, has_hash ? " (one hash)" : "");
}

static void do_state(Out *o, const Req *r) {
    char area[64] = "";
    float xyz[3], rot[9];
    int has_area = dbg_area(area), has_pos = dbg_player_pos(xyz), has_rot = has_pos && dbg_player_rot(rot);
    const RtGuestLayout *l = mem_layout();
    open_answer(o, r);
    out_f(o, "\"result\":{\"api_version\":%d,\"powered_on\":%s,\"presents\":%llu,\"area\":", TRACKER_API_VERSION,
          game_running() ? "true" : "false", (unsigned long long)frame_step_presents());
    if (has_area) out_text(o, area);
    else out_put(o, "null", 4);
    if (has_pos) {
        out_f(o, ",\"player\":{\"x\":%.4f,\"y\":%.4f,\"z\":%.4f,\"rotation\":", xyz[0], xyz[1], xyz[2]);
        if (has_rot)
            out_f(o, "[%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f]}", rot[0], rot[1], rot[2], rot[3], rot[4], rot[5],
                  rot[6], rot[7], rot[8]);
        else
            out_put(o, "null}", 5);
    } else {
        out_put(o, ",\"player\":null", 14);
    }
    out_f(o, ",\"game_data\":%s", gd_ready() ? "true" : "false");
    out_f(o, ",\"main\":\"0x%llx\",\"heap\":{\"base\":\"0x%llx\",\"size\":\"0x%llx\"}}}\n",
          (unsigned long long)MOD_MAIN_BASE, (unsigned long long)l->heap.base, (unsigned long long)l->heap.size);
    if (once(&said_first[M_STATE])) {
        if (has_pos)
            rt_log("tracker api: first nx2saria.state answered: area %s, Link at (%.2f, %.2f, %.2f)",
                   has_area ? area : "unknown", xyz[0], xyz[1], xyz[2]);
        else
            rt_log("tracker api: first nx2saria.state answered: area %s, no player yet", has_area ? area : "unknown");
    }
}

typedef struct {
    PlatTcp *c;
    int id;
    char peer[64];
    uint64_t t0, req[M_N], errors, bytes;
    int sent_any;
    const char *why;
} Client;

static void not_a_request(Client *cl, const char *p, size_t n) {
    char text[48];
    shown(text, sizeof text, p, n);
    cl->why = "it sent something that is not a JSON request";
    if (once(&said_refusal[M_OTHER][E_PARAMS]))
        rt_log("tracker api: client %d sent something that is not a JSON request (\"%s\"); closed. A web page's HTTP "
               "request or TLS handshake arrives this way.", cl->id, text);
}

static int handle_line(Client *cl, const char *line, size_t n, Out *o) {
    Req r;
    if (!parse_request(line, n, &r)) {
        not_a_request(cl, line, n);
        return 0;
    }
    int m = M_OTHER;
    for (int k = 0; k < M_OTHER && !r.method_cut && m == M_OTHER; k++)
        if (!strcmp(r.method, method_name[k])) m = k;
    uint64_t before = 0;
    for (int k = 0; k < M_N; k++) before += cl->req[k];
    if (!before && client_log_slot())
        rt_log("tracker api: client %d (%s) asks: its first request is %s (%d open)", cl->id, cl->peer,
               r.method[0] ? r.method : "without a method", __atomic_load_n(&open_clients, __ATOMIC_ACQUIRE));
    switch (m) {
    case M_STATUS: do_status(o, &r); break;
    case M_READ: do_read(o, &r); break;
    case M_SEARCH: do_search(o, &r); break;
    case M_STATE: do_state(o, &r); break;
    case M_GAMEDATA: do_game_data(o, &r); break;
    default:
        error_answer(o, &r, E_METHOD, M_OTHER, "unknown method \"%.32s\": this API is read-only and answers %s",
                     r.method, methods_text);
    }
    cl->req[m]++;
    __atomic_add_fetch(&win_req[m], 1, __ATOMIC_RELAXED);
    if (o->refused) cl->errors++;
    return 1;
}

static int starts_wrong(const char *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (p[i] != ' ' && p[i] != '\t' && p[i] != '\r') return p[i] != '{';
    return 0;
}

static int only_blanks(const char *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (p[i] != ' ' && p[i] != '\t' && p[i] != '\r') return 0;
    return 1;
}

static void counts(char *out, size_t cap, const uint64_t *req) {
    uint64_t all = 0;
    for (int m = 0; m < M_N; m++) all += req[m];
    int o = snprintf(out, cap, "%llu request%s", (unsigned long long)all, all == 1 ? "" : "s");
    for (int m = 0, k = 0; m < M_N && o > 0 && (size_t)o < cap; m++)
        if (req[m])
            o += snprintf(out + o, cap - (size_t)o, "%s%s %llu", k++ ? ", " : " (", method_name[m],
                          (unsigned long long)req[m]);
    if (all && o > 0 && (size_t)o < cap) snprintf(out + o, cap - (size_t)o, ")");
}

static void client_main(void *arg) {
    Client *cl = arg;
    plat_thread_priority(PLAT_PRIO_NORMAL);
    char *line = malloc(LINE_CAP);
    size_t have = 0;
    uint64_t heard = plat_now_ms(), line_since = 0;
    Out o = {0};
    cl->why = line ? "the client closed the connection" : "no memory for its requests";
    while (line) {
        int k = plat_tcp_recv(cl->c, line + have, LINE_CAP - have, 1000);
        if (k < 0) break;
        uint64_t now = plat_now_ms();
        if (!k) {
            if (have && now - line_since > LINE_MS) {
                cl->why = "it left a request unfinished for 5 s";
                break;
            }
            if (now - heard > IDLE_MS) {
                cl->why = "it sent nothing for 60 s";
                break;
            }
            continue;
        }
        cl->sent_any = 1;
        heard = now;
        if (!have) line_since = now;
        have += (size_t)k;
        int keep = 1;
        for (char *nl; keep && (nl = memchr(line, '\n', have));) {
            size_t n = (size_t)(nl - line);
            o.n = 0, o.bad = 0, o.refused = 0;
            if (!only_blanks(line, n) && !(keep = handle_line(cl, line, n, &o))) break;
            if (o.bad) {
                cl->why = "an answer did not fit in memory";
                keep = 0;
                break;
            }
            if (o.n && !plat_tcp_send(cl->c, o.p, o.n, SEND_MS)) {
                cl->why = "it did not take an answer in time, or the connection broke";
                keep = 0;
                break;
            }
            cl->bytes += o.n;
            __atomic_add_fetch(&win_bytes, o.n, __ATOMIC_RELAXED);
            if (o.cap > (1u << 20)) free(o.p), o.p = NULL, o.cap = 0;
            have -= n + 1;
            memmove(line, nl + 1, have);
            line_since = plat_now_ms();
        }
        if (!keep) break;
        if (starts_wrong(line, have)) {
            not_a_request(cl, line, have);
            break;
        }
        if (have == LINE_CAP) {
            cl->why = "it sent 4096 bytes without a line end";
            break;
        }
        if (have && now - line_since > LINE_MS) {
            cl->why = "it left a request unfinished for 5 s";
            break;
        }
    }
    free(line);
    free(o.p);
    plat_tcp_close(cl->c);
    int left = __atomic_sub_fetch(&open_clients, 1, __ATOMIC_ACQ_REL);
    if (!cl->sent_any && !strcmp(cl->why, "the client closed the connection")) {
        __atomic_add_fetch(&win_checks, 1, __ATOMIC_RELAXED);
        if (once(&said_check))
            rt_log("tracker api: a program connected and left without a request (a tracker checking whether the port "
                   "is taken); later checks are counted in the summaries");
    } else if (client_log_slot()) {
        char c[200];
        counts(c, sizeof c, cl->req);
        rt_log("tracker api: client %d (%s) left after %.1f s, %s: %s, %llu refused, %.2f MiB sent (%d open)", cl->id,
               cl->peer, (plat_now_ms() - cl->t0) / 1000.0, cl->why, c, (unsigned long long)cl->errors,
               cl->bytes / 1048576.0, left);
    }
    free(cl);
}

static PlatTcp *listen_now(int *err) { return plat_tcp_listen_loopback((uint16_t)port, err); }

static void summary(uint64_t window_ms) {
    static int lines;
    uint64_t req[M_N], all = 0;
    for (int m = 0; m < M_N; m++) all += req[m] = __atomic_exchange_n(&win_req[m], 0, __ATOMIC_RELAXED);
    uint64_t errors = __atomic_exchange_n(&win_errors, 0, __ATOMIC_RELAXED),
             bytes = __atomic_exchange_n(&win_bytes, 0, __ATOMIC_RELAXED),
             checks = __atomic_exchange_n(&win_checks, 0, __ATOMIC_RELAXED);
    if ((!all && !checks) || lines >= SUMMARY_LOGS) return;
    char c[200];
    int open = __atomic_load_n(&open_clients, __ATOMIC_ACQUIRE);
    counts(c, sizeof c, req);
    rt_log("tracker api: the last %llu min: %s, %llu refused, %.2f MiB sent, %llu port check%s, %d client%s open%s",
           (unsigned long long)(window_ms / 60000), c, (unsigned long long)errors, bytes / 1048576.0,
           (unsigned long long)checks, checks == 1 ? "" : "s", open, open == 1 ? "" : "s",
           ++lines == SUMMARY_LOGS ? " (the last summary of this session)" : "");
}

typedef struct { PlatTcp *l; int said_taken; } Listener;

static void listener_main(void *arg) {
    Listener *ls = arg;
    PlatTcp *l = ls->l;
    int tries = 0, said_wait = ls->said_taken, accept_errors = 0;
    free(ls);
    plat_thread_priority(PLAT_PRIO_BELOW_NORMAL);
    uint64_t window_at = plat_now_ms();
    for (;;) {
        uint64_t now = plat_now_ms();
        if (now - window_at >= SUMMARY_MS) {
            summary(now - window_at);
            window_at = now;
        }
        if (!l) {
            int err = 0;
            l = listen_now(&err);
            tries++;
            if (!l) {
                if (!said_wait++)
                    rt_log("tracker api: 127.0.0.1:%d is taken (error %d); trying again every %d s", port, err,
                           RETRY_LISTEN_MS / 1000);
                plat_sleep_ms(RETRY_LISTEN_MS);
                continue;
            }
            rt_log("tracker api: listening on 127.0.0.1:%d after %d tr%s", port, tries, tries == 1 ? "y" : "ies");
            said_wait = 0, tries = 0;
        }
        PlatAddr peer;
        int err = 0;
        PlatTcp *c = plat_tcp_accept(l, 1000, &peer, &err);
        if (!c) {
            if (!err) {
                accept_errors = 0;
                continue;
            }
            if (once(&said_accept))
                rt_log("tracker api: a connection could not be taken (error %d); waiting a moment before the next "
                       "(the first such error)", err);
            plat_sleep_ms(100);
            if (++accept_errors >= ACCEPT_ERRORS_MAX) {
                rt_log("tracker api: the listener failed %d times in a row; making it again", accept_errors);
                plat_tcp_close(l);
                l = NULL;
                accept_errors = 0;
            }
            continue;
        }
        accept_errors = 0;
        int id = __atomic_add_fetch(&client_seq, 1, __ATOMIC_ACQ_REL);
        char who[64] = "?";
        plat_addr_str(&peer, who, sizeof who);
        if (__atomic_add_fetch(&open_clients, 1, __ATOMIC_ACQ_REL) > CLIENTS_MAX) {
            __atomic_sub_fetch(&open_clients, 1, __ATOMIC_ACQ_REL);
            plat_tcp_close(c);
            if (client_log_slot())
                rt_log("tracker api: client %d (%s) refused: %d clients are connected already", id, who, CLIENTS_MAX);
            continue;
        }
        Client *cl = calloc(1, sizeof *cl);
        if (cl) {
            cl->c = c, cl->id = id, cl->t0 = plat_now_ms();
            snprintf(cl->peer, sizeof cl->peer, "%s", who);
        }
        if (!cl || !plat_thread_spawn(client_main, cl, "tracker client", 0)) {
            free(cl);
            plat_tcp_close(c);
            __atomic_sub_fetch(&open_clients, 1, __ATOMIC_ACQ_REL);
            if (client_log_slot()) rt_log("tracker api: cannot start a thread for client %d; closed it", id);
        }
    }
}

int tracker_api_on(void) { return cfg_bool("NX2SARIA_TRACKER_API"); }

void tracker_api_init(void) {
    port = cfg_int("NX2SARIA_TRACKER_API_PORT");
    if (port < 1024 || port > 65535) port = 6545;
    if (!tracker_api_on()) {
        rt_log("tracker api: off (tracker_api = 1 in nx2saria.ini or NX2SARIA_TRACKER_API=1 lets a map tracker on this "
               "PC read the game's memory, read-only, on 127.0.0.1:%d)", port);
        return;
    }
    snprintf(build_id, sizeof build_id, "%s", mods_main_build_id());
    uint64_t heap = mem_layout()->heap.base;
    char where[160];
    for (int m = 0, n = 0; m < M_OTHER; m++)
        if (m != M_GAMEDATA || RT_V111)
            n += snprintf(methods_text + n, sizeof methods_text - (size_t)n, "%s%s", m ? ", " : "", method_name[m]);
    snprintf(where, sizeof where, "the game's heap at %#llx, %s the 0x1000000000 trackers take heap pointers from",
             (unsigned long long)heap, heap >= HEAP_POINTER_FLOOR ? "above" : "below");
    int err = 0;
    Listener *ls = malloc(sizeof *ls);
    PlatTcp *l = listen_now(&err);
    if (l)
        rt_log("tracker api: on, read-only, listening on 127.0.0.1:%d (this PC only; %s; API version %d); %s", port,
               methods_text, TRACKER_API_VERSION, where);
    else
        rt_log("tracker api: on, but 127.0.0.1:%d is taken (error %d), by another program or a closing one; trying "
               "again every %d s; %s", port, err, RETRY_LISTEN_MS / 1000, where);
    if (ls) *ls = (Listener){l, !l};
    if (!ls || !plat_thread_spawn(listener_main, ls, "tracker api", 0)) {
        free(ls);
        plat_tcp_close(l);
        rt_log("tracker api: cannot start the listener thread; the API is off for this run");
    }
}

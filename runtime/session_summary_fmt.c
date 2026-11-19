// The session summary's JSON text (session_summary.h ss_format) and the small value rules it shares with the
// gatherer (ss_setting, ss_os_name, ss_frametime). Plain C with no runtime calls: tests/session_summary_check.c builds
// this file alone. No allocation and no locale-dependent formatting, since session_summary.c runs it on a failing
// thread at a fatal error.
#include <stdio.h>
#include <string.h>

#include "session_summary.h"

typedef struct {
    char *p;
    size_t cap, n;
    int full;   // something did not fit
} Buf;

static void put_raw(Buf *b, const char *s, size_t k) {
    if (b->full || b->n + k >= b->cap) {
        b->full = 1;
        return;
    }
    memcpy(b->p + b->n, s, k);
    b->n += k;
    b->p[b->n] = 0;
}

static void put(Buf *b, const char *s) { put_raw(b, s, strlen(s)); }

// Whole numbers through snprintf's %lld, which no locale changes; v below 0 is null, above max is max.
static void put_int(Buf *b, int64_t v, int64_t max) {
    if (v < 0) {
        put(b, "null");
        return;
    }
    if (max > 0 && v > max) v = max;
    char t[24];
    int k = snprintf(t, sizeof t, "%lld", (long long)v);
    put_raw(b, t, (size_t)k);
}

// One decimal, rounded, made from whole numbers: printf's %f takes the decimal separator of the C locale, and a
// library that sets another one would write "59,9". NaN and values below 0 are null, values above max are max.
static void put_dec1(Buf *b, double v, double max) {
    if (!(v >= 0)) {
        put(b, "null");
        return;
    }
    if (v > max) v = max;
    long long t = (long long)(v * 10 + 0.5);
    char s[32];
    int k = snprintf(s, sizeof s, "%lld.%d", t / 10, (int)(t % 10));
    put_raw(b, s, (size_t)k);
}

// The length of the UTF-8 sequence at s (n bytes left) when it is a valid one and not a C1 control (U+0080-U+009F);
// 0 otherwise. Overlong forms, surrogates and code points above U+10FFFF are invalid.
static int utf8_len(const unsigned char *s, size_t n) {
    unsigned c = s[0];
    int k = c >= 0xF0 && c <= 0xF4 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 && c < 0xE0 ? 2 : 0;
    if (!k || (size_t)k > n) return 0;
    for (int i = 1; i < k; i++)
        if ((s[i] & 0xC0) != 0x80) return 0;
    if (c == 0xC2 && s[1] < 0xA0) return 0;              // C1 controls
    if (c == 0xE0 && s[1] < 0xA0) return 0;              // overlong
    if (c == 0xED && s[1] >= 0xA0) return 0;             // surrogates
    if (c == 0xF0 && s[1] < 0x90) return 0;              // overlong
    if (c == 0xF4 && s[1] >= 0x90) return 0;             // above U+10FFFF
    return k;
}

// A free-text string as a JSON string of at most max characters (code points): quotes and backslashes escaped, tabs and
// line ends as one space, other control characters and invalid UTF-8 dropped, spaces at both ends left out.
static void put_text(Buf *b, const char *s, int max) {
    if (!s) {
        put(b, "null");
        return;
    }
    const unsigned char *p = (const unsigned char *)s;
    size_t n = strlen(s);
    put(b, "\"");
    int chars = 0, spaces = 0, any = 0;   // spaces held back until a character follows them
    for (size_t i = 0; i < n && chars < max;) {
        unsigned c = p[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (any) spaces++;
            i++;
            continue;
        }
        if (c < 0x20 || c == 0x7f) {
            i++;
            continue;
        }
        int k = c < 0x80 ? 1 : utf8_len(p + i, n - i);
        if (!k) {
            i++;
            continue;
        }
        for (; spaces && chars < max - 1; spaces--, chars++) put(b, " ");
        spaces = 0;
        if (c == '"') put(b, "\\\"");
        else if (c == '\\') put(b, "\\\\");
        else put_raw(b, (const char *)p + i, (size_t)k);
        chars++, any = 1;
        i += (size_t)k;
    }
    put(b, "\"");
}

// A display or settings value: 1..32 characters of [A-Za-z0-9_.:x+-], else "other"; NULL is null.
static void put_token(Buf *b, const char *s) {
    if (!s) {
        put(b, "null");
        return;
    }
    size_t n = strlen(s);
    int ok = n >= 1 && n <= 32;
    for (size_t i = 0; ok && i < n; i++) {
        char c = s[i];
        ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
             c == ':' || c == '+' || c == '-';
    }
    put(b, "\"");
    put(b, ok ? s : "other");
    put(b, "\"");
}

static void put_bool(Buf *b, int v) { put(b, v < 0 ? "null" : v ? "true" : "false"); }

static void put_hex16(Buf *b, int64_t v) {
    if (v < 0 || v > 0xffff) {
        put(b, "null");
        return;
    }
    char t[16];
    int k = snprintf(t, sizeof t, "\"0x%04x\"", (unsigned)v);
    put_raw(b, t, (size_t)k);
}

// The fixed values (schema, versions, end): the token rules, with "" for NULL instead of null.
static void put_required(Buf *b, const char *s) {
    if (!s || !*s) put(b, "\"\"");
    else put_token(b, s);
}

#define DAYS30 (30ll * 24 * 3600)
#define MEM_MAX 4194304ll

int ss_format(const SessionSummary *s, char *out, size_t cap) {
    if (!cap) return -1;
    Buf b = {out, cap, 0, 0};
    out[0] = 0;
    put(&b, "{\n  \"schema\": \"nx2saria.session/1\",\n  \"version\": ");
    put_required(&b, s->version);
    put(&b, ",\n  \"game_version\": ");
    put_required(&b, s->game_version);
    put(&b, ",\n  \"os\": {\"family\": ");
    put_token(&b, s->os_family && (!strcmp(s->os_family, "windows") || !strcmp(s->os_family, "linux")) ? s->os_family : NULL);
    put(&b, ", \"name\": ");
    put_text(&b, s->os_name, 64);
    put(&b, ", \"wine\": ");
    put_bool(&b, s->wine);
    put(&b, "},\n  \"cpu\": {\"name\": ");
    put_text(&b, s->cpu_name, 64);
    put(&b, ", \"threads\": ");
    put_int(&b, s->cpu_threads, 1024);
    put(&b, "},\n  \"ram_mb\": ");
    put_int(&b, s->ram_mb, MEM_MAX);
    put(&b, ",\n  \"gpu\": {\"name\": ");
    put_text(&b, s->gpu_name, 64);
    put(&b, ", \"vendor\": ");
    put_hex16(&b, s->gpu_vendor);
    put(&b, ", \"device\": ");
    put_hex16(&b, s->gpu_device);
    put(&b, ", \"driver\": ");
    put_text(&b, s->gpu_driver, 64);
    put(&b, ", \"vram_mb\": ");
    put_int(&b, s->vram_mb, MEM_MAX);
    put(&b, "},\n  \"display\": {\"output\": ");
    put_token(&b, s->output);
    put(&b, ", \"render_scale\": ");
    put_token(&b, s->render_scale);
    put(&b, ", \"fullscreen\": ");
    put_bool(&b, s->fullscreen);
    put(&b, ", \"game_aspect\": ");
    put_token(&b, s->game_aspect);
    put(&b, "},\n  \"settings\": {\"preset\": ");
    put_token(&b, s->preset);
    put(&b, ", \"upscaler\": ");
    put_token(&b, s->upscaler);
    put(&b, ", \"taa\": ");
    put_token(&b, s->taa);
    put(&b, ", \"dlss\": ");
    put_token(&b, s->dlss);
    put(&b, ", \"framegen\": ");
    put_token(&b, s->framegen);
    put(&b, ", \"fps_limit\": ");
    put_int(&b, s->fps_limit, 2000);
    put(&b, ", \"vk_present\": ");
    put_token(&b, s->vk_present);
    put(&b, "},\n  \"session\": {\"duration_s\": ");
    put_int(&b, s->duration_s, DAYS30);
    put(&b, ", \"frames\": ");
    put_int(&b, s->frames, 0);
    put(&b, ", \"end\": ");
    put_required(&b, s->end);
    put(&b, "},\n  \"frametime\": ");
    if (s->has_frametime) {
        put(&b, "{\"fps_mean\": ");
        put_dec1(&b, s->fps_mean, 2000);
        put(&b, ", \"fps_low_1pct\": ");
        put_dec1(&b, s->fps_low_1pct, 2000);
        put(&b, ", \"worst_ms\": ");
        put_dec1(&b, s->worst_ms, 600000);
        put(&b, ", \"over_25ms\": ");
        put_int(&b, s->over_25ms, 0);
        put(&b, ", \"over_100ms\": ");
        put_int(&b, s->over_100ms, 0);
        put(&b, "}");
    } else {   // no frame counted: an object of nulls, as every other group; the server takes no null group
        put(&b, "{\"fps_mean\": null, \"fps_low_1pct\": null, \"worst_ms\": null, \"over_25ms\": null, "
                "\"over_100ms\": null}");
    }
    put(&b, ",\n  \"startup\": {\"shader_prewarm_ms\": ");
    put_int(&b, s->shader_prewarm_ms, 0);
    put(&b, ", \"shaders_translated\": ");
    put_int(&b, s->shaders_translated, 0);
    put(&b, ", \"pipelines_built\": ");
    put_int(&b, s->pipelines_built, 0);
    put(&b, ", \"pipeline_build_ms\": ");
    put_int(&b, s->pipeline_build_ms, 0);
    put(&b, "},\n  \"fatal\": ");
    put_text(&b, s->fatal, 300);
    put(&b, "\n}\n");
    return b.full ? -1 : (int)b.n;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int same_ci(const char *a, size_t an, const char *b, size_t bn) {
    if (an != bn) return 0;
    for (size_t i = 0; i < an; i++)
        if (lower((unsigned char)a[i]) != lower((unsigned char)b[i])) return 0;
    return 1;
}

// A whole number of at most 9 digits, else -1.
static long whole(const char *s, size_t n) {
    if (!n || n > 9) return -1;
    long v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

const char *ss_setting(const char *value, const char *allowed, char *out, size_t cap) {
    if (!value || !*value) return NULL;
    if (!allowed) return value;
    size_t vn = strlen(value);
    for (const char *p = allowed; *p;) {
        size_t k = strcspn(p, "|");
        const char *dots = NULL;
        for (size_t i = 0; i + 1 < k; i++)
            if (p[i] == '.' && p[i + 1] == '.') dots = p + i;
        if (dots) {   // "lo..hi": the value as a whole number, written without leading zeros or sign
            long lo = whole(p, (size_t)(dots - p)), hi = whole(dots + 2, (size_t)(p + k - dots - 2)), v = whole(value, vn);
            if (lo >= 0 && hi >= lo && v >= lo && v <= hi) {
                snprintf(out, cap, "%ld", v);
                return out;
            }
        } else if (same_ci(p, k, value, vn)) {
            snprintf(out, cap, "%.*s", (int)k, p);
            return out;
        }
        p += k + (p[k] == '|');
    }
    return "other";
}

void ss_os_name(const char *raw, char *out, size_t cap) {
    if (!cap) return;
    out[0] = 0;
    if (!raw) return;
    unsigned major = 0, minor = 0, build = 0;
    if (sscanf(raw, "Windows %u.%u.%u", &major, &minor, &build) == 3) {
        if (major == 10 && minor == 0) snprintf(out, cap, "Windows %d (build %u)", build >= 22000 ? 11 : 10, build);
        else snprintf(out, cap, "Windows %u.%u (build %u)", major, minor, build);
        return;
    }
    snprintf(out, cap, "%s", raw);
    char *k = strstr(out, " (Linux ");   // linux.c plat_os_version adds the kernel's release after the name
    if (k && k > out) *k = 0;
}

void ss_frametime(const uint32_t *hist, int bins, uint64_t bin_ns, uint64_t frames, uint64_t sum_ns, uint64_t max_ns,
                  double *fps_mean, double *fps_low_1pct, double *worst_ms) {
    *fps_mean = *fps_low_1pct = *worst_ms = 0;
    if (!frames || !sum_ns) return;
    *fps_mean = frames * 1e9 / sum_ns;
    uint64_t cut = (frames + 99) / 100, acc = 0, low = 0;   // the slowest 1% of frames
    for (int b = bins - 1; b >= 0; b--) {
        acc += hist[b];
        if (acc >= cut) {
            low = (uint64_t)(b + 1) * bin_ns;   // upper edge of the bin
            break;
        }
    }
    *fps_low_1pct = low ? 1e9 / low : 0.0;
    *worst_ms = max_ns / 1e6;
}

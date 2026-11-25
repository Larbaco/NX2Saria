#ifndef QUIT_MENU_CORE_H
#define QUIT_MENU_CORE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define QM_MSBT_HEAD 0x20
#define QM_MAX_LABELS 4096

typedef struct { const char *label; const uint8_t *text; size_t n; } QmText;

typedef struct { const uint8_t *lbl, *txt; uint32_t lbl_n, txt_n, groups, texts; } QmMsbt;

static inline uint32_t qm_u32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static inline void qm_put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static inline uint32_t qm_label_hash(const char *s, size_t len, uint32_t groups) {
    uint32_t h = 0;
    for (size_t i = 0; i < len; i++) h = h * 0x492 + (uint8_t)s[i];
    return groups ? h % groups : 0;
}

static inline int qm_msbt_open(const uint8_t *p, size_t n, QmMsbt *m) {
    uint16_t sections;
    memset(m, 0, sizeof *m);
    if (n < QM_MSBT_HEAD || memcmp(p, "MsgStdBn", 8) || p[8] != 0xff || p[9] != 0xfe || p[0xc] != 0) return 0;
    memcpy(&sections, p + 0xe, 2);
    if (sections != 2) return 0;
    size_t o = QM_MSBT_HEAD;
    for (int i = 0; i < 2; i++) {
        if (n - o < 16) return 0;
        uint32_t size = qm_u32(p + o + 4);
        if (size > n - o - 16) return 0;
        if (!memcmp(p + o, "LBL1", 4)) m->lbl = p + o + 16, m->lbl_n = size;
        else if (!memcmp(p + o, "TXT2", 4)) m->txt = p + o + 16, m->txt_n = size;
        else return 0;
        o = (o + 16 + size + 15) & ~(size_t)15;
        if (o > n && i == 0) return 0;
    }
    if (!m->lbl || !m->txt || m->lbl_n < 4 || m->txt_n < 4) return 0;
    m->groups = qm_u32(m->lbl);
    m->texts = qm_u32(m->txt);
    if (!m->groups || m->groups > QM_MAX_LABELS || (uint64_t)m->groups * 8 + 4 > m->lbl_n) return 0;
    if (m->texts > QM_MAX_LABELS || (uint64_t)m->texts * 4 + 4 > m->txt_n) return 0;
    for (uint32_t i = 0; i < m->texts; i++) {
        uint32_t at = qm_u32(m->txt + 4 + i * 4), end = i + 1 < m->texts ? qm_u32(m->txt + 8 + i * 4) : m->txt_n;
        if (at < 4 + m->texts * 4 || at > end || end > m->txt_n) return 0;
    }
    return 1;
}

static inline int qm_msbt_text(const QmMsbt *m, uint32_t index, const uint8_t **text, size_t *n) {
    if (index >= m->texts) return 0;
    uint32_t at = qm_u32(m->txt + 4 + index * 4), end = index + 1 < m->texts ? qm_u32(m->txt + 8 + index * 4) : m->txt_n;
    const uint8_t *z = memchr(m->txt + at, 0, end - at);
    *text = m->txt + at;
    *n = z ? (size_t)(z - (m->txt + at)) : end - at;
    return 1;
}

typedef int (*QmLabelFn)(void *ctx, const char *label, size_t len, uint32_t index);

static inline int qm_msbt_labels(const QmMsbt *m, QmLabelFn fn, void *ctx) {
    int seen = 0;
    for (uint32_t g = 0; g < m->groups; g++) {
        uint32_t count = qm_u32(m->lbl + 4 + g * 8), at = qm_u32(m->lbl + 8 + g * 8);
        for (uint32_t k = 0; k < count; k++) {
            if (at >= m->lbl_n) return -1;
            uint32_t len = m->lbl[at];
            if ((uint64_t)at + 1 + len + 4 > m->lbl_n || ++seen > QM_MAX_LABELS) return -1;
            uint32_t index = qm_u32(m->lbl + at + 1 + len);
            if (index >= m->texts) return -1;
            if (fn && fn(ctx, (const char *)m->lbl + at + 1, len, index)) return seen;
            at += 1 + len + 4;
        }
    }
    return seen;
}

typedef struct { const char *want; size_t want_n; int64_t index; } QmFind;

static inline int qm_find_fn(void *ctx, const char *label, size_t len, uint32_t index) {
    QmFind *f = ctx;
    if (len != f->want_n || memcmp(label, f->want, len)) return 0;
    f->index = index;
    return 1;
}

static inline int qm_msbt_find(const uint8_t *p, size_t n, const char *label, const uint8_t **text, size_t *text_n) {
    QmMsbt m;
    QmFind f = {label, strlen(label), -1};
    if (!qm_msbt_open(p, n, &m) || qm_msbt_labels(&m, qm_find_fn, &f) < 0 || f.index < 0) return 0;
    return qm_msbt_text(&m, (uint32_t)f.index, text, text_n);
}

typedef struct { const char *name; size_t len; uint32_t index; } QmLabel;
typedef struct { QmLabel *l; int n, cap, bad; } QmList;

static inline int qm_list_fn(void *ctx, const char *label, size_t len, uint32_t index) {
    QmList *s = ctx;
    if (s->n >= s->cap) return s->bad = 1;
    s->l[s->n++] = (QmLabel){label, len, index};
    return 0;
}

static inline int qm_msbt_add(const uint8_t *p, size_t n, const QmText *add, int nadd, uint8_t **out, size_t *out_n) {
    QmMsbt m;
    *out = NULL, *out_n = 0;
    if (!qm_msbt_open(p, n, &m) || nadd < 0 || nadd > 64) return -1;
    int have = qm_msbt_labels(&m, NULL, NULL), result = -1, changed = 0;
    if (have < 0) return -1;
    QmList list = {malloc(((size_t)have + (size_t)nadd + 1) * sizeof(QmLabel)), 0, have + nadd, 0};
    QmText *texts = malloc(((size_t)m.texts + (size_t)nadd + 1) * sizeof *texts);
    uint8_t *o = NULL;
    uint32_t ntext = m.texts;
    if (!list.l || !texts || qm_msbt_labels(&m, qm_list_fn, &list) < 0 || list.bad) goto done;
    for (uint32_t i = 0; i < m.texts; i++) {
        texts[i].label = NULL;
        if (!qm_msbt_text(&m, i, &texts[i].text, &texts[i].n)) goto done;
        uint32_t at = qm_u32(m.txt + 4 + i * 4), end = i + 1 < m.texts ? qm_u32(m.txt + 8 + i * 4) : m.txt_n;
        if (texts[i].n == end - at) goto done;
    }
    for (int a = 0; a < nadd; a++) {
        size_t len = strlen(add[a].label);
        if (!len || len > 255 || memchr(add[a].text, 0, add[a].n)) goto done;
        int at = -1;
        for (int k = 0; k < list.n && at < 0; k++)
            if (list.l[k].len == len && !memcmp(list.l[k].name, add[a].label, len)) at = k;
        if (at >= 0) {
            QmText *t = &texts[list.l[at].index];
            if (t->n == add[a].n && !memcmp(t->text, add[a].text, t->n)) continue;
            t->text = add[a].text, t->n = add[a].n;
        } else {
            list.l[list.n++] = (QmLabel){add[a].label, len, ntext};
            texts[ntext].label = NULL, texts[ntext].text = add[a].text, texts[ntext].n = add[a].n, ntext++;
        }
        changed++;
    }
    if (!changed) {
        result = 0;
        goto done;
    }
    size_t lbl_n = 4 + (size_t)m.groups * 8, txt_n = 4 + (size_t)ntext * 4;
    for (int k = 0; k < list.n; k++) lbl_n += 1 + list.l[k].len + 4;
    for (uint32_t i = 0; i < ntext; i++) txt_n += texts[i].n + 1;
    size_t lbl_at = QM_MSBT_HEAD, txt_at = (lbl_at + 16 + lbl_n + 15) & ~(size_t)15,
           total = (txt_at + 16 + txt_n + 15) & ~(size_t)15;
    if (total > UINT32_MAX || !(o = malloc(total))) goto done;
    memset(o, 0xab, total);
    memcpy(o, p, QM_MSBT_HEAD);
    qm_put32(o + 0x12, (uint32_t)total);
    memcpy(o + lbl_at, "LBL1", 4);
    qm_put32(o + lbl_at + 4, (uint32_t)lbl_n);
    memset(o + lbl_at + 8, 0, 8);
    uint8_t *lb = o + lbl_at + 16;
    qm_put32(lb, m.groups);
    size_t w = 4 + (size_t)m.groups * 8;
    for (uint32_t g = 0; g < m.groups; g++) {
        uint32_t count = 0;
        qm_put32(lb + 8 + g * 8, (uint32_t)w);
        for (int k = 0; k < list.n; k++) {
            if (qm_label_hash(list.l[k].name, list.l[k].len, m.groups) != g) continue;
            lb[w] = (uint8_t)list.l[k].len;
            memcpy(lb + w + 1, list.l[k].name, list.l[k].len);
            qm_put32(lb + w + 1 + list.l[k].len, list.l[k].index);
            w += 1 + list.l[k].len + 4, count++;
        }
        qm_put32(lb + 4 + g * 8, count);
    }
    memcpy(o + txt_at, "TXT2", 4);
    qm_put32(o + txt_at + 4, (uint32_t)txt_n);
    memset(o + txt_at + 8, 0, 8);
    uint8_t *tx = o + txt_at + 16;
    qm_put32(tx, ntext);
    w = 4 + (size_t)ntext * 4;
    for (uint32_t i = 0; i < ntext; i++) {
        qm_put32(tx + 4 + i * 4, (uint32_t)w);
        memcpy(tx + w, texts[i].text, texts[i].n);
        tx[w + texts[i].n] = 0;
        w += texts[i].n + 1;
    }
    *out = o, *out_n = total, o = NULL;
    result = changed;
done:
    free(o);
    free(list.l);
    free(texts);
    return result;
}

enum { QM_LABEL_TITLE = 10, QM_LABEL_CANCEL = 11 };

static inline uint32_t qm_window_label(int three, uint32_t button, uint32_t label) {
    if (!three) return label;
    if (button == 0 && label == 3) return QM_LABEL_TITLE;
    if (button == 1 && label == 1) return QM_LABEL_CANCEL;
    return label;
}

typedef struct { int was_down, held; } QmPress;

static inline int qm_press(QmPress *s, int armed, int a_down, int *chosen) {
    *chosen = 0;
    if (!a_down) {
        s->was_down = s->held = 0;
        return 0;
    }
    if (!s->was_down && armed) s->held = 1, *chosen = 1;
    s->was_down = 1;
    return s->held;
}

#endif

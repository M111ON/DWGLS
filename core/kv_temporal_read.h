/* kv_temporal_read.h — read the temporal KV log BACK.
 *
 * The producer (kv_temporal.h + a host) appends two record kinds to a pool log:
 *
 *   {"v":3,"app":"kv-temporal","kind":"kv-anchor",
 *    "text":"{\"turn\":145,\"base_n_tokens\":2103,\"type_k\":8,\"type_v\":2,
 *             \"blob_off\":0,\"blob_len\":11000000,\"blob_fnv\":...}",
 *    "to":"all","prev":"...","hash":"..."}
 *
 *   {"v":3,...,"kind":"kv-delta",
 *    "text":"{\"turn\":146,\"parent\":145,\"tokens\":[...]}", ...}
 *
 * Recall must work from the log ALONE: a later process has no in-memory
 * frames, so it rebuilds a frame from (nearest anchor blob) + (that anchor's
 * token deltas). These readers are therefore part of the FORMAT CONTRACT, not
 * test scaffolding — if the log cannot reproduce the frames, the format failed
 * regardless of what the producer still remembers.
 *
 * Rules enforced here (each one cost a defect somewhere):
 *   - fields live inside the ESCAPED `text` string, so keys are matched as
 *     \"key\": and never as "key":
 *   - a record only counts when the line closes with '}', so a killed writer's
 *     truncated last line cannot silently shorten a replay
 *   - deltas and anchors come back ASCENDING by turn (the log is chronological,
 *     but an append run interleaves, so sort rather than trust)
 *
 * HOST-AGNOSTIC: no llama.h, no pool.h. The caller owns the file path.
 */
#ifndef KV_TEMPORAL_READ_H
#define KV_TEMPORAL_READ_H

#include "kv_temporal.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_MAX_FRAMES 8192          /* deltas a reader will hold at once */
#define LOG_MAX_ANCHORS 64
#define LOG_SLURP_MAX ((size_t)512 << 20)   /* refuse to slurp more than this */

/* whole file into memory. NULL on empty/unreadable/oversized. */
static inline char *kvl_slurp(const char *path, size_t *len_out) {
    *len_out = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return NULL; }
    if ((size_t)sz > LOG_SLURP_MAX) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = 0;
    *len_out = got;
    return buf;
}

/* Move past `"key"` → value, tolerating what a JSON writer actually emits.
 * Two writers produce this file: our host (compact: `"hash":"abc"`) and
 * pool.py (`json.dumps` default: `"hash": "abc"`). A parser that only accepts
 * one shape silently ignores the other writer's lines — measured: the chain
 * forked because the tail scan could not read a pool line's hash. */
static inline const char *kvl_after_key(const char *line, const char *key) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\\\"%s\\\"", key);      /* "key" inside text */
    const char *p = strstr(line, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* value of "key":<int> inside one log line; dflt when absent */
static inline long long kvl_num(const char *line, const char *key, long long dflt) {
    const char *p = kvl_after_key(line, key);
    if (!p) return dflt;
    char *end = NULL;
    long long v = strtoll(p, &end, 10);
    return end == p ? dflt : v;
}

/* unsigned variant: blob_fnv can exceed INT64_MAX (e.g. 16007757002510808865) */
static inline unsigned long long kvl_unum(const char *line, const char *key,
                                          unsigned long long dflt) {
    const char *p = kvl_after_key(line, key);
    if (!p) return dflt;
    char *end = NULL;
    unsigned long long v = strtoull(p, &end, 10);
    return end == p ? dflt : v;
}

/* token array of one delta line; caller frees *out. -1 when empty/absent. */
static inline int kvl_tokens(const char *line, int32_t **out, int32_t *n_out) {
    *out = NULL; *n_out = 0;
    const char *p = kvl_after_key(line, "tokens");
    if (!p || *p != '[') return -1;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    int cap = 32, n = 0;
    int32_t *v = (int32_t *)malloc((size_t)cap * sizeof(int32_t));
    if (!v) return -1;
    while (*p && *p != ']') {
        char *end = NULL;
        long t = strtol(p, &end, 10);
        if (end == p) break;
        if (n == cap) {
            cap *= 2;
            int32_t *nv = (int32_t *)realloc(v, (size_t)cap * sizeof(int32_t));
            if (!nv) { free(v); return -1; }
            v = nv;
        }
        v[n++] = (int32_t)t;
        p = end;
        if (*p == ',') p++;
    }
    *out = v; *n_out = n;
    return n > 0 ? 0 : -1;
}

/* ── one frame's replay payload ── */
typedef struct {
    int32_t  turn;
    int32_t  parent;      /* anchor turn this frame hangs off (itself if anchor) */
    int32_t  n;
    int32_t *tok;         /* malloc'd, n long */
} KVLogDelta;

static inline int kvl_delta_cmp(const void *a, const void *b) {
    return ((const KVLogDelta *)a)->turn - ((const KVLogDelta *)b)->turn;
}

static inline void kvl_anchor_sort(KVTAnchor *a, int n) {
    for (int i = 1; i < n; i++) {
        KVTAnchor k = a[i];
        int j = i - 1;
        while (j >= 0 && a[j].turn > k.turn) { a[j+1] = a[j]; j--; }
        a[j+1] = k;
    }
}

/* the record's kind lives at the TOP level of the line (unescaped, and
 * pool.py writes it with a space): match the value, not the punctuation */
static inline int kvl_kind_is(const char *line, const char *want) {
    const char *p = strstr(line, "\"kind\"");
    if (!p) return 0;
    p += 6;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    size_t n = strlen(want);
    return strncmp(p, "\"", 1) == 0 && strncmp(p + 1, want, n) == 0 && p[1 + n] == '"';
}

/* ── every delta must be stitchable onto THIS log's path ──
 *
 * `parent` is the anchor turn a delta hangs off (the nearest anchor at or before
 * its turn; an anchor turn hangs off itself). Replay picks that anchor, resumes
 * its blob, then walks deltas forward — so a delta whose parent is NOT the
 * nearest anchor on this path was written under another branch. Replaying it
 * anyway produces a plausible wrong state instead of an error, which is worse
 * than refusing: measured with a deliberately mis-parented record, the replay
 * succeeds and only the argmax comparison notices.
 *
 * Returns 0 when every delta stitches; -1 on the first bad one (turn/parent out).
 */
static inline int kvl_check_parents(const KVTAnchor *a, int na,
                                    const KVLogDelta *d, int nd,
                                    int32_t *bad_turn, int32_t *bad_parent) {
    for (int i = 0; i < nd; i++) {
        int32_t best = -1;
        for (int j = 0; j < na; j++)
            if (a[j].turn <= d[i].turn && a[j].turn > best) best = a[j].turn;
        int found = 0;
        for (int j = 0; j < na; j++) if (a[j].turn == d[i].parent) { found = 1; break; }
        if (best < 0 || !found || d[i].parent != best) {
            if (bad_turn) *bad_turn = d[i].turn;
            if (bad_parent) *bad_parent = d[i].parent;
            return -1;
        }
    }
    return 0;
}

/* anchors (resume points) + deltas (tokens to replay), both ascending.
 * Missing file = zero records, not an error. */
static inline void kvl_read(const char *path, KVTAnchor *anchors, int cap_a, int *na_out,
                            KVLogDelta *deltas, int cap_d, int *nd_out) {
    *na_out = 0; *nd_out = 0;
    size_t len = 0;
    char *buf = kvl_slurp(path, &len);
    if (!buf) return;
    char *line = buf;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        size_t ll = strlen(line);
        /* a line can end CRLF (any tool that re-serializes this file on Windows
           writes CRLF; our own host writes LF), so trim before judging it: the
           `}` test rejected every CRLF line and the reader silently saw an
           almost-empty log while the chain itself was fine */
        while (ll > 0 && (line[ll-1] == '\r' || line[ll-1] == ' ')) line[--ll] = 0;
        if (ll < 64 || line[ll-1] != '}') {          /* partial line: skip */
            line = nl ? nl + 1 : NULL;
            continue;
        }
        if (kvl_kind_is(line, "kv-anchor") && *na_out < cap_a) {
            KVTAnchor a; memset(&a, 0, sizeof(a));
            a.turn          = (int32_t)kvl_num(line, "turn", 0);
            a.base_n_tokens = (int32_t)kvl_num(line, "base_n_tokens", 0);
            a.kv_type_k     = (uint32_t)kvl_num(line, "type_k", 0);
            a.kv_type_v     = (uint32_t)kvl_num(line, "type_v", 0);
            a.blob_off      = kvl_unum(line, "blob_off", 0);
            a.blob_len      = kvl_unum(line, "blob_len", 0);
            a.base_fnv      = kvl_unum(line, "blob_fnv", 0);
            if (a.turn > 0) anchors[(*na_out)++] = a;
        } else if (kvl_kind_is(line, "kv-delta") && *nd_out < cap_d) {
            KVLogDelta d; memset(&d, 0, sizeof(d));
            d.turn   = (int32_t)kvl_num(line, "turn", 0);
            d.parent = (int32_t)kvl_num(line, "parent", 0);
            if (d.turn > 0 && kvl_tokens(line, &d.tok, &d.n) == 0) deltas[(*nd_out)++] = d;
        }
        line = nl ? nl + 1 : NULL;
    }
    free(buf);
    qsort(deltas, (size_t)*nd_out, sizeof(KVLogDelta), kvl_delta_cmp);
    kvl_anchor_sort(anchors, *na_out);
}

static inline KVLogDelta *kvl_find(KVLogDelta *deltas, int n, int32_t turn) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (deltas[mid].turn == turn) return &deltas[mid];
        if (deltas[mid].turn < turn) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

static inline void kvl_free(KVLogDelta *deltas, int n) {
    for (int i = 0; i < n; i++) free(deltas[i].tok);
}

#endif /* KV_TEMPORAL_READ_H */

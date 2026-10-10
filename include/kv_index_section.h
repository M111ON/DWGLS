/* kv_index_section.h — the KV <-> ann cache-section seam.
 *
 * Memory #7527 (IDEA, owner "ยังไม่กล้าพูดอะไรมาก"): the first place KV and
 * ann connect is the KV cache SECTION. KV carries an index (frames: tokens +
 * anchor refs + seek plan, core/kv_temporal.h); ann consumes an index
 * (entries: sid/file/npast/anchor/anchor2/dim/vec, kv_index.jsonl shape).
 * So this builder re-shapes one into the other — and nothing else:
 *
 *   - NO new route. anchor/anchor2 come from anch_route with the caller's
 *     table (trained kv_anchors.bin OR ac_build_table-derived — this header
 *     does not care which). ann's route consumes the section unchanged.
 *   - NO embedding here. The vec per frame is supplied by the host, embedded
 *     AHEAD OF TIME (feed the index before generation, never live-pull
 *     during it — ann's anchor route is an O(N) exact scan).
 *   - NO DB wiring, NO llama hook, NO live ann touch. Output is a .jsonl
 *     section file the operator concatenates onto an index (or a test
 *     loader reads back).
 *
 * Entry shape (matches kv_index.jsonl exactly):
 *   {"sid":"<32hex>","file":"<tag>/t<turn>","npast":N,
 *    "anchor":a,"anchor2":b,"dim":D,"vec":[...]}
 *   sid    = 32 hex from two FNV-1a passes (tokens ^ turn salt).
 *            Deterministic per (turn, content); unique within a section.
 *   file   = "<tag>/t<turn>" (tag = session id from the host, may be "").
 *   npast  = token count (same convention as the memcore-built index, where
 *            npast carries a length because the format has no other field).
 *   anchor/anchor2 = top-2 buckets from anch_route (anchor==anchor2 iff K==1).
 *
 * Header-only, std-only. Float emit uses %.9g: 9 significant digits round-
 * trip an IEEE-754 float bit-exactly through strtof, so a section file that
 * is written and re-read carries identical vecs.
 */
#ifndef KV_INDEX_SECTION_H
#define KV_INDEX_SECTION_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "anchor_route.h"   /* anch_route */

#define KVS_SID_LEN  32
#define KVS_FILE_CAP 64

/* ── one section entry (vec is borrowed, never copied) ── */
typedef struct {
    char  sid[KVS_SID_LEN + 1];
    char  file[KVS_FILE_CAP];
    int   npast;
    int   anchor;
    int   anchor2;
    int   dim;
    const float *vec;
} kvs_entry_t;

/* FNV-1a over int32 tokens with a 64-bit salt. */
static inline uint64_t kvs_fnv_tokens(const int32_t *tokens, int32_t n,
                                      uint64_t salt) {
    uint64_t h = 1469598103934665603ULL ^ salt;
    for (int32_t i = 0; i < n; i++) {
        h ^= (uint64_t)(uint32_t)tokens[i];
        h *= 1099511628211ULL;
    }
    /* avalanche the turn salt back in so empty frames still differ by turn */
    h ^= salt >> 29; h *= 1099511628211ULL;
    h ^= h >> 32;
    return h;
}

/* sid = 32 hex: FNV(tokens, salt=turn) + FNV(tokens, salt=~turn). */
static inline void kvs_sid(int32_t turn, const int32_t *tokens, int32_t n,
                           char out[KVS_SID_LEN + 1]) {
    static const char *hexd = "0123456789abcdef";
    uint64_t h1 = kvs_fnv_tokens(tokens, n, (uint64_t)(uint32_t)turn);
    uint64_t h2 = kvs_fnv_tokens(tokens, n, ~(uint64_t)(uint32_t)turn);
    for (int i = 0; i < 16; i++) {
        out[i]      = hexd[(h1 >> (60 - i * 4)) & 0xF];
        out[16 + i] = hexd[(h2 >> (60 - i * 4)) & 0xF];
    }
    out[32] = '\0';
}

/* file = "<tag>/t<turn>" (tag "" -> "kv-frame/t<turn>"). Truncates safely. */
static inline void kvs_file_of(int32_t turn, const char *tag,
                               char out[KVS_FILE_CAP]) {
    const char *t = (tag && tag[0]) ? tag : "kv-frame";
    snprintf(out, KVS_FILE_CAP, "%s/t%d", t, turn);
}

/* KV frame -> section entry. vec (dim floats) is borrowed from the caller.
 * C is the K x dim anchor table (any provenance). Returns 0 ok, -1 bad args.
 * anchor/anchor2 are the top-2 of anch_route (equal iff K == 1). */
static inline int kvs_from_frame(int32_t turn, const int32_t *tokens,
                                 int32_t n_tokens, const float *vec, int dim,
                                 const float *C, int K, const char *tag,
                                 kvs_entry_t *out) {
    if (!out || !vec || !C || K <= 0 || dim <= 0 || n_tokens < 0) return -1;
    if (n_tokens > 0 && !tokens) return -1;
    kvs_sid(turn, tokens, n_tokens, out->sid);
    kvs_file_of(turn, tag, out->file);
    out->npast  = n_tokens;
    out->dim    = dim;
    out->vec    = vec;
    int rb[ANCHR_MAXK];
    int got = anch_route(vec, C, K, dim, 2, rb);
    if (got <= 0) return -1;
    out->anchor  = rb[0];
    out->anchor2 = (got > 1) ? rb[1] : rb[0];
    return 0;
}

/* ── emit: entries -> .jsonl section file (kv_index.jsonl shape) ── */
static inline int kvs_emit_entry(FILE *fp, const kvs_entry_t *e) {
    if (!fp || !e || !e->vec || e->dim <= 0) return -1;
    if (fprintf(fp, "{\"sid\":\"%s\",\"file\":\"%s\",\"npast\":%d,"
                    "\"anchor\":%d,\"anchor2\":%d,\"dim\":%d,\"vec\":[",
                e->sid, e->file, e->npast, e->anchor, e->anchor2,
                e->dim) < 0) return -1;
    for (int d = 0; d < e->dim; d++) {
        if (fprintf(fp, d ? ",%.9g" : "%.9g", (double)e->vec[d]) < 0)
            return -1;
    }
    if (fprintf(fp, "]}\n") < 0) return -1;
    return 0;
}

static inline int kvs_emit_jsonl(const char *path, const kvs_entry_t *ents,
                                 int n) {
    if (!path || !ents || n < 0) return -1;
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    int rc = 0;
    for (int i = 0; i < n; i++)
        if (kvs_emit_entry(fp, &ents[i]) != 0) { rc = -1; break; }
    fclose(fp);
    return rc;
}

/* ── parse back our own emit (verifier / test loader, not a JSON parser) ──
 * Reads one line written by kvs_emit_entry into e; vec_out must hold at
 * least dim floats (dim is read from the line first — caller may call with
 * vec_out=NULL to probe the dim, then again with storage). e->vec aliases
 * vec_out. Returns 0 ok, -1 malformed. */
static inline int kvs_parse_line(const char *line, kvs_entry_t *e,
                                 float *vec_out, int vec_cap) {
    if (!line || !e) return -1;
    const char *p;
    char *end;

    p = strstr(line, "\"sid\":\"");
    if (!p) return -1;
    p += 7;
    if (strlen(p) < KVS_SID_LEN) return -1;
    memcpy(e->sid, p, KVS_SID_LEN);
    e->sid[KVS_SID_LEN] = '\0';

    p = strstr(line, "\"file\":\"");
    if (!p) return -1;
    p += 8;
    size_t fi = 0;
    while (*p && *p != '"' && fi + 1 < KVS_FILE_CAP) e->file[fi++] = *p++;
    if (*p != '"') return -1;
    e->file[fi] = '\0';

    p = strstr(line, "\"npast\":");
    if (!p) return -1;
    e->npast = (int)strtol(p + 8, &end, 10);
    if (end == p + 8) return -1;

    p = strstr(line, "\"anchor\":");
    if (!p) return -1;
    e->anchor = (int)strtol(p + 9, &end, 10);
    if (end == p + 9) return -1;

    p = strstr(line, "\"anchor2\":");
    if (!p) return -1;
    e->anchor2 = (int)strtol(p + 10, &end, 10);
    if (end == p + 10) return -1;

    p = strstr(line, "\"dim\":");
    if (!p) return -1;
    e->dim = (int)strtol(p + 6, &end, 10);
    if (end == p + 6 || e->dim <= 0) return -1;

    p = strstr(line, "\"vec\":[");
    if (!p) return -1;
    p += 7;
    if (vec_out) {
        if (e->dim > vec_cap) return -1;
        for (int d = 0; d < e->dim; d++) {
            while (*p == ' ' || *p == ',') p++;
            if (!*p || *p == ']') return -1;
            vec_out[d] = strtof(p, &end);
            if (end == p) return -1;
            p = end;
        }
        e->vec = vec_out;
    } else {
        e->vec = NULL;
    }
    return 0;
}

#endif /* KV_INDEX_SECTION_H */

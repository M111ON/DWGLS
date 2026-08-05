/* kis_codec_v6.h — Index-Based Mapping: slot[i] = (i*37) % 20736
 * Fixes v5 collision by decoupling weight VALUE from slot ADDRESS.
 * Each weight at index i gets a unique slot regardless of its value.
 * Weight VALUE is stored as data at that slot, not used for addressing.
 *
 * Header: [magic:4][n:4][mode:1][cb_data:...]
 *   mode=0: varint residuals (good for small n / small buffers)
 *   mode=1: bitmap residuals  (good for large n, ratio ≤ 1.13x)
 */
#ifndef KIS_V6_H
#define KIS_V6_H
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define V6_MAGIC  0x4B435636
#define V6_GRID   144
#define V6_SLOTS  (V6_GRID*V6_GRID)   /* 20736 */
#define V6_STRIDE 37                    /* coprime with 20736 */
#define V6_BM_WORDS ((V6_SLOTS + 63) / 64)  /* 324 uint64_t = 2592 bytes */
#define V6_BM_BYTES (V6_BM_WORDS * 8)

/* Index-based helix: each i maps to a unique slot */
static inline uint32_t v6_slot(uint32_t i) {
    return (i * V6_STRIDE) % V6_SLOTS;
}

/* ── Codebook: histogram only (~550B), same structure as v5 ── */
typedef struct { uint32_t h[256]; uint8_t act[32]; uint32_t na, nw; } V6CB;

static void v6_cb_build(V6CB *cb, const int8_t *w, uint32_t n) {
    memset(cb, 0, sizeof(*cb));
    cb->nw = n;
    for (uint32_t i = 0; i < n; i++)
        cb->h[(uint8_t)w[i]]++;
    for (int v = 0; v < 256; v++)
        if (cb->h[v] > 0) { cb->act[v >> 3] |= (1u << (v & 7)); cb->na++; }
}

static uint32_t v6_cb_enc(const V6CB *cb, uint8_t *o, uint32_t cap) {
    if (cap < 36) return 0;
    uint32_t off = 0;
    memcpy(o, &cb->nw, 4); off += 4;
    memcpy(o + off, cb->act, 32); off += 32;
    for (int v = 0; v < 256; v++) {
        uint32_t c = cb->h[v];
        if (!c) continue;
        while (c >= 0x80) { if (off < cap) o[off++] = (c & 0x7F) | 0x80u; c >>= 7; }
        if (off < cap) o[off++] = (uint8_t)c;
    }
    return off;
}

static int v6_cb_dec(const uint8_t *d, uint32_t len, V6CB *cb) {
    if (!d || !cb || len < 36) return -1;
    uint32_t off = 0;
    memcpy(&cb->nw, d, 4); off += 4;
    memcpy(cb->act, d + off, 32); off += 32;
    memset(cb->h, 0, sizeof(cb->h));
    cb->na = 0;
    for (int v = 0; v < 256 && off < len; v++) {
        if (!(cb->act[v >> 3] & (1u << (v & 7)))) continue;
        uint32_t c = 0, s = 0;
        while (off < len) {
            uint8_t b = d[off++];
            c |= (b & 0x7Fu) << s;
            s += 7;
            if (!(b & 0x80)) break;
        }
        cb->h[v] = c;
        cb->na++;
    }
    return 0;
}

static void v6_cb_recon(const V6CB *cb, int8_t *o, uint32_t n) {
    uint32_t p = 0;
    for (int v = 0; v < 256 && p < n; v++)
        for (uint32_t i = 0; i < cb->h[v] && p < n; i++)
            o[p++] = (int8_t)(uint8_t)v;
}

static uint32_t v6_cb_size(const uint8_t *d, uint32_t len) {
    if (len < 36) return 0;
    uint32_t off = 36;
    uint8_t act[32];
    memcpy(act, d + 4, 32);
    for (int v = 0; v < 256; v++) {
        if (!(act[v >> 3] & (1u << (v & 7)))) continue;
        while (off < len) { uint8_t b = d[off++]; if (!(b & 0x80)) break; }
    }
    return off;
}

/* ── Bitmap helpers ── */
static inline void bm_set(uint64_t *bm, uint32_t s) { bm[s >> 6] |= (1ULL << (s & 63)); }
static inline int  bm_test(const uint64_t *bm, uint32_t s) { return (bm[s >> 6] >> (s & 63)) & 1; }

/* ── Varint ── */
static uint32_t v6_vi_enc(uint32_t v, uint8_t *o, uint32_t cap, uint32_t off) {
    while (v >= 0x80 && off < cap) { o[off++] = (v & 0x7F) | 0x80u; v >>= 7; }
    if (off < cap) o[off++] = (uint8_t)v;
    return off;
}
static void v6_vi_dec(const uint8_t *d, uint32_t len, uint32_t *off, uint32_t *v) {
    *v = 0; uint32_t s = 0;
    while (*off < len) {
        uint8_t b = d[*off];
        *v |= (b & 0x7Fu) << s;
        s += 7;
        (*off)++;
        if (!(b & 0x80)) break;
    }
}

/* ── Encode: codebook + per-chunk residual ── */
static uint32_t v6_encode(const int8_t *w, uint32_t n, uint8_t *o, uint32_t cap) {
    if (!w || !o || cap < 64 || n == 0) return 0;

    V6CB cb;
    v6_cb_build(&cb, w, n);

    int8_t *sorted = (int8_t *)malloc(n);
    if (!sorted) return 0;
    v6_cb_recon(&cb, sorted, n);

    /* Choose mode: bitmap if buffer can hold header+cb+all_bitmaps+worst_case_diffs */
    uint32_t n_chunks = (n + V6_SLOTS - 1) / V6_SLOTS;
    uint32_t min_bitmap = 9 + V6_BM_BYTES * n_chunks + n;  /* header(9) + bitmaps + worst-case diffs */
    uint8_t mode = (cap >= min_bitmap) ? 1 : 0;

    uint32_t off = 0;
    uint32_t magic = V6_MAGIC;
    memcpy(o + off, &magic, 4); off += 4;
    memcpy(o + off, &n, 4); off += 4;
    o[off++] = mode;  /* 1 byte mode flag */

    uint32_t cb_len = v6_cb_enc(&cb, o + off, cap - off);
    off += cb_len;

    int8_t *og = (int8_t *)malloc(V6_SLOTS);
    int8_t *sg = (int8_t *)malloc(V6_SLOTS);
    uint64_t *bm = (mode) ? (uint64_t *)calloc(V6_BM_WORDS, 8) : NULL;
    if (!og || !sg) { free(sorted); free(og); free(sg); free(bm); return 0; }

    for (uint32_t ch = 0; ch < n_chunks; ch++) {
        uint32_t start = ch * V6_SLOTS;
        uint32_t bw = (start + V6_SLOTS <= n) ? V6_SLOTS : (n - start);

        memset(og, 0, V6_SLOTS);
        memset(sg, 0, V6_SLOTS);

        for (uint32_t i = 0; i < bw; i++) {
            uint32_t s = v6_slot(i);
            og[s] = w[start + i];
            sg[s] = sorted[start + i];
        }

        if (mode && bm) {
            /* ── Bitmap mode ── */
            memset(bm, 0, V6_BM_BYTES);
            uint32_t nz = 0;
            for (uint32_t s = 0; s < V6_SLOTS; s++)
                if (og[s] != sg[s]) { bm_set(bm, s); nz++; }
            if (off + V6_BM_BYTES + nz > cap) {
                free(sorted); free(og); free(sg); free(bm); return 0;
            }
            memcpy(o + off, bm, V6_BM_BYTES); off += V6_BM_BYTES;
            for (uint32_t s = 0; s < V6_SLOTS; s++)
                if (bm_test(bm, s))
                    o[off++] = (uint8_t)(og[s] ^ sg[s]);
        } else {
            /* ── Varint mode ── */
            uint32_t nz = 0;
            for (uint32_t s = 0; s < V6_SLOTS; s++)
                if (og[s] != sg[s]) nz++;
            if (off + 4 > cap) { free(sorted); free(og); free(sg); free(bm); return 0; }
            memcpy(o + off, &nz, 4); off += 4;
            uint32_t prev = 0;
            for (uint32_t s = 0; s < V6_SLOTS; s++) {
                if (og[s] == sg[s]) continue;
                off = v6_vi_enc(s - prev, o, cap, off);
                if (off + 1 > cap) { free(sorted); free(og); free(sg); free(bm); return 0; }
                o[off++] = (uint8_t)(og[s] ^ sg[s]);
                prev = s;
            }
        }
    }

    free(sorted); free(og); free(sg); free(bm);
    return off;
}
#define kis_v6_encode v6_encode

/* ── Decode ── */
static int v6_decode(const uint8_t *d, uint32_t dlen, int8_t *out, uint32_t n) {
    if (!d || !out || dlen < 13 || n == 0) return -1;

    uint32_t off = 0, magic;
    memcpy(&magic, d, 4); off += 4;
    if (magic != V6_MAGIC) return -2;

    uint32_t nn;
    memcpy(&nn, d + off, 4); off += 4;
    if (nn != n) return -3;

    uint8_t mode = d[off++];  /* read mode flag */

    V6CB cb;
    if (v6_cb_dec(d + off, dlen - off, &cb) != 0) return -4;
    uint32_t cb_total = v6_cb_size(d + off, dlen - off);
    off += cb_total;

    int8_t *sorted = (int8_t *)malloc(n);
    if (!sorted) return -5;
    v6_cb_recon(&cb, sorted, n);

    int8_t *sg = (int8_t *)malloc(V6_SLOTS);
    uint64_t *bm = (mode) ? (uint64_t *)malloc(V6_BM_BYTES) : NULL;
    if (!sg) { free(sorted); free(bm); return -6; }

    uint32_t n_chunks = (n + V6_SLOTS - 1) / V6_SLOTS;

    for (uint32_t ch = 0; ch < n_chunks; ch++) {
        uint32_t start = ch * V6_SLOTS;
        uint32_t bw = (start + V6_SLOTS <= n) ? V6_SLOTS : (n - start);

        memset(sg, 0, V6_SLOTS);
        for (uint32_t i = 0; i < bw; i++)
            sg[v6_slot(i)] = sorted[start + i];

        if (mode && bm) {
            /* Bitmap mode */
            if (off + V6_BM_BYTES > dlen) { free(sorted); free(sg); free(bm); return -7; }
            memcpy(bm, d + off, V6_BM_BYTES); off += V6_BM_BYTES;

            uint32_t nz = 0;
            for (int wi = 0; wi < V6_BM_WORDS; wi++) {
                uint64_t v = bm[wi];
                while (v) { v &= v - 1; nz++; }
            }
            if (off + nz > dlen) { free(sorted); free(sg); free(bm); return -8; }
            for (uint32_t s = 0; s < V6_SLOTS; s++)
                if (bm_test(bm, s))
                    sg[s] ^= d[off++];
        } else {
            /* Varint mode */
            if (off + 4 > dlen) { free(sorted); free(sg); free(bm); return -9; }
            uint32_t nz;
            memcpy(&nz, d + off, 4); off += 4;
            uint32_t prev = 0;
            for (uint32_t i = 0; i < nz && off < dlen; i++) {
                uint32_t delta;
                v6_vi_dec(d, dlen, &off, &delta);
                uint32_t s = prev + delta;
                if (s >= V6_SLOTS) { free(sorted); free(sg); free(bm); return -10; }
                if (off >= dlen) { free(sorted); free(sg); free(bm); return -11; }
                sg[s] ^= d[off++];
                prev = s;
            }
        }

        for (uint32_t i = 0; i < bw; i++)
            out[start + i] = sg[v6_slot(i)];
    }

    free(sorted); free(sg); free(bm);
    return 0;
}
#define kis_v6_decode v6_decode

/* ════════════════════════════════════════════════════════
   PRODUCTION API — buffer-size-first pattern
   ════════════════════════════════════════════════════════ */

/* v6_encode_buf — query required buffer size, then encode.
 * Pass buf=NULL, cap=0 to get size only. */
static inline uint32_t v6_encode_buf(const int8_t *w, uint32_t n,
                                     uint8_t *buf, uint32_t cap) {
    if (!w || n == 0) return 0;
    /* worst case: header(9) + cb(~580) + bitmap(2592*n_chunks) + n */
    uint32_t n_chunks = (n + V6_SLOTS - 1) / V6_SLOTS;
    uint32_t need = 9 + 580 + (uint32_t)(V6_BM_BYTES * n_chunks) + n + 64;
    if (!buf || cap < need) return need;
    return v6_encode(w, n, buf, cap);
}

/* v6_decode_buf — decode wrapper with consistent naming */
static inline int v6_decode_buf(const uint8_t *d, uint32_t dlen,
                                int8_t *out, uint32_t n) {
    return v6_decode(d, dlen, out, n);
}

/* v6_verify — compare decoded output against original, return 1 if lossless */
static inline int v6_verify(const uint8_t *d, uint32_t dlen,
                            const int8_t *ref, uint32_t n) {
    int8_t *tmp = (int8_t *)malloc(n);
    if (!tmp) return 0;
    int rc = v6_decode(d, dlen, tmp, n);
    if (rc != 0) { free(tmp); return 0; }
    int ok = (memcmp(tmp, ref, n) == 0);
    free(tmp);
    return ok;
}

/* v6_ratio — compressed size / raw size */
static inline float v6_ratio(const uint8_t *d, uint32_t dlen, uint32_t n) {
    (void)d; (void)dlen;
    return n > 0 ? (float)dlen / n : 0.0f;
}

#endif /* KIS_V6_H */

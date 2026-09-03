/* ═══════════════════════════════════════════════════════════════════════════
 * kis_codec_v6b.h — v6 + multi-type (int8 | fp16) + streaming + reduced malloc
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Inherits v6's proven core (stride-37 bijection, codebook + XOR residual).
 * Adds:
 *   1. v6_etype = Q8 (1 byte/elem) | FP16 (2 bytes/elem)
 *   2. v6b_stream_t stateful API: init → feed chunks → finalize
 *      (scratch buffer allocated ONCE = V6_SLOTS × value_size, reused)
 *   3. FP16 codebook = dense 8 KiB bitmap (65536 bits) + varint counts
 *
 * Sacred: stride=37, grid=144, slots=20736 — kept from v6.
 * Invariant: stride-37 × 20736 = bijection, guaranteed lossless.
 * ═══════════════════════════════════════════════════════════════════════════ */
#ifndef KIS_V6B_H
#define KIS_V6B_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define V6B_MAGIC  0x4B435636u   /* "V6bK" — distinct from v6 (0x4B435636) */
#define V6B_STRIDE 37u
#define V6B_GRID   144u
#define V6B_SLOTS  (V6B_GRID * V6B_GRID)        /* 20736 */
#define V6B_BM_W   ((V6B_SLOTS + 63) / 64)      /* 324 */
#define V6B_BM_B   (V6B_BM_W * 8)               /* 2592 */

/* Element types */
typedef enum { V6B_Q8 = 1, V6B_FP16 = 2 } v6_etype;

static inline uint32_t v6b_slot(uint32_t i) { return (i * V6B_STRIDE) % V6B_SLOTS; }
static inline uint32_t v6b_ebsz(v6_etype t) { return (t == V6B_FP16) ? 2u : 1u; }

/* ── Stride-37 bijection proof (compile-time check) ── */
static inline int v6b_check_bijection(void) {
    uint8_t seen[V6B_SLOTS];
    memset(seen, 0, V6B_SLOTS);
    for (uint32_t i = 0; i < V6B_SLOTS; i++) {
        uint32_t s = v6b_slot(i);
        if (s >= V6B_SLOTS || seen[s]) return 0;
        seen[s] = 1;
    }
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * STREAM STATE — one alloc, reused across chunks
 * ═══════════════════════════════════════════════════════════════════════════ */
typedef struct {
    v6_etype  type;
    uint32_t  n;           /* total elements seen so far */
    uint32_t  chunk;       /* current chunk index (0-based) */
    uint8_t  *og;          /* scratch: V6B_SLOTS × eb_size */
    uint8_t  *sg;          /* scratch: V6B_SLOTS × eb_size */
    uint8_t  *orig;        /* full original data (for codebook + encode) */
    uint8_t  *sorted;      /* full sorted data (for codebook + encode) */
    uint8_t  *cb;          /* codebook: bitmap + counts */
    uint32_t  cb_cap;      /* codebook capacity (bytes) */
    uint32_t  n_active;    /* distinct values in codebook */
    /* Per-chunk residual output goes to user-provided buffer */
} v6b_stream_t;

/* ── Codebook: bitmap (256 bits for Q8, 65536 bits for FP16) + varint counts ── */
static inline uint32_t v6b_cb_bm_bytes(v6_etype t) {
    return (t == V6B_FP16) ? 8192u : 32u;
}
static inline uint32_t v6b_cb_bm_bits(v6_etype t) {
    return (t == V6B_FP16) ? 65536u : 256u;
}
static inline void v6b_cb_set(uint8_t *bm, uint32_t v) {
    bm[v >> 3] |= (uint8_t)(1u << (v & 7));
}
static inline int  v6b_cb_test(const uint8_t *bm, uint32_t v) {
    return (bm[v >> 3] >> (v & 7)) & 1;
}

/* Build codebook from unsorted input. Bitmap + per-active-value count (varint).
 * For Q8: scan all bytes, set bitmap bit per value, count occurrences.
 * For FP16: scan all 16-bit values, same approach. */
static uint32_t v6b_cb_build(v6_etype t, const uint8_t *data, uint32_t n,
                              uint8_t *cb, uint32_t cap) {
    uint32_t bm_bytes = v6b_cb_bm_bytes(t);
    (void)v6b_ebsz(t);
    if (cap < bm_bytes + 4) return 0;
    memset(cb, 0, bm_bytes);
    uint32_t off = bm_bytes;

    /* Q8: histogram[256] scratch. FP16: histogram in cb above bitmap. */
    if (t == V6B_Q8) {
        uint32_t h[256] = {0};
        for (uint32_t i = 0; i < n; i++) h[data[i]]++;
        for (uint32_t v = 0; v < 256; v++) {
            if (!h[v]) continue;
            v6b_cb_set(cb, v);
            uint32_t c = h[v];
            while (c >= 0x80) { if (off < cap) cb[off++] = (c & 0x7F) | 0x80u; c >>= 7; }
            if (off < cap) cb[off++] = (uint8_t)c;
        }
    } else { /* FP16 */
        uint32_t *h = (uint32_t *)calloc(65536, sizeof(uint32_t));
        if (!h) return 0;
        for (uint32_t i = 0; i < n; i++) {
            uint16_t v = (uint16_t)data[2*i] | ((uint16_t)data[2*i+1] << 8);
            h[v]++;
        }
        for (uint32_t v = 0; v < 65536; v++) {
            if (!h[v]) continue;
            v6b_cb_set(cb, v);
            uint32_t c = h[v];
            while (c >= 0x80) { if (off < cap) cb[off++] = (c & 0x7F) | 0x80u; c >>= 7; }
            if (off < cap) cb[off++] = (uint8_t)c;
        }
        free(h);
    }
    /* ponytail: cap-exceeded codebook silently truncates distinct values,
     * so decoder reconstructs wrong sorted[]. Caller must size cb >= bm_bytes + n
     * (worst case: 1 varint byte per active element). Add this check at header time. */
    return off;
}

/* Reconstruct sorted[] from codebook. Returns bytes consumed from cb. */
static uint32_t v6b_cb_recon(v6_etype t, const uint8_t *cb, uint32_t cb_len,
                              uint8_t *sorted, uint32_t n) {
    uint32_t bm_bytes = v6b_cb_bm_bytes(t);
    (void)v6b_ebsz(t);
    if (cb_len < bm_bytes) return 0;
    uint32_t off = bm_bytes;
    uint32_t p = 0;
    uint32_t bits = v6b_cb_bm_bits(t);
    for (uint32_t v = 0; v < bits && p < n; v++) {
        if (!v6b_cb_test(cb, v)) continue;
        /* varint count */
        uint32_t c = 0, s = 0;
        while (off < cb_len) {
            uint8_t b = cb[off++];
            c |= (uint32_t)(b & 0x7F) << s;
            s += 7;
            if (!(b & 0x80)) break;
        }
        for (uint32_t k = 0; k < c && p < n; k++, p++) {
            if (t == V6B_FP16) {
                sorted[2*p]   = (uint8_t)(v & 0xFF);
                sorted[2*p+1] = (uint8_t)((v >> 8) & 0xFF);
            } else {
                sorted[p] = (uint8_t)v;
            }
        }
    }
    return off;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * STREAMING ENCODE
 * ═══════════════════════════════════════════════════════════════════════════ */

static int v6b_init(v6b_stream_t *st, v6_etype t) {
    if (!st) return -1;
    uint32_t eb = v6b_ebsz(t);
    st->type = t;
    st->n = 0;
    st->chunk = 0;
    st->n_active = 0;
    st->og = NULL;
    st->sg = NULL;
    st->orig = NULL;
    st->sorted = NULL;
    st->cb = NULL;
    st->cb_cap = 0;
    st->og = (uint8_t *)calloc(V6B_SLOTS, eb);
    st->sg = (uint8_t *)calloc(V6B_SLOTS, eb);
    /* cb cap = bm_bytes + worst-case 1 varint per element. n_max caller passes via v6b_collect;
     * use a generous default. If caller knows n, prefer pre-sizing via v6b_collect first. */
    st->cb = (uint8_t *)malloc(v6b_cb_bm_bytes(t) + 4096u);
    st->cb_cap = v6b_cb_bm_bytes(t) + 4096u;
    if (!st->og || !st->sg || !st->cb) {
        free(st->og); free(st->sg); free(st->cb);
        st->og = st->sg = st->cb = NULL;
        return -2;
    }
    return 0;
}

static void v6b_free(v6b_stream_t *st) {
    if (!st) return;
    free(st->og); free(st->sg); free(st->orig); free(st->sorted); free(st->cb);
    st->og = st->sg = st->orig = st->sorted = st->cb = NULL;
}

/* Two-pass: first pass collects full data into orig[] (input buffer);
 * v6b_header() builds codebook + sorted[]; v6b_emit() writes per-chunk residuals. */

/* Pass 1: collect n bytes of data. Allocates orig only (sorted built in v6b_header). */
static int v6b_collect(v6b_stream_t *st, const void *data, uint32_t n_elems) {
    if (!st || !data) return -1;
    uint32_t eb = v6b_ebsz(st->type);
    uint32_t bytes = n_elems * eb;
    if (!st->orig) {
        st->orig = (uint8_t *)malloc(bytes);
        if (!st->orig) return -2;
    } else {
        uint8_t *g = (uint8_t *)realloc(st->orig, st->n * eb + bytes);
        if (!g) return -2;
        st->orig = g;
    }
    memcpy(st->orig + st->n * eb, data, bytes);
    st->n += n_elems;
    return 0;
}

/* Pass 2: emit one chunk's residual. Returns bytes written, 0 on done. */
static uint32_t v6b_emit(v6b_stream_t *st, uint8_t *out, uint32_t cap) {
    if (!st || !out || !st->orig || !st->sorted) return 0;
    uint32_t eb = v6b_ebsz(st->type);
    uint32_t start = st->chunk * V6B_SLOTS;
    if (start >= st->n) return 0;
    uint32_t bw = (start + V6B_SLOTS <= st->n) ? V6B_SLOTS : (st->n - start);

    memset(st->og, 0, V6B_SLOTS * eb);
    memset(st->sg, 0, V6B_SLOTS * eb);

    /* og: original at stride-37 slot; sg: sorted at same slot */
    for (uint32_t i = 0; i < bw; i++) {
        uint32_t s = v6b_slot(i);
        memcpy(st->og + s * eb, st->orig + (start + i) * eb, eb);
        memcpy(st->sg + s * eb, st->sorted + (start + i) * eb, eb);
    }
    /* count differing slots (compute before allocating) */
    uint32_t nz = 0;
    for (uint32_t s = 0; s < V6B_SLOTS; s++) {
        if (memcmp(st->og + s * eb, st->sg + s * eb, eb) != 0) nz++;
    }
    /* worst case per nz: 5 varint + eb */
    uint32_t need = 4 + nz * (5u + eb);
    if (cap < need) return 0;

    uint32_t off = 0;
    out[off++] = (uint8_t)(nz & 0xFF);
    out[off++] = (uint8_t)((nz >> 8) & 0xFF);
    out[off++] = (uint8_t)((nz >> 16) & 0xFF);
    out[off++] = (uint8_t)((nz >> 24) & 0xFF);
    uint32_t prev = 0;
    for (uint32_t s = 0; s < V6B_SLOTS; s++) {
        if (memcmp(st->og + s * eb, st->sg + s * eb, eb) == 0) continue;
        uint32_t delta = s - prev;
        while (delta >= 0x80) { out[off++] = (delta & 0x7F) | 0x80u; delta >>= 7; }
        out[off++] = (uint8_t)delta;
        for (uint32_t k = 0; k < eb; k++) out[off++] = st->og[s*eb+k] ^ st->sg[s*eb+k];
        prev = s;
    }
    st->chunk++;
    return off;
}

/* Final header: magic(4) + n(4) + type(1) + cb_len(2) + cb[cb_len] */
static uint32_t v6b_header(v6b_stream_t *st, uint8_t *out, uint32_t cap) {
    if (!st) return 0;
    uint32_t bm = v6b_cb_bm_bytes(st->type);
    /* Worst-case codebook: bm + n varint bytes (1 byte each if counts ≤ 127). */
    uint32_t need_cap = bm + st->n;
    if (st->cb_cap < need_cap) {
        uint8_t *nb = (uint8_t *)realloc(st->cb, need_cap);
        if (!nb) return 0;
        st->cb = nb;
        st->cb_cap = need_cap;
    }
    /* build codebook + sorted (must rebuild here for cap-correct codebook) */
    if (!st->sorted) {
        st->sorted = (uint8_t *)malloc(st->n * v6b_ebsz(st->type));
        if (!st->sorted) return 0;
    }
    uint32_t cb_len = v6b_cb_build(st->type, st->orig, st->n, st->cb, st->cb_cap);
    if (cb_len == 0) return 0;
    if (v6b_cb_recon(st->type, st->cb, cb_len, st->sorted, st->n) == 0) return 0;
    uint32_t bits = v6b_cb_bm_bits(st->type);
    uint32_t na = 0;
    for (uint32_t v = 0; v < bits; v++) na += v6b_cb_test(st->cb, v);
    st->n_active = na;
    uint32_t need = 4 + 4 + 1 + 2 + cb_len;
    if (!out || cap < need) return need;
    uint32_t off = 0;
    out[off++] = (uint8_t)(V6B_MAGIC & 0xFF);
    out[off++] = (uint8_t)((V6B_MAGIC >> 8) & 0xFF);
    out[off++] = (uint8_t)((V6B_MAGIC >> 16) & 0xFF);
    out[off++] = (uint8_t)((V6B_MAGIC >> 24) & 0xFF);
    out[off++] = (uint8_t)(st->n & 0xFF);
    out[off++] = (uint8_t)((st->n >> 8) & 0xFF);
    out[off++] = (uint8_t)((st->n >> 16) & 0xFF);
    out[off++] = (uint8_t)((st->n >> 24) & 0xFF);
    out[off++] = (uint8_t)st->type;
    out[off++] = (uint8_t)(cb_len & 0xFF);
    out[off++] = (uint8_t)((cb_len >> 8) & 0xFF);
    memcpy(out + off, st->cb, cb_len);
    off += cb_len;
    return off;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * STREAMING DECODE
 * ═══════════════════════════════════════════════════════════════════════════ */
typedef struct {
    v6_etype type;
    uint32_t n;
    uint32_t chunk;
    uint8_t *sg;          /* scratch: V6B_SLOTS × eb_size */
    uint8_t *sorted;      /* full sorted reconstruction */
    uint32_t cb_len;
    uint32_t off;         /* current read offset into src */
    const uint8_t *src;   /* input buffer */
    uint32_t src_len;
} v6b_dec_t;

static int v6b_dec_init(v6b_dec_t *d, const uint8_t *src, uint32_t src_len) {
    if (!d || !src || src_len < 11) return -1;
    uint32_t off = 0;
    uint32_t magic = (uint32_t)src[0] | ((uint32_t)src[1]<<8)
                   | ((uint32_t)src[2]<<16) | ((uint32_t)src[3]<<24);
    off += 4;
    if (magic != V6B_MAGIC) return -2;
    uint32_t n = (uint32_t)src[off] | ((uint32_t)src[off+1]<<8)
               | ((uint32_t)src[off+2]<<16) | ((uint32_t)src[off+3]<<24);
    off += 4;
    v6_etype t = (v6_etype)src[off++];
    if (t != V6B_Q8 && t != V6B_FP16) return -3;
    uint32_t cb_len = (uint32_t)src[off] | ((uint32_t)src[off+1]<<8);
    off += 2;
    if (off + cb_len > src_len) return -4;
    uint32_t eb = v6b_ebsz(t);
    d->type = t;
    d->n = n;
    d->chunk = 0;
    d->off = off + cb_len;
    d->src = src;
    d->src_len = src_len;
    d->cb_len = cb_len;
    d->sg = (uint8_t *)calloc(V6B_SLOTS, eb);
    d->sorted = (uint8_t *)malloc(n * eb);
    if (!d->sg || !d->sorted) { free(d->sg); free(d->sorted); return -5; }
    if (v6b_cb_recon(t, src + off, cb_len, d->sorted, n) == 0) {
        free(d->sg); free(d->sorted); return -6;
    }
    return 0;
}

static void v6b_dec_free(v6b_dec_t *d) {
    if (!d) return;
    free(d->sg); free(d->sorted);
    d->sg = d->sorted = NULL;
}

/* Decode one chunk into out_buf. Returns bytes written, 0 when done. */
static uint32_t v6b_dec_chunk(v6b_dec_t *d, uint8_t *out_buf, uint32_t out_cap) {
    if (!d || !out_buf) return 0;
    uint32_t eb = v6b_ebsz(d->type);
    uint32_t start = d->chunk * V6B_SLOTS;
    if (start >= d->n) return 0;
    uint32_t bw = (start + V6B_SLOTS <= d->n) ? V6B_SLOTS : (d->n - start);
    if (out_cap < bw * eb) return 0;
    if (d->off + 4 > d->src_len) return 0;

    memset(d->sg, 0, V6B_SLOTS * eb);
    for (uint32_t i = 0; i < bw; i++) {
        uint32_t s = v6b_slot(i);
        memcpy(d->sg + s * eb, d->sorted + (start + i) * eb, eb);
    }

    uint32_t nz = (uint32_t)d->src[d->off] | ((uint32_t)d->src[d->off+1]<<8)
                | ((uint32_t)d->src[d->off+2]<<16) | ((uint32_t)d->src[d->off+3]<<24);
    d->off += 4;
    uint32_t prev = 0;
    for (uint32_t i = 0; i < nz; i++) {
        if (d->off >= d->src_len) return 0;
        uint32_t delta = 0, s = 0;
        while (d->off < d->src_len) {
            uint8_t b = d->src[d->off++];
            delta |= (uint32_t)(b & 0x7F) << s;
            s += 7;
            if (!(b & 0x80)) break;
        }
        uint32_t slot = prev + delta;
        if (slot >= V6B_SLOTS) return 0;
        if (d->off + eb > d->src_len) return 0;
        for (uint32_t k = 0; k < eb; k++) d->sg[slot*eb+k] ^= d->src[d->off++];
        prev = slot;
    }
    for (uint32_t i = 0; i < bw; i++) {
        uint32_t s = v6b_slot(i);
        memcpy(out_buf + i * eb, d->sg + s * eb, eb);
    }
    d->chunk++;
    return bw * eb;
}

/* Decode everything at once. Returns total bytes written or 0 on err. */
static uint32_t v6b_decode_all(const uint8_t *src, uint32_t src_len,
                                uint8_t *out, uint32_t out_cap) {
    v6b_dec_t d = {0};
    if (v6b_dec_init(&d, src, src_len) != 0) return 0;
    uint32_t eb = v6b_ebsz(d.type);
    uint32_t need = d.n * eb;
    if (out_cap < need) { v6b_dec_free(&d); return 0; }
    uint32_t total = 0;
    while (1) {
        uint32_t got = v6b_dec_chunk(&d, out + total, out_cap - total);
        if (got == 0) break;
        total += got;
    }
    v6b_dec_free(&d);
    return total;
}

/* Verify: decode and memcmp against ref. */
static int v6b_verify(const uint8_t *src, uint32_t src_len,
                       const void *ref, uint32_t n_elems) {
    v6_etype t = (v6_etype)src[8];
    uint32_t eb = v6b_ebsz(t);
    uint8_t *tmp = (uint8_t *)malloc(n_elems * eb);
    if (!tmp) return 0;
    uint32_t got = v6b_decode_all(src, src_len, tmp, n_elems * eb);
    int ok = (got == n_elems * eb) && (memcmp(tmp, ref, n_elems * eb) == 0);
    free(tmp);
    return ok;
}

#endif /* KIS_V6B_H */

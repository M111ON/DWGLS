/* ═══════════════════════════════════════════════════════════════════════════
 * kis_codec_v4.h — Full Codec: Codebook + Position Reconstruction
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * LAYER 1 — Codebook (from v3): active bitmap + RLE counts
 *   Tells us: which values exist, how many of each
 *   Size: ~550B for ANY model
 *
 * LAYER 2 — Position Permutation (NEW): where each weight goes
 *   Encodes: original_index[sorted_pos] for each weight
 *   Compression: delta encoding + varint (small deltas → small output)
 *
 * DECODE PIPELINE:
 *   1. Read codebook → sorted_values[0..N-1]
 *   2. Read permutation → original_positions[0..N-1]
 *   3. output[original_positions[i]] = sorted_values[i]
 *
 * LOSSLESS: sorted_values are from codebook (proven), permutation is exact
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifndef KIS_CODEC_V4_H
#define KIS_CODEC_V4_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ═══════ LAYER 1: CODEBOOK (from v3) ═══════════════════════════════════════ */

typedef struct {
    uint32_t histo[256];    /* count per Q8 value (0..255) */
    uint8_t  active[32];    /* bitmap: which values present */
    uint32_t n_active;      /* distinct values */
    uint32_t n_weights;     /* total weights */
} KIS_V4_Codebook;

static void kis_v4_codebook_build(KIS_V4_Codebook *cb, const int8_t *weights, uint32_t n) {
    memset(cb, 0, sizeof(*cb));
    cb->n_weights = n;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t v = (uint8_t)weights[i];
        cb->histo[v]++;
    }
    for (int v = 0; v < 256; v++) {
        if (cb->histo[v] > 0) {
            cb->active[v >> 3] |= (uint8_t)(1u << (v & 7));
            cb->n_active++;
        }
    }
}

/* Encode codebook → compact buffer */
static uint32_t kis_v4_codebook_encode(const KIS_V4_Codebook *cb,
                                        uint8_t *out, uint32_t cap) {
    if (!cb || !out || cap < 36) return 0;
    uint32_t off = 0;

    /* No magic here — parent header has KCV4 */
    memcpy(out + off, &cb->n_weights, 4); off += 4;
    memcpy(out + off, cb->active, 32); off += 32;

    for (int v = 0; v < 256; v++) {
        uint32_t cnt = cb->histo[v];
        if (cnt == 0) continue;
        while (cnt >= 0x80) {
            if (off >= cap) return off;
            out[off++] = (uint8_t)(cnt & 0x7F) | 0x80u;
            cnt >>= 7;
        }
        if (off >= cap) return off;
        out[off++] = (uint8_t)cnt;
    }
    return off;
}

/* Decode codebook from buffer */
static int kis_v4_codebook_decode(const uint8_t *data, uint32_t data_len,
                                   KIS_V4_Codebook *cb) {
    if (!data || !cb || data_len < 36) return -1;
    uint32_t off = 0;

    /* No magic check here — parent header has KCV4 */
    memcpy(&cb->n_weights, data + off, 4); off += 4;
    memcpy(cb->active, data + off, 32); off += 32;

    memset(cb->histo, 0, sizeof(cb->histo));
    cb->n_active = 0;
    for (int v = 0; v < 256 && off < data_len; v++) {
        if (!(cb->active[v >> 3] & (1u << (v & 7)))) continue;
        uint32_t cnt = 0, shift = 0;
        for (; off < data_len; ) {
            uint8_t byte = data[off++];
            cnt |= (uint32_t)(byte & 0x7F) << shift;
            shift += 7;
            if (!(byte & 0x80)) break;
        }
        cb->histo[v] = cnt;
        cb->n_active++;
    }
    return 0;
}

/* Reconstruct sorted weights from codebook */
static void kis_v4_codebook_reconstruct(const KIS_V4_Codebook *cb,
                                         int8_t *output, uint32_t output_n) {
    if (!cb || !output) return;
    uint32_t pos = 0;
    for (int v = 0; v < 256 && pos < output_n; v++) {
        uint32_t cnt = cb->histo[v];
        int8_t val = (int8_t)(uint8_t)v;
        for (uint32_t i = 0; i < cnt && pos < output_n; i++) {
            output[pos++] = val;
        }
    }
}

/* ═══════ LAYER 2: POSITION PERMUTATION ═══════════════════════════════════════ */

/*
 * Permutation encoding strategy:
 *   1. Sort weights by value → sorted_values[0..N-1]
 *   2. Track original index for each sorted position → perm[i] = original_index
 *   3. Delta encode: delta[i] = perm[i] - perm[i-1] (with perm[-1] = 0)
 *   4. Varint encode each delta (small deltas → 1-2 bytes)
 *
 * This exploits locality: nearby weights in sorted order often have
 * nearby original indices (especially for structured data like NN weights).
 */

/* Zigzag varint encode int32 (signed) → output buffer, returns bytes written */
static uint32_t varint_encode_int(int32_t val, uint8_t *out, uint32_t cap, uint32_t off) {
    /* Zigzag: 0, -1, 1, -2, 2, ... → 0, 1, 2, 3, 4, ... */
    uint32_t uval = (val >= 0) ? (uint32_t)(val << 1) : (uint32_t)((-val << 1) - 1);
    while (uval >= 0x80 && off < cap) {
        out[off++] = (uint8_t)(uval & 0x7F) | 0x80u;
        uval >>= 7;
    }
    if (off < cap) out[off++] = (uint8_t)uval;
    return off;
}

/* Zigzag varint decode → signed int32, returns new offset */
static uint32_t varint_decode_int(const uint8_t *data, uint32_t data_len,
                                   uint32_t off, int32_t *val) {
    uint32_t uval = 0;
    uint32_t shift = 0;
    while (off < data_len) {
        uint8_t byte = data[off++];
        uval |= (uint32_t)(byte & 0x7F) << shift;
        shift += 7;
        if (!(byte & 0x80)) break;
    }
    *val = (uval & 1) ? -((int32_t)(uval >> 1) + 1) : (int32_t)(uval >> 1);
    return off;
}

/* Varint encode uint32 → output buffer, returns bytes written */
static uint32_t varint_encode(uint32_t val, uint8_t *out, uint32_t cap, uint32_t off) {
    while (val >= 0x80 && off < cap) {
        out[off++] = (uint8_t)(val & 0x7F) | 0x80u;
        val >>= 7;
    }
    if (off < cap) out[off++] = (uint8_t)val;
    return off;
}

/* Varint decode from buffer → value, returns new offset */
static uint32_t varint_decode(const uint8_t *data, uint32_t data_len,
                               uint32_t off, uint32_t *val) {
    *val = 0;
    uint32_t shift = 0;
    while (off < data_len) {
        uint8_t byte = data[off++];
        *val |= (uint32_t)(byte & 0x7F) << shift;
        shift += 7;
        if (!(byte & 0x80)) break;
    }
    return off;
}

/*
 * Encode permutation from original weights.
 *
 * Input:  weights[0..N-1] — original Q8 weights
 * Output: out[0..cap-1] — encoded permutation buffer
 * Returns: bytes written
 *
 * Algorithm:
 *   1. Create sorted index array: sorted_idx[i] = index of i-th smallest weight
 *   2. This IS the permutation: sorted_idx[i] tells us where sorted_values[i]
 *      came from in the original array
 *   3. Delta encode: delta[i] = sorted_idx[i] - sorted_idx[i-1]
 *   4. Zigzag varint encode each signed delta
 */
/* Sort comparator: by weight value, then by original index for stability */
typedef struct { int8_t val; uint32_t idx; } SortEntry;
/* Sort comparator: by CODE (matches codebook reconstruct order), then by original index */
static int sort_by_code(const void *a, const void *b) {
    const SortEntry *sa = (const SortEntry *)a;
    const SortEntry *sb = (const SortEntry *)b;
    int code_a = (int)((uint8_t)sa->val);
    int code_b = (int)((uint8_t)sb->val);
    if (code_a != code_b) return code_a - code_b;
    return (int)sa->idx - (int)sb->idx;
}

static uint32_t kis_v4_perm_encode(const int8_t *weights, uint32_t n,
                                     uint8_t *out, uint32_t cap) {
    if (!weights || !out || cap < 8) return 0;

    /* Header: magic + n_weights */
    uint32_t off = 0;
    uint32_t magic = 0x5045524D; /* "PERM" */
    memcpy(out + off, &magic, 4); off += 4;
    memcpy(out + off, &n, 4); off += 4;

    /* Build sorted index array using qsort by CODE */
    SortEntry *entries = (SortEntry *)malloc(n * sizeof(SortEntry));
    if (!entries) return 0;

    for (uint32_t i = 0; i < n; i++) {
        entries[i].val = weights[i];
        entries[i].idx = i;
    }
    qsort(entries, n, sizeof(SortEntry), sort_by_code);

    /* Signed delta encode + zigzag varint */
    int32_t prev = 0;
    for (uint32_t i = 0; i < n; i++) {
        int32_t delta = (int32_t)entries[i].idx - prev;
        off = varint_encode_int(delta, out, cap, off);
        prev = (int32_t)entries[i].idx;
    }

    free(entries);
    return off;
}

/*
 * Decode permutation → sorted_idx[0..N-1]
 *
 * Input:  data[0..data_len-1] — encoded permutation buffer
 * Output: sorted_idx[0..N-1] — decoded sorted index array
 * Returns: 0 on success
 */
static int kis_v4_perm_decode(const uint8_t *data, uint32_t data_len,
                                uint32_t *sorted_idx, uint32_t n) {
    if (!data || !sorted_idx || data_len < 8) return -1;

    uint32_t off = 0;
    uint32_t magic; memcpy(&magic, data + off, 4); off += 4;
    if (magic != 0x5045524D) return -2; /* "PERM" */
    uint32_t stored_n; memcpy(&stored_n, data + off, 4); off += 4;
    if (stored_n != n) return -3;

    int32_t prev = 0;
    for (uint32_t i = 0; i < n; i++) {
        int32_t delta;
        off = varint_decode_int(data, data_len, off, &delta);
        sorted_idx[i] = (uint32_t)(prev + delta);
        prev = (int32_t)sorted_idx[i];
    }

    return 0;
}

/* ═══════ FULL CODEC: ENCODE / DECODE ═══════════════════════════════════════ */

/*
 * Encode weights → codec buffer
 *
 * Format:
 *   [codebook_bytes (varies)] [perm_bytes (varies)]
 *
 * Returns: total bytes written
 */
static uint32_t kis_v4_encode(const int8_t *weights, uint32_t n,
                               uint8_t *out, uint32_t cap) {
    if (!weights || !out || cap < 100) return 0;

    /* Header: magic(4) + cb_size(4) */
    uint32_t off = 0;
    uint32_t magic = 0x4B435634; /* "KCV4" */
    memcpy(out + off, &magic, 4); off += 4;
    off += 4; /* skip cb_size for now, fill later */

    /* Layer 1: Codebook */
    KIS_V4_Codebook cb;
    kis_v4_codebook_build(&cb, weights, n);
    uint32_t cb_bytes = kis_v4_codebook_encode(&cb, out + off, cap - off);
    if (cb_bytes == 0) return 0;

    /* Fill cb_size */
    memcpy(out + 4, &cb_bytes, 4);
    off += cb_bytes;

    /* Layer 2: Permutation */
    uint32_t perm_bytes = kis_v4_perm_encode(weights, n, out + off, cap - off);
    if (perm_bytes == 0) return 0;

    return off + perm_bytes;
}

/*
 * Decode codec buffer → weights
 *
 * Returns: 0 on success
 */
static int kis_v4_decode(const uint8_t *data, uint32_t data_len,
                           int8_t *output, uint32_t output_n) {
    if (!data || !output || data_len < 8) return -1;

    /* Header: magic(4) + cb_size(4) */
    uint32_t off = 0;
    uint32_t magic; memcpy(&magic, data + off, 4); off += 4;
    if (magic != 0x4B435634) return -2; /* "KCV4" */
    uint32_t cb_size; memcpy(&cb_size, data + off, 4); off += 4;

    /* Layer 1: Decode codebook */
    KIS_V4_Codebook cb;
    int rc = kis_v4_codebook_decode(data + off, cb_size, &cb);
    if (rc != 0) return rc;
    if (cb.n_weights != output_n) return -10;
    off += cb_size;

    /* Reconstruct sorted values */
    int8_t *sorted_vals = (int8_t *)malloc(output_n);
    if (!sorted_vals) return -20;
    kis_v4_codebook_reconstruct(&cb, sorted_vals, output_n);

    /* Layer 2: Decode permutation */
    uint32_t *sorted_idx = (uint32_t *)malloc(output_n * sizeof(uint32_t));
    if (!sorted_idx) { free(sorted_vals); return -21; }

    rc = kis_v4_perm_decode(data + off, data_len - off,
                             sorted_idx, output_n);
    if (rc != 0) { free(sorted_vals); free(sorted_idx); return rc; }

    /* Apply permutation: output[sorted_idx[i]] = sorted_vals[i] */
    for (uint32_t i = 0; i < output_n; i++) {
        output[sorted_idx[i]] = sorted_vals[i];
    }

    free(sorted_vals);
    free(sorted_idx);
    return 0;
}

/* ═══════ VERIFICATION ═══════════════════════════════════════════════════════ */

/*
 * Full roundtrip test: encode → decode → compare
 * Returns: number of mismatches (0 = lossless)
 */
static uint32_t kis_v4_roundtrip_test(const int8_t *original, uint32_t n) {
    uint32_t buf_size = n * 2; /* permutation can be up to ~2x for random data */
    uint8_t *buf = (uint8_t *)malloc(buf_size);
    int8_t  *decoded = (int8_t *)malloc(n);
    if (!buf || !decoded) { free(buf); free(decoded); return n; }

    uint32_t encoded = kis_v4_encode(original, n, buf, buf_size);
    int rc = kis_v4_decode(buf, encoded, decoded, n);

    uint32_t mismatches = 0;
    if (rc == 0) {
        for (uint32_t i = 0; i < n; i++) {
            if (decoded[i] != original[i]) mismatches++;
        }
    } else {
        mismatches = n; /* decode failed */
    }

    free(buf);
    free(decoded);
    return mismatches;
}

/* ═══════ PRINT STATS ═══════════════════════════════════════════════════════ */

static void kis_v4_print_stats(const KIS_V4_Codebook *cb, uint32_t codec_bytes) {
    if (!cb) return;
    printf("╔══ KIS CODEC v4 (Full: Codebook + Permutation) ══╗\n");
    printf("║ Weights:    %u\n", cb->n_weights);
    printf("║ Distinct:   %u / 256\n", cb->n_active);
    printf("║ Codec:      %uB\n", codec_bytes);
    printf("║ Raw:        %.2f MB\n", cb->n_weights / 1048576.0);
    printf("║ Ratio:      %.2fx\n",
           cb->n_weights > 0 ? (double)cb->n_weights / codec_bytes : 0);
    printf("╚═════════════════════════════════════════════════╝\n");
}

#endif /* KIS_CODEC_V4_H */

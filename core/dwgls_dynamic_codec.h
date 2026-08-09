/*
 * dwgls_dynamic_codec.h — Dynamic Encode & Compress Storage
 * ════════════════════════════════════════════════════════════════════
 *
 * Automatically classifies data and selects optimal encoding strategy.
 * 5 strategies ranked by compression potential:
 *
 *   SPARSE    — ≤5% non-zero: store (count, position, value) triples
 *   CODEBOOK  — repeated values: histogram + residuals
 *   DELTA     — sequential small diffs: varint encode differences
 *   BITPACK   — structured few-bit data: bitmap + packed values
 *   RAW       — high entropy fallback: store as-is
 *
 * Container format:
 *   [Header 32B][Payload variable][CRC32 4B]
 *
 * PRINCIPLE: MAP not COMPRESS — coordinate = address
 * SACRED: 20736, 1728, 144, 12
 *
 * BUILD: gcc -O2 -Wall -Icore -o test_dynamic tests/test_dynamic_codec.c -lm
 * ════════════════════════════════════════════════════════════════════
 */

#ifndef DWGLS_DYNAMIC_CODEC_H
#define DWGLS_DYNAMIC_CODEC_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ════════════════════════════════════════════════════════════════
   CONSTANTS
   ════════════════════════════════════════════════════════════════ */

#define DYN_MAGIC       0x44594E43u  /* "DYNC" */
#define DYN_VERSION     1u
#define DYN_HEADER_SZ   32u
#define DYN_MAX_SLOTS   20736u
#define DYN_CRC_POLY    0xEDB88320u

/* Encoding strategies */
#define DYN_STRAT_RAW       0u   /* no compression                    */
#define DYN_STRAT_SPARSE    1u   /* ≤5% non-zero → (pos,val) pairs   */
#define DYN_STRAT_CODEBOOK  2u   /* repeated values → histogram       */
#define DYN_STRAT_DELTA     3u   /* sequential small diffs → varint   */
#define DYN_STRAT_BITPACK   4u   /* few active bits → bitmap + pack   */

/* Classification thresholds (from Q8_0 weight analysis) */
#define DYN_SPARSE_THRESHOLD    0.05  /* ≤5% non-zero → SPARSE        */
#define DYN_DELTA_THRESHOLD     0.15  /* avg_diff < 15% range → DELTA  */
#define DYN_BITPACK_THRESHOLD   0.30  /* <30% unique bits → BITPACK    */
#define DYN_CODEBOOK_THRESHOLD  0.40  /* <40% unique values → CODEBOOK */

/* ════════════════════════════════════════════════════════════════
   HEADER (32 bytes, packed)
   ════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t magic;          /* DYN_MAGIC                          */
    uint32_t version;        /* DYN_VERSION                        */
    uint32_t n_elems;        /* number of int8_t elements          */
    uint32_t strategy;       /* DYN_STRAT_*                        */
    uint32_t payload_size;   /* bytes after header (before CRC)    */
    uint32_t raw_size;       /* original uncompressed size          */
    uint32_t checksum;       /* CRC32 of payload                   */
    uint32_t reserved;       /* alignment / future use             */
} DynHeader;

/* ════════════════════════════════════════════════════════════════
   CRC32 (ISO 3309)
   ════════════════════════════════════════════════════════════════ */

static inline uint32_t dyn_crc32(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (DYN_CRC_POLY & (-(int32_t)(crc & 1)));
    }
    return crc ^ 0xFFFFFFFF;
}

/* ════════════════════════════════════════════════════════════════
   DATA CLASSIFIER
   ════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t n_zero;         /* count of zero values               */
    uint32_t n_unique;       /* count of unique non-zero values    */
    uint32_t total_range;    /* max - min of values                */
    uint32_t avg_abs_diff;   /* average absolute difference        */
    uint8_t  histogram[256]; /* value distribution (for Q8: as uint8_t) */
    uint8_t  strategy;       /* recommended DYN_STRAT_*            */
    float    sparsity;       /* fraction of zeros                  */
    float    unique_ratio;   /* unique / total                     */
} DynProfile;

/* Classify int8_t weight array and recommend encoding strategy. */
static inline void dyn_classify(const int8_t *data, uint32_t n,
                                 DynProfile *prof)
{
    if (!data || !prof || n == 0) { if (prof) memset(prof, 0, sizeof(*prof)); return; }

    memset(prof, 0, sizeof(*prof));

    /* Build histogram and count zeros */
    int8_t min_val = data[0], max_val = data[0];
    for (uint32_t i = 0; i < n; i++) {
        prof->histogram[(uint8_t)data[i]]++;
        if (data[i] == 0) prof->n_zero++;
        if (data[i] < min_val) min_val = data[i];
        if (data[i] > max_val) max_val = data[i];
    }
    prof->sparsity = (float)prof->n_zero / (float)n;

    /* Count unique values */
    for (int v = 0; v < 256; v++) {
        if (prof->histogram[v] > 0) prof->n_unique++;
    }
    prof->unique_ratio = (float)prof->n_unique / 256.0f;
    prof->total_range = (uint32_t)(max_val - min_val);

    /* Compute average absolute difference (sequential) */
    uint64_t sum_diff = 0;
    for (uint32_t i = 1; i < n; i++) {
        int32_t d = (int32_t)data[i] - (int32_t)data[i-1];
        sum_diff += (uint32_t)(d < 0 ? -d : d);
    }
    prof->avg_abs_diff = (uint32_t)(sum_diff / (n > 1 ? n - 1 : 1));

    /* ── Strategy Selection ──────────────────────────────────── */
    if (prof->sparsity >= (1.0f - DYN_SPARSE_THRESHOLD)) {
        prof->strategy = DYN_STRAT_SPARSE;
    } else if (prof->unique_ratio <= DYN_CODEBOOK_THRESHOLD) {
        prof->strategy = DYN_STRAT_CODEBOOK;
    } else if (prof->avg_abs_diff < (uint32_t)(prof->total_range * DYN_DELTA_THRESHOLD)) {
        prof->strategy = DYN_STRAT_DELTA;
    } else if (prof->unique_ratio <= DYN_BITPACK_THRESHOLD) {
        prof->strategy = DYN_STRAT_BITPACK;
    } else {
        prof->strategy = DYN_STRAT_RAW;
    }
}

/* ════════════════════════════════════════════════════════════════
   ENCODERS — each writes payload, returns bytes written
   ════════════════════════════════════════════════════════════════ */

/* ── RAW: no compression, just copy ──────────────────────────── */
static inline uint32_t dyn_encode_raw(const int8_t *data, uint32_t n,
                                       uint8_t *out, uint32_t cap)
{
    uint32_t needed = n;  /* 1 byte per element */
    if (needed > cap) return 0;
    memcpy(out, data, n);
    return n;
}

/* ── SPARSE: store (position, value) pairs ─────────────────────
 * Format: [n_nonzero:4][pos:2,val:1]... (compact)
 * position stored as 2 bytes (up to 65535), value as 1 byte.
 */
static inline uint32_t dyn_encode_sparse(const int8_t *data, uint32_t n,
                                          uint8_t *out, uint32_t cap)
{
    /* Count non-zero */
    uint32_t nz = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (data[i] != 0) nz++;
    }

    uint32_t needed = 4 + nz * 3;  /* count + (pos2 + val1) × nz */
    if (needed > cap) return 0;

    uint32_t off = 0;
    memcpy(out + off, &nz, 4); off += 4;

    for (uint32_t i = 0; i < n; i++) {
        if (data[i] != 0) {
            uint16_t pos = (uint16_t)i;
            memcpy(out + off, &pos, 2); off += 2;
            out[off++] = (uint8_t)data[i];
        }
    }
    return off;
}

/* ── CODEBOOK: histogram + residuals ───────────────────────────
 * Format: [n_unique:4][value:1,count:varint]... [residuals:1B each]
 * Residuals store original order using codebook index.
 */
static inline uint32_t dyn_encode_codebook(const int8_t *data, uint32_t n,
                                            uint8_t *out, uint32_t cap)
{
    /* Build histogram */
    uint32_t hist[256] = {0};
    for (uint32_t i = 0; i < n; i++) hist[(uint8_t)data[i]]++;

    /* Count unique */
    uint32_t nu = 0;
    uint8_t  values[256];
    for (int v = 0; v < 256; v++) {
        if (hist[v] > 0) { values[nu++] = (uint8_t)v; }
    }

    /* Header: n_unique(4) + per-entry (value(1) + varint(count)) */
    uint32_t header_est = 4 + nu * 6;  /* generous estimate */
    uint32_t payload_est = n;           /* 1 byte per residual */
    uint32_t needed = header_est + payload_est;
    if (needed > cap) return 0;

    uint32_t off = 0;
    memcpy(out + off, &nu, 4); off += 4;

    /* Write codebook: value + count */
    for (uint32_t i = 0; i < nu; i++) {
        uint8_t val = values[i];
        uint32_t cnt = hist[val];
        out[off++] = val;
        /* Varint encode count */
        while (cnt >= 0x80) { out[off++] = (cnt & 0x7F) | 0x80u; cnt >>= 7; }
        out[off++] = (uint8_t)cnt;
    }

    /* Write residuals: for each element, find its codebook index */
    for (uint32_t i = 0; i < n; i++) {
        uint8_t val = (uint8_t)data[i];
        uint8_t idx = 0;
        for (uint32_t j = 0; j < nu; j++) {
            if (values[j] == val) { idx = (uint8_t)j; break; }
        }
        out[off++] = idx;
    }
    return off;
}

/* ── DELTA: store differences between consecutive elements ─────
 * Format: [first_val:1][varint(|delta|) with sign bit]...
 * Good for sequential data with small changes.
 */
static inline uint32_t dyn_encode_delta(const int8_t *data, uint32_t n,
                                         uint8_t *out, uint32_t cap)
{
    if (n == 0) return 0;
    uint32_t needed = 1 + n * 2;  /* worst case: 2 bytes per delta */
    if (needed > cap) return 0;

    uint32_t off = 0;
    out[off++] = (uint8_t)data[0];

    for (uint32_t i = 1; i < n; i++) {
        int32_t delta = (int32_t)data[i] - (uint32_t)data[i-1];
        uint8_t sign = (delta < 0) ? 1 : 0;
        uint32_t abs_d = (uint32_t)(delta < 0 ? -delta : delta);
        uint32_t encoded = (abs_d << 1) | sign;
        /* Varint */
        while (encoded >= 0x80) { out[off++] = (encoded & 0x7F) | 0x80u; encoded >>= 7; }
        out[off++] = (uint8_t)encoded;
    }
    return off;
}

/* ── BITPACK: bitmap + packed small values ─────────────────────
 * Format: [n_bits:4][bitmap:ceil(n/8)][packed values]
 * For data where each value fits in B bits (B chosen from distribution).
 */
static inline uint32_t dyn_encode_bitpack(const int8_t *data, uint32_t n,
                                            uint8_t *out, uint32_t cap)
{
    if (n == 0) return 0;

    /* Find bits needed per value */
    uint8_t max_val = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t v = (uint8_t)data[i];
        if (v > max_val) max_val = v;
    }
    uint8_t bits = 1;
    while (bits < 8 && (1u << bits) <= max_val) bits++;

    uint32_t bm_bytes = (n + 7) / 8;
    uint32_t packed_bytes = (n * bits + 7) / 8;
    uint32_t needed = 4 + bm_bytes + packed_bytes;
    if (needed > cap) return 0;

    uint32_t off = 0;
    memcpy(out + off, &n, 4); off += 4;
    out[off++] = bits;

    /* Write bitmap: which positions have non-zero values */
    memset(out + off, 0, bm_bytes);
    for (uint32_t i = 0; i < n; i++) {
        if (data[i] != 0) out[off + i / 8] |= (1u << (i % 8));
    }
    off += bm_bytes;

    /* Pack values */
    uint32_t bit_acc = 0;
    uint8_t  bit_count = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t v = (uint8_t)data[i];
        bit_acc |= ((uint32_t)v << bit_count);
        bit_count += bits;
        while (bit_count >= 8) {
            out[off++] = (uint8_t)(bit_acc & 0xFF);
            bit_acc >>= 8;
            bit_count -= 8;
        }
    }
    if (bit_count > 0) out[off++] = (uint8_t)(bit_acc & 0xFF);

    return off;
}

/* ════════════════════════════════════════════════════════════════
   DECODE — dispatch by strategy
   ════════════════════════════════════════════════════════════════ */

/* Decode raw: just copy */
static inline int dyn_decode_raw(const uint8_t *payload, uint32_t payload_size,
                                  int8_t *out, uint32_t n)
{
    if (payload_size < n) return -1;
    memcpy(out, payload, n);
    return 0;
}

/* Decode sparse: (pos,val) pairs → full array */
static inline int dyn_decode_sparse(const uint8_t *payload, uint32_t payload_size,
                                     int8_t *out, uint32_t n)
{
    if (payload_size < 4) return -1;
    uint32_t nz = 0;
    memcpy(&nz, payload, 4);
    if (4 + nz * 3 > payload_size) return -2;

    memset(out, 0, n);
    uint32_t off = 4;
    for (uint32_t i = 0; i < nz; i++) {
        uint16_t pos;
        memcpy(&pos, payload + off, 2); off += 2;
        if (pos < n) out[pos] = (int8_t)payload[off];
        off++;
    }
    return 0;
}

/* Decode codebook: histogram + residuals */
static inline int dyn_decode_codebook(const uint8_t *payload, uint32_t payload_size,
                                       int8_t *out, uint32_t n)
{
    if (payload_size < 4) return -1;
    uint32_t off = 0;
    uint32_t nu = 0;
    memcpy(&nu, payload + off, 4); off += 4;

    uint8_t  values[256];
    for (uint32_t i = 0; i < nu; i++) {
        if (off >= payload_size) return -2;
        values[i] = payload[off++];
        /* Varint decode count */
        uint32_t cnt = 0, shift = 0;
        do {
            if (off >= payload_size) return -3;
            uint8_t b = payload[off++];
            cnt |= ((uint32_t)(b & 0x7F) << shift);
            shift += 7;
        } while (payload[off - 1] & 0x80);
    }

    /* Decode residuals */
    for (uint32_t i = 0; i < n && off < payload_size; i++) {
        uint8_t idx = payload[off++];
        if (idx < nu) out[i] = (int8_t)values[idx];
        else out[i] = 0;
    }
    return 0;
}

/* Decode delta: first_val + varint deltas */
static inline int dyn_decode_delta(const uint8_t *payload, uint32_t payload_size,
                                    int8_t *out, uint32_t n)
{
    if (payload_size < 1 || n == 0) return -1;
    uint32_t off = 0;
    out[0] = (int8_t)payload[off++];

    for (uint32_t i = 1; i < n; i++) {
        /* Varint decode */
        uint32_t encoded = 0, shift = 0;
        do {
            if (off >= payload_size) return -2;
            uint8_t b = payload[off++];
            encoded |= ((uint32_t)(b & 0x7F) << shift);
            shift += 7;
        } while (payload[off - 1] & 0x80);

        uint32_t sign = encoded & 1;
        int32_t delta = (int32_t)(encoded >> 1);
        if (sign) delta = -delta;
        out[i] = (int8_t)((int32_t)out[i-1] + delta);
    }
    return 0;
}

/* Decode bitpack: bitmap + packed values */
static inline int dyn_decode_bitpack(const uint8_t *payload, uint32_t payload_size,
                                      int8_t *out, uint32_t n)
{
    if (payload_size < 5) return -1;
    uint32_t off = 0;
    uint32_t stored_n = 0;
    memcpy(&stored_n, payload + off, 4); off += 4;
    uint8_t bits = payload[off++];

    uint32_t bm_bytes = (stored_n + 7) / 8;
    if (off + bm_bytes > payload_size) return -2;

    /* Read bitmap */
    const uint8_t *bitmap = payload + off;
    off += bm_bytes;

    /* Unpack values */
    uint32_t bit_acc = 0;
    uint8_t  bit_count = 0;
    for (uint32_t i = 0; i < stored_n && i < n; i++) {
        while (bit_count < bits) {
            if (off >= payload_size) return -3;
            bit_acc |= ((uint32_t)payload[off++] << bit_count);
            bit_count += 8;
        }
        uint8_t val = (uint8_t)(bit_acc & ((1u << bits) - 1));
        bit_acc >>= bits;
        bit_count -= bits;

        /* Zero if bitmap says so */
        if (!(bitmap[i / 8] & (1u << (i % 8)))) val = 0;
        out[i] = (int8_t)val;
    }
    return 0;
}

/* ════════════════════════════════════════════════════════════════
   HIGH-LEVEL API: encode / decode / verify
   ════════════════════════════════════════════════════════════════ */

typedef struct {
    DynHeader  header;
    DynProfile profile;
    uint8_t    payload[DYN_MAX_SLOTS + 4096]; /* worst case */
    uint32_t   payload_size;
} DynContainer;

/* Initialize container */
static inline void dyn_init(DynContainer *dc)
{
    if (!dc) return;
    memset(dc, 0, sizeof(*dc));
    dc->header.magic = DYN_MAGIC;
    dc->header.version = DYN_VERSION;
}

/* Encode: classify + encode + build container.
 * Returns 0 on success, -1 on error. */
static inline int dyn_encode(DynContainer *dc,
                              const int8_t *data, uint32_t n)
{
    if (!dc || !data || n == 0 || n > DYN_MAX_SLOTS) return -1;

    dyn_classify(data, n, &dc->profile);

    dc->header.n_elems = n;
    dc->header.raw_size = n;
    dc->header.strategy = dc->profile.strategy;

    uint8_t *out = dc->payload;
    uint32_t cap = sizeof(dc->payload);

    switch (dc->profile.strategy) {
        case DYN_STRAT_SPARSE:
            dc->payload_size = dyn_encode_sparse(data, n, out, cap);
            break;
        case DYN_STRAT_CODEBOOK:
            dc->payload_size = dyn_encode_codebook(data, n, out, cap);
            break;
        case DYN_STRAT_DELTA:
            dc->payload_size = dyn_encode_delta(data, n, out, cap);
            break;
        case DYN_STRAT_BITPACK:
            dc->payload_size = dyn_encode_bitpack(data, n, out, cap);
            break;
        default:
            dc->payload_size = dyn_encode_raw(data, n, out, cap);
            dc->header.strategy = DYN_STRAT_RAW;
            break;
    }

    if (dc->payload_size == 0) return -2;  /* encode failed */

    /* NEVER-EXPAND GUARANTEE (Aug 10, 2026):
     * A codec must never store more bytes than the raw input. Strategy
     * payloads (esp. CODEBOOK's byte-per-index) can outgrow raw on
     * incompressible data — if so, fall back to RAW so the container
     * always stores the SMALLEST representation. Previously an expanded
     * payload was persisted as-is (324B for 144B random → overhead 2.54x). */
    if (dc->payload_size > n) {
        dc->payload_size = dyn_encode_raw(data, n, out, cap);
        dc->header.strategy = DYN_STRAT_RAW;
    }

    dc->header.payload_size = dc->payload_size;
    dc->header.checksum = dyn_crc32(dc->payload, dc->payload_size);
    return 0;
}

/* Decode: read container → reconstruct data.
 * Returns 0 on success, -1 on error, -2 on CRC mismatch. */
static inline int dyn_decode(const DynContainer *dc,
                              int8_t *out, uint32_t n)
{
    if (!dc || !out || n == 0) return -1;
    if (dc->header.magic != DYN_MAGIC) return -3;

    /* Verify CRC */
    uint32_t crc = dyn_crc32(dc->payload, dc->header.payload_size);
    if (crc != dc->header.checksum) return -2;

    switch (dc->header.strategy) {
        case DYN_STRAT_RAW:
            return dyn_decode_raw(dc->payload, dc->header.payload_size, out, n);
        case DYN_STRAT_SPARSE:
            return dyn_decode_sparse(dc->payload, dc->header.payload_size, out, n);
        case DYN_STRAT_CODEBOOK:
            return dyn_decode_codebook(dc->payload, dc->header.payload_size, out, n);
        case DYN_STRAT_DELTA:
            return dyn_decode_delta(dc->payload, dc->header.payload_size, out, n);
        case DYN_STRAT_BITPACK:
            return dyn_decode_bitpack(dc->payload, dc->header.payload_size, out, n);
        default:
            return -4;  /* unknown strategy */
    }
}

/* Verify: roundtrip check. Returns 0 if lossless. */
static inline int dyn_verify(const int8_t *original, uint32_t n,
                              const DynContainer *dc)
{
    if (!original || !dc || n == 0) return -1;

    int8_t *reconstructed = (int8_t *)malloc(n);
    if (!reconstructed) return -1;

    int rc = dyn_decode(dc, reconstructed, n);
    if (rc != 0) { free(reconstructed); return rc; }

    int match = (memcmp(original, reconstructed, n) == 0);
    free(reconstructed);
    return match ? 0 : -5;
}

/* ════════════════════════════════════════════════════════════════
   UTILITY
   ════════════════════════════════════════════════════════════════ */

/* Compute compression ratio (encoded / original). <1.0 = compression. */
static inline float dyn_ratio(const DynContainer *dc)
{
    if (!dc || dc->header.raw_size == 0) return 1.0f;
    uint32_t total = DYN_HEADER_SZ + dc->header.payload_size + 4; /* header + payload + CRC */
    return (float)total / (float)dc->header.raw_size;
}

/* Get strategy name */
static inline const char* dyn_strategy_name(uint8_t strategy)
{
    switch (strategy) {
        case DYN_STRAT_RAW:      return "RAW";
        case DYN_STRAT_SPARSE:   return "SPARSE";
        case DYN_STRAT_CODEBOOK: return "CODEBOOK";
        case DYN_STRAT_DELTA:    return "DELTA";
        case DYN_STRAT_BITPACK:  return "BITPACK";
        default:                 return "UNKNOWN";
    }
}

/* Print profile */
static inline void dyn_print_profile(const DynProfile *prof)
{
    if (!prof) return;
    printf("  Sparsity: %.1f%% | Unique: %u/256 (%.1f%%) | Range: %u | AvgDiff: %u\n",
           prof->sparsity * 100.0f, prof->n_unique, prof->unique_ratio * 100.0f,
           prof->total_range, prof->avg_abs_diff);
    printf("  Strategy: %s\n", dyn_strategy_name(prof->strategy));
}

#endif /* DWGLS_DYNAMIC_CODEC_H */

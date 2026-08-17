/*
 * hyper_delta.h — Hyperbolic Delta Format
 *
 * Delta = what KIS lost at given scale step
 * Hyperbolic stores delta only (small), not full data
 *
 * Formula: full_value[i] = kis_value[i] + delta[i]
 *
 * Size: 20,736 bytes + 16 bytes header = ~20 KB
 * Access: O(1) — direct index, no search
 *
 * BUILD: gcc -O2 -I. -o test_hyper_delta_format tests/test_hyper_delta_format.c -lm
 */
#pragma once

#include <stdint.h>
#include <string.h>

/* ── Constants ──────────────────────────────────────────────────────────── */
#define HYPER_DELTA_MAGIC    0x48444C54u   /* "HDLT" */
#define HYPER_DELTA_VERSION  1u
#define HYPER_DELTA_SLOTS    20736u        /* KIS_20736 */

/* ── Delta Header (16 bytes) ──────────────────────────────────────────── */
typedef struct {
    uint32_t magic;        /* 0x48444C54 "HDLT" */
    uint32_t version;      /* 1 */
    uint32_t kis_slots;    /* 20736 */
    uint32_t scale_step;   /* which KIS scale produced this delta */
} HyperDeltaHeader;

/* ── Full Delta (20,752 bytes) ────────────────────────────────────────── */
typedef struct {
    HyperDeltaHeader header;
    uint8_t          data[HYPER_DELTA_SLOTS];  /* one byte per slot */
} HyperDelta;

/* ── Functions ─────────────────────────────────────────────────────────── */

/*
 * hyper_delta_init — Initialize delta with header
 */
static inline void hyper_delta_init(HyperDelta *d, uint32_t scale_step) {
    d->header.magic       = HYPER_DELTA_MAGIC;
    d->header.version     = HYPER_DELTA_VERSION;
    d->header.kis_slots   = HYPER_DELTA_SLOTS;
    d->header.scale_step  = scale_step;
    memset(d->data, 0, HYPER_DELTA_SLOTS);
}

/*
 * hyper_delta_calculate — Calculate delta = original - kis_coarse
 *
 * delta[i] = original[i] - kis_coarse[i]
 *
 * This is the core operation:
 * - original = full precision data
 * - kis_coarse = KIS projection at given scale (lossy)
 * - delta = what was lost (precision)
 */
static inline void hyper_delta_calculate(
    HyperDelta *d,
    const uint8_t *original,     /* full precision data */
    const uint32_t *kis_coarse,  /* KIS projection at scale */
    uint32_t n                    /* number of elements */
) {
    for (uint32_t i = 0; i < n && i < HYPER_DELTA_SLOTS; i++) {
        int diff = (int)original[i] - (int)(kis_coarse[i] & 0xFF);
        d->data[i] = (uint8_t)(diff & 0xFF);
    }
}

/*
 * hyper_delta_recover — Recover full precision from KIS + delta
 *
 * recovered[i] = kis_value[i] + delta[i]
 *
 * This is the inverse of calculate:
 * - kis_value = KIS projection (coarse)
 * - delta = what was lost
 * - recovered = full precision (lossless)
 */
static inline void hyper_delta_recover(
    const HyperDelta *d,
    const uint32_t *kis_value,  /* KIS projection */
    uint8_t *recovered,         /* output: full precision */
    uint32_t n                   /* number of elements */
) {
    for (uint32_t i = 0; i < n && i < HYPER_DELTA_SLOTS; i++) {
        recovered[i] = (uint8_t)((kis_value[i] + d->data[i]) & 0xFF);
    }
}

/*
 * hyper_delta_verify — Verify delta is correct
 *
 * Returns 1 if KIS + delta = original, 0 otherwise
 */
static inline int hyper_delta_verify(
    const HyperDelta *d,
    const uint8_t *original,
    const uint32_t *kis_value,
    uint32_t n
) {
    for (uint32_t i = 0; i < n && i < HYPER_DELTA_SLOTS; i++) {
        uint8_t recovered = (uint8_t)((kis_value[i] + d->data[i]) & 0xFF);
        if (recovered != original[i]) return 0;
    }
    return 1;
}

/*
 * hyper_delta_size — Total size of delta in bytes
 */
static inline uint32_t hyper_delta_size(void) {
    return sizeof(HyperDelta);  /* 20,752 bytes */
}

/*
 * hyper_delta_is_valid — Check if delta header is valid
 */
static inline int hyper_delta_is_valid(const HyperDelta *d) {
    return d->header.magic      == HYPER_DELTA_MAGIC &&
           d->header.version    == HYPER_DELTA_VERSION &&
           d->header.kis_slots  == HYPER_DELTA_SLOTS;
}

/* ════════════════════════════════════════════════════════════
   Pred+Ent Delta — scale-predict residual + Huffman (T1.1c)
   ════════════════════════════════════════════════════════════
   แทนที่ full delta (1 B/slot):
     pred[i] = kis_coarse[center ของ 2×2 block] (B=2 — depth k=2 envelope)
     residual = (original − pred) & 0xFF
     huffman-coded → d->data  (256 B lens + coded bytes)
   recover:  pred + decode(residual) = original (lossless)

   base ไม่ต้องเก็บ — pred มาจาก kis_coarse (reader มีอยู่แล้ว — scale view)
   ขนาดจริง = 256 + coded ≤ 256 + n (+fallback 8-bit → ≤ n)
   ต้องการ n == 20736 (144×144) — B=2 → 72×72 blocks                 */
#define HDENT_COLS 144u
#define HDENT_BC   72u      /* 144/2 */

typedef struct {
    HyperDeltaHeader header;
    uint32_t data_len;                        /* 256 lens + coded residual */
    uint8_t  data[HYPER_DELTA_SLOTS + 512];   /* ≤ 256 + 20736 + slack      */
} HyperDeltaEnt;

#include "huff_codec.h"

/* pred = coarse view ของ slot i — packed key เก็บ x3 ที่ bits 20-31
   (x3 = พิกัด projected = scale view — ตัวที่ข้อมูลถูกบีบไปเหลืออยู่) */
static inline uint8_t hdent_pred(const uint32_t *kis, uint32_t i) {
    return (uint8_t)((kis[i] >> 20) & 0xFFu);
}

static inline void hyper_delta_ent_calculate(HyperDeltaEnt *d, uint32_t scale_step,
                                             const uint8_t *original,
                                             const uint32_t *kis_coarse,
                                             uint32_t n) {
    memset(d, 0, sizeof(*d));
    d->header.magic      = HYPER_DELTA_MAGIC;
    d->header.version    = HYPER_DELTA_VERSION;
    d->header.kis_slots  = HYPER_DELTA_SLOTS;
    d->header.scale_step = scale_step;
    if (n == 0 || n > HYPER_DELTA_SLOTS) return;

    uint8_t residual[HYPER_DELTA_SLOTS];
    uint64_t freq[256] = {0};
    for (uint32_t i = 0; i < n; i++) {
        uint8_t p = hdent_pred(kis_coarse, i);
        residual[i] = (uint8_t)((original[i] - p) & 0xFFu);
        freq[residual[i]]++;
    }

    HuffModel m;
    huff_build(&m, freq);
    memcpy(d->data, m.lens, 256);
    uint32_t coded = huff_encode(&m, residual, n, d->data + 256,
                                 HYPER_DELTA_SLOTS + 256);
    d->data_len = 256u + coded;
}

static inline int hyper_delta_ent_recover(const HyperDeltaEnt *d,
                                          const uint32_t *kis_coarse,
                                          uint8_t *recovered, uint32_t n) {
    if (!d || n == 0 || n > HYPER_DELTA_SLOTS) return 0;
    if (d->data_len < 256u || d->data_len > 256u + HYPER_DELTA_SLOTS + 512u)
        return 0;
    HuffModel m;
    huff_rebuild(&m, d->data);
    uint8_t residual[HYPER_DELTA_SLOTS];
    if (huff_decode(&m, d->data + 256, d->data_len - 256u, residual, n) != 0)
        return 0;
    for (uint32_t i = 0; i < n; i++)
        recovered[i] = (uint8_t)(hdent_pred(kis_coarse, i) + residual[i]);
    return 1;
}

/* ขนาด delta จริง (bytes) */
static inline uint32_t hyper_delta_ent_size(const HyperDeltaEnt *d) {
    return (uint32_t)(sizeof(d->header) + sizeof(d->data_len) + d->data_len);
}

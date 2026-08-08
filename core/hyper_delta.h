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

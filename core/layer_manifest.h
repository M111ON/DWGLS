/* =====================================================================
 * layer_manifest.h — DWGLS layer contract + connection rules
 * =====================================================================
 * Single source of truth for the layer stack and the RULES for wiring
 * between layers. Wrong-layer bugs are caught by machine checks built
 * on these declarations (see tools/layer_manifest_probe.c).
 *
 *   L0 external bytes   GGUF / files / llama state (variable length!)
 *   L1 parts            128KB blocks; FINAL part has true len < 128KB
 *                       rule R1: compare/memcmp uses TRUE length only
 *   L2 slots            60 RID slots, addr = l*60 + w
 *                       rule R2: permutation bijectivity checked BEFORE use
 *   L3 views            pent/tri/snubL/snubR/hosoya/zeck
 *                       rule R3: multi-view XOR consensus for verify;
 *                                disagreement localizes broken path
 *   L4 frames           tess frame = 8 cubes x 144 slots
 *                       rule R4: logits/streams compared ONLY at matched
 *                                decode index (position-sensitive!)
 *
 * PERF note: view choice & connection order affect locality (page
 * faults / cache lines). Effects are MEASURED (probe P-gates), never
 * assumed.
 * =================================================================== */
#ifndef DWGLS_LAYER_MANIFEST_H
#define DWGLS_LAYER_MANIFEST_H

#include <stdint.h>
#include <string.h>

#define LM_PART_BYTES   (128u * 1024u)
#define LM_SLOTS        60u
#define LM_FRAME_CUBES  8u
#define LM_FRAME_STRIDE 144u

/* layer ids */
enum {
    LM_L0_BYTES = 0,
    LM_L1_PARTS,
    LM_L2_SLOTS,
    LM_L3_VIEWS,
    LM_L4_FRAMES,
    LM_LAYER_COUNT
};

/* ---- rule R2: permutation must be bijective before use --------------
 * returns 1 if v[0..n) is a permutation of 0..n-1                     */
static inline int lm_check_permutation(const uint8_t *v, uint32_t n) {
    uint8_t hit[256];
    if (n > 256) return 0;
    memset(hit, 0, sizeof(hit));
    for (uint32_t i = 0; i < n; i++) {
        if (v[i] >= n || hit[v[i]]) return 0;
        hit[v[i]] = 1;
    }
    return 1;
}

/* ---- rule R1: true-length compare (tail-safe) -----------------------
 * compares part buffers using their true lengths; full_len only when
 * both sides actually have it                                        */
static inline int lm_part_equal(const uint8_t *a, uint32_t alen,
                                const uint8_t *b, uint32_t blen) {
    if (alen != blen) return 0;
    return memcmp(a, b, alen) == 0;
}

/* ---- rule R3: n-way XOR consensus -----------------------------------
 * xor_i computed externally; returns index of first dissenting view,
 * or -1 when all agree                                              */
static inline int lm_consensus_first_dissent(const uint64_t *xo, int n) {
    for (int i = 1; i < n; i++)
        if (xo[i] != xo[0]) return i;
    return -1;
}

/* ---- rule R4: position-matched float compare ------------------------
 * bitwise first; on mismatch reports max abs diff (off-by-one shows
 * as large maxdiff ~O(10), noise shows as ~1e-4)                    */
static inline int lm_logits_equal(const float *a, const float *b,
                                  uint32_t n, float *maxdiff_out) {
    if (memcmp(a, b, sizeof(float) * (size_t)n) == 0) {
        if (maxdiff_out) *maxdiff_out = 0.0f;
        return 1;
    }
    float md = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        float d = a[i] - b[i];
        if (d < 0) d = -d;
        if (d > md) md = d;
    }
    if (maxdiff_out) *maxdiff_out = md;
    return 0;
}

#endif /* DWGLS_LAYER_MANIFEST_H */

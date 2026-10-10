/* anchor_centroid.h — derive an anchor-table (K x dim centroids) directly
 * from RDH-fed anchors. "derive, don't store": no k-means, no .bin file —
 * the centroids are a deterministic function of the anchor identities.
 *
 * WHY (owner picture, memory #7526):
 *   ann wants an anchor table (trained centroids) for anch_assign/anch_route.
 *   kineticfan/gear can GENERATE that table; the missing piece was the
 *   converter. This is that converter: an anchor's RDH pattern + letter +
 *   weight -> a dim-vector centroid, in the exact layout ann expects
 *   (K x dim row-major float, the shape anch_load/anch_assign consume).
 *
 * Chain:
 *   aw_anchor_t (RDH pattern/letter/weight)
 *     -> ac_centroid_of()   : one anchor -> one dim-vector
 *     -> ac_build_table()   : anchor seeds -> K x dim table  (= kv_anchors.bin shape)
 *     -> anch_save()/anch_load() (anchor_route.h)  : byte-stable file
 *     -> al_letter_of()/al_orbit_of() (anchor_letter.h) : 6 letters -> 720 orbit
 *
 * Determinism: pure function of the anchor bytes + index. No RNG, no malloc
 * on the per-centroid path (table build allocates once via the caller).
 *
 * Header-only, std-only.
 */
#ifndef ANCHOR_CENTROID_H
#define ANCHOR_CENTROID_H

#include <stdint.h>
#include <stddef.h>

#include "anchor_weight.h"   /* aw_anchor_t, AW_OUTER, AW_LETTERS */
#include "anchor_route.h"    /* ANCHR_MAXK, ANCHR_MAXD, anch_save */

/* one deterministic pseudo-value in [-1, 1] from (anchor, slot, dim-index).
 * Uses the anchor's RDH patterns + weights as the entropy source, mixed so
 * each dim index is independent. */
static inline float ac_centroid_value(const aw_anchor_t *a, int dim_idx) {
    /* pull from the anchor's 8 x 64-bit patterns: 512 bits of material */
    int s = dim_idx & (int)(AW_OUTER - 1);
    int g = (dim_idx >> 3) & 63;
    uint64_t p = a->pattern[s];
    uint64_t x = (p >> (g & 63)) | (p << ((64 - g) & 63));
    x ^= (uint64_t)a->letter * 0x9E3779B97F4A7C15ull;
    x ^= (uint64_t)a->weight[s] * 0xC2B2AE3D27D4EB4Full;
    x ^= (uint64_t)dim_idx * 0x165667B19E3779F9ull;
    x *= 0x9E3779B97F4A7C15ull;
    x ^= x >> 29; x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 32;
    /* map to [-1, 1] */
    int32_t v = (int32_t)(x & 0x7FFFFFFFu) - 0x40000000;
    return (float)v / 1073741824.0f;
}

/* one anchor -> dim-vector centroid (writes dim floats). */
static inline void ac_centroid_of(const aw_anchor_t *a, float *out, int dim) {
    for (int d = 0; d < dim; d++) out[d] = ac_centroid_value(a, d);
}

/* Build a K x dim anchor table from K anchor seeds.
 * seeds[i] is a byte buffer (seed_len bytes) for anchor i; the table is the
 * exact layout ann's anch_load/anch_assign expect (row-major float).
 * Returns 0 on success, -1 on bad args. No allocation here — caller owns
 * table (K*dim floats). */
static inline int ac_build_table(const uint8_t *seeds, size_t seed_len,
                                 int K, int dim, float *table) {
    if (!seeds || !table || K <= 0 || dim <= 0) return -1;
    if (K > ANCHR_MAXK || dim > ANCHR_MAXD) return -1;
    for (int k = 0; k < K; k++) {
        aw_anchor_t a;
        aw_anchor_build(seeds + (size_t)k * seed_len, seed_len, 0, &a);
        ac_centroid_of(&a, table + (size_t)k * dim, dim);
    }
    return 0;
}

/* Convenience: build a table and persist it in ann's exact file format
 * (anchor_route.h anch_save: magic,K,dim,ntrained,checksum + K*dim floats).
 * Returns anch_save's result (0 = ok). */
static inline int ac_build_and_save(const uint8_t *seeds, size_t seed_len,
                                    int K, int dim, float *table,
                                    const char *path) {
    if (ac_build_table(seeds, seed_len, K, dim, table) != 0) return -1;
    return anch_save(path, table, K, dim, K);
}

/* ── link the anchor into ann's LETTER/FACE layer (anchor_letter.h) ──────
 * al_letter_of() names 6 faces from centroid energy. The RDH anchor already
 * carries 6 direction letters; ac_face_letter_of() maps them onto the same
 * face slots so the two meanings coincide: RDH letter order == face order. */
static inline void ac_face_letters(const aw_anchor_t *a, uint8_t out[6]) {
    for (int f = 0; f < 6; f++) out[f] = a->dir_letter[f];
}

/* ── gear/kineticfan link ──────────────────────────────────────────────
 * fan24_gear tooth (dc mod 8, dx mod 3) -> an anchor seed variant: the same
 * logical anchor at a different gear position yields a different centroid,
 * so the 24-tooth rim addresses 24 distinct table rows per anchor. This is
 * the "climate change" hook: moving (a,b)/tooth relocates the set without
 * re-embedding the vector (memory #7505, #7525). */
static inline uint32_t ac_gear_seed(uint32_t anchor_id, uint8_t dc, uint8_t dx) {
    uint32_t s = anchor_id;
    s = s * 31u + (uint32_t)dc;
    s = s * 31u + (uint32_t)dx;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}

#endif /* ANCHOR_CENTROID_H */

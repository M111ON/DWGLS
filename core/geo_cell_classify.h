/* ═══════════════════════════════════════════════════════════════════════════
 * geo_cell_classify.h — Cell Type Classification & Statistics
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Classifies weight positions into 8 cell types based on 3-bit parity:
 *   bit 2: generation parity (0=even→icosa, 1=odd→dodeca)
 *   bit 1: face parity
 *   bit 0: slot parity
 *
 * Cell types: III(0), IID(1), IDI(2), IDD(3), DII(4), DID(5), DDI(6), DDD(7)
 *
 * Depends: geo_cube_addr.h
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifndef GEO_CELL_CLASSIFY_H
#define GEO_CELL_CLASSIFY_H

#include "geo_cube_addr.h"
#include <string.h>

/* ═══════════════════════════════════════════════════════════════
   CLASSIFY — address → cell type (0-7)
   ═══════════════════════════════════════════════════════════════ */

static inline uint8_t geo_cell_classify(GeoCubeAddr addr) {
    return ((addr.generation & 1) << 2) |
           ((addr.face       & 1) << 1) |
           ( addr.slot       & 1);
}

/* ═══════════════════════════════════════════════════════════════
   NAME — cell type → display string
   ═══════════════════════════════════════════════════════════════ */

static inline const char* geo_cell_classify_name(uint8_t ct) {
    return cell_type_name(ct);  /* delegates to geo_cube_addr.h */
}

/* ═══════════════════════════════════════════════════════════════
   STATS — scan weight array, classify, print distribution
   ═══════════════════════════════════════════════════════════════
   weights:    flat array of n_weights floats
   n_weights:  number of elements
   max_gen:    highest generation to address into
   ═══════════════════════════════════════════════════════════════ */

static inline void geo_cell_classify_stats(const float *weights,
                                           uint32_t n_weights,
                                           uint32_t max_gen)
{
    /* Per-generation, per-cell-type counters */
    uint32_t hist[CUBE_ADDR_GEN_MAX + 1][8];
    memset(hist, 0, sizeof(hist));

    for (uint32_t i = 0; i < n_weights; i++) {
        /* Map flat index → address via geo_flat_to_addr */
        GeoCubeAddr addr = geo_flat_to_addr(i % 20736u);
        uint8_t ct = geo_cell_classify(addr);
        if (addr.generation <= max_gen && ct < 8) {
            hist[addr.generation][ct]++;
        }
    }

    printf("===============================================================\n");
    printf("  Cell Classification Stats\n");
    printf("---------------------------------------------------------------\n");
    printf("  Weights scanned: %u\n", n_weights);
    printf("  Max generation:  %u\n", max_gen);
    printf("---------------------------------------------------------------\n");

    for (uint32_t g = 0; g <= max_gen && g <= CUBE_ADDR_GEN_MAX; g++) {
        printf("  Gen %2u:", g);
        for (uint8_t ct = 0; ct < 8; ct++) {
            printf(" %s:%u", geo_cell_classify_name(ct), hist[g][ct]);
        }
        printf("\n");
    }
    printf("===============================================================\n");
}

/* ═══════════════════════════════════════════════════════════════
   VERIFY — all gen/face/slot combos map to valid types
   ═══════════════════════════════════════════════════════════════ */

static inline int verify_cell_classify(uint32_t max_gen) {
    int pass = 0, fail = 0;

    for (uint32_t g = 0; g <= max_gen && g <= CUBE_ADDR_GEN_MAX; g++) {
        uint16_t spf = slots_per_face(g);
        for (uint8_t f = 0; f < CUBE_ADDR_FACES; f++) {
            for (uint16_t s = 0; s < spf; s++) {
                GeoCubeAddr addr = geo_cube_addr(g, f, s);
                uint8_t ct = geo_cell_classify(addr);

                /* Must be 0-7 */
                if (ct > 7) {
                    fail++;
                    if (fail <= 3) {
                        printf("  FAIL: gen=%u face=%u slot=%u → ct=%u (out of range)\n",
                               g, f, s, ct);
                    }
                    continue;
                }

                /* Verify parity bits match */
                uint8_t exp = ((g & 1) << 2) | ((f & 1) << 1) | (s & 1);
                if (ct == exp) {
                    pass++;
                } else {
                    fail++;
                    if (fail <= 3) {
                        printf("  FAIL: gen=%u face=%u slot=%u → ct=%u (expected %u)\n",
                               g, f, s, ct, exp);
                    }
                }
            }
        }
    }

    printf("  Cell classify: %d PASS / %d FAIL\n", pass, fail);
    return fail == 0;
}

#endif /* GEO_CELL_CLASSIFY_H */

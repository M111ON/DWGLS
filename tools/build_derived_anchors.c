/* build_derived_anchors.c — derive an ann anchor table with NO training.
 *
 * Uses include/anchor_centroid.h (ac_build_table + anch_save): 8 gear seeds
 * (ac_gear_seed, anchor 0..7 at tooth (0,0)) -> deterministic 8x384 table
 * in ann's exact kv_anchors.bin shape. For the derived-vs-trained A/B:
 * point a scratch ann_search at this file via ANN_ANCH_PATH and compare
 * routed top-k against the live (trained-table) instance.
 *
 * Usage: build_derived_anchors [out.bin] [ntr] [dc] [dx] [anchor_base] [seedmode]
 *   ntr = index entry count the table will serve (default 59748).
 *   dc, dx, anchor_base = gear-seed params (defaults 0, 0, 0): row k uses
 *   ac_gear_seed(anchor_base + k, dc, dx). Different params = different
 *   (deterministic) tables = the seed-sensitivity axis of the A/B.
 *   seedmode 0 = gear seeds (S4 converter story); 1 = plain byte strings
 *   "s1-row-k" (S1 active-full story: ordinary per-anchor builds).
 *   norm 1 = unit-normalize rows (spread fix: removes the norm term from
 *   L2 ranking so direction decides; see ac_normalize_table).
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "anchor_centroid.h"

#define DK 8
#define DDIM 384

int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : "build/derived_anchors.bin";
    int ntr = argc > 2 ? atoi(argv[2]) : 59748;
    int dc = argc > 3 ? atoi(argv[3]) : 0;
    int dx = argc > 4 ? atoi(argv[4]) : 0;
    int abase = argc > 5 ? atoi(argv[5]) : 0;
    int seedmode = argc > 6 ? atoi(argv[6]) : 0;
    int norm = argc > 7 ? atoi(argv[7]) : 0;

    /* 8 seeds, one per table row. Gear (S4) or plain strings (S1). */
    uint8_t seeds[DK * 16];
    size_t slen = 4;
    if (seedmode == 1) {
        slen = 16;
        for (int k = 0; k < DK; k++) {
            memset(seeds + (size_t)k * slen, 0, slen);
            snprintf((char *)seeds + (size_t)k * slen, slen, "s1-row-%d", abase + k);
        }
    } else {
        for (int k = 0; k < DK; k++) {
            uint32_t s = ac_gear_seed((uint32_t)(abase + k), (uint8_t)dc, (uint8_t)dx);
            seeds[k * 4 + 0] = (uint8_t)(s & 0xFF);
            seeds[k * 4 + 1] = (uint8_t)((s >> 8) & 0xFF);
            seeds[k * 4 + 2] = (uint8_t)((s >> 16) & 0xFF);
            seeds[k * 4 + 3] = (uint8_t)((s >> 24) & 0xFF);
            printf("  row %d  gear_seed=0x%08x\n", k, s);
        }
    }

    float *table = (float *)malloc((size_t)DK * DDIM * sizeof(float));
    if (!table) { fprintf(stderr, "oom\n"); return 1; }
    /* ntr must EQUAL the index entry count the table will serve: the
     * server gates the anchor route on antr == nent (ann_search.c
     * lz_route_cached) and runs unrouted otherwise. ntr is metadata only;
     * routing uses just the K x dim rows. */
    clock_t t0 = clock();
    if (ac_build_table(seeds, slen, DK, DDIM, table) != 0) { free(table); return 1; }
    /* clock() resolution (~15ms) dwarfs this build; arena-grade timing is
     * ~0.14 ms for 8x384 (see tools/anchor_arena.c). Printed as-is. */
    double ms = (double)(clock() - t0) / CLOCKS_PER_SEC * 1000.0;
    if (norm) ac_normalize_table(table, DK, DDIM);
    int rc = anch_save(out, table, DK, DDIM, ntr);
    free(table);
    if (rc != 0) { fprintf(stderr, "build/save failed\n"); return 1; }
    printf("wrote %s (%d x %d, seedmode %d, norm %d, build %.1f ms)\n",
           out, DK, DDIM, seedmode, norm, ms);
    return 0;
}

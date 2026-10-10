/* build_derived_anchors.c — derive an ann anchor table with NO training.
 *
 * Uses include/anchor_centroid.h (ac_build_and_save): 8 gear seeds
 * (ac_gear_seed, anchor 0..7 at tooth (0,0)) -> deterministic 8x384 table
 * in ann's exact kv_anchors.bin shape. For the derived-vs-trained A/B:
 * point a scratch ann_search at this file via ANN_ANCH_PATH and compare
 * routed top-k against the live (trained-table) instance.
 *
 * Usage: build_derived_anchors [out.bin] [ntr]
 *   ntr = index entry count the table will serve (default 59748).
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "anchor_centroid.h"

#define DK 8
#define DDIM 384

int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : "build/derived_anchors.bin";
    int ntr = argc > 2 ? atoi(argv[2]) : 59748;

    /* 8 gear seeds, one per table row (anchor_id k at tooth dc=0, dx=0).
     * The seed choice IS the capo: moving (anchor_id,dc,dx) relocates the
     * whole table without re-embedding anything. Fixed here for the A/B. */
    uint8_t seeds[DK * 4];
    for (int k = 0; k < DK; k++) {
        uint32_t s = ac_gear_seed((uint32_t)k, 0, 0);
        seeds[k * 4 + 0] = (uint8_t)(s & 0xFF);
        seeds[k * 4 + 1] = (uint8_t)((s >> 8) & 0xFF);
        seeds[k * 4 + 2] = (uint8_t)((s >> 16) & 0xFF);
        seeds[k * 4 + 3] = (uint8_t)((s >> 24) & 0xFF);
        printf("  row %d  gear_seed=0x%08x\n", k, s);
    }

    float *table = (float *)malloc((size_t)DK * DDIM * sizeof(float));
    if (!table) { fprintf(stderr, "oom\n"); return 1; }
    /* ntr must EQUAL the index entry count the table will serve: the
     * server gates the anchor route on antr == nent (ann_search.c
     * lz_route_cached) and runs unrouted otherwise. ntr is metadata only;
     * routing uses just the K x dim rows. */
    if (ac_build_table(seeds, 4, DK, DDIM, table) != 0) { free(table); return 1; }
    int rc = anch_save(out, table, DK, DDIM, ntr);
    free(table);
    if (rc != 0) { fprintf(stderr, "build/save failed\n"); return 1; }
    printf("wrote %s (%d x %d)\n", out, DK, DDIM);
    return 0;
}

/* build_trap_table.c — trap-painted anchor tables for Heat 1+2.
 *
 * Each table row is painted by a FIRST PASSERBY (not a gear seed): register
 * a trap, first-catch with passerby bytes at full width, then ac_centroid_of
 * the painted anchor into the row. Deterministic per (variant, corpus).
 *
 * Variants isolate two effects (see header anchor_trap.h):
 *   V0: row i <- corpus P[i]            (reference painted table)
 *   V1: row i <- corpus Q[i]            (different painters -> different partition)
 *   V2: row i <- P[(i+3)%8]             (SAME 8 centroids as V0, relabeled ->
 *                                        isolates the label effect)
 *   V3: row i <- i even ? P[i] : Q[i]   (mixed painters)
 * Corpora P/Q: 8 fixed 64-byte strings each (xorshift, distinct seeds).
 *
 * Usage: build_trap_table out.bin ntr variant(0..3)
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "anchor_trap.h"
#include "anchor_centroid.h"

#define TK 8
#define TDIM 384

static void xfill(uint8_t *b, size_t n, uint32_t seed) {
    uint32_t s = seed ? seed : 1u;
    for (size_t i = 0; i < n; i++) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; b[i] = (uint8_t)s; }
}

int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : "build/trap_table.bin";
    int ntr = argc > 2 ? atoi(argv[2]) : 59756;
    int variant = argc > 3 ? atoi(argv[3]) : 0;
    if (variant < 0 || variant > 3) { fprintf(stderr, "variant 0..3\n"); return 1; }

    uint8_t P[TK][64], Q[TK][64];
    for (int i = 0; i < TK; i++) {
        xfill(P[i], 64, 100u + (uint32_t)i);
        xfill(Q[i], 64, 200u + (uint32_t)i);
    }

    float *table = (float *)malloc((size_t)TK * TDIM * sizeof(float));
    if (!table) { fprintf(stderr, "oom\n"); return 1; }
    for (int k = 0; k < TK; k++) {
        uint8_t *pb;
        if (variant == 0) pb = P[k];
        else if (variant == 1) pb = Q[k];
        else if (variant == 2) pb = P[(k + 3) % TK];
        else pb = (k % 2 == 0) ? P[k] : Q[k];
        at_trap_t t;
        uint8_t seed[AT_SEED_LEN];
        uint32_t gs = ac_gear_seed((uint32_t)k, 0, 0);
        seed[0] = (uint8_t)gs; seed[1] = (uint8_t)(gs >> 8);
        seed[2] = (uint8_t)(gs >> 16); seed[3] = (uint8_t)(gs >> 24);
        for (int i = 4; i < AT_SEED_LEN; i++) seed[i] = 0;
        at_register(seed, &t);
        int rc = at_catch(&t, pb, 64, (uint8_t)(k & 1), 64, NULL, NULL);
        if (rc != 1) { fprintf(stderr, "row %d first catch failed\n", k); free(table); return 1; }
        ac_centroid_of(&t.anchor, table + (size_t)k * TDIM, TDIM);
        printf("  row %d letter=0x%02x pair=%d\n",
               k, t.anchor.letter, aw_pair_id(&t.anchor));
    }
    int rc = anch_save(out, table, TK, TDIM, ntr);
    free(table);
    if (rc != 0) { fprintf(stderr, "save failed\n"); return 1; }
    printf("wrote %s (variant %d, ntr %d)\n", out, variant, ntr);
    return 0;
}

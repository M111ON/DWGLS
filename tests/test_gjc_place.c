/* test_gjc_place.c — PROOF: geo_jump as container placed on BreathingFS.
 * balls -> scatter by router walk -> towers(blocks) -> gather -> memcmp.
 * BUILD: gcc -O2 -Wall -Icore -o /tmp/gjc_place tests/test_gjc_place.c
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "geo_jump_container.h"

#define NBALLS 2000u
#define BALLSZ 6u   /* R,G,B,size_lo,size_hi,pat */

int main(void) {
    static BreathingFS fs;
    bfs_init(&fs);

    /* ball population: mixed int/binary payload, unique */
    static int8_t balls[NBALLS * BALLSZ];
    srand(7);
    for (uint32_t i = 0; i < NBALLS; i++) {
        balls[i * BALLSZ + 0] = (int8_t)(rand() % 256);
        balls[i * BALLSZ + 1] = (int8_t)(rand() % 256);
        balls[i * BALLSZ + 2] = (int8_t)(rand() % 256);
        uint32_t sz = (uint32_t)rand() % 20736;
        balls[i * BALLSZ + 3] = (int8_t)(sz & 255);
        balls[i * BALLSZ + 4] = (int8_t)((sz >> 8) & 255);
        balls[i * BALLSZ + 5] = (int8_t)(rand() % 144);
    }
    uint32_t n = NBALLS * BALLSZ; /* 12000 */

    /* T1: MOD-37 walk (full-period bijection) store+load */
    GeoJumpRouter rmod = { JUMP_MOD, 37, 0, 0 };
    int rc = gjc_store(&fs, "balls", 500, &rmod, balls, n);
    printf("T1 store MOD-37 head=500: rc=%d (0=ok)\n", rc);

    /* ball #0 address = nodes[0..5] */
    static uint32_t nodes[GEO_FULL];
    gjc_walk(500, &rmod, 6, nodes);
    printf("   ball#0 ->");
    for (int i = 0; i < 6; i++)
        printf(" (T%u+%-3u)", nodes[i] / 144, nodes[i] % 144);
    printf("\n");

    static int8_t back[NBALLS * BALLSZ];
    rc = gjc_load(&fs, "balls", 500, &rmod, back, n);
    uint32_t bad = n;
    if (rc == 0) { bad = 0; for (uint32_t i = 0; i < n; i++) if (back[i] != balls[i]) bad++; }
    printf("T1 load MOD-37: rc=%d mismatch=%u/%u %s\n", rc, bad, n, (rc == 0 && !bad) ? "LOSSLESS" : "BREAK");

    /* T2: DNA resume — open index, re-walk from DNA alone */
    GJCIndex dna;
    memset(&dna, 0, sizeof(dna));
    rc = gjc_open(&fs, "balls", &dna);
    printf("T2 dna open: rc=%d head=%u n=%u router=(type=%d,p=%u)\n",
           rc, dna.head, dna.n, dna.router.type, dna.router.param);
    memset(back, 0, sizeof(back));
    bad = n;
    if (rc == 0) {
        rc = gjc_load(&fs, "balls", dna.head, &dna.router, back, dna.n);
        if (rc == 0) { bad = 0; for (uint32_t i = 0; i < n; i++) if (back[i] != balls[i]) bad++; }
    }
    printf("T2 load-from-dna: rc=%d mismatch=%u/%u %s\n", rc, bad, n, (rc == 0 && !bad) ? "LOSSLESS" : "BREAK");

    /* T3: wrong router must NOT decode (router is the key, not a view) */
    GeoJumpRouter rbad = { JUMP_MOD, 5, 0, 0 };
    memset(back, 0, sizeof(back));
    rc = gjc_load(&fs, "balls", 500, &rbad, back, n);
    printf("T3 wrong-router stride-5: rc=%d (expect -3 key-mismatch, no garbage out)\n", rc);

    /* T4: HILBERT walk validity as container (distinctness gate) */
    GeoJumpRouter rhil = { JUMP_HILBERT, 1, 1, 1 };
    rc = gjc_walk(500, &rhil, n, nodes);
    printf("T4 hilbert walk n=%u: rc=%d (0=valid container walk, -1=revisit->unfit)\n", n, rc);
    return 0;
}

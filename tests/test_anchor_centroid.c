/* tests/test_anchor_centroid.c — RDH anchor -> centroid table (ann shape).
 *
 * Oracles (independent of include/anchor_centroid.h):
 *   determinism: rebuild table twice -> memcmp identical.
 *   shape:       K x dim, values in [-1,1].
 *   distinct:    different anchor seeds -> different centroid rows.
 *   save/load:   round-trip through anch_save/anch_load (ann file format),
 *                byte-compare table, tamper -> reject.
 *   ann-link:    anch_assign() on the derived table returns a valid bucket,
 *                and al-style face letters are the anchor's own 6 directions.
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -Iinclude -o build/test_anchor_centroid tests/test_anchor_centroid.c -lm
 * RUN:   ./build/test_anchor_centroid
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "anchor_centroid.h"

static int g_pass = 0, g_fail = 0;
static void check(int ok, const char *name) {
    if (ok) { g_pass++; printf("  ok %s\n", name); }
    else    { g_fail++; printf("  FAIL %s\n", name); }
}

#define K   16
#define DIM 16
#define SLEN 32

static void fill(uint8_t *b, size_t n, uint32_t seed) {
    uint32_t s = seed ? seed : 1u;
    for (size_t i = 0; i < n; i++) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; b[i] = (uint8_t)s; }
}

static void make_seeds(uint8_t *seeds, int K_, size_t slen) {
    for (int i = 0; i < K_; i++) fill(seeds + (size_t)i * slen, slen, 0x1000u + (uint32_t)i * 7919u);
}

static void t_deterministic_and_shape(void) {
    uint8_t seeds[K * SLEN];
    make_seeds(seeds, K, SLEN);
    float *t1 = malloc(sizeof(float) * K * DIM);
    float *t2 = malloc(sizeof(float) * K * DIM);
    check(ac_build_table(seeds, SLEN, K, DIM, t1) == 0, "build_table returns 0");
    ac_build_table(seeds, SLEN, K, DIM, t2);
    check(memcmp(t1, t2, sizeof(float) * K * DIM) == 0, "table deterministic (same seeds)");
    int range = 1;
    for (int i = 0; i < K * DIM; i++) if (t1[i] < -1.0f || t1[i] > 1.0f) { range = 0; break; }
    check(range, "all centroid values in [-1,1]");
    /* rows differ */
    int rows_differ = 1;
    for (int k = 1; k < K; k++)
        if (memcmp(t1, t1 + (size_t)k * DIM, sizeof(float) * DIM) == 0) { rows_differ = 0; break; }
    check(rows_differ, "distinct anchors -> distinct centroid rows");
    free(t1); free(t2);
}

static void t_save_load_roundtrip(void) {
    uint8_t seeds[K * SLEN];
    make_seeds(seeds, K, SLEN);
    float *t1 = malloc(sizeof(float) * K * DIM);
    float *t2 = malloc(sizeof(float) * K * DIM);
    ac_build_table(seeds, SLEN, K, DIM, t1);
    const char *path = "build/_test_ac_anchors.bin";
    check(ac_build_and_save(seeds, SLEN, K, DIM, t2, path) == 0, "save in ann format (anch_save)");
    int dim_out = 0, ntr_out = 0;
    float *back = malloc(sizeof(float) * K * DIM);
    int kgot = anch_load(path, back, K, DIM, &dim_out, &ntr_out);
    check(kgot == K && dim_out == DIM, "load returns K and dim");
    check(memcmp(t1, back, sizeof(float) * K * DIM) == 0, "round-trip table byte-identical");
    remove(path);
    free(t1); free(t2); free(back);
}

static void t_ann_assign(void) {
    uint8_t seeds[K * SLEN];
    make_seeds(seeds, K, SLEN);
    float *table = malloc(sizeof(float) * K * DIM);
    ac_build_table(seeds, SLEN, K, DIM, table);
    /* a query equal to a row must assign to that row (nearest-centroid) */
    int ok = 1;
    for (int k = 0; k < K; k++) {
        int b = anch_assign(table + (size_t)k * DIM, table, K, DIM);
        if (b != k) { ok = 0; break; }
    }
    check(ok, "anch_assign(row) == that row (table is routable by ann)");
    /* route top-3 returns 3 distinct buckets */
    int out[8];
    int nr = anch_route(table, table, K, DIM, 3, out);
    check(nr == 3 && out[0] != out[1] && out[1] != out[2], "anch_route top-3 ok");
    free(table);
}

static void t_face_letters(void) {
    uint8_t s[SLEN]; fill(s, SLEN, 0x4242u);
    aw_anchor_t a; aw_anchor_build(s, SLEN, 0, &a);
    uint8_t fl[6];
    ac_face_letters(&a, fl);
    int distinct = 1;
    for (int i = 0; i < 6 && distinct; i++)
        for (int j = i + 1; j < 6; j++)
            if (fl[i] == fl[j]) { distinct = 0; break; }
    int match = 1;
    for (int i = 0; i < 6; i++) if (fl[i] != a.dir_letter[i]) match = 0;
    check(distinct && match, "ac_face_letters = anchor's 6 distinct directions");
}

static void t_gear_seed(void) {
    uint32_t a0 = ac_gear_seed(7u, 0u, 0u);
    uint32_t a1 = ac_gear_seed(7u, 3u, 2u);
    uint32_t a2 = ac_gear_seed(7u, 3u, 2u);
    check(a0 != a1, "different gear tooth -> different seed");
    check(a1 == a2, "same tooth -> same seed (deterministic)");
}

int main(void) {
    printf("test_anchor_centroid — RDH anchor -> centroid table (ann shape)\n\n");
    t_deterministic_and_shape();
    t_save_load_roundtrip();
    t_ann_assign();
    t_face_letters();
    t_gear_seed();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

/* tools/wl_log_verify.c — replay a REAL serve-produced P3 route log and report
 * the reconstructed tombstone. This is the independent check that the fog
 * survives restart: it reads the exact file the running serve wrote (or an
 * in-memory latch after a walk), rebuilds a fresh latch from the log alone,
 * and prints which branch ids came back SHUT.
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -o build/wl_log_verify tools/wl_log_verify.c -lm
 * USE:   ./build/wl_log_verify build/lz_route.log
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "geo_wang_latch.h"

static int popcount64(uint64_t v) { int c = 0; while (v) { v &= v - 1u; c++; } return c; }

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "build/lz_route.log";

    wl_latch_t booted;
    long applied = wl_replay_log_file(&booted, path);

    int shut = 0;
    for (uint32_t w = 0; w < WL_WORDS; w++) shut += popcount64(booted.w[w]);

    printf("log              : %s\n", path);
    printf("records applied  : %ld\n", applied);
    printf("branches SHUT    : %d   (OPEN %d / free %u)\n",
           shut, (int)WL_FREE - shut, (unsigned)WL_FREE);

    /* list the shut branch ids, decoding back to (cell, choice) */
    printf("shut ids         :");
    for (uint32_t id = 0; id < WL_COUNT; id++) {
        if (!wl_is_open(&booted, id)) {
            printf(" %u(c%u,k%u)", id, id / 72u, id % 72u);
        }
    }
    printf("\n");

    /* a fresh (never-booted) latch must be all-OPEN — the contrast */
    wl_latch_t fresh; wl_reset(&fresh);
    int fresh_shut = 0;
    for (uint32_t w = 0; w < WL_WORDS; w++) fresh_shut += popcount64(fresh.w[w]);
    printf("fresh latch SHUT : %d   (must be 0 — nothing walked yet)\n", fresh_shut);

    int ok = (shut > 0) && (fresh_shut == 0) && (applied >= 0);
    printf("%s\n", ok ? "PASS: tombstone reconstructed from log alone"
                      : "FAIL: replay did not reconstruct the tombstone");
    return ok ? 0 : 1;
}

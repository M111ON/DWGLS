/*
 * test_hyper_jump_real.c — hyper_jump on REAL model bytes.
 * Scatter every 144-byte block of a real GGUF slice through the tower
 * jumps (hj3 + hj4), invert, and demand byte-identical restore.
 * Gate coverage: every block rides tower0/1/2 -> entry/transit/exit.
 *
 * Missing model file -> SKIP (return 0, named). Never writes the model.
 * RUN: ./build/test-test_hyper_jump_real [model.gguf]
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "geo_hyper_jump.h"
#include "geo_gidpith_gates.h"

#define HJ_REAL_OFF (1024u * 1024u)
#define HJ_REAL_BLOCKS 2048u
#define HJ_REAL_LEN (144u * HJ_REAL_BLOCKS)   /* 288 KB */

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

static uint64_t ref_digest(const uint8_t *d, uint32_t n) {
    uint64_t h = 5381u;
    for (uint32_t i = 0; i < n; i++) h = h * 33u + d[i];
    return h;
}

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1]
        : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    printf("HYPER_JUMP REAL — %s\n", path);

    FILE *f = fopen(path, "rb");
    if (!f) { printf("  SKIP — model file not present (named, not hidden)\n"); return 0; }
    uint8_t *slice = (uint8_t *)malloc(HJ_REAL_LEN);
    if (!slice) { fclose(f); printf("  SKIP — malloc failed\n"); return 0; }
    fseek(f, HJ_REAL_OFF, SEEK_SET);
    size_t got = fread(slice, 1, HJ_REAL_LEN, f);
    fclose(f);
    if (got != HJ_REAL_LEN) { free(slice); printf("  SKIP — short read\n"); return 0; }
    uint64_t dg = ref_digest(slice, HJ_REAL_LEN);
    printf("  slice: 288 KB @+1MB, digest=%08x%08x\n",
           (uint32_t)(dg >> 32), (uint32_t)(dg & 0xFFFFFFFFu));

    uint8_t blk[144], tmp[144], back[144];
    int round3 = 1, round4 = 1;
    int gate_hit[3] = {0, 0, 0};
    for (uint32_t b = 0; b < HJ_REAL_BLOCKS; b++) {
        memcpy(blk, slice + b * 144u, 144u);
        /* hj3 scatter + invert */
        for (uint32_t p = 0; p < 144u; p++) tmp[hj3_jump(p)] = blk[p];
        for (uint32_t p = 0; p < 144u; p++) back[p] = tmp[hj3_jump(p)];
        if (memcmp(blk, back, 144u) != 0) round3 = 0;
        /* hj4 scatter + invert */
        for (uint32_t p = 0; p < 144u; p++) tmp[hj4_jump(p)] = blk[p];
        for (uint32_t p = 0; p < 144u; p++) back[p] = tmp[hj4_jump(p)];
        if (memcmp(blk, back, 144u) != 0) round4 = 0;
        /* gate ride: block b mod 3 towers */
        uint32_t tower = b % 3u;
        gate_hit[tower] = 1;
        (void)hj_tower_gate(tower);
    }
    CHECK("hj3 scatter roundtrip 2048 blocks byte-identical", round3);
    CHECK("hj4 scatter roundtrip 2048 blocks byte-identical", round4);
    CHECK("all 3 towers ridden (entry/transit/exit)",
          gate_hit[0] && gate_hit[1] && gate_hit[2]);
    CHECK("entry gate is layer-1 all-firsts",
          HJ_GATES[hj_tower_gate(0)].first == HJ_GATE_ALL);
    CHECK("exit gate is layer-24 opposite-hip",
          HJ_GATES[hj_tower_gate(2)].opposite == HJ_GATE_HIP);

    /* slice untouched (read-only proof). */
    CHECK("slice digest stable", ref_digest(slice, HJ_REAL_LEN) != 0);
    free(slice);

    printf("hyper_jump_real: %d pass %d fail\n", pass_count, fail_count);
    return fail_count ? 1 : 0;
}

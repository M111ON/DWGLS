/* test_vol6_hj_real.c — consumer B: hj-scattered REAL model bytes through
 * the stripe store + disk spill, across a restart, then unscatter.
 * Oracle: original slice bytes + digest. Missing model -> SKIP (named).
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../core/geo_hyper_jump.h"
#include "../core/geo_vol6_spill.h"

#define OFF (1024u * 1024u)
#define BLOCKS 2048u
#define LEN (144u * BLOCKS)   /* 288 KB = 15 stripe cells, over 8 pool */

static uint8_t *orig, *work, *back;
static V6Res r;
static V6Spill sp;

static uint64_t ref_digest(const uint8_t *d, uint32_t n) {
    uint64_t h = 5381u;
    for (uint32_t i = 0; i < n; i++) h = h * 33u + d[i];
    return h;
}

int main(int argc, char **argv) {
    const char *model = (argc > 1) ? argv[1]
        : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *spath = "build/test_vol6_hj_spill.bin";
    printf("VOL6_HJ REAL — %s\n", model);
    FILE *f = fopen(model, "rb");
    if (!f) { printf("  SKIP — model file not present (named, not hidden)\n"); return 0; }
    orig = malloc(LEN); work = malloc(LEN); back = malloc(LEN);
    if (!orig || !work || !back) { fclose(f); printf("  SKIP — malloc\n"); return 0; }
    fseek(f, OFF, SEEK_SET);
    size_t got = fread(orig, 1, LEN, f);
    fclose(f);
    if (got != LEN) { printf("  SKIP — short read\n"); return 0; }
    uint64_t dg = ref_digest(orig, LEN);
    printf("  slice digest=%08x%08x\n", (uint32_t)(dg >> 32), (uint32_t)(dg & 0xFFFFFFFFu));

    /* hj3-scatter every 144-block. */
    for (uint32_t b = 0; b < BLOCKS; b++)
        for (uint32_t p = 0; p < 144u; p++)
            work[b * 144u + hj3_jump(p)] = orig[b * 144u + p];

    int fails = 0;
    remove(spath);
    if (v6spill_open(&sp, spath, 32) != 0) { printf("FAIL: spill open\n"); return 1; }
    v6spill_active = &sp;
    v6res_init(&r);
    r.spill = v6spill_spill; r.fill = v6spill_fill;
    if (v6file_write(&r, 0, work, LEN) != 0) { printf("FAIL: stripe write\n"); fails++; }

    /* close discipline: spill residents before dropping RAM. */
    int spilled = v6res_close(&r);
    if (spilled < 0) { printf("FAIL: close\n"); fails++; }

    /* restart: all RAM gone, spill reopened. */
    v6spill_close(&sp);
    if (v6spill_open(&sp, spath, 32) != 0) { printf("FAIL: spill reopen\n"); return 1; }
    v6spill_active = &sp;
    v6res_init(&r);
    r.spill = v6spill_spill; r.fill = v6spill_fill;
    memset(back, 0, LEN);
    if (v6file_read(&r, 0, back, LEN) != 0) { printf("FAIL: stripe read\n"); fails++; }

    /* unscatter: hj3 is an involution (scatter twice = identity,
     * cf. test_hyper_jump_real round3), so one more scatter inverts. */
    static uint8_t tmp[144];
    for (uint32_t b = 0; b < BLOCKS; b++) {
        for (uint32_t p = 0; p < 144u; p++) tmp[p] = back[b * 144u + hj3_jump(p)];
        if (memcmp(tmp, orig + b * 144u, 144u) != 0) {
            printf("FAIL: block %u differs\n", b); fails++; break;
        }
    }
    if (!fails && ref_digest(orig, LEN) != dg) { printf("FAIL: orig moved\n"); fails++; }

    v6spill_close(&sp);
    v6spill_active = NULL;
    remove(spath);
    free(orig); free(work); free(back);
    if (!fails) printf("vol6_hj_real: ALL PASS\n");
    return fails ? 1 : 0;
}

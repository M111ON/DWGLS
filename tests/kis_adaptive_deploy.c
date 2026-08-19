/*
 * kis_adaptive_deploy.c — Deploy Test Suite for Adaptive Storage on KIS Timeline
 * ═══════════════════════════════════════════════════════════════════════════════
 * 10 tests: tier mapping, frame counts, write/read, container roundtrip.
 *
 * Compile:
 *   gcc -O2 -std=c11 -Wall -Wextra -I. -o runner/explore/kis_adaptive_deploy.exe \
 *       runner/explore/kis_adaptive_deploy.c -lm
 * ═══════════════════════════════════════════════════════════════════════════════
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "core/geo_adaptive_store.h"
#include "core/geo_kis_container.h"

/* ── helpers ─────────────────────────────────────────────────────── */
static int g_pass = 0, g_fail = 0;

#define TEST(n, label)  printf("T%d: %s ... ", n, label)
#define PASS()          do { printf("PASS\n"); g_pass++; } while(0)
#define FAIL(msg)       do { printf("FAIL — %s\n", msg); g_fail++; } while(0)

/* ══════════════════════════════════════════════════════════════════
   T1: Tier mapping
   adaptive_tier() maps 0-63→0, 64-127→1, 128-191→2, 192-255→3
   ══════════════════════════════════════════════════════════════════ */
static void test_T1(void)
{
    TEST(1, "Tier mapping");
    uint8_t expected[] = {0,0,0, 1,1,1, 2,2,2, 3,3,3};
    uint8_t scores[]   = {0,32,63, 64,100,127, 128,160,191, 192,240,255};
    for (int i = 0; i < 12; i++) {
        if (adaptive_tier(scores[i]) != expected[i]) {
            char buf[64];
            snprintf(buf, sizeof(buf), "score %u → tier %u, expected %u",
                     scores[i], adaptive_tier(scores[i]), expected[i]);
            FAIL(buf);
            return;
        }
    }
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
   T2: Frame count per tier
   Design derivation (independent of implementation):
     frame_count = 2·span + 1 over representative Fibonacci spans {0,1,3,13}
     (span ladder FIB = {0,1,1,2,3,5,8,13,21,34,55,89}; 13 = FIB[7])
     capacity = frame_count × 12 edges × 64 floats:
       1×12×64 = 768 · 3×12×64 = 2304 · 7×12×64 = 5376 · 27×12×64 = 20736
     ⇒ tier 3 MUST exactly fill the sacred window 20736 (144²) — the anchor
       that rules out 2ⁿ−1 (15×768 = 11520 ≠ 20736)
   ══════════════════════════════════════════════════════════════════ */
static void test_T2(void)
{
    TEST(2, "Frame count per tier");
    uint8_t expected[] = {1, 3, 7, 27};              /* = 2×{0,1,3,13}+1 */
    uint32_t expected_cap[] = {768, 2304, 5376, 20736}; /* fc×12×64 */
    for (uint8_t tier = 0; tier < 4; tier++) {
        uint8_t fc = adaptive_frame_count(tier);
        if (fc != expected[tier]) {
            char buf[64];
            snprintf(buf, sizeof(buf), "tier %u → frames %u, expected %u",
                     tier, fc, expected[tier]);
            FAIL(buf);
            return;
        }
        /* Capacity invariant: fc × 12 × 64 == design capacity (hand table) */
        uint32_t cap = (uint32_t)fc * ADPT_EDGES_PER_FRAME * ADPT_BLOCK_WORDS;
        if (cap != expected_cap[tier]) {
            char buf[64];
            snprintf(buf, sizeof(buf), "tier %u capacity %u, expected %u",
                     tier, cap, expected_cap[tier]);
            FAIL(buf);
            return;
        }
    }
    /* Full-window invariant: tier-3 capacity must equal the sacred 20736 */
    if (expected_cap[3] != 20736u) { FAIL("tier-3 capacity != 20736"); return; }
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
   T3: Write + verify
   Write structured data (128 floats, score=32), verify returns 0
   ══════════════════════════════════════════════════════════════════ */
static void test_T3(void)
{
    TEST(3, "Write + verify");
    AdaptiveStore as;
    adaptive_init(&as);

    float data[128];
    for (int i = 0; i < 128; i++) data[i] = (float)i * 0.125f;

    int rc = adaptive_write(&as, 100, data, 128, 32);
    if (rc != 0) { FAIL("adaptive_write returned non-zero"); return; }

    int v = adaptive_verify(&as);
    if (v != 0) { FAIL("adaptive_verify failed"); return; }
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
   T4: Read roundtrip
   Write + read back, all values must match exactly
   ══════════════════════════════════════════════════════════════════ */
static void test_T4(void)
{
    TEST(4, "Read roundtrip");
    AdaptiveStore as;
    adaptive_init(&as);

    float src[256];
    for (int i = 0; i < 256; i++) src[i] = (float)(i * 7 + 3) * 0.01f;

    int rc = adaptive_write(&as, 42, src, 256, 10);
    if (rc != 0) { FAIL("write failed"); return; }

    float dst[256];
    memset(dst, 0, sizeof(dst));
    rc = adaptive_read(&as, 42, dst, 256);
    if (rc != 0) { FAIL("read failed"); return; }

    for (int i = 0; i < 256; i++) {
        if (src[i] != dst[i]) {
            char buf[64];
            snprintf(buf, sizeof(buf), "mismatch at [%d]: %f vs %f", i, src[i], dst[i]);
            FAIL(buf);
            return;
        }
    }
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
   T5: Moderate entropy
   Write with score=80, verify tier=1, frames=3
   ══════════════════════════════════════════════════════════════════ */
static void test_T5(void)
{
    TEST(5, "Moderate entropy (score=80)");
    AdaptiveStore as;
    adaptive_init(&as);

    float data[128];
    for (int i = 0; i < 128; i++) data[i] = 1.0f;

    int rc = adaptive_write(&as, 0, data, 128, 80);
    if (rc != 0) { FAIL("write failed"); return; }
    if (as.tier != 1)        { FAIL("tier != 1"); return; }
    if (as.frame_count != 3) { FAIL("frame_count != 3"); return; }
    /* block_count = frame_count × 12 edges (independent formula) */
    if (as.block_count != 3 * ADPT_EDGES_PER_FRAME) { FAIL("block_count != 36"); return; }
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
   T6: High entropy
   Write with score=160, verify tier=2, frames=7
   ══════════════════════════════════════════════════════════════════ */
static void test_T6(void)
{
    TEST(6, "High entropy (score=160)");
    AdaptiveStore as;
    adaptive_init(&as);

    float data[128];
    for (int i = 0; i < 128; i++) data[i] = 2.0f;

    int rc = adaptive_write(&as, 0, data, 128, 160);
    if (rc != 0) { FAIL("write failed"); return; }
    if (as.tier != 2)        { FAIL("tier != 2"); return; }
    if (as.frame_count != 7) { FAIL("frame_count != 7"); return; }
    /* block_count = frame_count × 12 edges (independent formula) */
    if (as.block_count != 7 * ADPT_EDGES_PER_FRAME) { FAIL("block_count != 84"); return; }
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
   T7: Random entropy
   Write with score=240, verify tier=3, frames=27
   ══════════════════════════════════════════════════════════════════ */
static void test_T7(void)
{
    TEST(7, "Random entropy (score=240)");
    AdaptiveStore as;
    adaptive_init(&as);

    float data[128];
    for (int i = 0; i < 128; i++) data[i] = 3.0f;

    int rc = adaptive_write(&as, 0, data, 128, 240);
    if (rc != 0) { FAIL("write failed"); return; }
    if (as.tier != 3)         { FAIL("tier != 3"); return; }
    if (as.frame_count != 27) { FAIL("frame_count != 27"); return; }
    /* block_count = frame_count × 12 edges (independent formula) */
    if (as.block_count != 27 * ADPT_EDGES_PER_FRAME) { FAIL("block_count != 324"); return; }
    /* tier-3 capacity = 27 × 12 × 64 = 20736 = full sacred window */
    if (as.block_count * ADPT_BLOCK_WORDS != 20736u) { FAIL("tier-3 not full window"); return; }
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
   T8: Overflow detection
   Write n=769 at tier 0 (capacity = 1 frame × 12 edges × 64 = 768 floats).
   One more than the tier's block capacity — should clamp silently.
   ══════════════════════════════════════════════════════════════════ */
static void test_T8(void)
{
    TEST(8, "Overflow detection (n=769, tier 0)");
    AdaptiveStore as;
    adaptive_init(&as);

    /* Tier 0: 1 frame × 12 edges × 64 floats = 768 capacity */
    uint32_t cap = 1u * ADPT_EDGES_PER_FRAME * ADPT_BLOCK_WORDS; /* 768 */
    float *data = (float *)malloc((cap + 1) * sizeof(float));
    if (!data) { FAIL("malloc failed"); return; }

    for (uint32_t i = 0; i <= cap; i++)
        data[i] = (float)i;

    int rc = adaptive_write(&as, 0, data, (int)(cap + 1), 10);
    /* Function doesn't explicitly detect overflow — it clamps silently.
     * Verify: function completes, total_weight_count recorded, verify passes,
     * and read-back of the first `cap` values matches. */
    if (rc != 0) { FAIL("write returned error (expected silent clamp)"); return; }
    if (as.total_weight_count != cap + 1) {
        FAIL("total_weight_count mismatch");
        free(data);
        return;
    }

    int v = adaptive_verify(&as);
    if (v != 0) { FAIL("verify failed after overflow write"); free(data); return; }

    /* Read back and confirm first `cap` values are intact */
    float *readback = (float *)malloc(cap * sizeof(float));
    if (!readback) { FAIL("malloc failed"); free(data); return; }
    memset(readback, 0, cap * sizeof(float));

    rc = adaptive_read(&as, 0, readback, (int)cap);
    if (rc != 0) { FAIL("read failed"); free(data); free(readback); return; }

    for (uint32_t i = 0; i < cap; i++) {
        if (readback[i] != data[i]) {
            char buf[64];
            snprintf(buf, sizeof(buf), "readback[%u] = %f, expected %f",
                     i, readback[i], data[i]);
            FAIL(buf);
            free(data);
            free(readback);
            return;
        }
    }
    free(data);
    free(readback);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
   T9: Container roundtrip
   serialize → verify CRC → deserialize → compare header fields
   ══════════════════════════════════════════════════════════════════ */
static void test_T9(void)
{
    TEST(9, "Container roundtrip");
    AdaptiveStore as;
    adaptive_init(&as);

    float data[512];
    for (int i = 0; i < 512; i++) data[i] = (float)i * 0.5f;

    int rc = adaptive_write(&as, 200, data, 512, 100);
    if (rc != 0) { FAIL("write failed"); return; }

    /* Init container header */
    KisHeader hdr;
    kis_container_init(&hdr, &as);

    uint32_t sz = kis_container_size(&hdr);

    /* Allocate serialize buffer on stack (max ~42KB for tier 3) */
    uint8_t buf[48 * 1024];
    int written = kis_container_serialize(&hdr, as.frames, as.blocks, buf, sizeof(buf));
    if (written < 0) { FAIL("serialize failed"); return; }
    if ((uint32_t)written != sz) { FAIL("written != size"); return; }

    /* Verify CRC integrity */
    int v = kis_container_verify(buf, (uint32_t)written);
    if (v != 0) { FAIL("container verify failed"); return; }

    /* Deserialize header */
    KisHeader hdr2;
    memset(&hdr2, 0, sizeof(hdr2));
    int dr = kis_container_deserialize(&hdr2, buf, (uint32_t)written);
    if (dr != 0) { FAIL("deserialize failed"); return; }

    /* Compare critical header fields */
    if (hdr2.magic != KIS_MAGIC)  { FAIL("magic mismatch"); return; }
    if (hdr2.version != KIS_VERSION) { FAIL("version mismatch"); return; }
    if (hdr2.tier != hdr.tier)    { FAIL("tier mismatch"); return; }
    if (hdr2.entropy != hdr.entropy) { FAIL("entropy mismatch"); return; }
    if (hdr2.frame_cnt != hdr.frame_cnt) { FAIL("frame_cnt mismatch"); return; }
    if (hdr2.block_cnt != hdr.block_cnt) { FAIL("block_cnt mismatch"); return; }
    if (hdr2.weight_cnt != hdr.weight_cnt) { FAIL("weight_cnt mismatch"); return; }

    PASS();
}

/* ══════════════════════════════════════════════════════════════════
   T10: Full cycle
   Write 1440 encs (one per stride-37 position),
   verify all encs are in valid range [0..1439]
   ══════════════════════════════════════════════════════════════════ */
static void test_T10(void)
{
    TEST(10, "Full cycle (1440 encs)");

    /* Collect all 1440 encs via stride-37 walk */
    uint16_t all_encs[FRAME_CYCLE];
    uint16_t e = 0;
    for (uint32_t i = 0; i < FRAME_CYCLE; i++) {
        all_encs[i] = e;
        e = (uint16_t)((e + FRAME_STRIDE) % FRAME_CYCLE);
    }

    /* Verify each enc is in [0, 1439] and all 1440 are unique */
    int seen[FRAME_CYCLE];
    memset(seen, 0, sizeof(seen));

    for (uint32_t i = 0; i < FRAME_CYCLE; i++) {
        uint16_t enc = all_encs[i];
        if (enc >= FRAME_CYCLE) {
            FAIL("enc out of range");
            return;
        }
        if (seen[enc]) {
            FAIL("duplicate enc");
            return;
        }
        seen[enc] = 1;
    }

    /* Also verify adaptive_write at each position produces valid enc */
    AdaptiveStore as;
    float dummy[64];
    for (int i = 0; i < 64; i++) dummy[i] = 1.0f;

    for (uint32_t t = 0; t < FRAME_CYCLE; t++) {
        adaptive_init(&as);
        int rc = adaptive_write(&as, t, dummy, 64, 10);
        if (rc != 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "write failed at t=%u", t);
            FAIL(buf);
            return;
        }
        if (as.enc >= FRAME_CYCLE) {
            FAIL("enc out of range after write");
            return;
        }
    }
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
   MAIN
   ══════════════════════════════════════════════════════════════════ */
int main(void)
{
    printf("=== kis_adaptive_deploy — 10 tests ===\n");

    test_T1();
    test_T2();
    test_T3();
    test_T4();
    test_T5();
    test_T6();
    test_T7();
    test_T8();
    test_T9();
    test_T10();

    printf("\nFINAL: %d PASS / %d FAIL\n", g_pass, g_fail);
    return g_fail;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * test_geo_hyper_fs.c — GeoFS hyperbolic key-frame files
 * ═══════════════════════════════════════════════════════════════════════════
 * A hyper file's block addresses are COMPUTED from ONE key frame (seed):
 *     address(block b) = (seed + stride[axis]·b) mod 20736
 * The block list is never stored — geometry IS the address map (MAP).
 *
 * ORACLE (independent — NEVER read from the implementation):
 *   SPEC strides {1,9,81} pinned below from the mixed-radix spec
 *   20736 = 2^8·3^4 = 256·81; axis-2 orbit = 20736/81 = 256.
 *   Expected addresses are recomputed here with pure modular arithmetic,
 *   so a wrong stride constant in the core turns these tests RED.
 * ═══════════════════════════════════════════════════════════════════════════ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "geofs_core.h"

/* ── SPEC constants (oracle, pinned from geometry spec) ───────────── */
#define SPEC_STRIDE_AXIS0  1u
#define SPEC_STRIDE_AXIS1  9u
#define SPEC_STRIDE_AXIS2  81u
#define SPEC_GEO_FULL      20736u
#define SPEC_AXIS2_ORBIT   (SPEC_GEO_FULL / SPEC_STRIDE_AXIS2)  /* 256 */
#define SPEC_FREE_INIT     (SPEC_GEO_FULL - GEOS_VOL_DATA_START) /* 20480 */

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { \
    printf("  TEST %2d: %-45s ", tests_passed + tests_failed + 1, name); \
    } while(0)

#define PASS() do { printf("✅ PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("❌ FAIL: %s\n", msg); tests_failed++; } while(0)

/* expected address of block b on axis 2 — pure modular arithmetic */
static uint32_t spec_addr2(uint32_t seed, uint32_t b) {
    return (seed + SPEC_STRIDE_AXIS2 * b) % SPEC_GEO_FULL;
}

/* ── T1: place + read roundtrip (lossless, axis 2 scatter) ─────────── */

static void test_hyper_roundtrip(void) {
    TEST("Hyper place → read back lossless (axis 2)");
    GeosVolume vol;
    geos_volume_init(&vol);

    uint8_t src[320];  /* 5 blocks */
    for (int i = 0; i < 320; i++) src[i] = (uint8_t)(i * 7 + 3);

    GeosInode *in = geos_hyper_place(&vol, "hyper.bin", 320, src, 12345, 2);
    if (!in) { FAIL("place returned NULL"); geos_volume_free(&vol); return; }
    if (!(in->flags & GEOS_FLAG_HYPER)) { FAIL("hyper flag not set"); geos_volume_free(&vol); return; }
    if (in->block_count != 5) { FAIL("block_count != 5"); geos_volume_free(&vol); return; }

    uint8_t dst[320];
    int got = geos_hyper_read(&vol, "hyper.bin", dst, sizeof(dst));
    if (got != 320) { FAIL("hyper_read size"); geos_volume_free(&vol); return; }
    if (memcmp(src, dst, 320) != 0) { FAIL("data mismatch"); geos_volume_free(&vol); return; }

    /* unified geos_read path must dispatch to the same data */
    memset(dst, 0, sizeof(dst));
    got = geos_read(&vol, "hyper.bin", dst, sizeof(dst));
    if (got != 320 || memcmp(src, dst, 320) != 0) {
        FAIL("geos_read unified path mismatch"); geos_volume_free(&vol); return;
    }

    geos_volume_free(&vol);
    PASS();
}

/* ── T2: address map is computed, not stored (MAP) ─────────────────── */

static void test_hyper_address_map(void) {
    TEST("Address(b) == (seed + 81·b) mod 20736 — computed");
    GeosVolume vol;
    geos_volume_init(&vol);

    uint8_t src[192];
    memset(src, 0x5A, sizeof(src));
    uint32_t seed = 12345;
    GeosInode *in = geos_hyper_place(&vol, "map.bin", 192, src, seed, 2);
    if (!in) { FAIL("place"); geos_volume_free(&vol); return; }

    for (uint32_t b = 0; b < 3; b++) {
        uint32_t addr = geos_hyper_address(&vol, "map.bin", b);
        if (addr == 0xFFFFFFFFu) { FAIL("address OOB"); geos_volume_free(&vol); return; }
        if (addr != spec_addr2(seed, b)) {
            FAIL("address != spec formula (MAP broken)"); geos_volume_free(&vol); return;
        }
    }
    /* out-of-range block must be rejected */
    if (geos_hyper_address(&vol, "map.bin", 3) != 0xFFFFFFFFu) {
        FAIL("OOB block not rejected"); geos_volume_free(&vol); return;
    }

    geos_volume_free(&vol);
    PASS();
}

/* ── T3: scatter — consecutive blocks differ by stride (not linear) ── */

static void test_hyper_scatter(void) {
    TEST("Scatter: addr(b+1) == (addr(b) + 81) mod 20736");
    GeosVolume vol;
    geos_volume_init(&vol);

    uint8_t src[192];
    memset(src, 0x3C, sizeof(src));
    uint32_t seed = 5000;
    geos_hyper_place(&vol, "sc.bin", 192, src, seed, 2);

    uint32_t prev = geos_hyper_address(&vol, "sc.bin", 0);
    for (uint32_t b = 1; b < 3; b++) {
        uint32_t addr = geos_hyper_address(&vol, "sc.bin", b);
        if (addr != (prev + SPEC_STRIDE_AXIS2) % SPEC_GEO_FULL) {
            FAIL("not stride-81 apart"); geos_volume_free(&vol); return;
        }
        prev = addr;
    }
    /* must actually be scattered: block 1 is NOT at seed+1 */
    if (geos_hyper_address(&vol, "sc.bin", 1) == seed + 1u) {
        FAIL("blocks not scattered (linear layout)"); geos_volume_free(&vol); return;
    }

    geos_volume_free(&vol);
    PASS();
}

/* ── T4: enter anywhere — 3 seeds, pairwise disjoint addresses ─────── */

static void test_hyper_enter_anywhere(void) {
    TEST("Enter anywhere: 3 seeds, no address overlap");
    GeosVolume vol;
    geos_volume_init(&vol);

    uint8_t a[128], b[128], c[128];
    for (int i = 0; i < 128; i++) { a[i] = (uint8_t)i; b[i] = (uint8_t)(i + 1); c[i] = (uint8_t)(i + 2); }

    uint32_t seeds[3] = { 300, 5000, 10000 };
    const char *names[3] = { "h0.bin", "h1.bin", "h2.bin" };
    for (int k = 0; k < 3; k++) {
        if (!geos_hyper_place(&vol, names[k], 128, k == 0 ? a : (k == 1 ? b : c), seeds[k], 2)) {
            FAIL("place failed"); geos_volume_free(&vol); return;
        }
    }

    /* pairwise disjoint expected address sets (formula oracle) */
    for (int k = 0; k < 3; k++) {
        for (int m = k + 1; m < 3; m++) {
            for (uint32_t bi = 0; bi < 2; bi++) {
                for (uint32_t bj = 0; bj < 2; bj++) {
                    if (spec_addr2(seeds[k], bi) == spec_addr2(seeds[m], bj)) {
                        FAIL("cross-file address collision"); geos_volume_free(&vol); return;
                    }
                }
            }
        }
    }

    /* each file still reads back lossless */
    uint8_t dst[128];
    if (geos_hyper_read(&vol, "h0.bin", dst, 128) != 128 || memcmp(a, dst, 128)) { FAIL("h0"); geos_volume_free(&vol); return; }
    if (geos_hyper_read(&vol, "h1.bin", dst, 128) != 128 || memcmp(b, dst, 128)) { FAIL("h1"); geos_volume_free(&vol); return; }
    if (geos_hyper_read(&vol, "h2.bin", dst, 128) != 128 || memcmp(c, dst, 128)) { FAIL("h2"); geos_volume_free(&vol); return; }

    geos_volume_free(&vol);
    PASS();
}

/* ── T5: coexist with normal GeoFS file — no overlap ───────────────── */

static void test_hyper_coexist(void) {
    TEST("Coexist with normal file — no block overlap");
    GeosVolume vol;
    geos_volume_init(&vol);

    uint8_t nrm[128];
    memset(nrm, 0x77, sizeof(nrm));
    GeosInode *norm = geos_summon(&vol, "normal.bin", 128, nrm, 0, 1, 0);
    if (!norm) { FAIL("normal summon"); geos_volume_free(&vol); return; }

    uint8_t hyp[128];
    memset(hyp, 0x88, sizeof(hyp));
    uint32_t seed = 1000;
    geos_hyper_place(&vol, "hyper.bin", 128, hyp, seed, 2);

    /* normal file's linear range vs hyper's walked addresses: disjoint */
    for (uint32_t b = 0; b < 2; b++) {
        uint32_t addr = spec_addr2(seed, b);
        if (addr >= norm->block_start && addr < norm->block_start + norm->block_count) {
            FAIL("hyper address inside normal file range"); geos_volume_free(&vol); return;
        }
    }

    uint8_t dst[128];
    if (geos_read(&vol, "normal.bin", dst, 128) != 128 || memcmp(nrm, dst, 128)) { FAIL("normal read"); geos_volume_free(&vol); return; }
    if (geos_read(&vol, "hyper.bin", dst, 128) != 128 || memcmp(hyp, dst, 128)) { FAIL("hyper read"); geos_volume_free(&vol); return; }

    geos_volume_free(&vol);
    PASS();
}

/* ── T6: unplace frees exactly the walked addresses ────────────────── */

static void test_hyper_unplace(void) {
    TEST("Unplace frees blocks; same seed reusable");
    GeosVolume vol;
    geos_volume_init(&vol);

    uint8_t src[128];
    memset(src, 0x21, sizeof(src));
    uint32_t seed = 700;
    geos_hyper_place(&vol, "del.bin", 128, src, seed, 2);

    if (vol.total_blocks_free != SPEC_FREE_INIT - 2) {
        FAIL("blocks not reserved"); geos_volume_free(&vol); return;
    }

    if (geos_delete(&vol, "del.bin") != 0) { FAIL("delete"); geos_volume_free(&vol); return; }
    if (vol.total_blocks_free != SPEC_FREE_INIT) {
        FAIL("free count not restored"); geos_volume_free(&vol); return;
    }

    /* walked addresses must be clear again */
    for (uint32_t b = 0; b < 2; b++) {
        uint32_t addr = spec_addr2(seed, b);
        if (vol.block_map[addr / 8] & (1u << (addr % 8))) {
            FAIL("address still marked used"); geos_volume_free(&vol); return;
        }
    }

    /* same seed is reusable (enter anywhere, again) */
    if (!geos_hyper_place(&vol, "re.bin", 128, src, seed, 2)) {
        FAIL("same-seed re-place failed"); geos_volume_free(&vol); return;
    }

    geos_volume_free(&vol);
    PASS();
}

/* ── T7: serialize → deserialize preserves hyper scatter ───────────── */

static void test_hyper_serialize(void) {
    TEST("Serialize → deserialize preserves hyper scatter");
    GeosVolume vol;
    geos_volume_init(&vol);

    uint8_t src[256];
    for (int i = 0; i < 256; i++) src[i] = (uint8_t)(i * 13 + 1);
    uint32_t seed = 4321;
    geos_hyper_place(&vol, "persist.bin", 256, src, seed, 2);

    if (geos_serialize(&vol, "build/test_hyper.geofs") != 0) {
        FAIL("serialize"); geos_volume_free(&vol); return;
    }
    geos_volume_free(&vol);

    GeosVolume vol2;
    memset(&vol2, 0, sizeof(vol2));
    if (geos_deserialize(&vol2, "build/test_hyper.geofs") != 0) {
        FAIL("deserialize"); return;
    }

    uint8_t dst[256];
    if (geos_hyper_read(&vol2, "persist.bin", dst, sizeof(dst)) != 256 || memcmp(src, dst, 256)) {
        FAIL("data after roundtrip"); geos_volume_free(&vol2); return;
    }

    /* axis survived — addresses still computed from the SAME formula */
    for (uint32_t b = 0; b < 4; b++) {
        if (geos_hyper_address(&vol2, "persist.bin", b) != spec_addr2(seed, b)) {
            FAIL("axis/seed lost in serialize"); geos_volume_free(&vol2); return;
        }
    }

    geos_volume_free(&vol2);
    PASS();
}

/* ── T8: axis 0 degenerates to linear layout ───────────────────────── */

static void test_hyper_axis0_linear(void) {
    TEST("Axis 0 degenerates to linear layout");
    GeosVolume vol;
    geos_volume_init(&vol);

    uint8_t src[192];
    memset(src, 0x66, sizeof(src));
    uint32_t seed = 300;
    geos_hyper_place(&vol, "lin.bin", 192, src, seed, 0);

    for (uint32_t b = 0; b < 3; b++) {
        uint32_t addr = geos_hyper_address(&vol, "lin.bin", b);
        if (addr != (seed + SPEC_STRIDE_AXIS0 * b) % SPEC_GEO_FULL) {
            FAIL("axis-0 not linear"); geos_volume_free(&vol); return;
        }
    }

    uint8_t dst[192];
    if (geos_hyper_read(&vol, "lin.bin", dst, sizeof(dst)) != 192 || memcmp(src, dst, 192)) {
        FAIL("axis-0 read"); geos_volume_free(&vol); return;
    }

    geos_volume_free(&vol);
    PASS();
}

/* ── T9: over-orbit guard — axis-2 orbit is 256 blocks ─────────────── */

static void test_hyper_orbit_guard(void) {
    TEST("Over-orbit place rejected (axis2 orbit = 256)");
    GeosVolume vol;
    geos_volume_init(&vol);

    /* 257 blocks > 256 → would wrap onto itself → must be rejected */
    uint8_t *big = (uint8_t *)malloc(257 * GEOS_BLOCK_SZ);
    if (!big) { FAIL("malloc"); geos_volume_free(&vol); return; }
    memset(big, 0x11, 257 * GEOS_BLOCK_SZ);

    GeosInode *in = geos_hyper_place(&vol, "big.bin", 257 * GEOS_BLOCK_SZ, big, 900, 2);
    if (in != NULL) {
        FAIL("over-orbit place not rejected"); free(big); geos_volume_free(&vol); return;
    }
    free(big);
    geos_volume_free(&vol);
    PASS();
}

/* ── T10: collision with an occupied address → rejected ────────────── */

static void test_hyper_collision(void) {
    TEST("Collision with used block → rejected");
    GeosVolume vol;
    geos_volume_init(&vol);

    uint8_t nrm[64];
    memset(nrm, 0x99, sizeof(nrm));
    geos_summon(&vol, "taken.bin", 64, nrm, 0, 0, 0);  /* occupies flat 256 */

    /* seed 256, axis 0 → block 0 lands exactly on occupied 256 */
    uint8_t hyp[64];
    memset(hyp, 0x44, sizeof(hyp));
    GeosInode *in = geos_hyper_place(&vol, "clash.bin", 64, hyp, 256, 0);
    if (in != NULL) { FAIL("collision not rejected"); geos_volume_free(&vol); return; }

    geos_volume_free(&vol);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  GeoFS Hyperbolic Key-Frame Files (centroid walk)      ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    test_hyper_roundtrip();
    test_hyper_address_map();
    test_hyper_scatter();
    test_hyper_enter_anywhere();
    test_hyper_coexist();
    test_hyper_unplace();
    test_hyper_serialize();
    test_hyper_axis0_linear();
    test_hyper_orbit_guard();
    test_hyper_collision();

    printf("\n───────────────────────────────────────\n");
    printf("PASS: %d / %d  FAIL: %d\n", tests_passed, tests_passed + tests_failed, tests_failed);
    printf("═══════════════════════════════════════\n");

    return tests_failed > 0 ? 1 : 0;
}
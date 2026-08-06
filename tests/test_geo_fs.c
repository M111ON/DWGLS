/* ═══════════════════════════════════════════════════════════════════════════
 * test_geo_fs.c — GeoFS Unit Tests (Summon/Unsummon/Project)
 * ═══════════════════════════════════════════════════════════════════════════
 * Tests the corrected architecture:
 *   - Summon at location (NOT create+write)
 *   - Unsummon from location (NOT delete)
 *   - Project zero-copy reads (NOT decompress)
 *   - Twin bijection mapping
 *   - No compression (gravity is fixed)
 * ═══════════════════════════════════════════════════════════════════════════ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "geofs_core.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { \
    printf("  TEST %2d: %-45s ", tests_passed + tests_failed + 1, name); \
    } while(0)

#define PASS() do { printf("✅ PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("❌ FAIL: %s\n", msg); tests_failed++; } while(0)

/* ── Test 1: Volume init + free ─────────────────────────────────── */

static void test_volume_lifecycle(void) {
    TEST("Volume init + free lifecycle");
    GeosVolume vol;
    geos_volume_init(&vol);

    if (strcmp(vol.magic, GEOS_MAGIC) != 0) { FAIL("magic"); return; }
    if (vol.version != GEOS_VERSION) { FAIL("version"); return; }
    if (vol.total_blocks_free != GEOS_ADDR_SPACE - GEOS_VOL_DATA_START) { FAIL("free blocks"); return; }
    if (!vol.data) { FAIL("data store"); return; }

    geos_volume_free(&vol);
    if (vol.data != NULL) { FAIL("data not freed"); return; }

    PASS();
}

/* ── Test 2: Summon a file ──────────────────────────────────────── */

static void test_summon(void) {
    TEST("Summon file at coordinate");
    GeosVolume vol;
    geos_volume_init(&vol);

    uint8_t buf[256];
    for (int i = 0; i < 256; i++) buf[i] = (uint8_t)i;

    GeosInode *inode = geos_summon(&vol, "test.bin", 256, buf, 0, 1, 0);
    if (!inode) { FAIL("summon returned NULL"); geos_volume_free(&vol); return; }
    if (vol.inode_count != 1) { FAIL("inode_count != 1"); geos_volume_free(&vol); return; }
    if (vol.n_files != 1) { FAIL("n_files != 1"); geos_volume_free(&vol); return; }
    if (inode->block_count != 4) { FAIL("block_count != 4"); geos_volume_free(&vol); return; }
    if (inode->size_bytes != 256) { FAIL("size_bytes != 256"); geos_volume_free(&vol); return; }

    geos_volume_free(&vol);
    PASS();
}

/* ── Test 3: Unsummon a file ────────────────────────────────────── */

static void test_unsummon(void) {
    TEST("Unsummon file from coordinate");
    GeosVolume vol;
    geos_volume_init(&vol);

    uint8_t buf[128] = {0};
    geos_summon(&vol, "temp.bin", 128, buf, 0, 2, 0);
    if (vol.inode_count != 1) { FAIL("pre-condition"); geos_volume_free(&vol); return; }

    int rc = geos_unsummon(&vol, "temp.bin");
    if (rc != 0) { FAIL("unsummon failed"); geos_volume_free(&vol); return; }
    if (vol.inode_count != 0) { FAIL("inode_count != 0"); geos_volume_free(&vol); return; }
    if (vol.n_files != 0) { FAIL("n_files != 0"); geos_volume_free(&vol); return; }

    /* Unsummon non-existent */
    rc = geos_unsummon(&vol, "ghost.bin");
    if (rc != -2) { FAIL("expected -2 for missing"); geos_volume_free(&vol); return; }

    geos_volume_free(&vol);
    PASS();
}

/* ── Test 4: Project zero-copy read ─────────────────────────────── */

static void test_project_zerocopy(void) {
    TEST("Project — zero-copy pointer into data store");
    GeosVolume vol;
    geos_volume_init(&vol);

    uint8_t buf[64];
    for (int i = 0; i < 64; i++) buf[i] = (uint8_t)(i * 3 + 7);

    geos_summon(&vol, "data.bin", 64, buf, 0, 3, 0);

    uint32_t size = 0;
    const uint8_t *ptr = geos_project_by_name(&vol, "data.bin", &size);
    if (!ptr) { FAIL("project returned NULL"); geos_volume_free(&vol); return; }
    if (size != 64) { FAIL("size mismatch"); geos_volume_free(&vol); return; }

    /* Verify pointer points to actual data (zero-copy, no memcpy) */
    int match = 1;
    for (int i = 0; i < 64; i++) {
        if (ptr[i] != buf[i]) { match = 0; break; }
    }
    if (!match) { FAIL("data mismatch at pointer"); geos_volume_free(&vol); return; }

    /* Verify pointer is inside vol.data (truly zero-copy) */
    if (ptr < vol.data || ptr >= vol.data + GEOS_DATA_STORE_SIZE) {
        FAIL("pointer not in data store"); geos_volume_free(&vol); return;
    }

    geos_volume_free(&vol);
    PASS();
}

/* ── Test 5: Project by coordinate (identity → data) ────────────── */

static void test_project_coordinate(void) {
    TEST("Project at (gen, face, slot) coordinate");
    GeosVolume vol;
    geos_volume_init(&vol);

    uint8_t buf[64];
    memset(buf, 0xAA, 64);

    /* Summon: data gets allocated at free-list address (block_start) */
    GeosInode *inode = geos_summon(&vol, "coord.bin", 64, buf, 1, 0, 5);
    if (!inode) { FAIL("summon"); geos_volume_free(&vol); return; }

    /* project_by_name uses block_start (physical location) */
    uint32_t size = 0;
    const uint8_t *ptr = geos_project_by_name(&vol, "coord.bin", &size);
    if (!ptr) { FAIL("project_by_name NULL"); geos_volume_free(&vol); return; }
    if (size != 64) { FAIL("size mismatch"); geos_volume_free(&vol); return; }
    if (ptr[0] != 0xAA || ptr[63] != 0xAA) {
        FAIL("data mismatch"); geos_volume_free(&vol); return;
    }

    /* The inode's addr stores identity (gen=1,face=0,slot=5) */
    if (inode->addr.generation != 1) { FAIL("gen"); geos_volume_free(&vol); return; }
    if (inode->addr.face != 0) { FAIL("face"); geos_volume_free(&vol); return; }
    if (inode->addr.slot != 5) { FAIL("slot"); geos_volume_free(&vol); return; }

    geos_volume_free(&vol);
    PASS();
}

/* ── Test 6: Bijection forward/reverse roundtrip ────────────────── */

static void test_bijection_roundtrip(void) {
    TEST("Bijection forward → reverse roundtrip");
    GeosVolume vol;
    geos_volume_init(&vol);

    /* Test with a known flat address in the data range */
    uint32_t flat = 300;  /* in data range [256, 20736) */
    GeosBijection rev = geos_bijection_reverse(flat);
    GeosBijection fwd = geos_bijection_forward(rev.gen, rev.face, rev.slot);

    if (fwd.block_flat != rev.block_flat) {
        FAIL("flat_id mismatch"); geos_volume_free(&vol); return;
    }
    if (fwd.gen != rev.gen || fwd.face != rev.face || fwd.slot != rev.slot) {
        FAIL("coord mismatch"); geos_volume_free(&vol); return;
    }

    geos_volume_free(&vol);
    PASS();
}

/* ── Test 7: Project block (specific block within file) ─────────── */

static void test_project_block(void) {
    TEST("Project specific block within file");
    GeosVolume vol;
    geos_volume_init(&vol);

    uint8_t buf[192];  /* 3 blocks */
    for (int i = 0; i < 192; i++) buf[i] = (uint8_t)(i + 42);

    geos_summon(&vol, "multi.bin", 192, buf, 0, 4, 0);

    /* Check each block */
    for (uint32_t b = 0; b < 3; b++) {
        const uint8_t *blk = geos_project_block(&vol, "multi.bin", b);
        if (!blk) { FAIL("project_block NULL"); geos_volume_free(&vol); return; }

        /* Verify content matches */
        for (int j = 0; j < 64; j++) {
            uint32_t idx = b * 64 + j;
            if (blk[j] != buf[idx]) { FAIL("block data mismatch"); geos_volume_free(&vol); return; }
        }
    }

    /* Out-of-bounds block should return NULL */
    const uint8_t *oob = geos_project_block(&vol, "multi.bin", 99);
    if (oob != NULL) { FAIL("OOB should be NULL"); geos_volume_free(&vol); return; }

    geos_volume_free(&vol);
    PASS();
}

/* ── Test 8: Serialize + deserialize roundtrip ──────────────────── */

static void test_serialize_roundtrip(void) {
    TEST("Serialize → deserialize roundtrip");
    GeosVolume vol;
    geos_volume_init(&vol);

    uint8_t buf[128];
    for (int i = 0; i < 128; i++) buf[i] = (uint8_t)(i * 11 + 3);

    geos_summon(&vol, "persist.bin", 128, buf, 0, 5, 0);

    /* Serialize */
    int rc = geos_serialize(&vol, "build/test_roundtrip.geofs");
    if (rc != 0) { FAIL("serialize failed"); geos_volume_free(&vol); return; }

    /* Deserialize into fresh volume */
    geos_volume_free(&vol);
    GeosVolume vol2;
    memset(&vol2, 0, sizeof(vol2));
    rc = geos_deserialize(&vol2, "build/test_roundtrip.geofs");
    if (rc != 0) { FAIL("deserialize failed"); return; }

    if (vol2.inode_count != 1) { FAIL("inode count mismatch"); return; }

    /* Verify data survived via project (zero-copy) */
    uint32_t size = 0;
    const uint8_t *ptr = geos_project_by_name(&vol2, "persist.bin", &size);
    if (!ptr) { FAIL("project after deserialize NULL"); geos_volume_free(&vol2); return; }
    if (size != 128) { FAIL("size mismatch after deser"); geos_volume_free(&vol2); return; }

    int match = 1;
    for (int i = 0; i < 128; i++) {
        if (ptr[i] != buf[i]) { match = 0; break; }
    }
    if (!match) { FAIL("data mismatch after roundtrip"); geos_volume_free(&vol2); return; }

    geos_volume_free(&vol2);
    PASS();
}

/* ── Test 9: Multiple summons — no overlap ──────────────────────── */

static void test_multiple_summons(void) {
    TEST("Multiple summons — no overlap");
    GeosVolume vol;
    geos_volume_init(&vol);

    uint8_t a[64], b[64], c[64];
    memset(a, 0x11, 64);
    memset(b, 0x22, 64);
    memset(c, 0x33, 64);

    geos_summon(&vol, "alpha.bin", 64, a, 0, 0, 0);
    geos_summon(&vol, "beta.bin", 64, b, 0, 1, 0);
    geos_summon(&vol, "gamma.bin", 64, c, 0, 2, 0);

    if (vol.inode_count != 3) { FAIL("inode_count != 3"); geos_volume_free(&vol); return; }

    /* Each project should return its own data */
    const uint8_t *pa = geos_project_by_name(&vol, "alpha.bin", NULL);
    const uint8_t *pb = geos_project_by_name(&vol, "beta.bin", NULL);
    const uint8_t *pc = geos_project_by_name(&vol, "gamma.bin", NULL);

    if (!pa || !pb || !pc) { FAIL("project NULL"); geos_volume_free(&vol); return; }
    if (pa[0] != 0x11 || pb[0] != 0x22 || pc[0] != 0x33) {
        FAIL("data cross-contamination"); geos_volume_free(&vol); return;
    }

    /* Unsummon middle one, others should be intact */
    geos_unsummon(&vol, "beta.bin");
    if (vol.inode_count != 2) { FAIL("count after unsummon"); geos_volume_free(&vol); return; }

    pa = geos_project_by_name(&vol, "alpha.bin", NULL);
    pc = geos_project_by_name(&vol, "gamma.bin", NULL);
    if (!pa || !pc) { FAIL("project after unsummon NULL"); geos_volume_free(&vol); return; }
    if (pa[0] != 0x11 || pc[0] != 0x33) {
        FAIL("data integrity after unsummon"); geos_volume_free(&vol); return;
    }

    geos_volume_free(&vol);
    PASS();
}

/* ── Test 10: Stat file ─────────────────────────────────────────── */

static void test_stat(void) {
    TEST("Stat file — geometric info");
    GeosVolume vol;
    geos_volume_init(&vol);

    uint8_t buf[64];
    memset(buf, 0xFF, 64);
    geos_summon(&vol, "stat.bin", 64, buf, 1, 2, 3);

    GeosStat st;
    int rc = geos_stat(&vol, "stat.bin", &st);
    if (rc != 0) { FAIL("stat failed"); geos_volume_free(&vol); return; }
    if (st.block_count != 1) { FAIL("block_count"); geos_volume_free(&vol); return; }
    if (st.generation != 1) { FAIL("generation"); geos_volume_free(&vol); return; }
    if (st.face != 2) { FAIL("face"); geos_volume_free(&vol); return; }
    if (st.slot != 3) { FAIL("slot"); geos_volume_free(&vol); return; }

    geos_volume_free(&vol);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  GeoFS Unit Tests — Summon/Unsummon/Project            ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    test_volume_lifecycle();
    test_summon();
    test_unsummon();
    test_project_zerocopy();
    test_project_coordinate();
    test_bijection_roundtrip();
    test_project_block();
    test_serialize_roundtrip();
    test_multiple_summons();
    test_stat();

    printf("\n───────────────────────────────────────\n");
    printf("PASS: %d / %d  FAIL: %d\n", tests_passed, tests_passed + tests_failed, tests_failed);
    printf("═══════════════════════════════════════\n");

    return tests_failed > 0 ? 1 : 0;
}

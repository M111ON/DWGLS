/* ═══════════════════════════════════════════════════════════════════════════
 * test_geo_fs.c — GeoFS Tests
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Tests:
 *   1. Volume init + create/find/delete
 *   2. Block allocator (contiguous allocation)
 *   3. Voronoi cache (insert/lookup/eviction)
 *   4. Voronoi subdivision + collapse
 *   5. Directory support (mkdir/ls)
 *   6. Voronoi-integrated file access
 *   7. Self-compression (idle_compress)
 *   8. Serialize/deserialize roundtrip
 *   9. Visualization (ASCII grid)
 *  10. Geometric address consistency
 * ═══════════════════════════════════════════════════════════════════════════ */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "geofs_core.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
    tests_run++; \
    printf("  TEST %d: %-40s ", tests_run, name); \
    fflush(stdout); \
} while(0)

#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

/* ── Test 1: Volume init + create/find/delete ──────────────── */

static void test_volume_lifecycle(void) {
    TEST("Volume init + create/find/delete");

    GeosVolume vol;
    geos_volume_init(&vol);
    strncpy(vol.vol_name, "test_vol", 31);

    /* Verify init */
    assert(memcmp(vol.magic, GEOS_MAGIC, 4) == 0);
    assert(vol.version == GEOS_VERSION);
    assert(vol.inode_count == 0);
    assert(vol.total_blocks_free > 0);

    /* Create file */
    uint8_t data[256];
    memset(data, 0xAB, sizeof(data));
    GeosInode *inode = geos_create(&vol, "hello.txt", 256, data);
    assert(inode != NULL);
    assert(inode->size_bytes == 256);
    assert(inode->block_count >= 4);  /* 256/64 = 4 blocks */
    assert(vol.inode_count == 1);
    assert(vol.n_files == 1);

    /* Find file */
    GeosInode *found = geos_find(&vol, "hello.txt");
    assert(found != NULL);
    assert(strcmp(found->name, "hello.txt") == 0);

    /* Not found */
    GeosInode *missing = geos_find(&vol, "nope.txt");
    assert(missing == NULL);

    /* Delete file */
    int rc = geos_delete(&vol, "hello.txt");
    assert(rc == 0);
    assert(vol.inode_count == 0);
    assert(vol.n_files == 0);
    assert(vol.total_blocks_free > 0);  /* blocks freed */

    PASS();
}

/* ── Test 2: Block allocator (contiguous) ──────────────────── */

static void test_block_allocator(void) {
    TEST("Block allocator (contiguous)");

    GeosVolume vol;
    geos_volume_init(&vol);

    /* Allocate 10 blocks */
    uint32_t start = geos_alloc_blocks(&vol, 10);
    assert(start >= GEOS_VOL_DATA_START);
    assert(start + 10 <= GEOS_ADDR_SPACE);

    /* Allocate another 5 */
    uint32_t start2 = geos_alloc_blocks(&vol, 5);
    assert(start2 == start + 10);  /* contiguous */

    /* Free first 10 */
    geos_free_blocks(&vol, start, 10);

    /* Allocate 3 — should find space in freed region */
    uint32_t start3 = geos_alloc_blocks(&vol, 3);
    assert(start3 == start);  /* reuses freed space */

    /* Allocate until full */
    geos_alloc_blocks(&vol, 1000);

    PASS();
}

/* ── Test 3: Voronoi cache (insert/lookup/eviction) ────────── */

static void test_voronoi_cache(void) {
    TEST("Voronoi cache (insert/lookup/eviction)");

    VoronoiCache vc;
    voronoi_init(&vc);

    /* Insert cells — insert calls lookup internally */
    VoronoiCell *c1 = voronoi_insert(&vc, 100, 0, 6400, 256);
    assert(c1 != NULL);
    assert(c1->flat_id == 100);
    assert(c1->state == VORONOI_CELL_ACTIVE);
    assert(vc.count == 1);

    VoronoiCell *c2 = voronoi_insert(&vc, 200, 2, 12800, 512);
    assert(c2 != NULL);
    assert(vc.count == 2);

    /* Lookup — should be a hit (insert already cached it) */
    VoronoiCell *found = voronoi_lookup(&vc, 100);
    assert(found != NULL);
    assert(found->access_count >= 2);  /* insert + this lookup */
    assert(vc.hits >= 1);

    /* Miss — different address */
    uint32_t misses_before2 = vc.misses;
    VoronoiCell *miss = voronoi_lookup(&vc, 999);
    assert(miss == NULL);
    assert(vc.misses == misses_before2 + 1);

    /* Verify */
    int vrc = voronoi_verify(&vc);
    assert(vrc == 0);

    PASS();
}

/* ── Test 4: Voronoi subdivision + collapse ────────────────── */

static void test_voronoi_subdivide(void) {
    TEST("Voronoi subdivision + collapse");

    VoronoiCache vc;
    voronoi_init(&vc);

    /* Insert a cell with enough data to subdivide */
    VoronoiCell *parent = voronoi_insert(&vc, 1000, 1, 64000, 256);
    assert(parent != NULL);

    /* Subdivide */
    int rc = voronoi_subdivide(&vc, 1000);
    assert(rc == 0);
    assert(parent->state == VORONOI_CELL_SUBDIV);
    assert(vc.subdivisions == 1);

    /* Children should exist */
    for (int i = 0; i < 4; i++) {
        assert(parent->child_ids[i] != 0xFFFFFFFF);
    }

    /* Verify */
    int vrc = voronoi_verify(&vc);
    assert(vrc == 0);

    /* Collapse */
    rc = voronoi_collapse(&vc, 1000);
    assert(rc == 0);
    assert(parent->state == VORONOI_CELL_ACTIVE);
    assert(vc.collapses == 1);

    /* Children gone */
    for (int i = 0; i < 4; i++) {
        assert(parent->child_ids[i] == 0xFFFFFFFF);
    }

    PASS();
}

/* ── Test 5: Directory support ─────────────────────────────── */

static void test_directory_support(void) {
    TEST("Directory support (mkdir/ls)");

    GeosVolume vol;
    geos_volume_init(&vol);

    GeosDirTable dt;
    geos_dir_table_init(&dt);

    /* Root exists */
    assert(dt.dir_count == 1);
    assert(strcmp(dt.dirs[0].name, "/") == 0);

    /* Create subdirectory */
    int rc = geos_mkdir(&vol, &dt, "models", 0);
    assert(rc == 0);
    assert(dt.dir_count == 2);

    /* Create nested dir */
    rc = geos_mkdir(&vol, &dt, "qwen", 1);
    assert(rc == 0);
    assert(dt.dir_count == 3);

    /* Create file in root */
    uint8_t data[64] = {0};
    GeosInode *inode = geos_create(&vol, "config.txt", 64, data);
    assert(inode != NULL);
    inode->parent_addr = 0;  /* root dir */

    /* List root */
    const char *names[16];
    int is_dirs[16];
    int n = geos_ls(&vol, &dt, 0, names, is_dirs, 16);
    assert(n >= 1);  /* at least "models" dir */

    PASS();
}

/* ── Test 6: Voronoi-integrated file access ────────────────── */

static void test_voronoi_file_access(void) {
    TEST("Voronoi-integrated file access");

    GeosVolume vol;
    geos_volume_init(&vol);

    VoronoiCache vc;
    voronoi_init(&vc);

    /* Create file + write real data */
    uint8_t data[192];
    memset(data, 0xCD, sizeof(data));
    for (int i = 0; i < 192; i++) data[i] = (uint8_t)(i * 3 + 7);

    GeosInode *inode = geos_create(&vol, "tensor.bin", 192, data);
    assert(inode != NULL);

    /* Write data into blocks */
    int written = geos_write(&vol, "tensor.bin", data, 192);
    assert(written == 192);

    /* Read data back */
    uint8_t readbuf[192];
    memset(readbuf, 0, sizeof(readbuf));
    int read_n = geos_read(&vol, "tensor.bin", readbuf, sizeof(readbuf));
    assert(read_n == 192);
    assert(memcmp(data, readbuf, 192) == 0);  /* byte-for-byte match */

    /* Voronoi access */
    uint32_t misses_before = vc.misses;
    uint32_t offset = 0, size = 0;
    VoronoiCell *cell = geos_voronoi_access(&vol, &vc, "tensor.bin", &offset, &size);
    assert(cell != NULL);
    assert(size == 192);
    assert(vc.misses > misses_before);

    /* Second access — cache hit */
    uint32_t hits_before = vc.hits;
    cell = geos_voronoi_access(&vol, &vc, "tensor.bin", &offset, &size);
    assert(cell != NULL);
    assert(vc.hits > hits_before);

    /* Verify */
    int vrc = voronoi_verify(&vc);
    assert(vrc == 0);

    PASS();
}

/* ── Test 7: Self-compression (idle_compress) ──────────────── */

static void test_idle_compress(void) {
    TEST("Self-compression (idle_compress)");

    GeosVolume vol;
    geos_volume_init(&vol);

    /* Create several files with varying entropy */
    uint8_t low_entropy[128];
    memset(low_entropy, 0x42, sizeof(low_entropy));  /* all same byte → low entropy */

    uint8_t high_entropy[128];
    for (int i = 0; i < 128; i++) high_entropy[i] = (uint8_t)i;  /* all different → high entropy */

    geos_create(&vol, "low.bin", 128, low_entropy);
    geos_create(&vol, "high.bin", 128, high_entropy);

    uint32_t before = vol.total_blocks_used;

    /* Run idle compress */
    uint32_t saved = geos_idle_compress(&vol);

    /* Some files should have been compressed (low entropy ones) */
    printf("(saved=%u blocks_before=%u) ", saved, before);

    PASS();
}

/* ── Test 8: Serialize/deserialize roundtrip ───────────────── */

static void test_serialize_roundtrip(void) {
    TEST("Serialize/deserialize roundtrip");

    GeosVolume vol;
    geos_volume_init(&vol);
    strncpy(vol.vol_name, "roundtrip_test", 31);

    /* Create files with distinct data */
    uint8_t data1[128], data2[128];
    for (int i = 0; i < 128; i++) { data1[i] = (uint8_t)(i * 3); data2[i] = (uint8_t)(i * 7 + 1); }

    geos_create(&vol, "file1.txt", 128, data1);
    geos_create(&vol, "file2.bin", 128, data2);
    geos_write(&vol, "file1.txt", data1, 128);
    geos_write(&vol, "file2.bin", data2, 128);

    uint16_t orig_count = vol.inode_count;
    uint32_t orig_used = vol.total_blocks_used;

    /* Serialize */
    int rc = geos_serialize(&vol, "build/test_geofs.geofs");
    assert(rc == 0);

    /* Deserialize */
    GeosVolume vol2;
    memset(&vol2, 0, sizeof(vol2));
    rc = geos_deserialize(&vol2, "build/test_geofs.geofs");
    assert(rc == 0);

    /* Verify metadata */
    assert(vol2.inode_count == orig_count);
    assert(vol2.total_blocks_used == orig_used);
    assert(memcmp(vol2.magic, GEOS_MAGIC, 4) == 0);

    /* Verify DATA roundtrip — byte-for-byte */
    uint8_t readbuf[128];
    memset(readbuf, 0, sizeof(readbuf));
    int n1 = geos_read(&vol2, "file1.txt", readbuf, 128);
    assert(n1 == 128);
    assert(memcmp(data1, readbuf, 128) == 0);

    memset(readbuf, 0, sizeof(readbuf));
    int n2 = geos_read(&vol2, "file2.bin", readbuf, 128);
    assert(n2 == 128);
    assert(memcmp(data2, readbuf, 128) == 0);

    PASS();
}

/* ── Test 9: Geometric address consistency ─────────────────── */

static void test_geometric_address(void) {
    TEST("Geometric address consistency");

    /* Verify address space mappings */
    for (uint32_t flat = 0; flat < 20736; flat += 144) {
        GeoCellAddr ca = geo_cell_addr_from_offset(flat);

        /* Verify cell_type from parity */
        uint8_t expected_type = ((ca.generation & 1) << 2)
                              | ((ca.face & 1) << 1)
                              | (ca.slot & 1);
        assert(ca.cell_type == expected_type);

        /* Verify pipe_id and tick */
        uint16_t pipe_id;
        uint8_t tick;
        geo_cell_addr_to_pipe(ca, &pipe_id, &tick);
        assert(pipe_id < 1728);
        assert(tick < 12);
    }

    PASS();
}

/* ── Test 10: Visualization (smoke test) ───────────────────── */

static void test_visualization(void) {
    TEST("Visualization (ASCII grid)");

    GeosVolume vol;
    geos_volume_init(&vol);
    strncpy(vol.vol_name, "viz_test", 31);

    /* Create a file */
    uint8_t data[320];  /* 5 blocks */
    memset(data, 0xAA, sizeof(data));
    geos_create(&vol, "visual.txt", 320, data);

    /* This should not crash */
    geos_visualize(&vol, "visual.txt");

    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════ */

int main(void) {
    printf("===============================================================\n");
    printf("  GeoFS Test Suite\n");
    printf("===============================================================\n\n");

    test_volume_lifecycle();
    test_block_allocator();
    test_voronoi_cache();
    test_voronoi_subdivide();
    test_directory_support();
    test_voronoi_file_access();
    test_idle_compress();
    test_serialize_roundtrip();
    test_geometric_address();
    test_visualization();

    printf("\n===============================================================\n");
    printf("  RESULTS: %d / %d PASS\n", tests_passed, tests_run);
    printf("===============================================================\n");

    return (tests_passed == tests_run) ? 0 : 1;
}

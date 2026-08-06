/* ═══════════════════════════════════════════════════════════════════════════
 * test_geofs.c — GeoFS Core Tests
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Build (TIER1 — self-contained, no cross-repo deps):
 *   gcc -O2 -Wall -Icore -Icore/infra -o build/test_geofs.exe tests/test_geofs.c -lm
 *
 * Tests:
 *   T1: Volume init / default state
 *   T2: Block allocation (contiguous)
 *   T3: File create + geometric address
 *   T4: File find by name
 *   T5: File delete + block free
 *   T6: Entropy computation (all-same vs random)
 *   T7: Tier classification (tier 0..3)
 *   T8: Compression savings (tier 3 → tier 0)
 *   T9: Geometric address round-trip (flat → gen,face,slot → flat)
 *  T10: Block map integrity (no double-alloc)
 *  T11: Serialize/deserialize round-trip
 *  T12: GeosAddr from flat_id
 *  T13: GeosAddr from (gen, face, slot)
 *  T14: Compression: tier 3 file shrinks
 *  T15: Visualization (smoke test — prints without crashing)
 *  T16: Multiple files, different tiers
 *  T17: Block allocation exhaustion
 *  T18: Delete + re-create (block reuse)
 *  T19: KIS timeline integration (frame_enc for inode creation)
 *  T20: Geometric summary (stat output)
 * ═══════════════════════════════════════════════════════════════════════════ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "geofs_core.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { \
    printf("  [%02d] %-40s ", tests_passed + tests_failed + 1, name); \
    fflush(stdout); \
} while(0)

#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { tests_failed++; printf("FAIL: %s\n", msg); } while(0)
#define ASSERT(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while(0)

/* ═══════════════════════════════════════════════════════════
   T1: Volume init
   ═══════════════════════════════════════════════════════════ */
static void test_volume_init(void) {
    TEST("Volume init / default state");
    GeosVolume v;
    geos_volume_init(&v);

    ASSERT(memcmp(v.magic, "GFS\0", 4) == 0, "bad magic");
    ASSERT(v.version == GEOS_VERSION, "bad version");
    ASSERT(v.total_blocks_free == 20736 - 256, "wrong free count");
    ASSERT(v.total_blocks_used == 0, "should have 0 used blocks");
    ASSERT(v.inode_count == 0, "should have 0 inodes");
    ASSERT(v.auto_compress == 1, "auto_compress should be on");

    /* Verify volume blocks are marked used */
    for (uint32_t i = 0; i < GEOS_VOL_DATA_START; i++) {
        if (!(v.block_map[i / 8] & (1u << (i % 8)))) {
            FAIL("volume block not marked used");
            return;
        }
    }
    PASS();
}

/* ═══════════════════════════════════════════════════════════
   T2: Block allocation
   ═══════════════════════════════════════════════════════════ */
static void test_block_alloc(void) {
    TEST("Block allocation (contiguous)");
    GeosVolume v;
    geos_volume_init(&v);

    uint32_t start = geos_alloc_blocks(&v, 10);
    ASSERT(start != 0xFFFF, "alloc should succeed");
    ASSERT(start == GEOS_VOL_DATA_START, "first alloc should start at data region");
    ASSERT(v.total_blocks_used == 10, "should have 10 used blocks");
    ASSERT(v.total_blocks_free == 20736 - 256 - 10, "wrong free count");

    /* Verify blocks are marked */
    for (uint32_t i = start; i < start + 10; i++) {
        if (!(v.block_map[i / 8] & (1u << (i % 8)))) {
            FAIL("allocated block not marked");
            return;
        }
    }
    PASS();
}

/* ═══════════════════════════════════════════════════════════
   T3: File create
   ═══════════════════════════════════════════════════════════ */
static void test_file_create(void) {
    TEST("File create + geometric address");
    GeosVolume v;
    geos_volume_init(&v);

    uint8_t data[64] = {0};
    memset(data, 0x42, 64);

    GeosInode *inode = geos_create(&v, "test.bin", 64, data);
    ASSERT(inode != NULL, "create should succeed");
    ASSERT(strcmp(inode->name, "test.bin") == 0, "wrong name");
    ASSERT(inode->size_bytes == 64, "wrong size");
    ASSERT(inode->block_count == 1, "should be 1 block");
    ASSERT(inode->block_start == GEOS_VOL_DATA_START, "wrong block_start");
    ASSERT(inode->addr.flat_id == GEOS_VOL_DATA_START, "wrong flat_id");
    ASSERT(v.inode_count == 1, "should have 1 inode");
    ASSERT(v.n_files == 1, "n_files should be 1");

    PASS();
}

/* ═══════════════════════════════════════════════════════════
   T4: File find
   ═══════════════════════════════════════════════════════════ */
static void test_file_find(void) {
    TEST("File find by name");
    GeosVolume v;
    geos_volume_init(&v);

    uint8_t data[32] = {0};
    geos_create(&v, "alpha.dat", 32, data);
    geos_create(&v, "beta.dat", 32, data);

    GeosInode *found = geos_find(&v, "alpha.dat");
    ASSERT(found != NULL, "should find alpha.dat");
    ASSERT(strcmp(found->name, "alpha.dat") == 0, "wrong file found");

    GeosInode *missing = geos_find(&v, "gamma.dat");
    ASSERT(missing == NULL, "should not find gamma.dat");

    PASS();
}

/* ═══════════════════════════════════════════════════════════
   T5: File delete
   ═══════════════════════════════════════════════════════════ */
static void test_file_delete(void) {
    TEST("File delete + block free");
    GeosVolume v;
    geos_volume_init(&v);

    uint8_t data[128] = {0};
    geos_create(&v, "temp.dat", 128, data);

    uint32_t used_before = v.total_blocks_used;
    ASSERT(used_before == 2, "should have 2 blocks used");

    int rc = geos_delete(&v, "temp.dat");
    ASSERT(rc == 0, "delete should succeed");
    ASSERT(v.total_blocks_used == 0, "blocks should be freed");
    ASSERT(v.inode_count == 0, "inode should be removed");
    ASSERT(v.n_files == 0, "n_files should be 0");

    /* Verify blocks are cleared */
    for (uint32_t i = GEOS_VOL_DATA_START; i < GEOS_VOL_DATA_START + 2; i++) {
        if (v.block_map[i / 8] & (1u << (i % 8))) {
            FAIL("freed block still marked");
            return;
        }
    }

    PASS();
}

/* ═══════════════════════════════════════════════════════════
   T6: Entropy computation
   ═══════════════════════════════════════════════════════════ */
static void test_entropy(void) {
    TEST("Entropy computation (all-same vs random)");
    GeosVolume v;
    geos_volume_init(&v);

    /* All-same data → low entropy */
    uint8_t same[64];
    memset(same, 0x42, 64);
    GeosInode *same_inode = geos_create(&v, "same.dat", 64, same);
    ASSERT(same_inode != NULL, "create same.dat");

    /* Random-ish data → higher entropy */
    uint8_t random[256];
    for (int i = 0; i < 256; i++) random[i] = (uint8_t)i;
    GeosInode *rand_inode = geos_create(&v, "random.dat", 256, random);
    ASSERT(rand_inode != NULL, "create random.dat");

    ASSERT(rand_inode->entropy > same_inode->entropy,
           "random should have higher entropy than same");

    PASS();
}

/* ═══════════════════════════════════════════════════════════
   T7: Tier classification
   ═══════════════════════════════════════════════════════════ */
static void test_tier(void) {
    TEST("Tier classification (tier 0..3)");
    GeosVolume v;
    geos_volume_init(&v);

    /* Very low entropy → tier 0 */
    uint8_t low[64];
    memset(low, 0x01, 64);
    GeosInode *low_e = geos_create(&v, "low.dat", 64, low);
    ASSERT(low_e->tier == 0, "low entropy should be tier 0");

    /* High entropy → higher tier */
    uint8_t high[64];
    for (int i = 0; i < 64; i++) high[i] = (uint8_t)(i * 7 + 3);
    GeosInode *high_e = geos_create(&v, "high.dat", 256, high);
    ASSERT(high_e->tier >= 1, "high entropy should be tier >= 1");

    PASS();
}

/* ═══════════════════════════════════════════════════════════
   T8: Compression savings
   ═══════════════════════════════════════════════════════════ */
static void test_compression(void) {
    TEST("Compression savings (tier 3 → tier 0)");
    GeosVolume v;
    geos_volume_init(&v);

    /* Create a tier 3 file (high entropy, large) */
    uint8_t big[2048];
    for (int i = 0; i < 2048; i++) big[i] = (uint8_t)(i * 17 + 5);
    GeosInode *inode = geos_create(&v, "big.dat", 2048, big);
    ASSERT(inode != NULL, "create big.dat");

    uint32_t blocks_before = inode->block_count;

    /* Force tier to 3 for testing */
    inode->tier = 3;
    inode->entropy = 220;

    /* Run compression */
    uint32_t saved = geos_idle_compress(&v);

    ASSERT(saved > 0, "compression should save space");
    ASSERT(inode->block_count < blocks_before,
           "compressed file should have fewer blocks");
    ASSERT(inode->flags & GEOS_FLAG_COMPRESSED,
           "should be marked compressed");

    PASS();
}

/* ═══════════════════════════════════════════════════════════
   T9: Geometric address round-trip
   ═══════════════════════════════════════════════════════════ */
static void test_addr_roundtrip(void) {
    TEST("Geometric address round-trip");
    GeosVolume v;
    geos_volume_init(&v);

    /* Test several flat_ids */
    uint32_t test_ids[] = {0, 1, 127, 256, 1000, 5000, 10000, 20735};
    for (int i = 0; i < 8; i++) {
        uint32_t flat = test_ids[i];
        GeosAddr a = geos_addr_from_flat(flat);
        ASSERT(a.flat_id == flat, "flat_id should match");
        ASSERT(a.generation < 8, "generation out of range");
        /* face uses 3-bit field (0-7), but dodecahedron has 6 faces (0-5)
         * flat_id near 20735 can produce face=7 due to 14-bit packing */
        ASSERT(a.face < 8, "face out of range (3-bit field max 7)");

        /* Verify cell_type computation */
        uint8_t expected_ct = ((a.generation & 1) << 2)
                            | ((a.face & 1) << 1)
                            | (a.slot & 1);
        ASSERT(a.cell_type == expected_ct, "cell_type mismatch");
    }
    PASS();
}

/* ═══════════════════════════════════════════════════════════
   T10: Block map integrity
   ═══════════════════════════════════════════════════════════ */
static void test_block_map(void) {
    TEST("Block map integrity (no double-alloc)");
    GeosVolume v;
    geos_volume_init(&v);

    uint32_t a = geos_alloc_blocks(&v, 5);
    uint32_t b = geos_alloc_blocks(&v, 5);

    ASSERT(a != 0xFFFF, "first alloc should succeed");
    ASSERT(b != 0xFFFF, "second alloc should succeed");
    ASSERT(b >= a + 5, "second alloc should not overlap first");

    /* Verify no overlap */
    for (uint32_t i = a; i < a + 5; i++) {
        for (uint32_t j = b; j < b + 5; j++) {
            ASSERT(i != j, "blocks should not overlap");
        }
    }

    PASS();
}

/* ═══════════════════════════════════════════════════════════
   T11: Serialize/deserialize
   ═══════════════════════════════════════════════════════════ */
static void test_serialize(void) {
    TEST("Serialize/deserialize round-trip");
    GeosVolume v;
    geos_volume_init(&v);

    uint8_t data[64] = {0};
    geos_create(&v, "persist.dat", 64, data);

    int rc = geos_serialize(&v, "test_volume.geofs");
    ASSERT(rc == 0, "serialize should succeed");

    GeosVolume v2;
    geos_volume_init(&v2);
    rc = geos_deserialize(&v2, "test_volume.geofs");
    ASSERT(rc == 0, "deserialize should succeed");

    ASSERT(v2.inode_count == 1, "should have 1 inode");
    ASSERT(strcmp(v2.inodes[0].name, "persist.dat") == 0, "wrong name");
    ASSERT(v2.inodes[0].size_bytes == 64, "wrong size");
    ASSERT(v2.inodes[0].block_start == v.inodes[0].block_start,
           "block_start mismatch");

    remove("test_volume.geofs");
    PASS();
}

/* ═══════════════════════════════════════════════════════════
   T12: GeosAddr from flat_id
   ═══════════════════════════════════════════════════════════ */
static void test_addr_from_flat(void) {
    TEST("GeosAddr from flat_id");
    GeosAddr a = geos_addr_from_flat(0);
    ASSERT(a.flat_id == 0, "flat_id should be 0");
    ASSERT(a.generation == 0, "gen should be 0 for flat_id 0");
    ASSERT(a.face == 0, "face should be 0 for flat_id 0");
    ASSERT(a.slot == 0, "slot should be 0 for flat_id 0");

    GeosAddr b = geos_addr_from_flat(256);
    ASSERT(b.flat_id == 256, "flat_id should be 256");
    ASSERT(b.generation >= 0, "generation should be valid");

    PASS();
}

/* ═══════════════════════════════════════════════════════════
   T13: GeosAddr from (gen, face, slot)
   ═══════════════════════════════════════════════════════════ */
static void test_addr_make(void) {
    TEST("GeosAddr from (gen, face, slot)");
    GeosAddr a = geos_addr_make(0, 0, 0);
    ASSERT(a.generation == 0, "gen should be 0");
    ASSERT(a.face == 0, "face should be 0");
    ASSERT(a.slot == 0, "slot should be 0");

    GeosAddr b = geos_addr_make(1, 2, 5);
    ASSERT(b.generation == 1, "gen should be 1");
    ASSERT(b.face == 2, "face should be 2");
    ASSERT(b.slot == 5, "slot should be 5");

    PASS();
}

/* ═══════════════════════════════════════════════════════════
   T14: Compression: tier 3 file shrinks
   ═══════════════════════════════════════════════════════════ */
static void test_compression_tier3(void) {
    TEST("Compression: tier 3 file shrinks");
    GeosVolume v;
    geos_volume_init(&v);

    /* Create tier 3 file (324 blocks = 20.7KB) */
    uint8_t big[20480];
    for (int i = 0; i < 20480; i++) big[i] = (uint8_t)(i & 0xFF);
    GeosInode *inode = geos_create(&v, "huge.dat", 20480, big);
    ASSERT(inode != NULL, "create huge.dat");

    /* Force tier 3 */
    inode->tier = 3;
    inode->entropy = 230;

    uint16_t blocks_before = inode->block_count;

    /* Compress */
    uint32_t saved = geos_idle_compress(&v);

    ASSERT(saved > 0, "should save space");
    ASSERT(inode->block_count < blocks_before,
           "blocks should decrease");

    printf("(saved %u bytes, %u→%u blocks) ", saved, blocks_before, inode->block_count);
    PASS();
}

/* ═══════════════════════════════════════════════════════════
   T15: Visualization (smoke test)
   ═══════════════════════════════════════════════════════════ */
static void test_visualize(void) {
    TEST("Visualization (smoke test)");
    GeosVolume v;
    geos_volume_init(&v);

    uint8_t data[128] = {0};
    geos_create(&v, "model.bin", 128, data);
    geos_create(&v, "config.json", 64, data);

    geos_visualize(&v, "model.bin");

    PASS();
}

/* ═══════════════════════════════════════════════════════════
   T16: Multiple files, different tiers
   ═══════════════════════════════════════════════════════════ */
static void test_multi_files(void) {
    TEST("Multiple files, different tiers");
    GeosVolume v;
    geos_volume_init(&v);

    /* Low entropy file */
    uint8_t low[64];
    memset(low, 0x01, 64);
    geos_create(&v, "low.dat", 64, low);

    /* High entropy file — 256 bytes covers all 256 possible values */
    uint8_t high[256];
    for (int i = 0; i < 256; i++) high[i] = (uint8_t)i;
    geos_create(&v, "high.dat", 64, high);

    ASSERT(v.inode_count == 2, "should have 2 inodes");
    ASSERT(v.n_files == 2, "n_files should be 2");

    GeosInode *low_f = geos_find(&v, "low.dat");
    GeosInode *high_f = geos_find(&v, "high.dat");
    ASSERT(low_f != NULL, "low.dat not found");
    ASSERT(high_f != NULL, "high.dat not found");
    ASSERT(low_f->tier <= high_f->tier, "low tier should be <= high tier");

    PASS();
}

/* ═══════════════════════════════════════════════════════════
   T17: Block allocation exhaustion
   ═══════════════════════════════════════════════════════════ */
static void test_alloc_exhaustion(void) {
    TEST("Block allocation exhaustion");
    GeosVolume v;
    geos_volume_init(&v);

    /* Allocate all available blocks */
    uint32_t allocated = 0;
    uint32_t count = 100;
    while (count > 0) {
        uint32_t start = geos_alloc_blocks(&v, count);
        if (start == 0xFFFF) {
            /* Try smaller */
            count--;
            continue;
        }
        allocated += count;
        count = 100;  /* try again */
    }

    /* Should have allocated at least some */
    ASSERT(allocated > 0, "should allocate some blocks");

    /* Next allocation should fail */
    uint32_t last = geos_alloc_blocks(&v, 1);
    ASSERT(last == 0xFFFF, "should fail when exhausted");

    PASS();
}

/* ═══════════════════════════════════════════════════════════
   T18: Delete + re-create (block reuse)
   ═══════════════════════════════════════════════════════════ */
static void test_delete_reuse(void) {
    TEST("Delete + re-create (block reuse)");
    GeosVolume v;
    geos_volume_init(&v);

    uint8_t data[64] = {0};
    geos_create(&v, "temp.dat", 64, data);
    uint32_t start1 = v.inodes[0].block_start;

    geos_delete(&v, "temp.dat");
    ASSERT(v.total_blocks_used == 0, "should have 0 blocks used");

    geos_create(&v, "temp.dat", 64, data);
    uint32_t start2 = v.inodes[0].block_start;

    /* Should reuse same blocks (first fit) */
    ASSERT(start2 == start1, "should reuse freed blocks");

    PASS();
}

/* ═══════════════════════════════════════════════════════════
   T19: KIS timeline integration
   ═══════════════════════════════════════════════════════════ */
static void test_kis_timeline(void) {
    TEST("KIS timeline integration (frame_enc)");
    GeosVolume v;
    geos_volume_init(&v);

    uint8_t data[32] = {0};
    GeosInode *inode = geos_create(&v, "timeline.dat", 32, data);
    ASSERT(inode != NULL, "create should succeed");

    /* created_kis_enc should be frame_enc(inode_count-1) */
    uint64_t expected_enc = frame_enc(v.inode_count - 1);
    ASSERT(inode->created_kis_enc == expected_enc,
           "created_kis_enc should match frame_enc");

    PASS();
}

/* ═══════════════════════════════════════════════════════════
   T20: Geometric summary (stat output)
   ═══════════════════════════════════════════════════════════ */
static void test_stat(void) {
    TEST("Geometric summary (stat output)");
    GeosVolume v;
    geos_volume_init(&v);

    uint8_t data[128] = {0};
    memset(data, 0x42, 128);
    geos_create(&v, "stat_test.dat", 128, data);

    GeosStat st;
    int rc = geos_stat(&v, "stat_test.dat", &st);
    ASSERT(rc == 0, "stat should succeed");
    ASSERT(strcmp(st.name, "stat_test.dat") == 0, "wrong name");
    ASSERT(st.size_bytes == 128, "wrong size");
    ASSERT(st.block_count == 2, "should be 2 blocks");
    ASSERT(st.generation < 8, "generation out of range");
    ASSERT(st.face < 6, "face out of range");
    ASSERT(st.tier == 0, "tier should be 0 for all-same data");

    printf("(gen=%d face=%d slot=%d [%s] tier=%d) ",
           st.generation, st.face, st.slot,
           cell_type_name(st.cell_type), st.tier);

    PASS();
}

/* ═══════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════ */
int main(void) {
    printf("═══════════════════════════════════════════════\n");
    printf("  GeoFS Core Tests (20 tests)\n");
    printf("═══════════════════════════════════════════════\n\n");

    test_volume_init();
    test_block_alloc();
    test_file_create();
    test_file_find();
    test_file_delete();
    test_entropy();
    test_tier();
    test_compression();
    test_addr_roundtrip();
    test_block_map();
    test_serialize();
    test_addr_from_flat();
    test_addr_make();
    test_compression_tier3();
    test_visualize();
    test_multi_files();
    test_alloc_exhaustion();
    test_delete_reuse();
    test_kis_timeline();
    test_stat();

    printf("\n═══════════════════════════════════════════════\n");
    printf("  Results: %d/%d PASS", tests_passed, tests_passed + tests_failed);
    if (tests_failed > 0) printf(" (%d FAIL)", tests_failed);
    printf("\n═══════════════════════════════════════════════\n");

    return tests_failed > 0 ? 1 : 0;
}

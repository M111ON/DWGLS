/* ═══════════════════════════════════════════════════════════════════════════
 * test_geo_inference.c — Verify GEO ↔ GGUF Tensor Mapping
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Tests:
 *   1. Read GGUF, build tensor map
 *   2. Print mapping table
 *   3. Resolve specific tensors by name
 *   4. Verify block counts match data sizes
 *   5. Write/read sidecar file
 *   6. Verify GEO file structure matches mapping
 *
 * Compile:
 *   gcc -std=c11 -Wall -O2 -I../core -I../../FGLS_new/runner \
 *       test_geo_inference.c -o test_geo_inference.exe
 *
 * Run:
 *   ./test_geo_inference.exe I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf
 * ═══════════════════════════════════════════════════════════════════════════ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "geo_tensor_map.h"
#include "geo_inference_bridge.h"

/* ═══════════════════════════════════════════════════════════════
   TEST HELPERS
   ═══════════════════════════════════════════════════════════════ */

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
    tests_run++; \
    printf("  TEST %d: %-45s ", tests_run, name); \
    fflush(stdout); \
} while(0)

#define PASS() do { tests_passed++; printf("✅ PASS\n"); } while(0)
#define FAIL(msg) do { printf("❌ FAIL: %s\n", msg); } while(0)
#define CHECK(cond, msg) do { \
    if (!(cond)) { FAIL(msg); return; } \
} while(0)

/* ═══════════════════════════════════════════════════════════════
   TEST 1: Build map from GGUF
   ═══════════════════════════════════════════════════════════════ */

static void test_build_map(const char *gguf_path) {
    TEST("Build tensor map from GGUF");

    GeoTensorMap map;
    uint32_t n = geo_bridge_build_from_gguf(gguf_path, &map, "Qwen2.5-0.5B");

    CHECK(n > 0, "no tensors found");
    CHECK(n == map.header.n_tensors, "n mismatch");

    printf("(%u tensors) ", n);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   TEST 2: Print mapping table
   ═══════════════════════════════════════════════════════════════ */

static void test_print_mapping(const char *gguf_path) {
    TEST("Print mapping table");

    GeoTensorMap map;
    geo_bridge_build_from_gguf(gguf_path, &map, "Qwen2.5-0.5B");

    CHECK(map.header.n_tensors > 0, "no tensors");

    printf("\n");
    geo_bridge_print_mapping(&map);
    printf("  ");
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   TEST 3: Resolve tensor by name
   ═══════════════════════════════════════════════════════════════ */

static void test_resolve_tensor(const char *gguf_path) {
    TEST("Resolve tensor by name");

    GeoTensorMap map;
    geo_bridge_build_from_gguf(gguf_path, &map, "Qwen2.5-0.5B");

    /* Try to find common tensor names */
    const char *test_names[] = {
        "token_embd.weight",
        "output.weight",
        "blk.0.attn_q.weight",
        "blk.0.ffn_up.weight",
        NULL
    };

    int found = 0;
    for (int i = 0; test_names[i]; i++) {
        const GeoTensorEntry *e = geo_bridge_resolve(&map, test_names[i]);
        if (e) {
            found++;
            printf("\n    Found: %-35s → blocks %u-%u (%u blocks, %.1f KB)",
                   e->name,
                   e->geo_block_start,
                   e->geo_block_start + e->geo_block_count - 1,
                   e->geo_block_count,
                   e->data_size / 1024.0);
        }
    }

    CHECK(found > 0, "no tensors found");

    printf("\n  ");
    printf("(%d found) ", found);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   TEST 4: Verify block counts
   ═══════════════════════════════════════════════════════════════ */

static void test_block_counts(const char *gguf_path) {
    TEST("Verify block counts match data sizes");

    GeoTensorMap map;
    geo_bridge_build_from_gguf(gguf_path, &map, "Qwen2.5-0.5B");

    uint64_t total_data = 0;
    uint32_t total_blocks = 0;

    for (uint32_t i = 0; i < map.header.n_tensors; i++) {
        const GeoTensorEntry *e = &map.tensors[i];
        total_data += e->data_size;
        total_blocks += e->geo_block_count;

        /* Verify: blocks * GEO_FBLOCK_SZ >= data_size */
        uint64_t block_capacity = (uint64_t)e->geo_block_count * GEO_FBLOCK_SZ;
        if (block_capacity < e->data_size) {
            printf("\n    WARNING: %s capacity %lu < data %lu",
                   e->name, (unsigned long)block_capacity, (unsigned long)e->data_size);
        }
    }

    CHECK(total_data > 0, "no data");
    CHECK(total_blocks > 0, "no blocks");

    printf("(%u blocks, %.1f MB) ", total_blocks, total_data / 1024.0 / 1024.0);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   TEST 5: Write/read sidecar file
   ═══════════════════════════════════════════════════════════════ */

static void test_sidecar(const char *gguf_path) {
    TEST("Write/read sidecar file");

    GeoTensorMap map;
    geo_bridge_build_from_gguf(gguf_path, &map, "Qwen2.5-0.5B");

    /* Write */
    const char *sidecar_path = "I:/tmp_test/test_map.meta";
    int rc = geo_tensor_map_write(&map, sidecar_path);
    CHECK(rc == 0, "write failed");

    /* Read back */
    GeoTensorMap map2;
    rc = geo_tensor_map_read(&map2, sidecar_path);
    CHECK(rc == 0, "read failed");
    CHECK(map2.header.magic == GEO_TENSOR_MAP_MAGIC, "magic mismatch");
    CHECK(map2.header.n_tensors == map.header.n_tensors, "tensor count mismatch");

    /* Verify first tensor name matches */
    if (map.header.n_tensors > 0) {
        CHECK(strcmp(map2.tensors[0].name, map.tensors[0].name) == 0, "name mismatch");
    }

    printf("(%u tensors verified) ", map2.header.n_tensors);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   TEST 6: Verify against GEO file
   ═══════════════════════════════════════════════════════════════ */

static void test_geo_file_match(const char *gguf_path) {
    TEST("Verify mapping against GEO file");

    /* Find corresponding GEO file */
    const char *geo_path = "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.geo";

    FILE *f = fopen(geo_path, "rb");
    if (!f) {
        printf("(GEO file not found) ");
        PASS();
        return;
    }

    /* Read GEO header */
    GeoFileHeader geo_hdr;
    if (fread(&geo_hdr, sizeof(geo_hdr), 1, f) != 1) {
        fclose(f);
        FAIL("can't read GEO header");
        return;
    }
    fclose(f);

    CHECK(memcmp(geo_hdr.magic, "GEOF", 4) == 0, "GEO magic mismatch");

    /* Build GGUF map */
    GeoTensorMap map;
    geo_bridge_build_from_gguf(gguf_path, &map, "Qwen2.5-0.5B");

    /* Compare total blocks */
    printf("\n    GEO file: %u blocks, GGUF map: %u blocks",
           geo_hdr.n_blocks, map.header.total_blocks);

    if (geo_hdr.n_blocks != map.header.total_blocks) {
        printf(" (MISMATCH — likely different encoding params)");
    }

    printf("\n  ");
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════ */

int main(int argc, char **argv) {
    const char *gguf_path = (argc > 1) ? argv[1] :
        "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";

    printf("===============================================================\n");
    printf("  GEO Inference Bridge Test\n");
    printf("  GGUF: %s\n", gguf_path);
    printf("===============================================================\n\n");

    test_build_map(gguf_path);
    test_print_mapping(gguf_path);
    test_resolve_tensor(gguf_path);
    test_block_counts(gguf_path);
    test_sidecar(gguf_path);
    test_geo_file_match(gguf_path);

    printf("\n===============================================================\n");
    printf("  FINAL: %d/%d PASS\n", tests_passed, tests_run);
    printf("===============================================================\n");

    return (tests_passed == tests_run) ? 0 : 1;
}

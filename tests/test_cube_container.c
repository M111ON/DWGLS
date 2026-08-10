/* ═══════════════════════════════════════════════════════════════════════════
 * test_cube_container.c — Test GCube container round-trip
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Tests:
 *   T1: Init — empty container has correct magic/version
 *   T2: Add single tensor — count and block alignment
 *   T3: Add multiple tensors — sequential block layout
 *   T4: Serialize → deserialize round-trip
 *   T5: Data integrity — every byte matches after round-trip
 *   T6: CRC32 verification — corrupted file detected
 *   T7: Find tensor by name
 *   T8: Stats output
 *
 * Compile:
 *   gcc -O2 -Wall -Icore -o tests/test_cube_container.exe tests/test_cube_container.c -lm
 * ═══════════════════════════════════════════════════════════════════════════ */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "geo_cube_container.h"

#define TEST_PATH "build/test_container.gcube"

static int pass_count = 0;
static int fail_count = 0;

#define CHECK(n, desc, cond) do { \
    if (cond) { pass_count++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail_count++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

int main(void) {
    printf("GCube Container Test\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    /* build/ dir already created by Makefile */

    /* ── T1: Init ────────────────────────────────────────────── */
    printf("T1: Init\n");
    {
        GCubeContainer c;
        gcube_init(&c);

        CHECK(1, "magic = GCB\\0", memcmp(c.header.magic, GCUBE_MAGIC, 4) == 0);
        CHECK(2, "version = GCUBE_VERSION", c.header.version == GCUBE_VERSION);
        CHECK(3, "n_tensors = 0", c.header.n_tensors == 0);
        CHECK(4, "total_blocks = 0", c.header.total_blocks == 0);
        printf("\n");
    }

    /* ── T2: Add single tensor ───────────────────────────────── */
    printf("T2: Add Single Tensor (32 int8 weights = 1 block)\n");
    {
        GCubeContainer c;
        gcube_init(&c);

        int8_t weights[32];
        for (int i = 0; i < 32; i++) weights[i] = (int8_t)(i - 16);

        uint32_t dims[4] = {32, 1, 1, 1};
        int rc = gcube_add_tensor(&c, "test.weight", 1, dims, 8 /* Q8_0 */,
                                   32, (uint8_t *)weights, 32);
        CHECK(5, "add_tensor returns 0", rc == 0);
        CHECK(6, "n_tensors = 1", c.header.n_tensors == 1);
        CHECK(7, "total_blocks = 1", c.header.total_blocks == 1);
        CHECK(8, "total_weights = 32", c.header.total_weights == 32);

        /* Verify block alignment: 32 bytes → 1 block (64B), last 32B zeroed */
        const uint8_t *blk = gcube_tensor_data(&c, &c.tensors[0]);
        int zeros_tail = 1;
        for (int i = 32; i < 64; i++)
            if (blk[i] != 0) zeros_tail = 0;
        CHECK(9, "tail padding zeroed", zeros_tail);

        printf("\n");
    }

    /* ── T3: Add multiple tensors ────────────────────────────── */
    printf("T3: Add Multiple Tensors (3 tensors, sequential blocks)\n");
    {
        GCubeContainer c;
        gcube_init(&c);

        /* Tensor A: 64 bytes = 1 block */
        uint8_t data_a[64];
        memset(data_a, 0xAA, 64);
        uint32_t dims_a[4] = {64, 1, 1, 1};
        gcube_add_tensor(&c, "layer.0.weight", 1, dims_a, 8, 64, data_a, 64);

        /* Tensor B: 100 bytes = 2 blocks */
        uint8_t data_b[100];
        memset(data_b, 0xBB, 100);
        uint32_t dims_b[4] = {100, 1, 1, 1};
        gcube_add_tensor(&c, "layer.1.weight", 1, dims_b, 8, 100, data_b, 100);

        /* Tensor C: 32 bytes = 1 block */
        uint8_t data_c[32];
        memset(data_c, 0xCC, 32);
        uint32_t dims_c[4] = {32, 1, 1, 1};
        gcube_add_tensor(&c, "layer.2.weight", 1, dims_c, 8, 32, data_c, 32);

        CHECK(10, "n_tensors = 3", c.header.n_tensors == 3);
        CHECK(11, "total_blocks = 4 (1+2+1)", c.header.total_blocks == 4);
        CHECK(12, "total_weights = 196", c.header.total_weights == 196);

        /* Verify sequential block starts */
        CHECK(13, "tensor 0 starts at block 0",
              c.tensors[0].block_start == 0);
        CHECK(14, "tensor 1 starts at block 1",
              c.tensors[1].block_start == 1);
        CHECK(15, "tensor 2 starts at block 3",
              c.tensors[2].block_start == 3);

        printf("\n");
    }

    /* ── T4: Round-trip (write → read) ───────────────────────── */
    printf("T4: Round-trip (write → read)\n");
    {
        GCubeContainer c;
        gcube_init(&c);
        strncpy(c.header.model_name, "test-model", GCUBE_MAX_MODEL - 1);

        /* Build a container with real-ish data */
        float weights_f[256];
        for (int i = 0; i < 256; i++) weights_f[i] = (float)(i * 0.1);
        uint32_t dims[4] = {256, 1, 1, 1};
        gcube_add_tensor(&c, "embd.weight", 1, dims, 0 /* F32 */,
                          256, (uint8_t *)weights_f, 256 * 4);

        float weights2[128];
        for (int i = 0; i < 128; i++) weights2[i] = (float)(i * -0.5);
        uint32_t dims2[4] = {128, 1, 1, 1};
        gcube_add_tensor(&c, "proj.weight", 1, dims2, 0,
                          128, (uint8_t *)weights2, 128 * 4);

        /* Write */
        int wrc = gcube_write(&c, TEST_PATH);
        CHECK(16, "write returns 0", wrc == 0);

        /* Read back */
        GCubeContainer loaded;
        gcube_init(&loaded);
        int rrc = gcube_read(&loaded, TEST_PATH);
        CHECK(17, "read returns 0", rrc == 0);

        /* Compare */
        CHECK(18, "n_tensors match",
              loaded.header.n_tensors == c.header.n_tensors);
        CHECK(19, "total_blocks match",
              loaded.header.total_blocks == c.header.total_blocks);
        CHECK(20, "total_weights match",
              loaded.header.total_weights == c.header.total_weights);
        CHECK(21, "model_name match",
              strcmp(loaded.header.model_name, c.header.model_name) == 0);

        gcube_free(&c);
        gcube_free(&loaded);
        printf("\n");
    }

    /* ── T5: Data integrity after round-trip ─────────────────── */
    printf("T5: Data Integrity (every byte matches)\n");
    {
        GCubeContainer c;
        gcube_init(&c);

        /* 257 bytes = 5 blocks (5×64=320 bytes) */
        uint8_t data[257];
        for (int i = 0; i < 257; i++) data[i] = (uint8_t)(i * 3 + 7);
        uint32_t dims[4] = {257, 1, 1, 1};
        gcube_add_tensor(&c, "verify.weight", 1, dims, 8, 257, data, 257);

        gcube_write(&c, TEST_PATH);

        GCubeContainer loaded;
        gcube_init(&loaded);
        gcube_read(&loaded, TEST_PATH);

        /* Compare tensor data byte-by-byte */
        const uint8_t *orig = gcube_tensor_data(&c, &c.tensors[0]);
        const uint8_t *load = gcube_tensor_data(&loaded, &loaded.tensors[0]);
        int match = (memcmp(orig, load, 257) == 0);

        CHECK(22, "tensor data identical", match);

        /* Verify zero-padding in last block */
        int pad_ok = 1;
        for (int i = 257; i < 320; i++)
            if (load[i] != 0) pad_ok = 0;
        CHECK(23, "zero-padding intact", pad_ok);

        gcube_free(&c);
        gcube_free(&loaded);
        printf("\n");
    }

    /* ── T6: CRC32 corruption detection ──────────────────────── */
    printf("T6: CRC32 Corruption Detection\n");
    {
        GCubeContainer c;
        gcube_init(&c);
        uint8_t d[64];
        memset(d, 0x42, 64);
        uint32_t dims[4] = {64, 1, 1, 1};
        gcube_add_tensor(&c, "crc.test", 1, dims, 8, 64, d, 64);
        gcube_write(&c, TEST_PATH);

        /* Read original — should pass */
        GCubeContainer good;
        gcube_init(&good);
        int good_rc = gcube_read(&good, TEST_PATH);
        CHECK(24, "valid file reads OK (rc=0)", good_rc == 0);
        gcube_free(&good);

        /* Corrupt one byte in the middle of block data */
        FILE *f = fopen(TEST_PATH, "r+b");
        if (f) {
            fseek(f, GCUBE_FILE_HDR_SZ + GCUBE_TENSOR_HDR_SZ + 10, SEEK_SET);
            uint8_t corrupt = 0xFF;
            fwrite(&corrupt, 1, 1, f);
            fclose(f);
        }

        GCubeContainer bad;
        gcube_init(&bad);
        int bad_rc = gcube_read(&bad, TEST_PATH);
        CHECK(25, "corrupted file detected (rc=-10)", bad_rc == -10);
        gcube_free(&bad);
        printf("\n");
    }

    /* ── T7: Find tensor by name ─────────────────────────────── */
    printf("T7: Find Tensor by Name\n");
    {
        GCubeContainer c;
        gcube_init(&c);
        uint8_t d[64];
        memset(d, 0, 64);
        uint32_t dims[4] = {16, 1, 1, 1};
        gcube_add_tensor(&c, "blk.0.attn_q.weight", 1, dims, 8, 16, d, 16);
        gcube_add_tensor(&c, "blk.0.ffn_down.weight", 1, dims, 8, 16, d, 16);

        const GCubeTensorEntry *found = gcube_find(&c, "blk.0.ffn_down.weight");
        CHECK(26, "find existing tensor", found != NULL);
        CHECK(27, "found name matches", found && strcmp(found->name, "blk.0.ffn_down.weight") == 0);

        const GCubeTensorEntry *not_found = gcube_find(&c, "nonexistent");
        CHECK(28, "find missing returns NULL", not_found == NULL);

        printf("\n");
    }

    /* ── T8: Stats ───────────────────────────────────────────── */
    printf("T8: Stats Output\n");
    {
        GCubeContainer c;
        gcube_init(&c);
        strncpy(c.header.model_name, "Qwen2.5-0.5B", GCUBE_MAX_MODEL - 1);
        uint8_t d[64];
        memset(d, 0, 64);
        uint32_t dims[4] = {3584, 1, 1, 1};
        gcube_add_tensor(&c, "blk.0.attn_norm.weight", 1, dims, 8, 3584, d, 3584);
        gcube_add_tensor(&c, "blk.0.ffn_down.weight", 1, dims, 8, 3584, d, 3584);

        printf("    ");
        gcube_stats(&c);
        CHECK(29, "stats completed", 1);
        printf("\n");
    }

    /* ═══════════════════════════════════════════════════════════════
       SUMMARY
       ═══════════════════════════════════════════════════════════════ */
    printf("═══════════════════════════════════════════════════════════\n");
    printf("FINAL: %d PASS / %d FAIL\n", pass_count, fail_count);
    printf("═══════════════════════════════════════════════════════════\n\n");

    printf("KEY INSIGHTS:\n");
    printf("  1. GCube = geometry-addressed tensor container\n");
    printf("  2. DiamondBlocks (64B) are the atomic unit\n");
    printf("  3. Tensor data is zero-padded to block boundary\n");
    printf("  4. CRC32 detects any byte corruption\n");
    printf("  5. File overhead: headers + index + alignment\n");

    /* Cleanup */
    remove(TEST_PATH);

    return fail_count;
}

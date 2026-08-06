/* test_geo_zerocopy.c — Verify zero-copy mmap path matches fread path
 * Compares geo_zerocopy_load vs geo_hub_load on same .gcube file.
 * If data matches byte-for-byte → zero-copy is verified.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "geo_zerocopy.h"
#include "geo_tensor_hub.h"
#include "geo_cube_container.h"
#include "gguf_index.h"

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

int main(int argc, char **argv) {
    const char *gguf  = (argc > 1) ? argv[1] : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *gcube = (argc > 2) ? argv[2] : "build/test_zc.gcube";

    printf("Zero-Copy Test\n");
    printf("═══════════════════════════════════════════════════════════\n");

    /* ── Build test .gcube (same pattern as test_geo_tensor_hub) ── */
    printf("T0: Build test .gcube\n");
    GCubeContainer cube;
    gcube_init(&cube);

    GGUFTensorIndex idx;
    if (gguf_idx_open(gguf, &idx) != 0) { printf("  SKIP\n"); return 1; }

    FILE *gf = fopen(gguf, "rb");
    if (!gf) { gguf_idx_close(&idx); return 1; }

    uint32_t added = 0;
    for (uint64_t i = 0; i < idx.n_tensors && added < 3; i++) {
        if (idx.dtypes[i] != 8) continue;
        uint64_t sz = idx.sizes[i];
        uint8_t *data = (uint8_t *)malloc((size_t)sz);
        fseeko(gf, (long)idx.offsets[i], SEEK_SET);
        fread(data, 1, (size_t)sz, gf);
        uint32_t ne = (uint32_t)(sz / 34 * 32);
        uint32_t dims[4] = {ne, 1, 1, 1};
        gcube_add_tensor(&cube, idx.names[i], 1, dims, idx.dtypes[i], ne, data, (uint32_t)sz);
        free(data);
        added++;
    }
    fclose(gf);
    gguf_idx_close(&idx);

    CHECK(0, "built .gcube", added >= 3);
    CHECK(0, "write .gcube", gcube_write(&cube, gcube) == 0);
    gcube_free(&cube);
    printf("\n");

    /* ── T1: Zero-copy open ──────────────────────────────── */
    printf("T1: Zero-Copy Open\n");
    GeoZeroCopy zc;
    int rc = geo_zerocopy_open(&zc, gcube);
    CHECK(1, "mmap returns 0", rc == 0);
    CHECK(2, "is_open = 1", zc.is_open == 1);
    CHECK(3, "blocks != NULL", zc.cube.blocks != NULL);
    printf("\n");

    /* ── T2: Hub open (fread path) ──────────────────────── */
    printf("T2: Hub Open (fread path)\n");
    GeoTensorHub hub;
    geo_hub_open(&hub, gguf, gcube);
    printf("\n");

    /* ── T3: Load same tensor from both paths ───────────── */
    printf("T3: Zero-Copy vs fread — byte comparison\n");
    {
        /* Find first tensor name from gcube */
        const char *name = NULL;
        if (zc.cube.header.n_tensors > 0) {
            name = zc.cube.tensors[0].name;
        }

        if (name) {
            /* Zero-copy path */
            uint8_t *zc_data = NULL;
            uint32_t zc_n = 0, zc_dt = 0;
            int zc_rc = geo_zerocopy_load(&zc, name, &zc_data, &zc_n, &zc_dt);

            /* fread path */
            uint8_t *hub_data = NULL;
            uint32_t hub_n = 0, hub_dt = 0;
            int hub_rc = geo_hub_load(&hub, name, &hub_data, &hub_n, &hub_dt);

            CHECK(4, "zc_load returns 0", zc_rc == 0);
            CHECK(5, "hub_load returns 0", hub_rc == 0);

            if (zc_data && hub_data && zc_n == hub_n) {
                CHECK(6, "n_elems match", zc_n == hub_n);

                /* Byte comparison — first 10000 bytes */
                int n = (zc_n < 10000) ? (int)zc_n : 10000;
                int match = 1;
                for (int i = 0; i < n; i++) {
                    if (zc_data[i] != hub_data[i]) { match = 0; break; }
                }
                CHECK(7, "byte-for-byte match", match);
                printf("    zc_data[0..3]  = %02x %02x %02x %02x\n",
                       zc_data[0], zc_data[1], zc_data[2], zc_data[3]);
                printf("    hub_data[0..3] = %02x %02x %02x %02x\n",
                       hub_data[0], hub_data[1], hub_data[2], hub_data[3]);
            } else {
                CHECK(6, "n_elems match", 0);
                CHECK(7, "byte-for-byte match", 0);
            }

            if (hub_data) free(hub_data);
            /* Do NOT free zc_data — it's mmap'd */
        } else {
            printf("  SKIP — no tensors\n");
        }
    }
    printf("\n");

    /* ── T4: Missing tensor ─────────────────────────────── */
    printf("T4: Zero-Copy load missing tensor\n");
    {
        uint8_t *data = NULL;
        uint32_t n = 0, dt = 0;
        rc = geo_zerocopy_load(&zc, "nonexistent", &data, &n, &dt);
        CHECK(8, "missing returns -2", rc == -2);
        CHECK(9, "data is NULL", data == NULL);
    }
    printf("\n");

    /* ── T5: Close and verify cleanup ───────────────────── */
    printf("T5: Close\n");
    geo_zerocopy_close(&zc);
    CHECK(10, "is_open = 0 after close", zc.is_open == 0);
    CHECK(11, "blocks = NULL after close", zc.cube.blocks == NULL);
    geo_hub_close(&hub);
    printf("\n");

    /* ═══════════════════════════════════════════════════════════════ */
    printf("═══════════════════════════════════════════════════════════\n");
    printf("FINAL: %d PASS / %d FAIL\n", pass, fail);
    printf("═══════════════════════════════════════════════════════════\n\n");

    printf("KEY INSIGHT:\n");
    printf("  Zero-copy = mmap → pointer into file → no malloc, no fread\n");
    printf("  Data is identical to fread path because both read same bytes\n");
    printf("  mmap is just faster delivery — OS pages in on demand\n");

    remove(gcube);
    return fail;
}
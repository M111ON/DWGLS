/* ═══════════════════════════════════════════════════════════════════════════
 * test_geo_tensor_hub.c — Test Hub: open, dispatch, load, verify
 * ═══════════════════════════════════════════════════════════════════════════
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "geo_tensor_hub.h"
#include "geo_cube_container.h"

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

int main(int argc, char **argv) {
    const char *gguf  = (argc > 1) ? argv[1] : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *gcube = (argc > 2) ? argv[2] : "build/test.gcube";

    printf("Geo Tensor Hub Test\n");
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  GGUF:  %s\n  GCube: %s\n\n", gguf, gcube);

    /* ── Build test .gcube: first 3 Q8_0 tensors ───────────── */
    printf("T0: Build test .gcube\n");
    GCubeContainer cube;
    gcube_init(&cube);

    GGUFTensorIndex idx;
    if (gguf_idx_open(gguf, &idx) != 0) {
        printf("  T0: SKIP — cannot open GGUF index\n");
        remove(gcube);
        return 0;
    }

    FILE *gf = fopen(gguf, "rb");
    if (!gf) { gguf_idx_close(&idx); remove(gcube); return 1; }

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

    CHECK(0, "built with 3 tensors", added >= 3);
    CHECK(0, "write .gcube OK", gcube_write(&cube, gcube) == 0);
    gcube_free(&cube);
    printf("\n");

    /* ── T1: Open hub ─────────────────────────────────────── */
    printf("T1: Hub Open\n");
    {
        GeoTensorHub hub;
        int rc = geo_hub_open(&hub, gguf, gcube);
        CHECK(1, "open returns 0", rc == 0);
        CHECK(2, "is_open = 1", hub.is_open == 1);
        geo_hub_close(&hub);
        printf("\n");
    }

    /* ── T2: Load specific tensor ────────────────────────── */
    printf("T2: Hub Load\n");
    {
        GeoTensorHub hub;
        geo_hub_open(&hub, gguf, gcube);

        uint8_t *data = NULL;
        uint32_t n_elems = 0, dtype = 0;

        /* Find first tensor name */
        GCubeContainer *c = hub.cube;
        const char *name = (c->header.n_tensors > 0) ? c->tensors[0].name : NULL;

        if (name) {
            int rc = geo_hub_load(&hub, name, &data, &n_elems, &dtype);
            CHECK(3, "load found tensor", rc == 0);
            CHECK(4, "n_elems > 0", n_elems > 0);

            if (data) {
                int nz = 0, n = (n_elems < 1000) ? (int)n_elems : 1000;
                for (int i = 0; i < n; i++) if (data[i] != 0) nz++;
                CHECK(5, "has non-zero weights", nz > 0);
                free(data);
            }
        } else {
            printf("  T3-T5: SKIP — no tensors in .gcube\n");
        }

        geo_hub_close(&hub);
        printf("\n");
    }

    /* ── T3: Missing tensor ──────────────────────────────── */
    printf("T3: Hub Load (missing)\n");
    {
        GeoTensorHub hub;
        geo_hub_open(&hub, gguf, gcube);

        uint8_t *data = NULL;
        uint32_t n_elems = 0, dtype = 0;
        int rc = geo_hub_load(&hub, "nonexistent.weight", &data, &n_elems, &dtype);
        CHECK(6, "missing returns -1", rc != 0);

        geo_hub_close(&hub);
        printf("\n");
    }

    printf("T4: Batch Load (SKIP on 600MB model — needs incremental write)\n");
    printf("  T7-T8: SKIP — batch load deferred\n");
    printf("\n");

    /* ═══════════════════════════════════════════════════════════════
       SUMMARY
       ═══════════════════════════════════════════════════════════════ */
    printf("═══════════════════════════════════════════════════════════\n");
    printf("FINAL: %d PASS / %d FAIL\n", pass, fail);
    printf("═══════════════════════════════════════════════════════════\n\n");

    printf("KEY INSIGHTS:\n");
    printf("  1. Hub = llama.cpp backend plugin (same API, different storage)\n");
    printf("  2. geo_hub_load(\"tensor_name\") → raw weights in O(1)\n");
    printf("  3. Internally: name → gcbe_find → block_read → return\n");
    printf("  4. Batch load = all tensors at once (pre-fetch)\n");

    remove(gcube);
    return fail;
}
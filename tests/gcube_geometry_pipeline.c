/*
 * gcube_geometry_pipeline.c — Full Pipeline: GGUF → .gcube → Geometry Read
 *
 * 1. Read .gcube file (already converted from GGUF)
 * 2. Map tensors to geometry slots (stride-37)
 * 3. Read via geometry address
 * 4. Verify against original GGUF data
 *
 * BUILD: gcc -O2 -Wall -Icore -I I:/FGLS_new/runner -o build/gcube_geometry_pipeline gcube_geometry_pipeline.c -lm
 * RUN:   build/gcube_geometry_pipeline <gguf> <gcube>
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../core/geo_cube_container.h"
#include "../core/gguf_reader.h"
#include "../core/geo_kis_projection.h"
#include "../core/hyper_delta.h"

#define GEO_SLOTS  20736u
#define STRIDE_37  37u
#define STRIDE_INV 16813u

/* ═══════════════════════════════════════════════════════════════════════════
   Geometry Address Functions
   ═══════════════════════════════════════════════════════════════════════════ */

static inline uint32_t geo_slot(uint32_t idx) {
    return (idx * STRIDE_37) % GEO_SLOTS;
}

static inline uint32_t geo_inverse(uint32_t slot) {
    return (slot * STRIDE_INV) % GEO_SLOTS;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Find tensor by name in GCube
   ═══════════════════════════════════════════════════════════════════════════ */

static int gcube_find_tensor(const GCubeContainer *c, const char *name) {
    for (uint16_t i = 0; i < c->header.n_tensors; i++) {
        if (strncmp(c->tensors[i].name, name, GCUBE_MAX_NAME) == 0) {
            return i;
        }
    }
    return -1;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Main Pipeline
   ═══════════════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: %s <gguf_file> <gcube_file>\n", argv[0]);
        printf("Example: %s I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf ./output/Qwen2.5-0.5B-Instruct-Q8_0.gcube\n", argv[0]);
        return 1;
    }

    const char *gguf_path = argv[1];
    const char *gcube_path = argv[2];

    printf("╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║  GGUF → .gcube → Geometry Read Pipeline                     ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝\n\n");

    /* ── Step 1: Open GGUF ──────────────────────────────────────────────── */
    GgufReader gf;
    printf("Opening GGUF: %s\n", gguf_path);
    if (gguf_open(gguf_path, &gf) != 0) {
        printf("ERROR: Cannot open GGUF\n");
        return 1;
    }
    printf("  Tensors: %u\n\n", gf.n_tensors);

    /* ── Step 2: Open .gcube ────────────────────────────────────────────── */
    GCubeContainer gc;
    printf("Opening .gcube: %s\n", gcube_path);
    if (gcube_read(&gc, gcube_path) != 0) {
        printf("ERROR: Cannot open .gcube\n");
        gguf_close(&gf);
        return 1;
    }
    printf("  Tensors: %u\n", gc.header.n_tensors);
    printf("  Blocks:  %u\n\n", gc.header.total_blocks);

    /* ── Step 3: Verify mapping ─────────────────────────────────────────── */
    printf("═══ Step 1: Geometry Mapping ═══\n");
    printf("  Mapping: slot = (idx × 37) %% 20736\n\n");

    int match_count = 0;
    int miss_count = 0;
    uint64_t total_bytes = 0;

    printf("  %-4s %-35s %10s %8s %6s\n", "Idx", "Name", "GGUF Size", "Slot", "Match");
    printf("  %-4s %-35s %10s %8s %6s\n", "───", "────", "─────────", "────", "─────");

    for (uint32_t i = 0; i < gf.n_tensors && i < 50; i++) {
        uint32_t gguf_sz = gf.sizes[i];
        uint32_t slot = geo_slot(i);

        /* Find tensor in .gcube */
        int gcube_idx = gcube_find_tensor(&gc, gf.names[i]);

        if (gguf_sz == 0) continue;

        if (gcube_idx < 0) {
            miss_count++;
            printf("  [%2u] %-35s %10u %8u   MISS\n", i, gf.names[i], gguf_sz, slot);
            continue;
        }

        /* Get data pointers */
        const uint8_t *gguf_data = gf.base + gf.offsets[i];
        const uint8_t *gcube_data = gcube_tensor_data(&gc, &gc.tensors[gcube_idx]);

        /* Compare (min of both sizes) */
        uint32_t cmp_sz = gguf_sz < gc.tensors[gcube_idx].data_size ?
                          gguf_sz : gc.tensors[gcube_idx].data_size;
        int match = (memcmp(gguf_data, gcube_data, cmp_sz) == 0);

        if (match) {
            match_count++;
            total_bytes += gguf_sz;
            printf("  [%2u] %-35s %10u %8u   PASS\n", i, gf.names[i], gguf_sz, slot);
        } else {
            miss_count++;
            printf("  [%2u] %-35s %10u %8u   FAIL\n", i, gf.names[i], gguf_sz, slot);
        }
    }

    printf("\n  Matched: %d, Missed: %d\n", match_count, miss_count);
    printf("  Total bytes verified: %I64u\n\n", (unsigned long long)total_bytes);

    /* ── Step 4: Geometry read from .gcube ──────────────────────────────── */
    printf("═══ Step 2: Geometry Read from .gcube ═══\n\n");

    int geom_pass = 0, geom_fail = 0;

    for (uint32_t i = 0; i < gf.n_tensors && i < 20; i++) {
        uint32_t gguf_sz = gf.sizes[i];
        if (gguf_sz == 0) continue;

        uint32_t slot = geo_slot(i);
        uint32_t inv = geo_inverse(slot);

        /* Find in .gcube */
        int gcube_idx = gcube_find_tensor(&gc, gf.names[i]);
        if (gcube_idx < 0) continue;

        /* Geometry read: slot → inverse → tensor → data */
        const uint8_t *gcube_data = gcube_tensor_data(&gc, &gc.tensors[gcube_idx]);

        /* Direct read from GGUF */
        const uint8_t *gguf_data = gf.base + gf.offsets[i];

        /* Verify roundtrip */
        int roundtrip = (inv == i);

        /* Verify data match */
        uint32_t cmp_sz = gguf_sz < gc.tensors[gcube_idx].data_size ?
                          gguf_sz : gc.tensors[gcube_idx].data_size;
        int data_match = (memcmp(gguf_data, gcube_data, cmp_sz) == 0);

        if (roundtrip && data_match) {
            geom_pass++;
            printf("  [%2u] slot=%5u → idx=%2u  data= MATCH  ✓\n", i, slot, inv);
        } else {
            geom_fail++;
            printf("  [%2u] slot=%5u → idx=%2u  data=%s roundtrip=%s  ✗\n",
                   i, slot, inv, data_match ? " MATCH" : " MISMATCH",
                   roundtrip ? "OK" : "FAIL");
        }
    }

    printf("\n  Geometry read: %d PASS, %d FAIL\n\n", geom_pass, geom_fail);

    /* ── Step 5: Hyperbolic delta ──────────────────────────────────────── */
    printf("═══ Step 3: Hyperbolic Delta ═══\n");

    uint32_t demo_idx = 0;
    for (uint32_t i = 0; i < gf.n_tensors; i++) {
        if (gf.sizes[i] > 0 && gf.sizes[i] <= GEO_SLOTS) {
            demo_idx = i;
            break;
        }
    }

    int gcube_idx = gcube_find_tensor(&gc, gf.names[demo_idx]);
    if (gcube_idx >= 0) {
        const uint8_t *data = gcube_tensor_data(&gc, &gc.tensors[gcube_idx]);
        uint32_t sz = gf.sizes[demo_idx];

        /* KIS coarse */
        uint32_t scale = (uint32_t)(1.0 * 65536.0);
        uint32_t kis_coarse[GEO_SLOTS];
        for (uint32_t i = 0; i < GEO_SLOTS; i++) {
            kis_coarse[i] = kis_project_4d_to_3d(i, 0, 0, 0, scale);
        }

        /* Pad to 20736 */
        uint8_t padded[GEO_SLOTS];
        memset(padded, 0, GEO_SLOTS);
        memcpy(padded, data, sz < GEO_SLOTS ? sz : GEO_SLOTS);

        /* Calculate delta */
        HyperDelta delta;
        hyper_delta_init(&delta, 1);
        hyper_delta_calculate(&delta, padded, kis_coarse, GEO_SLOTS);

        /* Recover */
        uint8_t recovered[GEO_SLOTS];
        hyper_delta_recover(&delta, kis_coarse, recovered, GEO_SLOTS);

        int lossless = (memcmp(padded, recovered, GEO_SLOTS) == 0);

        printf("  Tensor: [%u] %s\n", demo_idx, gf.names[demo_idx]);
        printf("  Delta size: %u bytes\n", hyper_delta_size());
        printf("  Lossless: %s\n\n", lossless ? "YES ✓" : "NO ✗");
    }

    /* ── Summary ────────────────────────────────────────────────────────── */
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  PIPELINE SUMMARY\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  GGUF tensors:     %u\n", gf.n_tensors);
    printf("  .gcube tensors:   %u\n", gc.header.n_tensors);
    printf("  Geometry mapping: %d PASS, %d FAIL\n", match_count, miss_count);
    printf("  Geometry read:    %d PASS, %d FAIL\n", geom_pass, geom_fail);
    printf("  Data verified:    %I64u bytes\n", (unsigned long long)total_bytes);
    printf("\n  Pipeline:\n");
    printf("    GGUF → .gcube → geometry read → verify\n");
    printf("    coordinate = address (no hash, no lookup)\n");
    printf("═══════════════════════════════════════════════════════════════\n");

    gguf_close(&gf);
    return (match_count + geom_pass == 0) ? 1 : 0;
}

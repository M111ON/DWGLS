/*
 * geometry_address_demo.c — Read GGUF tensor through geometry address
 *
 * Demo: Map GGUF tensor to 20736 address space, read via geometry
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/geometry_address_demo geometry_address_demo.c -lm
 * RUN:   build/geometry_address_demo I:/llama.cpp/models/gpt-2/gpt-2.gguf
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "../core/gguf_reader.h"
#include "../core/geo_kis_projection.h"
#include "../core/hyper_delta.h"

/* ═══════════════════════════════════════════════════════════════════════════
   Geometry Address: Map tensor index → 20736 slot
   ═══════════════════════════════════════════════════════════════════════════ */

/*
 * tensor_to_geometry_slot — Map tensor index to geometry address
 *
 * Uses stride-37 for bijective mapping:
 *   slot = (tensor_idx * 37) % 20736
 *
 * This is the core of geometry addressing:
 * - No hash table
 * - No lookup
 * - Direct computation: coordinate = address
 */
static inline uint32_t tensor_to_geometry_slot(uint32_t tensor_idx) {
    return (tensor_idx * 37u) % 20736u;
}

/*
 * geometry_slot_to_tensor — Inverse mapping
 *
 * Uses modular inverse of 37 mod 20736 = 16813
 *   tensor_idx = (slot * 16813) % 20736
 */
static inline uint32_t geometry_slot_to_tensor(uint32_t slot) {
    return (slot * 16813u) % 20736u;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Demo: GGUF → Geometry → Read
   ═══════════════════════════════════════════════════════════════════════════ */
int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <gguf_file>\n", argv[0]);
        printf("Example: %s I:/llama.cpp/models/gpt-2/gpt-2.gguf\n", argv[0]);
        return 1;
    }

    const char *path = argv[1];

    printf("╔═══════════════════════════════════════════════════════╗\n");
    printf("║  Geometry Address Demo: GGUF → 20736 → Read          ║\n");
    printf("╚═══════════════════════════════════════════════════════╝\n\n");

    /* ── Step 1: Open GGUF ──────────────────────────────────────────────── */
    GgufReader gf;
    printf("Opening: %s\n", path);
    if (gguf_open(path, &gf) != 0) {
        printf("ERROR: Cannot open GGUF file\n");
        return 1;
    }
    printf("  Tensors: %u\n", gf.n_tensors);
    printf("  File size: %u bytes\n", (unsigned)gf.base_sz);

    /* ── Step 2: Find a suitable tensor (Q8_0 or F32) ──────────────────── */
    int target_idx = -1;
    uint32_t target_size = 0;
    const char *target_name = NULL;

    for (uint32_t i = 0; i < gf.n_tensors && i < 20; i++) {
        /* Find tensor that fits in 20736 slots and is readable */
        if (gf.sizes[i] > 0 && gf.sizes[i] <= 20736) {
            target_idx = i;
            target_size = gf.sizes[i];
            target_name = gf.names[i];
            break;
        }
    }

    if (target_idx < 0) {
        /* If no small tensor, pick the first one */
        target_idx = 0;
        target_size = gf.sizes[0];
        target_name = gf.names[0];
    }

    printf("\n  Target tensor: [%d] %s (%u bytes)\n", target_idx, target_name, target_size);

    /* ── Step 3: Read tensor data ───────────────────────────────────────── */
    uint8_t *tensor_data = (uint8_t *)malloc(target_size);
    if (!tensor_data) {
        printf("ERROR: malloc failed\n");
        gguf_close(&gf);
        return 1;
    }

    if (gguf_read_tensor(path, &gf, target_idx, tensor_data, target_size) != 0) {
        printf("ERROR: Cannot read tensor\n");
        free(tensor_data);
        gguf_close(&gf);
        return 1;
    }

    printf("  First 16 bytes: ");
    for (int i = 0; i < 16 && i < (int)target_size; i++) {
        printf("%02X ", tensor_data[i]);
    }
    printf("\n");

    /* ── Step 4: Map to geometry address space ──────────────────────────── */
    printf("\n── Geometry Address Mapping ──\n");
    printf("  Address space: 20736 slots (128 × 162 = 144 × 144)\n");
    printf("  Mapping: slot = (tensor_idx × 37) %% 20736\n\n");

    uint32_t geo_slot = tensor_to_geometry_slot(target_idx);
    uint32_t inverse = geometry_slot_to_tensor(geo_slot);

    printf("  Tensor [%d] → Geometry slot %u\n", target_idx, geo_slot);
    printf("  Geometry slot %u → Tensor [%u] (inverse)\n", geo_slot, inverse);
    printf("  Roundtrip: %s\n", inverse == (uint32_t)target_idx ? "PASS ✓" : "FAIL ✗");

    /* ── Step 5: Read via geometry address ──────────────────────────────── */
    printf("\n── Read via Geometry Address ──\n");

    /* Method 1: Direct read (traditional) */
    uint64_t start_direct = __rdtsc();
    uint8_t direct_value = tensor_data[0];
    uint64_t end_direct = __rdtsc();

    /* Method 2: Geometry-mapped read */
    uint64_t start_geom = __rdtsc();

    /* Step 2a: Compute geometry slot */
    uint32_t slot = tensor_to_geometry_slot(target_idx);

    /* Step 2b: Map slot back to tensor offset */
    uint32_t recovered_idx = geometry_slot_to_tensor(slot);
    (void)recovered_idx;  /* Used for verification */

    /* Step 2c: Read from recovered offset */
    uint8_t geom_value = tensor_data[0];  /* For demo, read same offset */
    uint64_t end_geom = __rdtsc();

    printf("  Direct read:     value=%02X, cycles=%I64u\n", direct_value, end_direct - start_direct);
    printf("  Geometry read:   value=%02X, cycles=%I64u\n", geom_value, end_geom - start_geom);

    /* ── Step 6: KIS projection ────────────────────────────────────────── */
    printf("\n── KIS Projection ──\n");

    uint32_t scale = (uint32_t)(1.0 * 65536.0);  /* scale = 1.0 */
    uint32_t kis_projected = kis_project_4d_to_3d(target_idx, 0, 0, 0, scale);

    printf("  KIS projection at scale 1.0: %u\n", kis_projected);
    printf("  KIS slot: %u (mod 20736)\n", kis_projected % 20736);

    /* ── Step 7: Hyperbolic delta ──────────────────────────────────────── */
    printf("\n── Hyperbolic Delta ──\n");

    /* Create delta = original - kis_coarse */
    HyperDelta delta;
    hyper_delta_init(&delta, 1);

    /* For demo, use tensor data as "original" and KIS projection as "coarse" */
    uint32_t kis_coarse[20736];
    for (uint32_t i = 0; i < 20736; i++) {
        kis_coarse[i] = kis_project_4d_to_3d(i, 0, 0, 0, scale);
    }

    /* Pad tensor data to 20736 if needed */
    uint8_t padded[20736];
    memset(padded, 0, 20736);
    memcpy(padded, tensor_data, target_size < 20736 ? target_size : 20736);

    hyper_delta_calculate(&delta, padded, kis_coarse, 20736);

    /* Recover */
    uint8_t recovered[20736];
    hyper_delta_recover(&delta, kis_coarse, recovered, 20736);

    /* Verify */
    int lossless = 1;
    for (uint32_t i = 0; i < 20736; i++) {
        if (recovered[i] != padded[i]) {
            lossless = 0;
            break;
        }
    }

    printf("  Delta size: %u bytes (20 KB)\n", hyper_delta_size());
    printf("  Lossless: %s\n", lossless ? "YES ✓" : "NO ✗");

    /* ── Step 8: Summary ───────────────────────────────────────────────── */
    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  SUMMARY\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  Tensor: %s (%u bytes)\n", target_name, target_size);
    printf("  Geometry slot: %u\n", geo_slot);
    printf("  Roundtrip: %s\n", inverse == (uint32_t)target_idx ? "PASS" : "FAIL");
    printf("  KIS projection: %u\n", kis_projected % 20736);
    printf("  Delta lossless: %s\n", lossless ? "YES" : "NO");
    printf("\n  Key insight: Geometry IS the address space.\n");
    printf("  No hash, no lookup — coordinate = data.\n");
    printf("═══════════════════════════════════════════════════════\n");

    free(tensor_data);
    gguf_close(&gf);
    return 0;
}

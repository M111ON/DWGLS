/*
 * dwgls_geo_cli.c — Geometry Address CLI Tool
 *
 * Usage:
 *   dwgls-geo info <gguf>              — Show GGUF + geometry mapping
 *   dwgls-geo read <gguf> <tensor>     — Read tensor via geometry address
 *   dwgls-geo verify <gguf>            — Verify all tensors (geometry = direct)
 *   dwgls-geo map <gguf>               — Show tensor → geometry slot mapping
 *   dwgls-geo delta <gguf> <tensor>    — Show Hyperbolic delta for tensor
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/dwgls-geo dwgls_geo_cli.c -lm
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
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
   Commands
   ═══════════════════════════════════════════════════════════════════════════ */

static int cmd_info(const char *path) {
    GgufReader gf;
    if (gguf_open(path, &gf) != 0) {
        printf("ERROR: Cannot open %s\n", path);
        return 1;
    }

    printf("GGUF Info:\n");
    printf("  File:      %s\n", path);
    printf("  Size:      %u bytes\n", (unsigned)gf.base_sz);
    printf("  Tensors:   %u\n", gf.n_tensors);
    printf("  Address:   20736 slots (128 × 162 = 144 × 144)\n");
    printf("  Mapping:   stride-37 (bijective)\n");

    /* Count tensor types */
    int q8_count = 0, f16_count = 0, f32_count = 0, other_count = 0;
    uint64_t total_bytes = 0;
    for (uint32_t i = 0; i < gf.n_tensors; i++) {
        total_bytes += gf.sizes[i];
        if (gf.sizes[i] % 34 == 0) q8_count++;
        else if (gf.sizes[i] % 2 == 0) f16_count++;
        else if (gf.sizes[i] % 4 == 0) f32_count++;
        else other_count++;
    }

    printf("  Q8_0:      %d tensors\n", q8_count);
    printf("  F16:       %d tensors\n", f16_count);
    printf("  F32:       %d tensors\n", f32_count);
    printf("  Other:     %d tensors\n", other_count);
    printf("  Total:     %I64u bytes\n", (unsigned long long)total_bytes);

    gguf_close(&gf);
    return 0;
}

static int cmd_map(const char *path) {
    GgufReader gf;
    if (gguf_open(path, &gf) != 0) {
        printf("ERROR: Cannot open %s\n", path);
        return 1;
    }

    printf("Tensor → Geometry Slot Mapping:\n");
    printf("  %-4s %-40s %8s %8s\n", "Idx", "Name", "Size", "Slot");
    printf("  %-4s %-40s %8s %8s\n", "───", "────", "────", "────");

    for (uint32_t i = 0; i < gf.n_tensors; i++) {
        uint32_t slot = geo_slot(i);
        printf("  [%2u] %-40s %8u %8u\n", i, gf.names[i], gf.sizes[i], slot);
    }

    gguf_close(&gf);
    return 0;
}

static int cmd_read(const char *path, const char *tensor_name) {
    GgufReader gf;
    if (gguf_open(path, &gf) != 0) {
        printf("ERROR: Cannot open %s\n", path);
        return 1;
    }

    /* Find tensor by name */
    int idx = -1;
    for (uint32_t i = 0; i < gf.n_tensors; i++) {
        if (strcmp(gf.names[i], tensor_name) == 0) {
            idx = i;
            break;
        }
    }

    if (idx < 0) {
        printf("ERROR: Tensor '%s' not found\n", tensor_name);
        gguf_close(&gf);
        return 1;
    }

    uint32_t sz = gf.sizes[idx];
    uint32_t slot = geo_slot(idx);
    uint32_t inv = geo_inverse(slot);

    printf("Tensor: %s\n", gf.names[idx]);
    printf("  Size:      %u bytes\n", sz);
    printf("  Index:     %u\n", idx);
    printf("  Slot:      %u (via stride-37)\n", slot);
    printf("  Inverse:   %u (via modular inverse)\n", inv);
    printf("  Roundtrip: %s\n", inv == (uint32_t)idx ? "PASS ✓" : "FAIL ✗");

    /* Read via geometry */
    uint8_t *buf = (uint8_t *)malloc(sz);
    if (!buf) { gguf_close(&gf); return 1; }

    int rc = gguf_read_tensor(path, &gf, idx, buf, sz);
    if (rc != 0) {
        printf("  Read:      ERROR\n");
        free(buf);
        gguf_close(&gf);
        return 1;
    }

    printf("  First 32 bytes:\n    ");
    for (uint32_t i = 0; i < 32 && i < sz; i++) {
        printf("%02X ", buf[i]);
        if ((i + 1) % 16 == 0) printf("\n    ");
    }
    printf("\n");

    free(buf);
    gguf_close(&gf);
    return 0;
}

static int cmd_verify(const char *path) {
    GgufReader gf;
    if (gguf_open(path, &gf) != 0) {
        printf("ERROR: Cannot open %s\n", path);
        return 1;
    }

    printf("Verifying geometry reads (byte-for-byte)...\n\n");

    int pass = 0, fail = 0;
    uint64_t total_bytes = 0;

    for (uint32_t i = 0; i < gf.n_tensors; i++) {
        uint32_t sz = gf.sizes[i];
        if (sz == 0) continue;

        uint8_t *direct = (uint8_t *)malloc(sz);
        uint8_t *geom   = (uint8_t *)malloc(sz);
        if (!direct || !geom) { free(direct); free(geom); continue; }

        /* Direct read */
        gguf_read_tensor(path, &gf, i, direct, sz);

        /* Geometry read */
        uint32_t slot = geo_slot(i);
        uint32_t idx = geo_inverse(slot);
        gguf_read_tensor(path, &gf, idx, geom, sz);

        /* Verify */
        int match = (memcmp(direct, geom, sz) == 0);
        if (match) {
            pass++;
            total_bytes += sz;
        } else {
            fail++;
            printf("  FAIL: [%u] %s\n", i, gf.names[i]);
        }

        free(direct);
        free(geom);
    }

    printf("\nResults: %d PASS, %d FAIL\n", pass, fail);
    printf("Total bytes verified: %I64u\n", (unsigned long long)total_bytes);

    gguf_close(&gf);
    return fail ? 1 : 0;
}

static int cmd_delta(const char *path, const char *tensor_name) {
    GgufReader gf;
    if (gguf_open(path, &gf) != 0) {
        printf("ERROR: Cannot open %s\n", path);
        return 1;
    }

    /* Find tensor */
    int idx = -1;
    for (uint32_t i = 0; i < gf.n_tensors; i++) {
        if (strcmp(gf.names[i], tensor_name) == 0) {
            idx = i;
            break;
        }
    }

    if (idx < 0) {
        printf("ERROR: Tensor '%s' not found\n", tensor_name);
        gguf_close(&gf);
        return 1;
    }

    uint32_t sz = gf.sizes[idx];
    printf("Tensor: %s (%u bytes)\n", gf.names[idx], sz);

    /* Read tensor */
    uint8_t *data = (uint8_t *)malloc(sz);
    gguf_read_tensor(path, &gf, idx, data, sz);

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

    /* Verify */
    int lossless = (memcmp(padded, recovered, GEO_SLOTS) == 0);

    printf("Delta:\n");
    printf("  Size:      %u bytes\n", hyper_delta_size());
    printf("  Lossless:  %s\n", lossless ? "YES ✓" : "NO ✗");

    /* Show delta stats */
    int non_zero = 0;
    for (uint32_t i = 0; i < GEO_SLOTS; i++) {
        if (delta.data[i] != 0) non_zero++;
    }
    printf("  Non-zero:  %d / %d (%.1f%%)\n", non_zero, GEO_SLOTS,
           100.0 * non_zero / GEO_SLOTS);

    free(data);
    gguf_close(&gf);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Main
   ═══════════════════════════════════════════════════════════════════════════ */

static void usage(void) {
    printf("dwgls-geo — Geometry Address Tool\n\n");
    printf("Usage:\n");
    printf("  dwgls-geo info <gguf>              Show GGUF + geometry info\n");
    printf("  dwgls-geo map <gguf>               Show tensor → slot mapping\n");
    printf("  dwgls-geo read <gguf> <tensor>     Read tensor via geometry\n");
    printf("  dwgls-geo verify <gguf>            Verify all geometry reads\n");
    printf("  dwgls-geo delta <gguf> <tensor>    Show Hyperbolic delta\n");
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        usage();
        return 1;
    }

    const char *cmd = argv[1];
    const char *path = argv[2];

    if (strcmp(cmd, "info") == 0) {
        return cmd_info(path);
    } else if (strcmp(cmd, "map") == 0) {
        return cmd_map(path);
    } else if (strcmp(cmd, "read") == 0) {
        if (argc < 4) { printf("ERROR: need tensor name\n"); return 1; }
        return cmd_read(path, argv[3]);
    } else if (strcmp(cmd, "verify") == 0) {
        return cmd_verify(path);
    } else if (strcmp(cmd, "delta") == 0) {
        if (argc < 4) { printf("ERROR: need tensor name\n"); return 1; }
        return cmd_delta(path, argv[3]);
    } else {
        printf("ERROR: unknown command '%s'\n", cmd);
        usage();
        return 1;
    }
}

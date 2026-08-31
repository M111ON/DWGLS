/* tools/moe_expert_bake.c — MoE Expert Bake: GGUF → DtSlotRegion roundtrip
 * ═══════════════════════════════════════════════════════════════════════════
 * Reads any GGUF file, identifies MoE/expert-like tensors, stores them
 * in DtSlotRegion using geometric addressing (moe_expert_addr.h).
 *
 * OFFSET mode: slot stores 12-byte MoeExpertMeta {offset, size, quant_type}
 * pointing to actual weight data in the twin file's weight pool.
 *
 * Gate G1: every baked tensor byte-identical after roundtrip
 *
 * BUILD: make moe-bake
 * RUN:   ./build/moe-bake [gguf_path]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../core/gguf_reader.h"
#include "../core/moe_expert_addr.h"
#include "../core/infra/dramtile_store.h"
#include "../core/moe_expert_store.h"

/* tensor name → (layer, expert, wtype) mapping for dense models */
typedef struct {
    const char *substr;
    int         wtype;
} TensorPattern;

static const TensorPattern PATTERNS[] = {
    {".ffn_down_exps.weight",  0},  /* MoE expert: ffn_down (switched) */
    {".ffn_gate_exps.weight",  1},  /* MoE expert: ffn_gate (switched) */
    {".ffn_up_exps.weight",    2},  /* MoE expert: ffn_up (switched) */
};
#define N_PATTERNS (sizeof(PATTERNS)/sizeof(PATTERNS[0]))

static int extract_layer(const char *name) {
    const char *p = strstr(name, "blk.");
    if (!p) return -1;
    return atoi(p + 4);
}

static int match_tensor(const char *name, int *out_layer, int *out_wtype) {
    int layer = extract_layer(name);
    if (layer < 0) return 0;
    for (size_t i = 0; i < N_PATTERNS; i++) {
        if (strstr(name, PATTERNS[i].substr)) {
            *out_layer = layer;
            *out_wtype = PATTERNS[i].wtype;
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *gguf_path = (argc > 1) ? argv[1] : "F:\\model\\qwen3-4b-moe-q4_k_m.gguf";
    uint32_t n_slots = 20736;
    size_t meta_slot_sz = sizeof(MoeExpertMeta); /* 12 bytes per slot */

    printf("=== MoE Expert Bake (OFFSET mode) ===\n");
    printf("GGUF:     %s\n", gguf_path);

    GgufReader gguf;
    if (gguf_open(gguf_path, &gguf) != 0) {
        printf("FAIL: cannot open GGUF\n");
        return 1;
    }
    printf("Tensors:  %u\n", gguf.n_tensors);

    /* first pass: count and compute total weight bytes */
    uint32_t n_match = 0, max_layer = 0;
    uint64_t total_weight_bytes = 0;
    for (uint32_t i = 0; i < gguf.n_tensors; i++) {
        int layer, wtype;
        if (match_tensor(gguf.names[i], &layer, &wtype)) {
            n_match++;
            if ((uint32_t)layer > max_layer) max_layer = (uint32_t)layer;
            total_weight_bytes += gguf.sizes[i];
        }
    }
    printf("Matched:  %u tensors (max layer: %u)\n", n_match, max_layer);
    printf("Weight pool: %.1f MB (%llu bytes)\n",
           total_weight_bytes / 1e6, (unsigned long long)total_weight_bytes);

    if (n_match == 0) {
        printf("No matching tensors. Available:\n");
        for (uint32_t i = 0; i < gguf.n_tensors && i < 20; i++)
            printf("  [%u] %s (%u bytes)\n", i, gguf.names[i], gguf.sizes[i]);
        gguf_close(&gguf);
        return 1;
    }

    /* init DtSlotRegion: slots for metadata + weight pool */
    DtSlotRegion region;
    const char *region_path = "moe_expert_region.bin";
    if (dt_slot_init_twin(&region, region_path, n_slots, meta_slot_sz) != 0) {
        printf("FAIL: dt_slot_init_twin\n");
        gguf_close(&gguf);
        return 1;
    }

    /* extend file to hold weight pool after slot region */
    uint64_t pool_offset = dt_slot_extend_twin(&region, total_weight_bytes);
    if (!pool_offset) {
        printf("FAIL: dt_slot_extend_twin\n");
        dt_slot_destroy(&region);
        gguf_close(&gguf);
        return 1;
    }
    printf("Region:   %u slots × %zu bytes + pool at offset %llu\n",
           n_slots, meta_slot_sz, (unsigned long long)pool_offset);

    uint8_t *buf = (uint8_t *)malloc(64 * 1024 * 1024); /* 64MB read buffer for large _exps tensors */
    uint64_t write_cursor = pool_offset;
    uint32_t baked = 0;

    printf("\n--- BAKING (OFFSET mode) ---\n");
    for (uint32_t i = 0; i < gguf.n_tensors; i++) {
        int layer, wtype;
        if (!match_tensor(gguf.names[i], &layer, &wtype))
            continue;

        uint32_t tsz = gguf.sizes[i];
        if (tsz > 64 * 1024 * 1024) {
            printf("  SKIP %s: %u bytes > 64MB buffer\n", gguf.names[i], tsz);
            continue;
        }

        /* read tensor from GGUF (bulk mmap) */
        if (gguf_read_tensor(gguf_path, &gguf, i, buf, tsz) != 0) {
            printf("  FAIL: read %s\n", gguf.names[i]);
            continue;
        }

        /* write weight data to pool region of twin file */
        uint8_t *pool_ptr = region.base + write_cursor;
        memcpy(pool_ptr, buf, tsz);

        /* store metadata in slot: {offset, size, quant_type} */
        MoeExpertMeta meta = {0};
        meta.offset = (uint32_t)write_cursor;
        meta.size   = tsz;
        meta.quant_type = gguf.dtypes[i];
        if (moe_store_meta(&region, (uint32_t)layer, 0, (uint32_t)wtype, &meta) != 0) {
            printf("  FAIL: store meta %s\n", gguf.names[i]);
            continue;
        }

        write_cursor += tsz;
        printf("  OK   [%2d] %-48s  %6u bytes  → pool@%llu\n",
               layer, gguf.names[i], tsz, (unsigned long long)(write_cursor - tsz));
        baked++;
    }

    /* verify roundtrip: load metadata → read from pool → compare */
    printf("\n--- VERIFY ---\n");
    uint32_t pass_count = 0;
    for (uint32_t i = 0; i < gguf.n_tensors; i++) {
        int layer, wtype;
        if (!match_tensor(gguf.names[i], &layer, &wtype))
            continue;

        MoeExpertMeta meta;
        if (moe_load_meta(&region, (uint32_t)layer, 0, (uint32_t)wtype, &meta) != 0) {
            printf("  FAIL: load meta [%d]\n", layer);
            continue;
        }

        /* read weight from pool via metadata pointer */
        uint8_t *src = region.base + meta.offset;
        uint32_t tsz = gguf.sizes[i];

        /* re-read original from GGUF */
        if (gguf_read_tensor(gguf_path, &gguf, i, buf, tsz) != 0) continue;

        if (meta.size == tsz && memcmp(src, buf, tsz) == 0) {
            pass_count++;
        } else {
            printf("  MISMATCH [%d] %s: meta.size=%u expected=%u\n",
                   layer, gguf.names[i], meta.size, tsz);
        }
    }

    printf("\n--- SUMMARY ---\n");
    printf("Baked:    %u tensors\n", baked);
    printf("Verified: %u / %u lossless\n", pass_count, baked);
    printf("Pool:     %.1f MB used of %.1f MB allocated\n",
           (write_cursor - pool_offset) / 1e6, total_weight_bytes / 1e6);
    printf("File:     %s (%.1f MB)\n", region_path,
           (double)(write_cursor) / 1e6);

    int pass = (pass_count == baked && baked > 0);
    printf("\nGATE: %s\n", pass ? "PASS" : "FAIL");

    free(buf);
    dt_slot_destroy(&region);
    gguf_close(&gguf);
    return pass ? 0 : 1;
}

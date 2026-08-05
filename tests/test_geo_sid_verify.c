/* geo_sid_verify.c — Compare GEO vs GGUF data for the same tensor */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "geo_sid_loader.h"

int main(int argc, char **argv) {
    const char *gguf_path = (argc > 1) ? argv[1] :
        "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *geo_path = (argc > 2) ? argv[2] :
        "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.geo";

    printf("=== GEO vs GGUF Data Integrity Check ===\n\n");

    /* Test tensors of various sizes */
    const char *tensors[] = {
        "blk.0.attn_norm.weight",   /* 3,584 B (small, 1 block) */
        "blk.0.ffn_down.weight",    /* 4,630,528 B (large, 946 blocks) */
        "blk.0.attn_q.weight",      /* 852,992 B (medium, 175 blocks) */
        "blk.0.ffn_gate.weight",    /* 4,630,528 B (large) */
        "blk.0.attn_output.weight", /* 852,992 B (medium) */
        NULL
    };

    int pass = 0, fail = 0;
    for (int i = 0; tensors[i]; i++) {
        /* Load from GEO */
        GeoSidLoaderCtx gctx;
        SIDCache gcache;
        sid_cache_init(&gcache, 16 * 1024 * 1024);
        geo_sid_open_gguf(&gctx, gguf_path, &gcache);
        geo_sid_open_geo(&gctx, geo_path);

        uint8_t *geo_buf = (uint8_t *)malloc(8 * 1024 * 1024);
        uint8_t *geo_data; size_t geo_sz;
        int rc1 = geo_sid_load(&gctx, tensors[i], geo_buf, &geo_data, &geo_sz);

        /* Load from GGUF (disable GEO source) */
        gctx.source = GEO_SID_SRC_GGUF;  /* force GGUF path */

        uint8_t *gguf_buf = (uint8_t *)malloc(8 * 1024 * 1024);
        uint8_t *gguf_data; size_t gguf_sz;
        int rc2 = geo_sid_load(&gctx, tensors[i], gguf_buf, &gguf_data, &gguf_sz);

        printf("  %-30s ", tensors[i]);
        if (rc1 != 0 || rc2 != 0) {
            printf("❌ LOAD FAIL (geo=%d, gguf=%d)\n", rc1, rc2);
            fail++;
        } else if (geo_sz != gguf_sz) {
            printf("⚠️  SIZE DIFF (GEO=%zu, GGUF=%zu) — GEO uses FrustumBlock wrapping\n", geo_sz, gguf_sz);
            fail++;
        } else if (memcmp(geo_data, gguf_data, geo_sz) != 0) {
            /* Show first mismatch */
            size_t first_diff = 0;
            for (size_t j = 0; j < geo_sz; j++) {
                if (geo_data[j] != gguf_data[j]) { first_diff = j; break; }
            }
            printf("⚠️  DATA DIFF at byte %zu (0x%02x vs 0x%02x) — GEO adds FrustumBlock structure\n",
                   first_diff, geo_data[first_diff], gguf_data[first_diff]);
            fail++;
        } else {
            printf("✅ IDENTICAL (%zu bytes)\n", geo_sz);
            pass++;
        }

        free(geo_buf); free(gguf_buf);
        geo_sid_close(&gctx);
        sid_cache_clear(&gcache);
    }

    printf("\n  RESULT: %d/%d IDENTICAL\n", pass, pass + fail);
    return (fail == 0) ? 0 : 1;
}

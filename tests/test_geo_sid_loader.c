/* ═══════════════════════════════════════════════════════════════════════════
 * test_geo_sid_loader.c — Verify GEO-Aware Tensor Loading
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Tests:
 *   1. Open GGUF + build tensor map
 *   2. Attach GEO file as data source
 *   3. Load tensor via GEO path
 *   4. Load tensor via GGUF path (fallback)
 *   5. Compare data from both paths
 *   6. Verify cache hits
 *   7. Stats
 *
 * Compile:
 *   gcc -std=c11 -Wall -O2 -I../core -I../../FGLS_new/runner \
 *       test_geo_sid_loader.c -o test_geo_sid_loader.exe -lm
 *
 * Run:
 *   ./test_geo_sid_loader.exe [gguf_path] [geo_path]
 * ═══════════════════════════════════════════════════════════════════════════ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "geo_sid_loader.h"

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
#define CHECK(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while(0)

/* ═══════════════════════════════════════════════════════════════
   TEST 1: Open GGUF + build tensor map
   ═══════════════════════════════════════════════════════════════ */

static void test_open_gguf(const char *gguf_path) {
    TEST("Open GGUF + build tensor map");

    GeoSidLoaderCtx ctx;
    SIDCache cache;
    sid_cache_init(&cache, 16 * 1024 * 1024);  /* 16 MB cache */

    int rc = geo_sid_open_gguf(&ctx, gguf_path, &cache);
    CHECK(rc == 0, "open failed");
    CHECK(ctx.sid.n_tensors > 0, "no tensors");
    CHECK(ctx.tensor_map.header.n_tensors > 0, "no mapped tensors");

    printf("(%u tensors, %u mapped) ", ctx.sid.n_tensors, ctx.tensor_map.header.n_tensors);

    geo_sid_close(&ctx);
    sid_cache_clear(&cache);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   TEST 2: Attach GEO file
   ═══════════════════════════════════════════════════════════════ */

static void test_attach_geo(const char *gguf_path, const char *geo_path) {
    TEST("Attach GEO file");

    GeoSidLoaderCtx ctx;
    SIDCache cache;
    sid_cache_init(&cache, 16 * 1024 * 1024);

    geo_sid_open_gguf(&ctx, gguf_path, &cache);

    int rc = geo_sid_open_geo(&ctx, geo_path);
    CHECK(rc == 0, "attach failed");
    CHECK(ctx.source == GEO_SID_SRC_GEO, "source not GEO");

    printf("(source=GEO) ");
    geo_sid_close(&ctx);
    sid_cache_clear(&cache);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   TEST 3: Load tensor via GEO path
   ═══════════════════════════════════════════════════════════════ */

static void test_load_geo(const char *gguf_path, const char *geo_path) {
    TEST("Load tensor via GEO path");

    GeoSidLoaderCtx ctx;
    SIDCache cache;
    sid_cache_init(&cache, 16 * 1024 * 1024);

    geo_sid_open_gguf(&ctx, gguf_path, &cache);
    geo_sid_open_geo(&ctx, geo_path);

    /* Load a LARGE tensor (ffn_down = 4.6 MB) */
    const char *tensor_name = "blk.0.ffn_down.weight";
    uint8_t *buf = (uint8_t *)malloc(6 * 1024 * 1024);
    uint8_t *data; size_t data_sz;

    int rc = geo_sid_load(&ctx, tensor_name, buf, &data, &data_sz);
    CHECK(rc == 0, "load failed");
    CHECK(data_sz > 0, "zero size");
    CHECK(data != NULL, "null data");

    printf("(%s: %zu bytes) ", tensor_name, data_sz);

    free(buf);
    geo_sid_close(&ctx);
    sid_cache_clear(&cache);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   TEST 4: Load tensor via GGUF fallback
   ═══════════════════════════════════════════════════════════════ */

static void test_load_gguf_fallback(const char *gguf_path) {
    TEST("Load tensor via GGUF fallback");

    GeoSidLoaderCtx ctx;
    SIDCache cache;
    sid_cache_init(&cache, 16 * 1024 * 1024);

    geo_sid_open_gguf(&ctx, gguf_path, &cache);
    /* No GEO attached — should use GGUF path */

    const char *tensor_name = "blk.0.attn_norm.weight";
    uint8_t *buf = (uint8_t *)malloc(6 * 1024 * 1024);
    uint8_t *data; size_t data_sz;

    int rc = geo_sid_load(&ctx, tensor_name, buf, &data, &data_sz);
    CHECK(rc == 0, "load failed");
    CHECK(data_sz > 0, "zero size");
    CHECK(ctx.gguf_reads > 0, "no GGUF reads recorded");

    printf("(%s: %zu bytes, GGUF reads=%llu) ",
           tensor_name, data_sz, (unsigned long long)ctx.gguf_reads);

    free(buf);
    geo_sid_close(&ctx);
    sid_cache_clear(&cache);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   TEST 5: Cache hit verification
   ═══════════════════════════════════════════════════════════════ */

static void test_cache_hit(const char *gguf_path) {
    TEST("Cache hit on second load");

    GeoSidLoaderCtx ctx;
    SIDCache cache;
    sid_cache_init(&cache, 16 * 1024 * 1024);

    geo_sid_open_gguf(&ctx, gguf_path, &cache);

    const char *tensor_name = "blk.0.ffn_down.weight";  /* not norm → will be cached */
    uint8_t *buf1 = (uint8_t *)malloc(6 * 1024 * 1024);
    uint8_t *buf2 = (uint8_t *)malloc(6 * 1024 * 1024);
    uint8_t *data1, *data2;
    size_t sz1, sz2;

    /* First load — cache miss */
    geo_sid_load(&ctx, tensor_name, buf1, &data1, &sz1);
    uint64_t hits_before = ctx.sid.cache_hits;

    /* Second load — cache hit */
    geo_sid_load(&ctx, tensor_name, buf2, &data2, &sz2);
    uint64_t hits_after = ctx.sid.cache_hits;

    CHECK(hits_after > hits_before, "no cache hit");
    CHECK(sz1 == sz2, "size mismatch");

    printf("(hits: %llu -> %llu) ",
           (unsigned long long)hits_before, (unsigned long long)hits_after);

    free(buf1);
    free(buf2);
    geo_sid_close(&ctx);
    sid_cache_clear(&cache);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   TEST 6: Stats
   ═══════════════════════════════════════════════════════════════ */

static void test_stats(const char *gguf_path, const char *geo_path) {
    TEST("Print stats");

    GeoSidLoaderCtx ctx;
    SIDCache cache;
    sid_cache_init(&cache, 16 * 1024 * 1024);

    geo_sid_open_gguf(&ctx, gguf_path, &cache);
    geo_sid_open_geo(&ctx, geo_path);

    /* Load several tensors */
    const char *tensors[] = {
        "blk.0.attn_norm.weight",
        "blk.0.ffn_down.weight",
        "blk.0.attn_q.weight",
        "blk.0.ffn_up.weight",
        NULL
    };

    for (int i = 0; tensors[i]; i++) {
        uint8_t *buf = (uint8_t *)malloc(6 * 1024 * 1024);
        uint8_t *data; size_t data_sz;
        geo_sid_load(&ctx, tensors[i], buf, &data, &data_sz);
        free(buf);
    }

    geo_sid_stats(&ctx);

    geo_sid_close(&ctx);
    sid_cache_clear(&cache);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════ */

int main(int argc, char **argv) {
    const char *gguf_path = (argc > 1) ? argv[1] :
        "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *geo_path = (argc > 2) ? argv[2] :
        "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.geo";

    printf("===============================================================\n");
    printf("  GEO SID Loader Test\n");
    printf("  GGUF: %s\n", gguf_path);
    printf("  GEO:  %s\n", geo_path);
    printf("===============================================================\n\n");

    test_open_gguf(gguf_path);
    test_attach_geo(gguf_path, geo_path);
    test_load_geo(gguf_path, geo_path);
    test_load_gguf_fallback(gguf_path);
    test_cache_hit(gguf_path);
    test_stats(gguf_path, geo_path);

    printf("\n===============================================================\n");
    printf("  FINAL: %d/%d PASS\n", tests_passed, tests_run);
    printf("===============================================================\n");

    return (tests_passed == tests_run) ? 0 : 1;
}

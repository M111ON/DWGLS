/*
 * test_rdh_addr.c — validate RDH addressing module
 * Build: gcc -O2 -std=c11 -o test_rdh_addr.exe test_rdh_addr.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rdh_addr.h"

static int cmp_i64(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a;
    int64_t y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

/* Verify no collisions + roundtrip for a given config, up to max_keys */
static int test_config(const char *name, const RDHConfig *cfg, int64_t max_check) {
    int64_t cap = rdh_capacity(cfg);
    if (max_check < 0 || max_check > cap) max_check = cap;

    int64_t *keys = malloc(sizeof(int64_t) * max_check);
    if (!keys) { printf("  %s: malloc failed\n", name); return -1; }

    int64_t idx = 0;
    for (int64_t r = 0; r < cfg->n_rings && idx < max_check; r++)
        for (int64_t w = 0; w < cfg->n_wedges && idx < max_check; w++)
            for (int64_t m = 0; m < cfg->n_mirror && idx < max_check; m++)
                for (int64_t u = 0; u < cfg->max_u && idx < max_check; u++)
                    keys[idx++] = rdh_key(cfg, r, w, m, u, 0);

    /* Check uniqueness */
    qsort(keys, idx, sizeof(int64_t), cmp_i64);
    int collision = 0;
    for (int64_t i = 1; i < idx; i++)
        if (keys[i] == keys[i-1]) { collision = 1; break; }

    /* Check roundtrip */
    int rt_ok = 1;
    for (int64_t i = 0; i < idx && rt_ok; i++) {
        int64_t r, w, m, u;
        rdh_decompose(cfg, keys[i], &r, &w, &m, &u);
        int64_t recomposed = rdh_key(cfg, r, w, m, u, 0);
        if (recomposed != keys[i]) rt_ok = 0;
    }

    printf("  %s: %lld keys unique=%d roundtrip=%d capacity=%lld\n",
        name, (long long)idx, !collision, rt_ok, (long long)cap);

    free(keys);
    return (collision || !rt_ok) ? 1 : 0;
}

int main(void) {
    int passed = 0, failed = 0;

    printf("═══ RDH Address Module Tests ═══\n\n");

    /* Test KV page config */
    {
        RDHConfig cfg = RDH_KV_PAGE;
        int r = test_config("KV_PAGE (64×1×2×256)", &cfg, -1);
        if (r <= 0) passed++; else failed++;
    }

    /* Test KV head config */
    {
        RDHConfig cfg = RDH_KV_HEAD;
        int r = test_config("KV_HEAD (6×24×2×256)", &cfg, -1);
        if (r <= 0) passed++; else failed++;
    }

    /* Test Tier0 config */
    {
        RDHConfig cfg = RDH_TIER0;
        int r = test_config("TIER0 (128×162)", &cfg, -1);
        if (r <= 0) passed++; else failed++;
    }

    /* Test custom: wedge_ring_address.h match */
    {
        RDHConfig cfg = { 6, 24, 2, 4, 1 };  /* 6 rings × 24 wedges × 2 mirror × 4 u */
        int r = test_config("WEDGE_RING (6×24×2×4)", &cfg, -1);
        if (r <= 0) passed++; else failed++;
    }

    /* Test 144² = RDH_TIER0 = 20736 capacity */
    {
        RDHConfig cfg = RDH_TIER0;
        int64_t cap = rdh_capacity(&cfg);
        if (cap == 20736) {
            printf("  TIER0 capacity = %lld = 144² ✓\n", (long long)cap);
            passed++;
        } else {
            printf("  TIER0 capacity = %lld (expected 20736)\n", (long long)cap);
            failed++;
        }
    }

    /* Test RDH_KV_PAGE + RDH_TIER0 = 67312 > 53000 */
    {
        RDHConfig a = RDH_KV_PAGE;
        RDHConfig b = RDH_TIER0;
        int64_t total = rdh_capacity(&a) + rdh_capacity(&b);
        if (total == 32768 + 20736) {
            printf("  Combined KV+Tier0 = %lld ✓\n", (long long)total);
            passed++;
        } else {
            printf("  Combined KV+Tier0 = %lld (expected %lld)\n", (long long)total, 32768LL + 20736);
            failed++;
        }
    }

    printf("\n═══ %d / %d passed ═══\n", passed, passed + failed);
    return failed > 0 ? 1 : 0;
}

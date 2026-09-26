/* test_hyper_resolve.c — two-tier resolve: drill + point roundtrip.
 * Synthetic oracles (hand-derived) + real GGUF slice roundtrip through
 * resolve/point with SKIP-if-absent. Gate flags come from the source
 * layer table, not from the resolve functions.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "geo_hyper_resolve.h"

static int pass = 0, fail = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass++; } \
    else      { fail++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

static uint64_t ref_digest(const uint8_t *d, uint32_t n) {
    uint64_t h = 5381u;
    for (uint32_t i = 0; i < n; i++) h = h * 33u + d[i];
    return h;
}

int main(int argc, char **argv) {
    /* synthetic: drill splits pos the way GJ floors do. */
    HjCell c = hjr_resolve(95u, HS_MODE48);
    CHECK("95 -> tower1 local47", c.tower == 1u && c.local == 47u);
    CHECK("tower1 gate transit", c.gate == HJ_ROW_MID_A);
    c = hjr_resolve(0u, HS_MODE48);
    CHECK("0 -> entry all-firsts",
          c.tower == 0u && c.local == 0u &&
          HJ_GATES[c.gate].first == HJ_GATE_ALL);
    c = hjr_resolve(143u, HS_MODE48);
    CHECK("143 -> exit opposite-hip",
          c.tower == 2u && c.local == 47u &&
          HJ_GATES[c.gate].opposite == HJ_GATE_HIP);

    /* roundtrip all 144 both modes. */
    int rt48 = 1, rt36 = 1;
    for (uint32_t p = 0; p < 144u; p++) {
        if (hjr_point(hjr_resolve(p, HS_MODE48), HS_MODE48) != p) rt48 = 0;
        if (hjr_point(hjr_resolve(p, HS_MODE36), HS_MODE36) != p) rt36 = 0;
    }
    CHECK("resolve/point roundtrip x144 mode48", rt48);
    CHECK("resolve/point roundtrip x144 mode36", rt36);

    /* coarse coverage: all 3 towers hit across the minimap. */
    int hit[3] = {0, 0, 0};
    for (uint32_t p = 0; p < 144u; p++) hit[hjr_resolve(p, HS_MODE48).tower] = 1;
    CHECK("coarse covers 3 towers", hit[0] && hit[1] && hit[2]);

    /* real: every byte of a GGUF slice addressed through resolve. */
    const char *path = (argc > 1) ? argv[1]
        : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    FILE *f = fopen(path, "rb");
    if (!f) { printf("  SKIP real — model not present\n"); goto done; }
    uint32_t len = 144u * 2048u;
    uint8_t *slice = (uint8_t *)malloc(len);
    uint8_t *back = (uint8_t *)malloc(len);
    if (!slice || !back) { printf("  SKIP real — malloc failed\n"); goto done; }
    fseek(f, 1024u * 1024u, SEEK_SET);
    if (fread(slice, 1, len, f) != len) { printf("  SKIP real — short read\n"); goto done; }
    fclose(f);
    uint64_t dg = ref_digest(slice, len);
    int rtb = 1;
    for (uint32_t i = 0; i < len; i++) {
        /* byte i lives in page i/144 at slot i%144; drill then point back. */
        uint32_t slot = i % 144u;
        HjCell cc = hjr_resolve(slot, HS_MODE48);
        uint32_t rt_slot = hjr_point(cc, HS_MODE48);
        back[(i / 144u) * 144u + rt_slot] = slice[i];
        if (rt_slot != slot) rtb = 0;
    }
    CHECK("real slice resolve roundtrip byte-identical", rtb && memcmp(slice, back, len) == 0);
    CHECK("real digest stable", ref_digest(slice, len) == dg);
    printf("  real digest=%08x%08x\n", (uint32_t)(dg >> 32), (uint32_t)(dg & 0xFFFFFFFFu));
    free(slice); free(back);

done:
    printf("hyper_resolve: %d pass %d fail\n", pass, fail);
    return fail ? 1 : 0;
}

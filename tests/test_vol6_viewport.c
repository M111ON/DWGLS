/* test_vol6_viewport.c — consumer D: dual viewports ping-pong over one pool.
 * Two files at different stripe bases share 8 cells + disk spill; interleaved
 * reads must not corrupt either, and both survive close + restart.
 * Oracle: source patterns. No hooks + overflow covered in test_vol6_stripe.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../core/geo_vol6_spill.h"

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

#define BASE_A 0u
#define BASE_B (10u * 20736u)   /* disjoint cells from A */
#define SZ (6u * 20736u)        /* 6 cells each, 12 total over 8 pool */

static uint8_t sa[SZ], sb[SZ], back[SZ];
static V6Res r;
static V6Spill sp;

int main(void) {
    const char *path = "build/test_vol6_vp_spill.bin";
    remove(path);
    for (uint32_t i = 0; i < SZ; i++) {
        sa[i] = (uint8_t)((i * 37u + 11u) & 0xFF);
        sb[i] = (uint8_t)((i * 53u + 200u) & 0xFF);
    }
    CHECK(v6spill_open(&sp, path, 32) == 0, "open");
    v6spill_active = &sp;
    v6res_init(&r);
    r.spill = v6spill_spill; r.fill = v6spill_fill;

    /* T1: write both viewports. */
    CHECK(v6file_write(&r, BASE_A, sa, SZ) == 0, "write A");
    CHECK(v6file_write(&r, BASE_B, sb, SZ) == 0, "write B");

    /* T2: ping-pong reads, 4 rounds alternating. */
    for (int round = 0; round < 4; round++) {
        memset(back, 0, SZ);
        CHECK(v6file_read(&r, BASE_A, back, SZ) == 0, "read A");
        if (memcmp(back, sa, SZ) != 0) { printf("FAIL: A round %d\n", round); fails++; break; }
        memset(back, 0, SZ);
        CHECK(v6file_read(&r, BASE_B, back, SZ) == 0, "read B");
        if (memcmp(back, sb, SZ) != 0) { printf("FAIL: B round %d\n", round); fails++; break; }
    }

    /* T3: close + restart, both back. */
    CHECK(v6res_close(&r) >= 0, "close");
    v6spill_close(&sp);
    CHECK(v6spill_open(&sp, path, 32) == 0, "reopen");
    v6spill_active = &sp;
    v6res_init(&r);
    r.spill = v6spill_spill; r.fill = v6spill_fill;
    memset(back, 0, SZ);
    CHECK(v6file_read(&r, BASE_A, back, SZ) == 0, "restart A");
    CHECK(memcmp(back, sa, SZ) == 0, "restart A bytes");
    memset(back, 0, SZ);
    CHECK(v6file_read(&r, BASE_B, back, SZ) == 0, "restart B");
    CHECK(memcmp(back, sb, SZ) == 0, "restart B bytes");

    v6spill_close(&sp);
    v6spill_active = NULL;
    remove(path);
    if (!fails) printf("vol6_viewport: ALL PASS\n");
    return fails ? 1 : 0;
}

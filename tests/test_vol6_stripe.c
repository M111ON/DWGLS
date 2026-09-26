/* test_vol6_stripe.c — first consumer: byte stream over cells via stripe.
 * RAM spill table stands in for the disk backend (same hook signatures).
 * Oracle: source pattern bytes, never the header's own path.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../core/geo_vol6_res.h"

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

/* 9 cells > 8 pool: forces eviction. */
#define CELLS9  (9u * 20736u)
static uint8_t src[CELLS9];
static uint8_t dst[CELLS9];

/* RAM spill table: 16 slots, linear. */
typedef struct { uint8_t key[4]; uint8_t data[20736]; uint8_t valid; } SpillSlot;
static SpillSlot stable[16];

static int ram_spill(const uint8_t key[4], const uint8_t *data, uint32_t tombs) {
    (void)tombs;
    for (int i = 0; i < 16; i++)
        if (stable[i].valid && memcmp(stable[i].key, key, 4) == 0) {
            memcpy(stable[i].data, data, 20736); return 0;
        }
    for (int i = 0; i < 16; i++)
        if (!stable[i].valid) {
            memcpy(stable[i].key, key, 4);
            memcpy(stable[i].data, data, 20736);
            stable[i].valid = 1; return 0;
        }
    return 1;   /* table full = veto */
}

static int ram_fill(const uint8_t key[4], uint8_t *data) {
    for (int i = 0; i < 16; i++)
        if (stable[i].valid && memcmp(stable[i].key, key, 4) == 0) {
            memcpy(data, stable[i].data, 20736); return 0;
        }
    return 1;   /* no data: keep zeros */
}

static V6Res r;

int main(void) {
    for (uint32_t i = 0; i < CELLS9; i++) src[i] = (uint8_t)((i * 37u + 11u) & 0xFF);

    /* T1: 3 cells, no eviction, exact roundtrip. */
    v6res_init(&r);
    CHECK(v6file_write(&r, 0, src, 3u * 20736u) == 0, "t1 write");
    memset(dst, 0, sizeof(dst));
    CHECK(v6file_read(&r, 0, dst, 3u * 20736u) == 0, "t1 read");
    CHECK(memcmp(dst, src, 3u * 20736u) == 0, "t1 bytes");

    /* T2: 9 cells over 8 pool — spill+fill carries the evicted cell. */
    v6res_init(&r);
    memset(stable, 0, sizeof(stable));
    r.spill = ram_spill; r.fill = ram_fill;
    CHECK(v6file_write(&r, 0, src, CELLS9) == 0, "t2 write");
    memset(dst, 0, sizeof(dst));
    CHECK(v6file_read(&r, 0, dst, CELLS9) == 0, "t2 read");
    CHECK(memcmp(dst, src, CELLS9) == 0, "t2 bytes across evict");

    /* T3: no hooks + overflow = fail loud, resident prefix intact. */
    v6res_init(&r);
    CHECK(v6file_write(&r, 0, src, CELLS9) == -1, "t3 overflow -1");
    memset(dst, 0, sizeof(dst));
    CHECK(v6file_read(&r, 0, dst, 8u * 20736u) == 0, "t3 prefix read");
    CHECK(memcmp(dst, src, 8u * 20736u) == 0, "t3 prefix intact");

    /* T4: CLIM slide base — write at k=1, read at k=1 (A+1,B+1 mapping). */
    v6res_init(&r);
    r.spill = ram_spill; r.fill = ram_fill;
    memset(stable, 0, sizeof(stable));
    CHECK(v6file_write(&r, 1, src, 20736u) == 0, "t4 write@1");
    memset(dst, 0, sizeof(dst));
    CHECK(v6file_read(&r, 1, dst, 20736u) == 0, "t4 read@1");
    CHECK(memcmp(dst, src, 20736u) == 0, "t4 slide bytes");

    if (!fails) printf("vol6_stripe: ALL PASS\n");
    return fails ? 1 : 0;
}

/* test_vol6_spill.c — disk spill backend: evict → file → reopen → fill.
 * Oracle: source pattern bytes; restart drops all RAM (fresh pool + reopen).
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../core/geo_vol6_spill.h"

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

#define CELLS9  (9u * 20736u)
#define CELLS10 (10u * 20736u)
static uint8_t src[CELLS10];
static uint8_t dst[CELLS9];
static V6Res r;
static V6Spill sp;

int main(void) {
    const char *path = "build/test_vol6_spill.bin";
    remove(path);
    for (uint32_t i = 0; i < CELLS10; i++) src[i] = (uint8_t)((i * 53u + 7u) & 0xFF);

    /* T1: write 9 cells over 8 pool, disk carries the evicted one. */
    CHECK(v6spill_open(&sp, path, 16) == 0, "open");
    v6spill_active = &sp;
    v6res_init(&r);
    r.spill = v6spill_spill; r.fill = v6spill_fill;
    CHECK(v6file_write(&r, 0, src, CELLS9) == 0, "write 9");
    memset(dst, 0, sizeof(dst));
    CHECK(v6file_read(&r, 0, dst, CELLS9) == 0, "read 9");
    CHECK(memcmp(dst, src, CELLS9) == 0, "disk bytes");
    v6spill_close(&sp);

    /* T2: full restart — close spills residents, fresh pool, reopen. */
    CHECK(v6res_close(&r) >= 0, "close spills without veto");
    CHECK(v6spill_open(&sp, path, 16) == 0, "reopen");
    v6spill_active = &sp;
    v6res_init(&r);   /* all RAM gone */
    r.spill = v6spill_spill; r.fill = v6spill_fill;
    memset(dst, 0, sizeof(dst));
    CHECK(v6file_read(&r, 0, dst, CELLS9) == 0, "read after restart");
    CHECK(memcmp(dst, src, CELLS9) == 0, "restart bytes");
    v6spill_close(&sp);

    /* T3: spill file full = veto, fail loud (10 cells, 1 slot: 2nd evict vetoes). */
    {
        V6Spill s1;
        remove("build/test_vol6_spill1.bin");
        CHECK(v6spill_open(&s1, "build/test_vol6_spill1.bin", 1) == 0, "open1");
        v6spill_active = &s1;
        v6res_init(&r);
        r.spill = v6spill_spill; r.fill = v6spill_fill;
        CHECK(v6file_write(&r, 0, src, CELLS10) != 0, "full vetoes");
        v6spill_close(&s1);
        remove("build/test_vol6_spill1.bin");
    }
    v6spill_active = NULL;
    remove(path);

    if (!fails) printf("vol6_spill: ALL PASS\n");
    return fails ? 1 : 0;
}

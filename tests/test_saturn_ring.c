/* test_saturn_ring.c — Saturn-ring store acceptance (see SATURN-RING-SPEC) */
#include <stdio.h>
#include <string.h>
#include "core/saturn_ring.h"

static int fails = 0;
#define CHECK(c, msg) do { \
    if (!(c)) { printf("  FAIL: %s\n", msg); fails++; } \
} while (0)

int main(void) {
    SaturnStore s;
    /* ring0 hot: 8 slots, ring1 cold: 32 slots, slot 1KB, ratio 3:1 */
    CHECK(saturn_init(&s, 8, 32, 1024, 3, 1) == 0, "init");

    /* T1: roundtrip byte-identical */
    uint8_t wbuf[1024], rbuf[1024];
    for (int i = 0; i < 1024; i++) wbuf[i] = (uint8_t)(i * 13 + 7);
    CHECK(saturn_promote(&s, "weight.layer0", wbuf, 1024) == 0, "promote fits");
    uint8_t *p = saturn_get(&s, "weight.layer0");
    CHECK(p != NULL, "get hit");
    if (p) { memcpy(rbuf, p, 1024); CHECK(memcmp(rbuf, wbuf, 1024) == 0, "roundtrip identical"); }

    /* T2: fixed capacity — overfill replaces, never grows */
    for (int i = 0; i < 40; i++) {
        char nm[64]; snprintf(nm, sizeof(nm), "fill.%d", i);
        CHECK(saturn_promote(&s, nm, wbuf, 1024) >= 0, "overfill ok");
    }
    CHECK(s.hot.n_valid == 8, "hot stays 8 (fixed radius)");
    CHECK(saturn_verify(&s) == 0, "invariants hold under overfill");

    /* T3: oversize rejected, never split */
    uint8_t big[2048]; memset(big, 0xAA, sizeof(big));
    CHECK(saturn_promote(&s, "too.big", big, sizeof(big)) == -1, "oversize rejected");

    /* T4: demote path + cold roundtrip */
    CHECK(saturn_demote_cold(&s, "cold.item", wbuf, 512) == 0, "demote fits");
    p = saturn_get(&s, "cold.item");
    CHECK(p != NULL && memcmp(p, wbuf, 512) == 0, "cold roundtrip");

    /* T5: ratio needle — skewed workload favors hot placements.
       NOTE: ratio is a quota direction, not a hard lock; assert hot serves
       the hot set: re-get hot items, all must hit with identical bytes. */
    int hot_hits = 0;
    for (int i = 0; i < 100; i++) {
        char nm[64]; snprintf(nm, sizeof(nm), "hot.%d", i % 4);
        saturn_promote(&s, nm, wbuf, 1024);
        p = saturn_get(&s, nm);
        if (p && memcmp(p, wbuf, 1024) == 0) hot_hits++;
    }
    CHECK(hot_hits == 100, "hot set always serves identical");
    printf("  counters: promote=%lu demote=%lu evict=%lu hot_place=%lu cold_place=%lu\n",
           (unsigned long)s.n_promote, (unsigned long)s.n_demote,
           (unsigned long)s.n_evict,
           (unsigned long)s.hot.n_place, (unsigned long)s.cold.n_place);
    CHECK(saturn_verify(&s) == 0, "final invariants");

    saturn_free(&s);
    printf("SATURN_RING: %s\n", fails ? "FAIL" : "ALL PASS");
    return fails ? 1 : 0;
}

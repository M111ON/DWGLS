/* tests/test_win_cache.c — bounded window cache + prefetch skeleton.
 *
 * Oracle: hand-computed tick/victim sequences below, NOT the header's own
 * output. Mutation check: cap off-by-one / stale-tick / rank-window shift
 * must all go red.
 */
#include <stdio.h>
#include <stdint.h>
#include "../core/win_cache.h"

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS - %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL - %s\n", desc); } \
} while (0)

/* rank r -> windows [10*r, 10*r+1] (2 windows per rank, hand-fixed). */
static void rank2win(uint32_t rank, void *ctx, uint64_t *w0, uint64_t *w1) {
    (void)ctx; *w0 = (uint64_t)rank * 10; *w1 = (uint64_t)rank * 10 + 1;
}

int main(void) {
    wc_entry_t tab[8];
    win_cache_t c;
    uint64_t victim = 0;

    /* T1: demand admit until cap, then victim = oldest tick. */
    wc_init(&c, tab, 4);
    CHECK("T1a touch 1,2,3 = miss-admit", wc_touch(&c, 1, &victim) == 0
        && wc_touch(&c, 2, &victim) == 0 && wc_touch(&c, 3, &victim) == 0);
    CHECK("T1b n==3 hits==0 misses==3", c.n == 3 && c.hits == 0 && c.misses == 3);
    CHECK("T1c re-touch 2 = hit", wc_touch(&c, 2, &victim) == 1 && c.hits == 1);
    CHECK("T1d touch 4 = admit, n==4", wc_touch(&c, 4, &victim) == 0 && c.n == 4);
    /* ticks: 1->1, 2->2, 3->3, 2->4(hit), 4->5. Full. Touch 5: victim=oldest=win1(tick1). */
    CHECK("T1e full touch 5 -> -1 victim=1", wc_touch(&c, 5, &victim) == -1 && victim == 1);
    CHECK("T1f full: entry NOT removed (n==4)", c.n == 4);

    /* T2: prefetch hints admit flagged, demand hit converts + counts used. */
    wc_init(&c, tab, 8);
    uint32_t h = wc_prefetch(&c, 0, 2, rank2win, NULL); /* ranks 1,2 -> wins 10,11,20,21 */
    CHECK("T2a prefetch(0,K=2) = 4 hints", h == 4 && c.prefetch_hints == 4 && c.n == 4);
    CHECK("T2b demand touch hinted win10 = hit+used", wc_touch(&c, 10, &victim) == 1
        && c.prefetch_used == 1);
    CHECK("T2c untouched hint stays flagged", wc_find(&c, 11) >= 0 && tab[wc_find(&c, 11)].prefetched == 1);

    /* T3: full cache drops hints honestly (counted, never forces admission). */
    wc_init(&c, tab, 2);
    wc_touch(&c, 1, &victim); wc_touch(&c, 2, &victim); /* full, ticks 1,2 */
    uint64_t hints_before = c.prefetch_hints;
    wc_hint(&c, 99); /* no room */
    CHECK("T3a hint on full: dropped, n==2", c.n == 2 && c.prefetch_hints == hints_before + 1);
    CHECK("T3b win99 absent", wc_find(&c, 99) < 0);

    /* T4: MoE selection = caller passes selected ranks only; unselected never hinted. */
    wc_init(&c, tab, 8);
    /* simulate layer with 4 experts, router picks experts (ranks) 1 and 3 only. */
    uint64_t w0, w1;
    rank2win(1, NULL, &w0, &w1); for (uint64_t w = w0; w <= w1; w++) wc_hint(&c, w);
    rank2win(3, NULL, &w0, &w1); for (uint64_t w = w0; w <= w1; w++) wc_hint(&c, w);
    CHECK("T4a selected experts hinted (10,11,30,31)", wc_find(&c, 10) >= 0 && wc_find(&c, 31) >= 0);
    CHECK("T4b unselected experts absent (20,21)", wc_find(&c, 20) < 0 && wc_find(&c, 21) < 0);

    /* T5: victim is OLDEST tick, not smallest id. */
    wc_init(&c, tab, 3);
    wc_touch(&c, 30, &victim); /* tick1 */
    wc_touch(&c, 10, &victim); /* tick2 */
    wc_touch(&c, 20, &victim); /* tick3 */
    wc_touch(&c, 10, &victim); /* tick4 refresh win10 */
    CHECK("T5 victim=30 (oldest tick) not 10", wc_touch(&c, 40, &victim) == -1 && victim == 30);

    printf("FINAL: %d/%d PASS\n", pass_count, pass_count + fail_count);
    return fail_count ? 1 : 0;
}

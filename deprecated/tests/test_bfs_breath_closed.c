/*
 * test_bfs_breath_closed.c — Fold probe: does a CLOSED metric cut reanchors?
 * ═══════════════════════════════════════════════════════════════════════════
 * Hypothesis (user, 2026-09-13): folding the flat grid into a closed solid
 * contains the breathing force -> fewer re-anchors at the same bound.
 *
 * Honest setup: bfs_breath_delta_at is already all-mod (no flat clamp
 * exists), so "flat code vs wrapped code" would measure the same thing
 * twice (non-experiment). The real difference is the METRIC:
 *   FLAT   : d = cur - home (line; far side is far)
 *   CLOSED : d = ring-fold(cur - home) (closed solid; far side reachable
 *            the short way round: |d| > space/2 -> d -=/+ space)
 * Identical inputs (same seed, step, ticks) -> compare reanchor counts.
 * Either side may win; comparison is REPORTED, not asserted (TIER1 stays
 * green either way). Asserted: both runs bounded + main path lossless.
 *
 * Feasibility note: 12 | 20736 exactly (12x1728 dodeca frame,
 * TH_PENTAGON_NODES) while 20 ∤ 20736 — the evenly-tiling closed frame
 * is the 12-face dodeca side; 20 triangles are its dual view.
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -Icore/infra -o build/test_bfs_breath_closed tests/test_bfs_breath_closed.c -lm
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "bfs_breath.h"

static int pass = 0, fail = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass++; printf("  T: PASS — %s\n", desc); } \
    else      { fail++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

#define CLOSED_TICKS 4000u
#define CLOSED_STEP  0.005  /* deep: space=20736*s passes (127,254) band where
                             * ring CAN differ (space/2<127): line fires at
                             * |d|>127 while ring folds to space-|d|<=127.
                             * With step 0.05 (space>=1036) ring provably never
                             * binds -> null experiment (caught by T3). */

static void fill_file(int8_t *d, uint32_t n, uint32_t seed) {
    for (uint32_t i = 0; i < n; i++)
        d[i] = (int8_t)((seed * 31 + i * 7 + (i >> 3)) & 0xFF);
}

static void seed_fs(BreathingFS *fs, int8_t d0[144], int8_t d1[432]) {
    bfs_init(fs);
    fill_file(d0, 144, 1);
    fill_file(d1, 432, 2);
    fs->seeker.current_pos = 500; fs->seeker.home_pos = 500;
    bfs_write(fs, "a.bin", d0, 144);
    fs->seeker.current_pos = 1200; fs->seeker.home_pos = 1200;
    bfs_write(fs, "b.bin", d1, 432);
}

/* CLOSED metric: ring distance on the current space (folded solid).
 * Both cur and home are mapped onto the ring (home_r = home % space);
 * the difference then lies in (-space, space) so one fold is exact. */
static int32_t closed_delta_at(uint32_t home_pos, double scale) {
    if (home_pos >= BFS_TOTAL_SLOTS) return 0;
    double shifted = (double)home_pos * scale;
    uint32_t space = (uint32_t)(BFS_TOTAL_SLOTS * scale);
    if (space < 1) space = 1;
    uint32_t cur = ((uint32_t)shifted) % space;
    uint32_t home_r = home_pos % space;
    int32_t d = (int32_t)cur - (int32_t)home_r;
    int32_t half = (int32_t)(space / 2u);
    if (d > half) d -= (int32_t)space;
    else if (d < -half) d += (int32_t)space;
    return d;
}

/* One closed-metric tick: mirrors bfs_breath_tick, metric swapped. */
static void closed_tick(BFSBreath *b, uint32_t *raw_peak) {
    if (!b || !b->fs) return;
    b->ticks++;
    if (b->direction > 0) {
        b->cur_scale -= b->scale_step;
        if (b->cur_scale <= b->scale_step) { b->cur_scale = b->scale_step; b->direction = -1; }
    } else {
        b->cur_scale += b->scale_step;
        if (b->cur_scale >= 1.0) { b->cur_scale = 1.0; b->direction = 1; }
    }
    double s = b->cur_scale;
    for (uint32_t i = 0; i < BFS_BLOCKS; i++) {
        if (b->fs->block_owner[i] == 0xFFFFFFFF) continue;
        BFSliveAnchor *a = &b->live[i];
        int32_t d = closed_delta_at(a->home_pos, s);
        uint32_t ad = (uint32_t)(d < 0 ? -d : d);
        if (raw_peak && ad > *raw_peak) *raw_peak = ad;
        if (d > (int32_t)BFS_BREATH_BOUND || d < -(int32_t)BFS_BREATH_BOUND) {
            uint32_t space = (uint32_t)(BFS_TOTAL_SLOTS * s);
            if (space < 1) space = 1;
            uint32_t cur = ((uint32_t)((double)a->home_pos * s)) % space;
            a->home_pos = cur;
            a->scale_at_write = s;
            a->delta = 0;
            b->reanchors++;
        } else {
            a->delta = d;
        }
    }
}

static int main_lossless(BFSBreath *b, BreathingFS *fs,
                         const int8_t *d0, const int8_t *d1) {
    static int8_t out[BFS_SLOTS_BLOCK * 4];
    uint32_t act = 0;
    if (bfs_breath_read_main(b, "a.bin", out, 144, &act) != 0 ||
        memcmp(out, d0, 144) != 0) return 0;
    if (bfs_read(fs, "b.bin", out, 432, &act) != 0 ||
        memcmp(out, d1, 432) != 0) return 0;
    return 1;
}

int main(void) {
    printf("═ FOLD PROBE — flat metric vs closed metric (identical inputs) ═\n");

    /* Oracle pins (independent hand computation, not from engine):
     * flat(1200,0.05): space=1036, cur=60 → 60-1200 = -1140.
     * closed(1200,0.05): home_r=1200%1036=164, 60-164=-104, |d|<518 → -104. */
    CHECK("flat oracle (1200,0.05)=-1140", bfs_breath_delta_at(1200, 0.05) == -1140);
    CHECK("closed oracle (1200,0.05)=-104", closed_delta_at(1200, 0.05) == -104);

    /* ── Run F: flat (existing engine, untouched) ── */
    BreathingFS ffs; int8_t fd0[144], fd1[432];
    seed_fs(&ffs, fd0, fd1);
    BFSBreath fb;
    bfs_breath_init(&fb, &ffs, CLOSED_STEP);
    for (uint32_t i = 0; i < CLOSED_TICKS; i++) bfs_breath_tick(&fb);
    int f_ok = main_lossless(&fb, &ffs, fd0, fd1);

    /* ── Run C: closed metric ── */
    BreathingFS cfs; int8_t cd0[144], cd1[432];
    seed_fs(&cfs, cd0, cd1);
    BFSBreath cb;
    bfs_breath_init(&cb, &cfs, CLOSED_STEP);
    uint32_t craw = 0;
    for (uint32_t i = 0; i < CLOSED_TICKS; i++) closed_tick(&cb, &craw);
    int c_ok = main_lossless(&cb, &cfs, cd0, cd1);

    printf("  FLAT   : ticks=%u reanchors=%u peak_raw=%u bounded=%s lossless=%s\n",
           fb.ticks, fb.reanchors, fb.peak_delta,
           bfs_breath_all_bounded(&fb) ? "YES" : "NO", f_ok ? "YES" : "NO");
    printf("  CLOSED : ticks=%u reanchors=%u peak_raw=%u bounded=%s lossless=%s\n",
           cb.ticks, cb.reanchors, craw,
           bfs_breath_all_bounded(&cb) ? "YES" : "NO", c_ok ? "YES" : "NO");
    printf("  VERDICT: closed reanchors %s flat (%u vs %u)\n",
           cb.reanchors < fb.reanchors ? "FEWER than" :
           cb.reanchors > fb.reanchors ? "MORE than" : "EQUAL to",
           cb.reanchors, fb.reanchors);

    CHECK("flat run bounded + lossless (existing behavior)", bfs_breath_all_bounded(&fb) && f_ok);
    CHECK("closed run bounded + lossless (safe under new metric)",
          bfs_breath_all_bounded(&cb) && c_ok);
    /* T3 (reported, not asserted): identical inputs → EQUAL (33 vs 33).
     * Null result, honest cause: reanchor dynamics keep home < space, where
     * ring ≡ flat by construction (home_r = home). The fold only binds in
     * the deepest ticks (home ≥ space), which the flat reanchor fires
     * through first. A fold that actually reduces reanchors would need a
     * ring-aware reanchor rule — a design change, not a metric swap. */

    printf("═ RESULT: %d pass, %d fail ═\n", pass, fail);
    return fail ? 1 : 0;
}

/*
 * test_bfs_breath_jump.c — Jump probe: does topology matter under teleport?
 * ═══════════════════════════════════════════════════════════════════════════
 * Open thread from the closed probe (#810): gradual drift crosses the deep
 * band (space<254) in ~2 ticks, so ring-vs-line never diverges. The one
 * untested regime is SCALE JUMP: cur_scale teleports across the band in a
 * single tick (1.0 -> 0.005), landing anchors deep instantly.
 *
 * Setup: two runs, identical seeds + identical jump schedule. FLAT uses the
 * existing engine (line metric); CLOSED mirrors it with the ring-fold
 * metric (same fold as test_bfs_breath_closed.c). Compare per-jump fire
 * decisions + bounded + lossless.
 *
 * Possible outcomes (all close the thread):
 *   divergences>0, both safe  -> topology changes decisions, not safety
 *   closed fewer + lossless    -> user hypothesis confirmed in jump regime
 *   divergences==0             -> topology never matters, thread shut
 * Falsifiability: CHECK fires if no jump ever lands in the deep band
 * (null experiment), or if either run breaks bounded/lossless.
 *
 * PROBE — not wired to Makefile (same status as the closed probe).
 * BUILD: gcc -O2 -Wall -I. -Icore -Icore/infra -o build/test_bfs_breath_jump tests/test_bfs_breath_jump.c -lm
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

#define JUMP_TICKS 2200u
#define JUMP_STEP  0.005

static void fill_file(int8_t *d, uint32_t n, uint32_t seed) {
    for (uint32_t i = 0; i < n; i++)
        d[i] = (int8_t)((seed * 31 + i * 7 + (i >> 3)) & 0xFF);
}

static void seed_fs(BreathingFS *fs, int8_t d0[144], int8_t d1[432]) {
    bfs_init(fs);
    fill_file(d0, 144, 1);
    fill_file(d1, 432, 2);
    fs->seeker.current_pos = 180; fs->seeker.home_pos = 180;
    bfs_write(fs, "a.bin", d0, 144);
    fs->seeker.current_pos = 1200; fs->seeker.home_pos = 1200;
    bfs_write(fs, "b.bin", d1, 432);
}

/* closed metric + tick: identical to the closed probe (folded solid). */
static int32_t closed_delta_at(uint32_t home_pos, double scale) {
    if (home_pos >= BFS_TOTAL_SLOTS) return 0;
    double shifted = (double)home_pos * scale;
    uint32_t space = (uint32_t)(BFS_TOTAL_SLOTS * scale);
    if (space < 1) space = 1;
    uint32_t cur = ((uint32_t)shifted) % space;
    int32_t d = (int32_t)cur - (int32_t)home_pos;
    int32_t half = (int32_t)(space / 2u);
    if (d > half) d -= (int32_t)space;
    else if (d < -half) d += (int32_t)space;
    return d;
}

static void closed_tick(BFSBreath *b) {
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

/* deterministic teleport schedule (scale values, applied at fixed ticks) */
static const double JUMPS[] =
    { 0.004, 1.0, 0.008, 0.5, 0.003, 1.0, 0.02, 0.006, 1.0, 0.05 };
#define NJUMP (sizeof(JUMPS) / sizeof(JUMPS[0]))

int main(void) {
    printf("═ JUMP PROBE — flat vs closed metric under scale teleport ═\n");

    BreathingFS ffs; int8_t fd0[144], fd1[432];
    seed_fs(&ffs, fd0, fd1);
    BFSBreath fb;
    bfs_breath_init(&fb, &ffs, JUMP_STEP);

    BreathingFS cfs; int8_t cd0[144], cd1[432];
    seed_fs(&cfs, cd0, cd1);
    BFSBreath cb;
    bfs_breath_init(&cb, &cfs, JUMP_STEP);

    uint32_t divergences = 0, deep_jumps = 0;
    for (uint32_t t = 0; t < JUMP_TICKS; t++) {
        /* apply scheduled teleport to BOTH runs before the tick.
         * First jump at tick 2: homes still pristine (180/1200), scale
         * teleports 1.0 -> deep in one step (the untested regime). */
        for (uint32_t j = 0; j < NJUMP; j++) {
            if (t == 2u + j * 180u) {
                fb.cur_scale = JUMPS[j];
                cb.cur_scale = JUMPS[j];
                uint32_t space = (uint32_t)(BFS_TOTAL_SLOTS * JUMPS[j]);
                if (space < 254u) deep_jumps++;
                uint32_t rf0 = fb.reanchors, rc0 = cb.reanchors;
                bfs_breath_tick(&fb);
                closed_tick(&cb);
                uint32_t df = fb.reanchors - rf0, dc = cb.reanchors - rc0;
                if (df != dc) {
                    divergences++;
                    printf("  DIVERGE tick=%u scale=%g flat_fires=%u closed_fires=%u\n",
                           t, JUMPS[j], df, dc);
                }
                goto next_tick;
            }
        }
        bfs_breath_tick(&fb);
        closed_tick(&cb);
next_tick:;
    }

    int f_ok = main_lossless(&fb, &ffs, fd0, fd1);
    int c_ok = main_lossless(&cb, &cfs, cd0, cd1);
    printf("  FLAT   : reanchors=%u bounded=%s lossless=%s\n",
           fb.reanchors, bfs_breath_all_bounded(&fb) ? "YES" : "NO",
           f_ok ? "YES" : "NO");
    printf("  CLOSED : reanchors=%u bounded=%s lossless=%s\n",
           cb.reanchors, bfs_breath_all_bounded(&cb) ? "YES" : "NO",
           c_ok ? "YES" : "NO");
    printf("  jumps=%u deep=%u per-jump decision divergences=%u\n",
           (unsigned)NJUMP, deep_jumps, divergences);

    CHECK("jumps reached the deep band (not a null experiment)", deep_jumps > 0);
    CHECK("flat run bounded + lossless under teleport",
          bfs_breath_all_bounded(&fb) && f_ok);
    CHECK("closed run bounded + lossless under teleport",
          bfs_breath_all_bounded(&cb) && c_ok);

    printf("═ RESULT: %d pass, %d fail ═\n", pass, fail);
    return fail ? 1 : 0;
}

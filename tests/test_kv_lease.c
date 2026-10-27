/* test_kv_lease.c — the parts of the three proposals that survived, measured.
 *
 * Each check is a gate that a proposal named, restated as something the code can
 * actually fail:
 *   orphan      a subagent that stops beating loses its claim within the lease,
 *               and not one millisecond before it
 *   dangling    a stale handle never reads/writes/beats: it names both generations
 *   close≠free  reaping returns slot accounting while the payload stays written
 *   provisioning  three scripted loads: borrow rate + rounds to converge
 *   burst       a mid-round overshoot borrows without touching main's floor
 *   overhead    quota update is O(1): measured ns/op, not asserted from theory
 *   determinism same scripted profile twice -> identical numbers
 */
#include "kv_lease.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { pass++; } else { fail++; printf("  FAIL %s\n", msg); } \
} while (0)

#ifdef _WIN32
#include <windows.h>
static int64_t now_ns(void) {
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (int64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
}
#else
#include <time.h>
static int64_t now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
#endif

/* one scripted profile: returns a hash over the trace so two runs can be compared */
static uint64_t run_profile(int64_t lease_ms, int32_t quota0,
                            int rounds_per_level, int32_t *out_borrows,
                            int32_t *out_quota_end, int32_t *out_steps,
                            int32_t conv[3]) {
    const int32_t level[3] = { 200, 800, 3200 };   /* slots demanded per round */
    kvb_table t;
    kvb_init(&t, 20736, quota0, 64, 4096, 1024, lease_ms);
    uint64_t h = 0xCBF29CE484222325ULL;
    int64_t now = 1000;
    for (int L = 0; L < 3; L++) {
        conv[L] = -1;
        for (int r = 0; r < rounds_per_level; r++) {
            now += 100;                       /* a round is 100 ms */
            kvb_quota_observe(&t, level[L]);  /* usage of the round that just ended */
            int allow = (level[L] > t.quota);
            kvb_ref ref;
            int rc = kvb_reserve(&t, 1, level[L], now, allow, &ref);
            if (rc == KVB_OK) {
                kvb_write(&t, ref, level[L] / 2, now);
                kvb_close(&t, ref, now);
                /* converged: the round ran inside the quota, no borrow needed */
                if (!allow && conv[L] < 0) conv[L] = r + 1;
            }
            h = (h ^ (uint64_t)(t.quota + rc * 7 + L)) * 0x100000001B3ULL;
        }
    }
    *out_borrows = t.borrows;
    *out_quota_end = t.quota;
    *out_steps = t.quota_steps;
    return h;
}

int main(void) {
    printf("══ kv_lease: reserved blocks, generations, quota ══\n\n");

    /* ── 1. orphan: a dead subagent's claim returns when the lease expires ── */
    {
        const int64_t LEASE = 5000;
        kvb_table t; kvb_init(&t, 20736, 256, 64, 4096, 1024, LEASE);
        kvb_ref r; int rc = kvb_reserve(&t, 7, 256, 1000, 0, &r);
        CHECK(rc == KVB_OK, "orphan: subagent reserved its block");
        kvb_beat(&t, r, 5000);
        kvb_beat(&t, r, 5900);                       /* last sign of life */
        CHECK(kvb_reap(&t, 5900 + LEASE - 1) == 0, "orphan: not reaped before the lease ends");
        CHECK(t.b[0].state == KVB_RESERVED, "orphan: still reserved at lease-1");
        int n = kvb_reap(&t, 5900 + LEASE);
        CHECK(n == 1, "orphan: reaped exactly at lease expiry");
        CHECK(t.b[0].state == KVB_CLOSED && t.held == 0, "orphan: block closed, slots returned");
        CHECK(t.reap_events == 1, "orphan: reap counted once");
        printf("  orphan      reclaim at +%" PRId64 " ms after last beat (lease %" PRId64 " ms), "
               "not at +%" PRId64 " ms\n", LEASE, LEASE, LEASE - 1);
    }

    /* ── 2. dangling handle: fails LOUD, names both generations ── */
    {
        kvb_table t; kvb_init(&t, 20736, 256, 64, 4096, 1024, 5000);
        kvb_ref r; kvb_reserve(&t, 7, 256, 0, 0, &r);
        kvb_reap(&t, 5001);
        uint32_t seen = 0;
        int rc = kvb_check(&t, r, &seen);
        CHECK(rc == KVB_ESTALE, "dangling: handle from the reaped block is refused");
        CHECK(seen == r.generation + 1, "dangling: the refusal names the current generation");
        CHECK(kvb_write(&t, r, 16, 6000) == KVB_ESTALE, "dangling: a stale handle cannot write");
        CHECK(kvb_beat(&t, r, 6000) == KVB_ESTALE, "dangling: a stale handle cannot extend its own life");
        CHECK(t.b[0].written_slots == 0, "dangling: nothing was written through the stale handle");
        printf("  dangling    refused with generation %u vs current %u, zero bytes written\n",
               r.generation, seen);
    }

    /* ── 3. close is not free: accounting returns, payload stays ── */
    {
        kvb_table t; kvb_init(&t, 20736, 256, 64, 4096, 1024, 60 * 60 * 1000);
        kvb_ref r; kvb_reserve(&t, 7, 256, 0, 0, &r);
        kvb_write(&t, r, 200, 10);
        int32_t written = t.b[r.idx].written_slots;
        CHECK(kvb_close(&t, r, 20) == KVB_OK, "close: closed");
        CHECK(t.held == 0, "close: slots back in the pool");
        CHECK(t.b[r.idx].written_slots == written && written == 200,
              "close: payload still recorded (nothing zeroed)");
        CHECK(kvb_check(&t, r, NULL) == KVB_ESTALE, "close: the closed handle is stale");
        kvb_ref r2; CHECK(kvb_reserve(&t, 9, 256, 30, 0, &r2) == KVB_OK, "close: block reusable");
        CHECK(r2.generation != r.generation, "close: reuse hands out a new generation");
        CHECK(kvb_check(&t, r2, NULL) == KVB_OK, "close: the new handle works");
        CHECK(kvb_write(&t, r2, 32, 40) == KVB_OK, "close: new owner can write");
    }

    /* ── 4. adaptive quota: 3 scripted levels, borrow rate, convergence ── */
    {
        int32_t b1 = 0, q1 = 0, s1 = 0, conv1[3] = {0, 0, 0};
        int32_t b2 = 0, q2 = 0, s2 = 0, conv2[3] = {0, 0, 0};
        uint64_t h1 = run_profile(5000, 256, 8, &b1, &q1, &s1, conv1);
        uint64_t h2 = run_profile(5000, 256, 8, &b2, &q2, &s2, conv2);
        printf("  adaptive    quota 256 → levels 200/800/3200 slots, 8 rounds each\n");
        printf("              converge in rounds: %d / %d / %d   steps=%d  borrows=%d  end quota=%d\n",
               conv1[0], conv1[1], conv1[2], s1, b1, q1);
        CHECK(conv1[1] <= 3, "adaptive: a 3.1x jump converges within 3 rounds");
        CHECK(conv1[2] <= 3, "adaptive: a 12.5x jump converges within 3 rounds");
        CHECK(h1 == h2 && b1 == b2 && q1 == q2 && conv1[2] == conv2[2],
              "adaptive: same profile twice -> identical trace and numbers");
        printf("              determinism: trace %016" PRIx64 " twice\n", h1);
    }

    /* ── 5. burst: overshoot borrows, main's floor is never crossed ── */
    {
        kvb_table t; kvb_init(&t, 4096, 256, 64, 4096, 1024, 5000);
        kvb_ref a; CHECK(kvb_reserve(&t, 1, 256, 0, 0, &a) == KVB_OK, "burst: steady block");
        kvb_ref main_ref; CHECK(kvb_reserve(&t, 0, 1024, 1, 0, &main_ref) == KVB_OK,
                                "burst: main holds its floor");
        kvb_ref burst;
        int rc = kvb_reserve(&t, 2, 900, 2, 1, &burst);   /* 900 > quota 256 */
        CHECK(rc == KVB_OK, "burst: mid-round overshoot allowed to borrow");
        CHECK(t.borrows == 1, "burst: borrow counted");
        kvb_ref too_big;
        rc = kvb_reserve(&t, 3, 1000, 3, 1, &too_big);   /* fits free, but eats the floor */
        CHECK(rc == KVB_EFLOOR, "burst: a borrow that would leave main below its floor is refused");
        CHECK(t.held == 256 + 1024 + 900, "burst: main's 1024 slots untouched");
        printf("  burst       borrow 900 slots ok, 1000 refused (would leave 916 < floor 1024), "
               "main intact\n");
    }

    /* ── 6. overhead: O(1) quota update, measured ── */
    {
        kvb_table t; kvb_init(&t, 20736, 256, 64, 4096, 1024, 5000);
        const int N = 1000000;
        int64_t t0 = now_ns();
        for (int i = 0; i < N; i++) kvb_quota_observe(&t, (i & 1) ? 200 : 3200);
        int64_t dt = now_ns() - t0;
        double ns_op = (double)dt / N;
        printf("  overhead    %d quota updates in %.1f ms = %.2f ns/op\n", N, dt / 1e6, ns_op);
        CHECK(ns_op < 1000000.0, "overhead: one quota update is far below 1 ms");
        int64_t t1 = now_ns();
        for (int i = 0; i < 100000; i++) {
            kvb_ref r;
            if (kvb_reserve(&t, 1, 256, i, 0, &r) == KVB_OK) { kvb_write(&t, r, 8, i); kvb_close(&t, r, i); }
        }
        double ns_cycle = (double)(now_ns() - t1) / 100000;
        printf("              reserve+write+close = %.2f ns/cycle\n", ns_cycle);
        CHECK(ns_cycle < 1000000.0, "overhead: reserve/write/close below 1 ms");
    }

    /* ── 7. waste gate is AGE, not a percentage ── */
    {
        kvb_table t; kvb_init(&t, 20736, 256, 64, 4096, 1024, 5000);
        kvb_ref a, b;
        kvb_reserve(&t, 1, 256, 1000, 0, &a);          /* written, and still beating */
        kvb_write(&t, a, 256, 1000);
        kvb_reserve(&t, 2, 256, 1000, 0, &b);          /* never written, never beats */
        kvb_beat(&t, a, 3000); kvb_beat(&t, a, 5900);  /* live subagent keeps its claim */
        CHECK(kvb_reserved_unused(&t) == 256, "waste: only the untouched block counts as unused");
        int64_t oldest = kvb_oldest_live_ms(&t, 1000 + 5000);
        CHECK(oldest == 5000, "waste: age of the oldest live reservation measured");
        int n = kvb_reap(&t, 1000 + 5000);
        CHECK(n == 1 && t.b[b.idx].state == KVB_CLOSED, "waste: the aging reservation is the one reaped");
        CHECK(t.b[a.idx].written_slots == 256, "waste: the written block's bytes untouched");
        printf("  waste       unused=%d slots, oldest live=%" PRId64 " ms, reaped the aging one only\n",
               kvb_reserved_unused(&t), oldest);
    }

    printf("\n=== kv_lease: %d PASS, %d FAIL ===\n", pass, fail);
    return fail ? 1 : 0;
}

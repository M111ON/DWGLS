/*
 * test_planet_detach.c — Detached frame + entangle tail (design §134-142)
 *
 * Oracle: digest pinned by independently written ref loop; detach proven
 * by struct snapshot across 5000 main breath ticks; replay via REAL
 * fan24 fg_enc chain (independent code path).
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -Icore/infra -o build/test_planet_detach tests/test_planet_detach.c
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../core/geo_planet.h"
#include "../core/bfs_breath.h"

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

/* independent ref digest (own loop, same spec: h=5381,h=h*33+b) */
static uint32_t ref_digest(const int8_t *d, uint32_t n) {
    uint32_t h = 5381u;
    const int8_t *q = d, *end = d + n;
    for (; q < end; q++) h = h * 33u + (uint8_t)*q;
    return h;
}

static void fill_buf(int8_t *d, uint32_t n, uint32_t seed) {
    for (uint32_t i = 0; i < n; i++)
        d[i] = (int8_t)((seed * 31u + i * 7u + (i >> 3)) & 0xFFu);
}

int main(void) {
    printf("═ PLANET DETACH — frame + idle-zero tail + tombstone ═\n");

    int8_t buf[432];
    fill_buf(buf, 432, 7);

    /* ── T1: birth pins digest (independent oracle) ────────────── */
    Planet p1;
    planet_birth(&p1, 11u, 5u, 900u, buf, 432);
    CHECK("T1: birth digest == independent ref", p1.digest == ref_digest(buf, 432));
    CHECK("T1b: birth frame (id/home/birth/cur, clean tail)",
          p1.id == 11u && p1.home == 900u && p1.birth_w == 5u &&
          p1.cur_w == 5u && p1.tail_n == 0u && !p1.retired);

    /* ── T2: idle-zero — healthy verifies collect nothing ──────── */
    {
        int ok = 1;
        for (int i = 0; i < 50; i++)
            if (planet_verify(&p1, buf, 432) != 0) { ok = 0; break; }
        CHECK("T2: 50 clean verifies, tail stays 0 (idle-zero)", ok && p1.tail_n == 0u);
    }

    /* ── T3: detach — 5000 main breaths, planet struct bit-identical ── */
    {
        BreathingFS fs;
        bfs_init(&fs);
        int8_t d0[144], d1[432];
        fill_buf(d0, 144, 1);
        fill_buf(d1, 432, 2);
        fs.seeker.current_pos = 500; fs.seeker.home_pos = 500;
        bfs_write(&fs, "a.bin", d0, 144);
        fs.seeker.current_pos = 1200; fs.seeker.home_pos = 1200;
        bfs_write(&fs, "b.bin", d1, 432);
        BFSBreath b;
        bfs_breath_init(&b, &fs, 0.05);

        Planet snap = p1;   /* snapshot before churn */
        for (int i = 0; i < 5000; i++) bfs_breath_tick(&b);
        CHECK("T3: main churned (reanchors>0) yet planet untouched",
              b.reanchors > 0 && memcmp(&snap, &p1, sizeof(Planet)) == 0 &&
              planet_verify(&p1, buf, 432) == 0);
    }

    /* ── T4: error collect on 1-byte corruption ────────────────── */
    {
        int8_t bad[432];
        memcpy(bad, buf, 432);
        bad[100] ^= 0x40;
        int rc = planet_verify(&p1, bad, 432);
        CHECK("T4: mismatch collected (rc=1, tail=1, fields exact)",
              rc == 1 && p1.tail_n == 1u &&
              p1.tail[0].w == p1.cur_w &&
              p1.tail[0].expected == ref_digest(buf, 432) &&
              p1.tail[0].observed == ref_digest(bad, 432));
        CHECK("T4b: bytes restored -> clean again, tail keeps history",
              planet_verify(&p1, buf, 432) == 0 && p1.tail_n == 1u);
    }

    /* ── T5: tail overflow flag (9 errors, cap 8) ──────────────── */
    {
        for (int k = 0; k < 9; k++) {
            int8_t bad[432];
            memcpy(bad, buf, 432);
            bad[k] ^= (int8_t)(k + 1);
            planet_verify(&p1, bad, 432);
        }
        CHECK("T5: tail caps at 8 + overflow flag (nothing silently dropped)",
              p1.tail_n == 8u && p1.tail_overflow == 1u);
    }

    /* ── T6: birth-max — shrink ok, expansion beyond birth rejected ── */
    {
        /* NOTE: T4/T5 left tail_n=8; use fresh planet for clean rule check */
        Planet p;
        planet_birth(&p, 12u, 5u, 100u, buf, 432);
        int r1 = planet_shrink(&p, 7u);
        int r2 = planet_shrink(&p, 5u);   /* re-widen to birth: allowed */
        int r3 = planet_shrink(&p, 4u);   /* beyond birth: rejected */
        CHECK("T6: 7 ok, 5 ok (birth floor), 4 rejected + violation, W stays",
              r1 == 0 && r2 == 0 && r3 == -1 && p.violations == 1u && p.cur_w == 5u);
    }

    /* ── T7: tombstone on retire, severed after ────────────────── */
    {
        planet_retire(&p1, 9u);
        CHECK("T7: tombstone plate exact",
              p1.tomb.magic == PLANET_TOMB_MAGIC && p1.tomb.id == 11u &&
              p1.tomb.birth_w == 5u && p1.tomb.death_w == 9u &&
              p1.tomb.final_home == 900u &&
              p1.tomb.digest == ref_digest(buf, 432));
        CHECK("T7b: severed — verify=-2, shrink=-1 after retire",
              planet_verify(&p1, buf, 432) == -2 && planet_shrink(&p1, 9u) == -1);
    }

    /* ── T8: replay walks real fan24 chain; tamper diverges ────── */
    {
        Planet p2;
        planet_birth(&p2, 13u, 10u, 50u, buf, 432);
        /* build real chain 10 ->..-> w3 with fg_enc (independent path) */
        FGGearEv ev[3];
        uint32_t w = 10u, targets[3] = { 30u, 61u, 100u };
        for (int i = 0; i < 3; i++) {
            ev[i] = fg_enc(w, targets[i]);
            w = fg_dec(w, ev[i]);
        }
        int agree = planet_replay(&p2, ev, 3, w);
        ev[1].dc ^= 1u;   /* tamper one tooth */
        int div = planet_replay(&p2, ev, 3, w);
        CHECK("T8: replay agrees on true tail, diverges on tamper, -1 on NULL",
              agree == 0 && div == 1 && planet_replay(&p2, 0, 3, w) == -1);
    }

    printf("═ RESULT: %d pass, %d fail ═\n", pass_count, fail_count);
    return fail_count ? 1 : 0;
}

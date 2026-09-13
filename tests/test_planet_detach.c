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

/* independent ref digest (own loop, same spec: h=5381,h=h*33+b, u64) */
static uint64_t ref_digest(const int8_t *d, uint32_t n) {
    uint64_t h = 5381u;
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

    /* ── T5: tail full -> AUTO-REANCHOR (fresh epoch, scar kept) ── */
    {
        Planet q;
        planet_birth(&q, 14u, 5u, 200u, buf, 432);
        int8_t epoch[432];
        int rcs[9];
        for (int k = 0; k < 9; k++) {
            memcpy(epoch, buf, 432);
            epoch[k] ^= (int8_t)(k + 1);
            rcs[k] = planet_verify(&q, epoch, 432);
        }
        /* first 8 collected (rc=1), 9th triggers reanchor (rc=2):
         * baseline=last observed, tail cleared, scar + epoch counted */
        memcpy(epoch, buf, 432);
        epoch[8] ^= 9;
        uint64_t last_obs = ref_digest(epoch, 432);
        int ok = (rcs[7] == 1 && rcs[8] == 2 && q.tail_n == 0u &&
                  q.reanchors == 1u && q.tail_overflow == 1u &&
                  q.digest == last_obs);
        /* new baseline reads clean; next divergence collects again */
        int clean = planet_verify(&q, epoch, 432);
        epoch[9] ^= 0x11;
        int again = planet_verify(&q, epoch, 432);
        CHECK("T5: 8 collected, 9th reanchors (rc=2, fresh epoch, scar kept)",
              ok && clean == 0 && again == 1 && q.tail_n == 1u);
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
        CHECK("T7: tombstone plate exact (+origin audit)",
              p1.tomb.magic == PLANET_TOMB_MAGIC && p1.tomb.id == 11u &&
              p1.tomb.birth_w == 5u && p1.tomb.death_w == 9u &&
              p1.tomb.final_home == 900u &&
              p1.tomb.digest == ref_digest(buf, 432) &&
              p1.tomb.origin == ref_digest(buf, 432));
        CHECK("T7b: severed — verify=-2, shrink=-1 after retire",
              planet_verify(&p1, buf, 432) == -2 && planet_shrink(&p1, 9u) == -1);
    }

    /* ── T8: replay walks real fan24 chain; tamper diverges ────── */
    {
        Planet p2;
        planet_birth(&p2, 13u, 10u, 50u, buf, 432);
        /* gate shut at birth: replay refuses until trouble opens it */
        FGGearEv ev0_[1] = {{0u, 0u, 0u}};
        int shut = planet_replay(&p2, ev0_, 1, 10u);
        /* trouble round: mismatch on a copy opens the gate, bytes pristine */
        int8_t tmpb[432];
        memcpy(tmpb, buf, 432);
        tmpb[0] ^= 0x01;
        int opened = (planet_verify(&p2, tmpb, 432) == 1 && p2.link_open == 1u);
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
              shut == -3 && opened && agree == 0 && div == 1 &&
              planet_replay(&p2, 0, 3, w) == -1);
    }

    /* ── T9: restore from tombstone (deposit path) ─────────────── */
    {
        Planet r;
        planet_birth(&r, 21u, 6u, 300u, buf, 432);
        planet_retire(&r, 9u);
        PlanetTomb plate = r.tomb;   /* copy: restore spends the original */
        int ok = planet_restore(&r, &plate, 9u, buf, 432);
        int same = (r.id == 21u && r.home == 300u && r.birth_w == 6u &&
                    r.cur_w == 9u && r.digest == plate.digest &&
                    r.tail_n == 0u && !r.retired);
        int8_t bad[432];
        memcpy(bad, buf, 432);
        bad[7] ^= 0x08;
        int changed = planet_restore(&r, &plate, 9u, bad, 432);
        int shrink_violation = planet_restore(&r, &plate, 4u, buf, 432);
        PlanetTomb fake = plate;
        fake.magic = 0u;
        int badtomb = planet_restore(&r, &fake, 9u, buf, 432);
        int relive = planet_verify(&r, bad, 432);
        CHECK("T9: restore ok + soul intact, body-change -2, scale -3, bad tomb -1",
              ok == 0 && same && changed == -2 && shrink_violation == -3 &&
              badtomb == -1 && relive == 1 && r.tail_n == 1u);
    }

    /* ── T10: thaw — fail-closed keyed read ─────────────────────── */
    {
        Planet w;
        planet_birth(&w, 31u, 5u, 400u, buf, 432);
        uint64_t key = w.digest;
        int8_t bad[432];
        memcpy(bad, buf, 432);
        bad[3] ^= 0x02;
        int ok_key = (planet_thaw(&w, key, buf, 432) == buf);
        int bad_key = (planet_thaw(&w, key ^ 1u, buf, 432) == 0);
        int bad_bytes = (planet_thaw(&w, key, bad, 432) == 0);
        planet_retire(&w, 9u);
        int retired = (planet_thaw(&w, key, buf, 432) == 0);
        CHECK("T10: thaw returns buf on key+bytes, NULL on wrong key/bytes/retired",
              ok_key && bad_key && bad_bytes && retired);
    }

    /* ── T11: W-fold pins (v2 folds every entry % 144) ──────────────
     * Hand arithmetic (144*694 = 99936):
     *   100000-99936 = 64 | 100064-99936 = 128 | 99937-99936 = 1
     *   100010-99936 = 74. If any entry drops its fold, these go red. */
    {
        Planet p;
        planet_birth(&p, 41u, 100000u, 400u, buf, 432);
        int born_folded = (p.birth_w == 64u && p.cur_w == 64u);
        int ok_wide = (planet_shrink(&p, 100064u) == 0 && p.cur_w == 128u);
        int rej_narrow = (planet_shrink(&p, 99937u) == -1);
        int8_t tmpc[432];   /* trouble round opens the gate (bytes pristine) */
        memcpy(tmpc, buf, 432);
        tmpc[1] ^= 0x02;
        int opened = (planet_verify(&p, tmpc, 432) == 1 && p.link_open == 1u);
        static const FGGearEv ev0[1] = {{0u, 0u, 0u}}; /* Δ=0: stays 64 */
        int rep_fold = planet_replay(&p, ev0, 1, 100000u); /* folds to 64 */
        int rep_div = planet_replay(&p, ev0, 1, 65u);
        int rep_null = planet_replay(&p, 0, 1, 64u);   /* no tail: -1 */
        planet_retire(&p, 100010u);
        int tomb_folded = (p.tomb.death_w == 74u);
        CHECK("T11: large-W folded at birth/shrink/retire/replay",
              born_folded && ok_wide && rej_narrow && opened &&
              rep_fold == 0 && rep_div == 1 && rep_null == -1 && tomb_folded);
    }

    /* ── T12: gate lifecycle — shut birth, self-open, mask, self-close ── */
    {
        Planet g;
        planet_birth(&g, 51u, 0u, 600u, buf, 432);
        static const FGGearEv z[1] = {{0u, 0u, 0u}};
        int shut_birth = (planet_replay(&g, z, 1, 0u) == -3);
        int8_t tmpg[432];
        memcpy(tmpg, buf, 432);
        tmpg[2] ^= 0x04;
        int self_open = (planet_verify(&g, tmpg, 432) == 1 &&
                         g.link_open == 1u && g.clean_streak == 0u &&
                         planet_replay(&g, z, 1, 0u) == 0);
        /* mask proof: 10 events, first 8 Δ=0, last 2 Δ=+10 ({0,2,1}:
         * crt(2,1)=10). Full walk ends 20 (diverge); masked walk ends 0. */
        static const FGGearEv ten[10] = {
            {0u,0u,0u},{0u,0u,0u},{0u,0u,0u},{0u,0u,0u},{0u,0u,0u},
            {0u,0u,0u},{0u,0u,0u},{0u,0u,0u},{0u,2u,1u},{0u,2u,1u}
        };
        int masked = (planet_replay(&g, ten, 10, 0u) == 0);
        /* 3 consecutive cleans: gate shuts itself, tail dropped, scar kept */
        int c1 = planet_verify(&g, buf, 432);
        int c2 = planet_verify(&g, buf, 432);
        int c3 = planet_verify(&g, buf, 432);
        int self_close = (c1 == 0 && c2 == 0 && c3 == 0 &&
                          g.link_open == 0u && g.tail_n == 0u &&
                          planet_replay(&g, z, 1, 0u) == -3);
        CHECK("T12: shut at birth, opens on trouble, mask=8, closes after 3 clean",
              shut_birth && self_open && masked && self_close);
    }

    printf("═ RESULT: %d pass, %d fail ═\n", pass_count, fail_count);
    return fail_count ? 1 : 0;
}

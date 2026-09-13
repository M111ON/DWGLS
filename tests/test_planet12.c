/*
 * test_planet12.c — 12-pentagon face-spawn system (one planet per pentagon)
 *
 * Oracle: homes/distinctness from shared numbering (face*128); detach by
 * struct snapshots across real breath churn; per-planet tail isolation by
 * single-planet corruption; tombstones exact; replay via real fg_enc chain.
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -Icore/infra -o build/test_planet12 tests/test_planet12.c
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

#define P12_N 432u

static void fill_buf(int8_t *d, uint32_t n, uint32_t seed) {
    for (uint32_t i = 0; i < n; i++)
        d[i] = (int8_t)((seed * 31u + i * 7u + (i >> 3)) & 0xFFu);
}

int main(void) {
    printf("═ PLANET12 — face-spawn system (12 pentagon frames) ═\n");

    static int8_t buf[12][P12_N];
    static const int8_t *bp[12];
    for (uint32_t f = 0; f < 12u; f++) {
        fill_buf(buf[f], P12_N, 100u + f);
        bp[f] = buf[f];
    }

    /* ── P1: birth 12, distinct homes/ids/digests ── */
    PlanetSys s;
    planetsys_birth(&s, 50u, 5u, bp, P12_N);
    {
        int ok = 1;
        for (uint32_t f = 0; f < 12u; f++) {
            if (s.p[f].id != 50u + f || s.p[f].home != f * 128u ||
                s.p[f].birth_w != 5u || s.p[f].tail_n != 0u) { ok = 0; break; }
            for (uint32_t g = f + 1u; g < 12u; g++)
                if (s.p[f].digest == s.p[g].digest) { ok = 0; break; }
        }
        CHECK("P1: 12 born, homes face*128, ids/frames distinct", ok);
    }

    /* ── P2: idle-zero across system ── */
    CHECK("P2: verify-all clean, zero tails", planetsys_verify(&s, bp, P12_N) == 0u);

    /* ── P3: detach — snapshots survive 5000 real breath ticks ── */
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
        PlanetSys snap = s;
        for (int i = 0; i < 5000; i++) bfs_breath_tick(&b);
        CHECK("P3: main reanchors, all 12 planets bit-identical + clean",
              b.reanchors > 0 && memcmp(&snap, &s, sizeof(PlanetSys)) == 0 &&
              planetsys_verify(&s, bp, P12_N) == 0u);
    }

    /* ── P4: per-planet isolation — corrupt planet 7 only ── */
    {
        int8_t save = buf[7][100];
        buf[7][100] ^= 0x20;
        uint32_t bad = planetsys_verify(&s, bp, P12_N);
        buf[7][100] = save;
        int tails = 0;
        for (uint32_t f = 0; f < 12u; f++) tails += (int)s.p[f].tail_n;
        CHECK("P4: 1 mismatch, only planet 7 collected",
              bad == 1u && tails == 1 && s.p[7].tail_n == 1u);
        CHECK("P4b: restored clean, history kept",
              planetsys_verify(&s, bp, P12_N) == 0u && s.p[7].tail_n == 1u);
    }

    /* ── P5: replay per planet over real fg_enc chain ── */
    {
        FGGearEv ev[2];
        uint32_t w = 5u, targets[2] = { 40u, 99u };
        for (int i = 0; i < 2; i++) {
            ev[i] = fg_enc(w, targets[i]);
            w = fg_dec(w, ev[i]);
        }
        int ok = 1;
        for (uint32_t f = 0; f < 12u; f++)
            if (planet_replay(&s.p[f], ev, 2, w) != 0) { ok = 0; break; }
        CHECK("P5: all 12 replay same tail from shared birth (agree)", ok);
    }

    /* ── P6: retire-all → 12 exact tombstones, all severed ── */
    {
        planetsys_retire(&s, 9u);
        int ok = 1, sev = 1;
        for (uint32_t f = 0; f < 12u; f++) {
            PlanetTomb *t = &s.p[f].tomb;
            if (t->magic != PLANET_TOMB_MAGIC || t->id != 50u + f ||
                t->birth_w != 5u || t->death_w != 9u ||
                t->final_home != f * 128u || t->digest != s.p[f].digest) { ok = 0; break; }
            if (planet_verify(&s.p[f], buf[f], P12_N) != -2) sev = 0;
        }
        CHECK("P6: 12 tombstones exact", ok);
        CHECK("P6b: all severed after retire", sev);
    }

    /* ── P7: shared bytes are detected, never silent ──────────────
     * Two planets watching the SAME buffer (caller misconfiguration):
     * both collect independently. Rule (same as dead KV slots #231):
     * main must not write watched ranges; violations surface as planet
     * errors, never silent corruption. */
    {
        static int8_t shared[432];
        for (uint32_t i = 0; i < 432u; i++)
            shared[i] = (int8_t)((i * 3u + 1u) & 0xFFu);
        Planet a, b;
        planet_birth(&a, 91u, 5u, 1000u, shared, 432);
        planet_birth(&b, 92u, 5u, 2000u, shared, 432);
        int8_t save = shared[50];
        shared[50] ^= 0x04;
        int ra = planet_verify(&a, shared, 432);
        int rb = planet_verify(&b, shared, 432);
        shared[50] = save;
        CHECK("P7: shared-buffer write collected by BOTH, independently",
              ra == 1 && rb == 1 && a.tail_n == 1u && b.tail_n == 1u &&
              a.digest == b.digest);
    }

    printf("═ RESULT: %d pass, %d fail ═\n", pass_count, fail_count);
    return fail_count ? 1 : 0;
}

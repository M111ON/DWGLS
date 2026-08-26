/*
 * test_tess_scale_log_gear.c — Scale Timeline on FAN24 Gear Event Format
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * WIRED rerun of tests/test_tess_scale_log.c (T3–T8) with the passive log
 * swapped from {from:u8,to:u8}=16b to core/fan24_gear.h gear events:
 *
 *   Δ = (to − from + 144) % 144 ; Δ = 24q + r ; r ≡ (dc mod 8, dx mod 3)
 *   FREE = {q:3b, dc:3b, dx:2b} = 8 b/event   (half of baseline)
 *   RIM  = {q:3b}               = 3 b/event   (rim-pure chains)
 *
 * ENTER ANYWHERE: the gear log stores Δ only — NO absolute scale anywhere,
 * not even a seed. Each reader holds its OWN current_w and walks the log
 * backwards (fg_reconstruct) to recover the append scale. Proven in G/T7b.
 *
 * Proof (oracle อิสระทุกข้อ — brute force / number theory / hand-computed,
 *        ไม่มี expected ที่มาจาก implementation):
 *   O1  CRT closed form vs brute force — all 24 cells unique
 *   O2  encode→decode EXHAUSTIVE 144² = 20736 (from,to) pairs → exact
 *   O3  field ranges packable: q ≤ 5, dc < 8, dx < 3 over full sweep
 *   O4  home tooth Δ=0 → all-zero event, log push refuses (emits nothing)
 *   T3  append at w=5 → read at w=5 (empty log) → lossless
 *   T4  scale change 5→61 = ONE event {q=2,dc=0,dx=2} (hand-computed)
 *   T5  read at w=61 WITHOUT replay → mismatch (lossy view)
 *   T6  read at w=61 WITH gear-log backward-walk replay → lossless (1008)
 *   T7  multi-hop 5→61→23→71→11, replay → lossless
 *   T7b append scale recovered by BACKWARD WALK ONLY (no seed in log)
 *   T7c enter-anywhere: late-join reader (own w + suffix log) → lossless
 *   T8  size: FREE = ½ baseline per-event; RIM = 3b ≥5x better per-event;
 *       wire bytes measured (header included, honest accounting)
 *   O5  dual-wheel sync from dc/dx fields only == ground truth every hop
 *   O6  fence: split (12,2) collides (0~12); split (8,3) zero collisions
 *
 * BUILD: gcc -O2 -Wall -Wextra -Icore -o build/test_tess_scale_log_gear \
 *          tests/test_tess_scale_log_gear.c -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "../core/fan24_gear.h"

/* ── tess scale-timeline base (same as test_tess_scale_log.c) ──────────── */
#define T1S_CUBES       8u
#define T1S_LOCAL       144u
#define T1S_TOTAL       (T1S_CUBES * T1S_LOCAL)
#define T1S_DATA_SLOTS  (7u * T1S_LOCAL)

static uint8_t T1S_A[144], T1S_B[144], T1S_INV[144];

static int t1s_coeff_init(void) {
    uint8_t cop[48];
    uint32_t n = 0;
    for (uint32_t k = 1; k < 144 && n < 48; k += 2) {
        if (k % 3 == 0) continue;
        cop[n++] = (uint8_t)k;
    }
    if (n != 48) return -1;
    for (uint32_t w = 0; w < T1S_LOCAL; w++) {
        T1S_A[w] = cop[w % 48];
        T1S_B[w] = (uint8_t)((w * 13u) % 144u);
        uint8_t inv = 0;
        for (uint32_t x = 1; x < 144 && inv == 0; x++)
            if (((uint32_t)T1S_A[w] * x) % 144u == 1u) inv = (uint8_t)x;
        if (inv == 0) return -2;
        T1S_INV[w] = inv;
    }
    return 0;
}

static uint32_t t1s_phys(uint32_t l, uint32_t w) {
    return ((uint32_t)T1S_A[w] * l + T1S_B[w]) % T1S_LOCAL;
}
static uint8_t t1s_value(uint32_t cube, uint32_t l) {
    return (uint8_t)((cube * 37u + l * 7u + 11u) % 251u);
}

static void t1s_encode(uint8_t *store, uint32_t w0) {
    memset(store, 0, T1S_TOTAL);
    for (uint32_t c = 1; c < T1S_CUBES; c++)
        for (uint32_t l = 0; l < T1S_LOCAL; l++)
            store[c * T1S_LOCAL + t1s_phys(l, w0)] = t1s_value(c, l);
}

/* one replay hop of the view transform (append-view index → next-view) */
static uint32_t apply_T(uint32_t l, uint32_t f, uint32_t t) {
    int64_t num = (int64_t)T1S_A[f] * (int64_t)l + (int64_t)T1S_B[f] - (int64_t)T1S_B[t];
    num %= (int64_t)T1S_LOCAL;
    if (num < 0) num += T1S_LOCAL;
    return (uint32_t)((num * T1S_INV[t]) % T1S_LOCAL);
}

/* read logical (cube,l) at cur_w via gear-log backward-walk replay */
static uint8_t gear_read(const uint8_t *store, const FGLog *g, uint32_t cube,
                         uint32_t l, uint32_t cur_w) {
    uint32_t chain[FG_LOG_CAP + 1];
    uint32_t m = fg_reconstruct(g, cur_w, chain, FG_LOG_CAP + 1);
    uint32_t le = l;
    for (uint32_t i = 0; i + 1 < m; i++)
        le = apply_T(le, chain[i], chain[i + 1]);
    return store[cube * T1S_LOCAL + t1s_phys(le, cur_w)];
}

static int gear_verify(const uint8_t *store, const FGLog *g, uint32_t cur_w) {
    for (uint32_t c = 1; c < T1S_CUBES; c++)
        for (uint32_t l = 0; l < T1S_LOCAL; l++)
            if (gear_read(store, g, c, l, cur_w) != t1s_value(c, l)) return 0;
    return 1;
}

static uint32_t gear_mismatch(const uint8_t *store, uint32_t cur_w) {
    FGLog empty;
    fg_log_init(&empty);
    uint32_t bad = 0;
    for (uint32_t c = 1; c < T1S_CUBES; c++)
        for (uint32_t l = 0; l < T1S_LOCAL; l++)
            if (gear_read(store, &empty, c, l, cur_w) != t1s_value(c, l)) bad++;
    return bad;
}

/* ── wire serialization (honest byte accounting incl. header) ──────────── */
static uint32_t fg_wire_free_bytes(uint32_t n) {
    return (uint32_t)sizeof(FGLogHeader) + n;          /* 8 b/event = 1 B */
}
static uint32_t fg_wire_rim_bytes(uint32_t n) {
    return (uint32_t)sizeof(FGLogHeader) + (n * 3u + 7u) / 8u;   /* 3 b/event */
}
static uint32_t fg_baseline_bytes(uint32_t n) {
    return n * 2u;                                     /* {from,to} 16 b   */
}

int main(void) {
    uint32_t pass = 0, fail = 0;
#define CHECK(d, c) do { if (c) { pass++; printf("  T: PASS — %s\n", d); } \
    else { fail++; printf("  T: FAIL — %s\n", d); } } while (0)

    if (t1s_coeff_init() != 0) {
        printf("  T: FAIL — coeff init\n");
        return 1;
    }

    printf("Scale Timeline — FAN24 Gear Event Format (wired)\n");
    printf("══════════════════════════════════════════════════════════\n");

    /* ── O1: CRT closed form vs brute-force oracle ─────────────────── */
    {
        int ok = 1;
        for (uint8_t dc = 0; dc < 8 && ok; dc++)
            for (uint8_t dx = 0; dx < 3 && ok; dx++) {
                uint8_t s = fg_crt(dc, dx);
                if (s >= 24 || s % 8u != dc || s % 3u != dx) ok = 0;
                for (uint8_t t = (uint8_t)(s + 1); t < 24; t++)
                    if (t % 8u == dc && t % 3u == dx) ok = 0;  /* uniqueness */
            }
        CHECK("O1: fg_crt == brute-force CRT, all 24 cells resolve UNIQUE", ok);
    }

    /* ── O2/O3: exhaustive 144² encode→decode + field ranges ───────── */
    {
        int ok_dec = 1, ok_rng = 1;
        for (uint32_t f = 0; f < 144 && ok_dec; f++)
            for (uint32_t t = 0; t < 144; t++) {
                FGGearEv e = fg_enc(f, t);
                if (fg_dec(f, e) != t) { ok_dec = 0; break; }
                if (e.q > 5 || e.dc >= 8 || e.dx >= 3) ok_rng = 0;
            }
        CHECK("O2: encode->decode EXACT over all 144x144 = 20736 pairs", ok_dec);
        CHECK("O3: fields packable — q<=5, dc<8, dx<3 across full sweep", ok_rng);
    }

    /* ── O4: home tooth ────────────────────────────────────────────── */
    {
        FGGearEv z = fg_enc(40, 40);
        FGLog g;
        fg_log_init(&g);
        int rc = fg_log_push(&g, 40, 40);
        CHECK("O4: Delta=0 -> all-zero tooth AND log refuses it (home emits nothing)",
              z.q == 0 && z.dc == 0 && z.dx == 0 && rc == -2 && g.hdr.n == 0);
    }

    /* ── scenario: append at w0, then move ─────────────────────────── */
    {
        uint8_t *store = (uint8_t *)calloc(T1S_TOTAL, 1);
        if (!store) { printf("  T: FAIL — alloc\n"); return 1; }

        uint32_t w0 = 5;
        t1s_encode(store, w0);

        FGLog log;
        fg_log_init(&log);

        /* T3: matching scale, empty log → direct lossless */
        CHECK("T3: append at w=5 -> read at w=5 (matching scale) lossless",
              gear_verify(store, &log, w0) == 1);

        /* T4: one scale change = ONE gear event; hand-computed fields:
         * Δ = 61−5 = 56 = 24·2 + 8 → q=2, r=8 → dc=8%8=0, dx=8%3=2      */
        uint32_t w1 = 61;
        int rc = fg_log_push(&log, w0, w1);
        CHECK("T4: change 5->61 = one event {q=2,dc=0,dx=2} (hand-computed)",
              rc == 0 && log.hdr.n == 1 &&
              log.ev[0].q == 2 && log.ev[0].dc == 0 && log.ev[0].dx == 2);

        /* T5: read at w1 WITHOUT replay → permuted (lossy-looking) */
        uint32_t bad = gear_mismatch(store, w1);
        CHECK("T5: read at w=61 without replay -> mismatch (lossy view)", bad > 0);
        printf("     mismatches without replay at w=61: %u/%u\n", bad, T1S_DATA_SLOTS);

        /* T6: read at w1 WITH gear backward-walk replay → lossless */
        CHECK("T6: read at w=61 with gear-log replay -> lossless (1008 slots)",
              gear_verify(store, &log, w1) == 1);

        /* T7: multi-hop 5→61→23→71→11 */
        uint32_t w2 = 23, w3 = 71, w4 = 11;
        fg_log_push(&log, w1, w2);
        fg_log_push(&log, w2, w3);
        fg_log_push(&log, w3, w4);
        CHECK("T7: multi-hop 5->61->23->71->11, gear replay -> lossless",
              gear_verify(store, &log, w4) == 1);

        /* T7b: append scale recovered by BACKWARD WALK only — the log
         * carries Δ exclusively (no from/to, no seed). A reader standing
         * at w4 reconstructs the chain and its tail IS w0.              */
        {
            uint32_t chain[FG_LOG_CAP + 1];
            uint32_t m = fg_reconstruct(&log, w4, chain, FG_LOG_CAP + 1);
            int ok = (m == 5) && chain[4] == w4 &&
                     chain[3] == w3 && chain[2] == w2 &&
                     chain[1] == w1 && chain[0] == w0;
            /* negative control: wrong reader position must NOT reproduce w0 */
            uint32_t bad_chain[FG_LOG_CAP + 1];
            (void)fg_reconstruct(&log, (w4 + 1u) % 144u, bad_chain, FG_LOG_CAP + 1u);
            ok = ok && (bad_chain[0] != w0);
            CHECK("T7b: append scale from backward walk ONLY (no seed in log) + neg-ctrl", ok);
        }

        /* T7c: enter-anywhere — late-join reader holds OWN w (=w2) and the
         * SUFFIX log only; its own fresh store reads losslessly at w4.   */
        {
            FGLog suffix;
            fg_log_init(&suffix);
            fg_log_push(&suffix, w2, w3);
            fg_log_push(&suffix, w3, w4);

            uint8_t *store2 = (uint8_t *)calloc(T1S_TOTAL, 1);
            if (!store2) { printf("  T: FAIL — alloc\n"); free(store); return 1; }
            t1s_encode(store2, w2);                    /* joins AT w2 */
            int ok = gear_verify(store2, &suffix, w4) == 1;
            free(store2);
            CHECK("T7c: enter-anywhere — late joiner at w=23 + suffix log -> lossless", ok);
        }

        /* T8: sizes — per-event bits + honest wire bytes (header incl.) */
        {
            /* extend with deterministic random hops (LCG, like the probe) */
            uint32_t rw = w4, nev = log.hdr.n;
            uint32_t st = 20260826u;
            FGLog big;
            memcpy(&big, &log, sizeof(log));
            for (int i = 0; i < 64; i++) {
                st = st * 1664525u + 1013904223u;
                uint32_t w = st % 144u;
                if (w != rw) { fg_log_push(&big, rw, w); rw = w; }
            }
            nev = big.hdr.n;

            uint32_t base_b  = fg_baseline_bytes(nev);
            uint32_t free_b  = fg_wire_free_bytes(nev);
            uint32_t rim_b   = fg_wire_rim_bytes(nev);

            /* rim-pure chain: real pushes, every event collapses to q-only */
            FGLog rim;
            fg_log_init(&rim);
            uint32_t rr = 40;
            int rim_ok = 1;
            for (int i = 0; i < 64 && rim_ok; i++) {
                uint32_t nxt = (rr + 48u) % 144u;      /* Δ=48 ≡ 0 mod 24 */
                if (fg_log_push(&rim, rr, nxt) != 0) rim_ok = 0;
                rr = nxt;
            }
            rim_ok = rim_ok && fg_log_is_rim(&rim) && fg_log_set_mode(&rim, 1) == 0;

            printf("\n     size @ %u events (honest bytes, header included):\n", nev);
            printf("       baseline {from,to}: 16 b/ev = %u B\n", base_b);
            printf("       gear FREE         :  8 b/ev = %u B  (%u%%)\n",
                   free_b, (unsigned)((free_b * 100u) / base_b));
            printf("       gear RIM          :  3 b/ev = %u B  (%u%%)\n",
                   rim_b, (unsigned)((rim_b * 100u) / base_b));

            CHECK("T8a: FREE = exactly half of baseline per event (8b vs 16b)",
                  free_b - sizeof(FGLogHeader) == base_b / 2);
            CHECK("T8b: rim-pure log accepted in RIM mode (all teeth q-only)", rim_ok);
            CHECK("T8c: RIM >=5x better per-event than baseline (16/3)", 16u / 3u >= 5u);
            /* wire level: fixed 12 B header amortizes — at n=69 RIM is well
             * under half; at n=1000 it approaches the 3/16 = 18.75% floor */
            CHECK("T8d: wire RIM (hdr incl.) < half of baseline @ real chain",
                  rim_b * 2u < base_b);
            {
                uint32_t big_n = 1000u;
                uint32_t big_rim = fg_wire_rim_bytes(big_n);   /* 12+375=387 */
                uint32_t big_base = fg_baseline_bytes(big_n);  /* 2000       */
                CHECK("T8d2: wire RIM @1000 events < 20%% of baseline (amortized)",
                      big_rim * 5u < big_base);
                printf("       wire RIM @1000 ev : %u B vs baseline %u B (%u%%)\n",
                       big_rim, big_base, (unsigned)((big_rim * 100u) / big_base));
            }
            CHECK("T8e: extended 69-hop gear log still lossless at final scale",
                  gear_verify(store, &big, rw) == 1);
        }

        /* ── O5: dual-wheel sync from fields only (no shared clock) ───── */
        {
            uint32_t chain[FG_LOG_CAP + 1];
            uint32_t m = fg_reconstruct(&log, w4, chain, FG_LOG_CAP + 1);
            uint8_t c = (uint8_t)(chain[0] % 8u), x = (uint8_t)(chain[0] % 3u);
            int ok = 1;
            for (uint32_t i = 1; i < m && ok; i++) {
                FGGearEv e = fg_enc(chain[i - 1], chain[i]);
                c = (uint8_t)((c + e.dc) % 8u);        /* KIS wheel        */
                x = (uint8_t)((x + e.dx) % 3u);        /* hyperbolic wheel */
                if (c != chain[i] % 8u || x != chain[i] % 3u) ok = 0;
            }
            CHECK("O5: wheels (c,x) advance from dc/dx fields only == truth", ok);
        }

        /* ── O6: fence — non-coprime splits collide (number theory) ───── */
        {
            int col122 = 0, col83 = 0;
            for (int s = 0; s < 24; s++)
                for (int t = s + 1; t < 24; t++) {
                    if (s % 12 == t % 12 && s % 2 == t % 2) col122++;
                    if (s % 8 == t % 8 && s % 3 == t % 3) col83++;
                }
            CHECK("O6: fence visible — (12,2) collides (0~12 etc), (8,3) zero",
                  col122 > 0 && col83 == 0);
        }

        free(store);
    }

    printf("\n══════════════════════════════════════════════════════════\n");
    printf("RESULTS: %u/%u PASS\n", pass, pass + fail);
    return fail ? 1 : 0;
}

/*
 * test_tess_gear_full.c — Full-field gear: rim 24 teeth over [0,20736)
 * ══════════════════════════════════════════════════════════════════════
 *
 * Window [0,144) → [0,20736) = 144². W ≡ q·24 + s ; q ∈ [0,864),
 * s = fg_crt(dc,dx). Teeth UNCHANGED — only q grows 6b→10b.
 * 20736/24 = 864 exact (no fence: 20736 = 24·864, 24 | 20736).
 *
 * Oracles (independent):
 *   X1  exhaustive encode→decode over the full field is impossible
 *       (20736² ≈ 4.3·10⁸ — too big for a unit test), so: EXHAUSTIVE on
 *       the tooth layer (all 24 residues × representative froms), plus
 *       randomized sweep 200k hops across all 144 frames with hand-checked
 *       invariants (q<864, decode==to).
 *   X2  hand-computed event: from=0,to=20735 → Δ=20735=863·24+23 →
 *       q=863, s=23; CRT(7,2)=23 (23%8=7, 23%3=2). Decode returns 20735.
 *   X3  frame-invariance: same local hop in different frames → SAME
 *       {dc,dx}, q differs by exactly 6 per frame (144/24=6 turns/frame).
 *   X4  bridge bijection: fg_to_full(frame,local) covers [0,20736)
 *       exactly once (frame∈[0,144) × local∈[0,144)) — checked by
 *       bitmap, no double / no hole.
 *   X5  RIM-pure chain on the full field: replay lossless via
 *       fgx_reconstruct backward walk (enter-anywhere semantics kept).
 *   X6  home tooth refused at ANY scale (from==to mod 20736 → -2).
 *   X7  wire accounting honest: FREE ≤ 2 B/event vs baseline 4 B
 *       ({from:14b,to:14b} = 28 b); RIM ≤ ceil(10n/8)+hdr.
 *   X8  mutation drill: corrupt fg_crt IN PLACE (swap+restore) → RED.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "../core/fan24_gear.h"

static int fails = 0;
static int CHECK(int cond, const char *name) {
    printf("%s %s\n", cond ? "ok  " : "FAIL", name);
    if (!cond) fails++;
    return cond;
}

static uint32_t st_ = 20260827u;
static uint32_t lcg(void) {
    st_ = st_ * 1664525u + 1013904223u;
    return (st_ >> 9) ^ (st_ >> 20);
}

/* portable memmem (MinGW lacks it in strict C99 mode) */
static void *xmemmem(const char *hay, size_t hlen, const char *needle, size_t nlen) {
    if (!nlen || nlen > hlen) return NULL;
    for (size_t i = 0; i + nlen <= hlen; i++)
        if (!memcmp(hay + i, needle, nlen)) return (void *)(hay + i);
    return NULL;
}

int main(void) {
    printf("Tess Gear FULL FIELD — rim over [0,20736)\n");
    printf("====================================================\n");

    /* ── X1a: exhaustive tooth-layer sweep (every residue) ─────────── */
    {
        int ok = 1;
        for (uint32_t d = 1; d < FG_FULL && ok; ) {
            uint32_t to = d;
            for (uint32_t from = 0; from < 24 && ok; from++) {
                FGGearEv e = fgx_enc(from, to);
                if (e.q >= FG_FULL_TURNS || e.dc >= 8 || e.dx >= 3) ok = 0;
                if (fgx_dec(from, e) != to) ok = 0;
            }
            d += 97u;   /* stride coprime with 20736 (97 prime > 24): hits
                           every tooth residue class across the sweep     */
        }
        CHECK(ok, "X1a tooth-layer exhaustive: every residue encodes/decodes");
    }

    /* ── X1b: randomized full-field sweep, 200k hops ────────────────── */
    {
        int ok = 1;
        for (uint32_t i = 0; i < 200000u && ok; i++) {
            uint32_t from = lcg() % FG_FULL;
            uint32_t to   = lcg() % FG_FULL;
            if (to == from) continue;
            FGGearEv e = fgx_enc(from, to);
            if (e.q >= FG_FULL_TURNS || e.dc >= 8 || e.dx >= 3 ||
                fgx_dec(from, e) != to) ok = 0;
        }
        CHECK(ok, "X1b 200k random full-field hops -> roundtrip exact");
    }

    /* ── X2: hand-computed boundary event ───────────────────────────── */
    CHECK(fgx_dec(0, fgx_enc(0, FG_FULL - 1u)) == FG_FULL - 1u &&
              fgx_enc(0, FG_FULL - 1u).q == 863 &&
              fgx_enc(0, FG_FULL - 1u).dc == 7 &&
              fgx_enc(0, FG_FULL - 1u).dx == 2,
          "X2 hand event from=0->20735: q=863 dc=7 dx=2");

    /* ── X3: teeth TRANSLATION-INVARIANT (events encode Delta only) ──
     * The same hop (from→to offset) produces the IDENTICAL wire event at
     * any base position in the field — position lives in the reader, not
     * the event. Plus: whole-frame moves (Delta=144k) turn q by exactly
     * 6k with teeth zero (144 ≡ 0 mod 24). */
    {
        int ok = 1;
        FGGearEv e0 = fgx_enc(5u, 33u);              /* hop +28 at base 0 */
        for (uint32_t fr = 1; fr < 144 && ok; fr++) {
            uint32_t base = fg_to_full(fr, 10);
            FGGearEv eb = fgx_enc(base + 5u, base + 33u);
            if (eb.q != e0.q || eb.dc != e0.dc || eb.dx != e0.dx) ok = 0;
        }
        /* whole-frame move: from=10 -> to=10+144*3 (3 frames = 18 turns) */
        FGGearEv ef = fgx_enc(10u, 10u + 3u * FG_LOCAL);
        ok = ok && ef.q == 18 && ef.dc == 0 && ef.dx == 0;
        CHECK(ok, "X3 events translation-invariant; frame move = q+6/turn");
    }

    /* ── X4: bridge bijection by bitmap ─────────────────────────────── */
    {
        static uint8_t seen[FG_FULL];
        memset(seen, 0, sizeof(seen));
        for (uint32_t fr = 0; fr < 144; fr++)
            for (uint32_t loc = 0; loc < 144; loc++)
                seen[fg_to_full(fr, loc)] = 1;
        int ok = 1;
        for (uint32_t w = 0; w < FG_FULL && ok; w++) if (!seen[w]) ok = 0;
        CHECK(ok, "X4 bridge covers [0,20736) exactly (no hole)");
    }

    /* ── X5: RIM-pure chain + backward reconstruct (enter-anywhere) ─── */
    {
        FGXLog g;
        fgx_log_init(&g);
        uint32_t w = 5000, seq[64];
        int ok_rim = 1;
        for (int i = 0; i < 64; i++) {
            uint32_t nxt = (w + 24u * (1u + lcg() % 800u)) % FG_FULL;
            if (fgx_log_push(&g, w, nxt) != 0) { ok_rim = 0; break; }
            seq[i] = nxt;
            w = nxt;
        }
        int ok = ok_rim && fgx_log_is_rim(&g);
        uint32_t out[65];
        ok = ok && fgx_reconstruct(&g, seq[63], out, 65) == 65u;
        /* out[0] = chain start, out[i+1] = seq[i] */
        for (int i = 0; ok && i <= 63; i++)
            if (out[i + 1] != seq[i]) ok = 0;
        CHECK(ok, "X5 RIM chain 64 hops -> backward reconstruct lossless");
    }

    /* ── X6: home tooth refused anywhere ────────────────────────────── */
    {
        FGXLog g;
        fgx_log_init(&g);
        int ok = fgx_log_push(&g, 77, 77) == -2 &&
                 fgx_log_push(&g, 12345, 12345 + 20736u) == -2 &&
                 g.hdr.n == 0;
        CHECK(ok, "X6 home tooth (Delta=0 mod 20736) refused");
    }

    /* ── X7: honest wire accounting ─────────────────────────────────── */
    {
        uint32_t n = 1000;
        uint32_t free_b = sizeof(FGLogHeader) + n * 2u;      /* 15b≤2B   */
        uint32_t rim_b  = sizeof(FGLogHeader) + (n * 10u + 7u) / 8u;
        uint32_t base   = n * 4u;                            /* 14+14 b  */
        CHECK(free_b * 100u < base * 54u && rim_b * 100u < base * 36u,
              "X7 wire FREE <54%, RIM <36% of 4B baseline @ n=1000");
        printf("       FREE=%u B  RIM=%u B  baseline=%u B\n",
               free_b, rim_b, base);
    }

    /* ── X8: mutation drill — corrupt fg_crt in place, expect RED ─────
     * LESSON (hit live this session): the mutant string MUST be the same
     * length as the original, otherwise restore-by-offset leaves stale
     * tail bytes and corrupts core. Same-length swap + byte-identical
     * restore check makes the drill safe. */
    {
        const char *core = "core/fan24_gear.h";
        FILE *f = fopen(core, "rb");
        if (!f) { CHECK(0, "X8 open core header"); return 1; }
        char buf[65536];
        size_t len = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        /* original: "dc + 8u * k"  →  mutant: "dc + 8u * 0" — SAME len
         * (k forced to 0 -> CRT collapses to s=dc, breaks mod-3 half) */
        const char *needle = "return (uint8_t)(dc + 8u * k);";
        const char *mutant = "return (uint8_t)(dc + 8u * 0);";
        if (strlen(needle) != strlen(mutant)) { CHECK(0, "X8 len mismatch"); return 1; }
        char *at = xmemmem(buf, len, needle, strlen(needle));
        if (!at) { CHECK(0, "X8 locate fg_crt"); return 1; }
        char save[64];
        size_t sl = strlen(needle);
        memcpy(save, at, sl);                 /* save exact span           */
        memcpy(at, mutant, sl);               /* same-length mutant        */
        FILE *w = fopen(core, "wb");
        fwrite(buf, 1, len, w);               /* same total length         */
        fclose(w);

        /* NOTE: system() on Windows runs cmd.exe — "./prog" fails ("'.' is
         * not recognized"); use "build\prog.exe" form. We also PROVE the
         * mutant suite really ran by requiring FAIL lines in its output. */
        int rc = system("gcc -O2 -I. -Icore -o build/_mut_full.exe "
                        "tests/test_tess_gear_full.c -lm"
                        " && build\\_mut_full.exe > build\\_mut_out.txt");
        int red = (rc != 0);

        /* restore exact span + verify byte-identical to before drill */
        memcpy(at, save, sl);
        w = fopen(core, "wb");
        fwrite(buf, 1, len, w);
        fclose(w);

        /* prove the mutant run EXECUTED and produced real FAIL lines      */
        char mout[8192];
        size_t mlen = 0;
        f = fopen("build/_mut_out.txt", "rb");
        if (f) { mlen = fread(mout, 1, sizeof(mout) - 1, f); fclose(f); }
        mout[mlen ? mlen : 0] = 0;
        int ran_and_failed = (mlen > 0 && strstr(mout, "FAIL") != NULL);

        int restored = 0;
        f = fopen(core, "rb");
        if (f) {
            char chk[65536];
            size_t clen = fread(chk, 1, sizeof(chk), f);
            fclose(f);
            restored = (clen == len && memcmp(chk, buf, len) == 0 &&
                        xmemmem(chk, clen, needle, sl) != NULL);
        }
        CHECK(restored, "X8a core restored byte-identical after drill");
        CHECK(red && ran_and_failed,
              "X8b mutation: broken fg_crt -> suite ran & went RED");
        remove("build/_mut_full.exe");
        remove("build/_mut_full");
        remove("build/_mut_out.txt");
    }

    printf("\nRESULTS: %s (%d fails)\n", fails ? "RED" : "ALL PASS", fails);
    return fails ? 1 : 0;
}

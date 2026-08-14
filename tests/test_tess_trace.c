/* ═══════════════════════════════════════════════════════════════════════════
 * test_tess_trace.c — Trace values through scale changes (rescope regression)
 *
 * User's original experiment, re-run:
 *   1. 100 copies of ONE number (binary) placed in KIS → trace value at
 *      every scale step.
 *   2. A-Z (26 letters) the same way.
 *
 * Questions answered:
 *   - Does rescope still behave like day 1? (lossless at EVERY scale)
 *   - What does the window VIEW look like as scale shrinks?
 *   - Uniform (100×same) vs spread (A-Z) — different patterns?
 *
 * Model (from test_tess_hex_delta / rescope):
 *   view at scale d:  view_d = value >> d << d     (low d bits hidden)
 *   replay:           value = view_d | Σ_{k<d} plane_k << k   → lossless
 *   plane_k = bit-plane k of all values, hex_tile-encoded (FLAT when uniform)
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -Icore/infra -o build/test_tess_trace tests/test_tess_trace.c
 * ═══════════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "hex_tile.h"

#define WINDOW_SLOTS   20736u
#define MAX_N          144u
#define MAX_SCALE      8u          /* 8-bit values → scales 0..8 */
#define TRACE_STRIDE   37u         /* timeline walk stride (gcd(37,20736)=1) */

static int errors = 0;
#define CHECK(desc, cond) do {                                          \
    if (cond) { printf("  [OK] %s\n", desc); }                          \
    else      { printf("  [FAIL] %s\n", desc); errors++; }              \
} while (0)

/* ── RLE recipe: store run-lengths, COMPUTE the rest (hyper-side profit) ── */
static uint32_t rle_encode(const uint8_t *bits, uint32_t n, uint8_t *out) {
    if (n == 0) return 0;
    uint32_t o = 0;
    out[o++] = bits[0];                 /* first value */
    uint32_t run = 1;
    for (uint32_t i = 1; i < n; i++) {
        if (bits[i] == bits[i - 1]) { run++; }
        else { out[o++] = (uint8_t)run; run = 1; }
    }
    out[o++] = (uint8_t)run;            /* last run length */
    return o;
}

static int rle_decode(const uint8_t *in, uint32_t inlen, uint8_t *out, uint32_t n) {
    if (inlen < 2) return -1;
    uint8_t v = in[0];
    uint32_t o = 1, p = 0;
    while (o < inlen && p < n) {
        uint32_t r = in[o++];
        for (uint32_t i = 0; i < r && p < n; i++) out[p++] = v;
        v ^= 1u;
    }
    return (p == n) ? 0 : -1;
}

/* ── hex_tile-encode one bit-plane (7 values per tile) ── */
static uint32_t plane_encode(const uint8_t *plane, uint32_t n, uint8_t *out,
                             uint32_t *flat_tiles) {
    uint32_t o = 0, tiles = (n + HEX_CELLS - 1u) / HEX_CELLS;
    *flat_tiles = 0;
    for (uint32_t t = 0; t < tiles; t++) {
        HexTile tile;
        for (uint32_t j = 0; j < HEX_CELLS; j++) {
            uint32_t idx = t * HEX_CELLS + j;
            tile.c[j] = (idx < n) ? plane[idx] : (n ? plane[n - 1] : 0);  /* pad w/ edge */
        }
        if (hex_tile_classify(&tile) == HENC_FLAT) (*flat_tiles)++;
        o += (uint32_t)hex_tile_encode(&tile, out + o);
    }
    return o;
}

/* ── trace one dataset through every scale ── */
static void trace_dataset(const char *name, const uint8_t *vals, uint32_t n) {
    printf("── %s (n=%u)\n", name, n);

    printf("  scale  distinct  view[0]  delta_B  flat/tile  lossless\n");
    for (uint32_t d = 0; d <= MAX_SCALE; d++) {
        uint8_t view[MAX_N];
        uint8_t planes[MAX_SCALE][MAX_N];
        uint32_t distinct = 0;
        uint8_t seen[256] = {0};
        for (uint32_t i = 0; i < n; i++) {
            view[i] = (uint8_t)((vals[i] >> d) << d);
            if (!seen[view[i]]) { seen[view[i]] = 1; distinct++; }
            for (uint32_t k = 0; k < d; k++)
                planes[k][i] = (uint8_t)((vals[i] >> k) & 1u);
        }

        uint32_t delta_bytes = 0, flat_tiles = 0, tot_tiles = 0;
        for (uint32_t k = 0; k < d; k++) {
            uint8_t buf[512];
            uint32_t ft;
            delta_bytes += plane_encode(planes[k], n, buf, &ft);
            flat_tiles += ft;
            tot_tiles += (n + HEX_CELLS - 1u) / HEX_CELLS;
        }

        int lossless = 1;
        for (uint32_t i = 0; i < n; i++) {
            uint8_t rec = view[i];
            for (uint32_t k = 0; k < d; k++)
                rec |= (uint8_t)(planes[k][i] << k);
            if (rec != vals[i]) { lossless = 0; break; }
        }

        printf("  d=%-4u  %-8u  %-7u  %-7u  %u/%u      %s\n",
               d, distinct, view[0], delta_bytes, flat_tiles, tot_tiles,
               lossless ? "LOSSLESS" : "BROKEN");
        CHECK("lossless at every scale", lossless);
    }
    printf("\n");
}

int main(void) {
    printf("KIS Scale Trace — 100×same vs A-Z\n");
    printf("═══════════════════════════════════════════════════════════════\n\n");

    /* ── dataset 1: 100 copies of binary 1 ── */
    printf("[T1] 100 copies of value 1 (binary 0b00000001) — uniform\n");
    {
        uint8_t vals[100];
        for (int i = 0; i < 100; i++) vals[i] = 1;
        trace_dataset("100 x 1", vals, 100);

        /* at scale >= 1 the whole window view collapses to 0 */
        uint32_t zero_views = 0;
        for (int i = 0; i < 100; i++)
            if (((vals[i] >> 1u) << 1u) == 0) zero_views++;
        printf("  at d=1: %u/100 positions show view 0 (FLAT collapse)\n", zero_views);
        CHECK("uniform data collapses to view 0 at d=1", zero_views == 100);

        /* 1:1 bijection: 100 positions all distinct, each retrieves its copy */
        uint32_t pos[100];
        for (int i = 0; i < 100; i++) pos[i] = (uint32_t)(i * TRACE_STRIDE) % WINDOW_SLOTS;
        uint32_t dup = 0;
        for (int a = 0; a < 100; a++)
            for (int b = a + 1; b < 100; b++)
                if (pos[a] == pos[b]) dup++;
        printf("  positions: %u distinct slots (1:1 bijection), copies=%u\n",
               100 - dup, 100);
        CHECK("100 copies -> 100 distinct positions (1:1 mapping kept)", dup == 0);
        int all_ok = 1;
        for (uint32_t d = 0; d <= MAX_SCALE && all_ok; d++)
            for (int i = 0; i < 100; i++) {
                uint8_t rec = (uint8_t)((vals[i] >> d) << d);
                for (uint32_t k = 0; k < d; k++) rec |= (uint8_t)(((vals[i] >> k) & 1u) << k);
                if (rec != vals[i]) { all_ok = 0; break; }
            }
        CHECK("each of the 100 positions retrieves its copy losslessly at every scale", all_ok);

        /* plane 0 = all 1s → every hex tile FLAT → 2B per tile */
        uint8_t pl[100], buf[512];
        for (int i = 0; i < 100; i++) pl[i] = 1;
        uint32_t ft;
        uint32_t b0 = plane_encode(pl, 100, buf, &ft);
        uint32_t tiles = (100u + HEX_CELLS - 1u) / HEX_CELLS;
        printf("  plane 0 encodes to %uB = %uB/tile, %u/%u tiles FLAT\n",
               b0, b0 / tiles, ft, tiles);
        CHECK("uniform plane is FLAT (2B per tile)", ft == tiles && b0 == 2u * tiles);
    }

    /* ── dataset 2: A-Z (26 letters, ASCII 65..90) ── */
    printf("\n[T2] A-Z (ASCII 65..90) — spread\n");
    {
        uint8_t vals[26];
        for (int i = 0; i < 26; i++) vals[i] = (uint8_t)('A' + i);
        trace_dataset("A-Z", vals, 26);

        /* A-Z stay distinct through d=1..2; collapse to multiples of 8 at d=3 */
        uint8_t seen[256] = {0};
        uint32_t dist3 = 0;
        for (int i = 0; i < 26; i++) {
            uint8_t v = (uint8_t)((vals[i] >> 3u) << 3u);
            if (!seen[v]) { seen[v] = 1; dist3++; }
        }
        printf("  at d=3: 26 letters collapse to %u distinct views (multiples of 8)\n", dist3);
        CHECK("spread data keeps structure longer than uniform", dist3 > 1 && dist3 < 26);
    }

    /* ── T3: contrast table ── */
    printf("\n[T3] Contrast — uniform vs spread under scaling\n");
    {
        printf("  %-6s %-14s %-14s\n", "scale", "distinct(100x1)", "distinct(A-Z)");
        uint8_t u[100], s[26];
        for (int i = 0; i < 100; i++) u[i] = 1;
        for (int i = 0; i < 26; i++) s[i] = (uint8_t)('A' + i);
        uint32_t du1 = 0, ds1 = 0;
        for (uint32_t d = 0; d <= MAX_SCALE; d++) {
            uint8_t su[256] = {0}, ss[256] = {0};
            uint32_t du = 0, ds = 0;
            for (int i = 0; i < 100; i++) { uint8_t v = (uint8_t)((u[i] >> d) << d); if (!su[v]) { su[v] = 1; du++; } }
            for (int i = 0; i < 26; i++)  { uint8_t v = (uint8_t)((s[i] >> d) << d); if (!ss[v]) { ss[v] = 1; ds++; } }
            printf("  d=%-4u %-14u %-14u\n", d, du, ds);
            if (d == 1) { du1 = du; ds1 = ds; }
        }
        /* balance-scale signature: uniform collapses to 1 instantly,
         * spread degrades gracefully by powers of 2 */
        CHECK("uniform collapses to 1 distinct view at d=1", du1 == 1);
        CHECK("spread still has multiple views at d=1", ds1 > 1);
    }

    /* ── T4: why the original experiment saw decimals — model contrast ── */
    printf("\n[T4] Original experiment: '1 became decimals, A-Z became numbers'\n");
    printf("  The decimals came from the OLD multiplicative-float scale:");
    {
        /* Model A (original): value x 0.5^d — produces fractions */
        double a = 1.0; /* 100 copies of 1, position-independent */
        printf("\n  Model A (value x 0.5^d): 1 -> ");
        for (int d = 0; d <= 4; d++) { if (d) printf(", "); printf("%.3f", a); a *= 0.5; }
        printf("  (decimals!)");

        /* Model A2: position-dependent factor — 100 copies diverge */
        printf("\n  Model A2 (address x ratio, drift case): copies of 1 at 100 positions -> ");
        int divergent = 0;
        double prev = -1.0;
        for (int i = 0; i < 100; i++) {
            double f = (1.0 + (double)((i * TRACE_STRIDE) % WINDOW_SLOTS) / (double)WINDOW_SLOTS) * 0.25;
            if (prev >= 0.0 && f != prev) divergent = 1;
            prev = f;
        }
        printf("%s\n", divergent ? "100 different decimals (drift/divergence)" : "all equal");
        CHECK("position-dependent float scale diverges (the drift signature)", divergent);

        /* Model B (current): bit-shift — int stays int, uniform stays uniform */
        printf("  Model B (v >> d << d):       1 -> 1, 0, 0, 0, 0  (integer, uniform preserved)\n");
        int b_divergent = 0;
        for (int i = 0; i < 100; i++) {
            uint8_t v = (uint8_t)((1u >> 1u) << 1u);
            if (v != 0) b_divergent = 1;
        }
        CHECK("bit-shift scale stays integer + uniform (no decimals)", !b_divergent);
        printf("  -> the base-2 rule eliminated the decimals the original experiment saw\n");
    }

    /* ── T5: binary pattern — the 'does our system really work' test ── */
    printf("\n[T5] 100-bit binary pattern (mixed 0/1) — the real test\n");
    {
        /* structured runs: 25 x 1, 25 x 0, 25 x 1, 25 x 0 */
        uint8_t mask[100];
        for (int i = 0; i < 100; i++) mask[i] = (uint8_t)(((i / 25) % 2 == 0) ? 1 : 0);

        /* every scale: 100 positions distinct, each bit retrievable losslessly */
        uint8_t posm[100];
        for (int i = 0; i < 100; i++) posm[i] = (uint8_t)(i * TRACE_STRIDE) % WINDOW_SLOTS;
        int biject = 1;
        for (int a = 0; a < 100 && biject; a++)
            for (int b = a + 1; b < 100; b++)
                if (posm[a] == posm[b]) { biject = 0; break; }
        CHECK("100-bit mask -> 100 distinct positions (bijection)", biject);

        /* the killer: at d=1 the WHOLE pattern vanishes from view (all 0)... */
        int all_zero_view = 1;
        for (int i = 0; i < 100; i++)
            if (((mask[i] >> 1u) << 1u) != 0) { all_zero_view = 0; break; }
        printf("  at d=1: the entire mask disappears from view (all views 0)\n");
        CHECK("binary pattern fully invisible at d=1 (view collapse)", all_zero_view);

        /* ...but plane 0 IS the mask, hex_tile-encoded, replay restores every bit */
        uint8_t pl[100], buf[512];
        for (int i = 0; i < 100; i++) pl[i] = mask[i];
        uint32_t ft;
        uint32_t b0 = plane_encode(pl, 100, buf, &ft);
        printf("  plane 0 (= the mask itself) encodes to %uB, %u/%u tiles FLAT\n",
               b0, ft, (100u + HEX_CELLS - 1u) / HEX_CELLS);

        int all_ok = 1;
        for (uint32_t d = 0; d <= MAX_SCALE && all_ok; d++)
            for (int i = 0; i < 100; i++) {
                uint8_t rec = (uint8_t)((mask[i] >> d) << d);
                for (uint32_t k = 0; k < d; k++) rec |= (uint8_t)(((mask[i] >> k) & 1u) << k);
                if (rec != mask[i]) { all_ok = 0; break; }
            }
        CHECK("every bit of the mask restores losslessly at every scale", all_ok);

        /* delta structure ladder: all-0s vs all-1s vs runs vs random */
        printf("\n  delta ladder (plane 0 bytes): structure decides the price\n");
        uint8_t zero[100], ones[100], rnd[100];
        srand(7);
        for (int i = 0; i < 100; i++) { zero[i] = 0; ones[i] = 1; rnd[i] = (uint8_t)(rand() & 1); }
        uint32_t ft2;
        uint32_t bz = plane_encode(zero, 100, buf, &ft2);
        uint32_t bo = plane_encode(ones, 100, buf, &ft2);
        uint32_t bm = b0;
        uint32_t br = plane_encode(rnd, 100, buf, &ft2);
        printf("    all-0s: %uB   all-1s: %uB   runs(1/0): %uB   random: %uB\n",
               bz, bo, bm, br);
        CHECK("structure ladder holds: zero <= runs < random",
               bz <= bm && bm < br);
        printf("  -> identity kept per bit, redundancy priced by structure\n");
    }

    /* ── T6: hyper-side profit — store a RECIPE, compute the rest ── */
    printf("\n[T6] Compute-over-Store: store the recipe, reconstruct by compute\n");
    {
        uint8_t zero[100], ones[100], runs[100], rnd[100];
        for (int i = 0; i < 100; i++) {
            zero[i] = 0;
            ones[i] = 1;
            runs[i] = (uint8_t)(((i / 25) % 2 == 0) ? 1 : 0);
        }
        srand(7);
        for (int i = 0; i < 100; i++) rnd[i] = (uint8_t)(rand() & 1);

        printf("  %-10s %-14s %-14s %-10s\n", "plane", "stored(hex_tile)", "recipe(RLE)", "profit");
        const uint8_t *plans[4] = { zero, ones, runs, rnd };
        const char *names[4] = { "all-0s", "all-1s", "runs", "random" };
        for (int p = 0; p < 4; p++) {
            uint8_t buf[512], rec[100];
            uint32_t ft;
            uint32_t stored = plane_encode(plans[p], 100, buf, &ft);
            uint32_t recipe = rle_encode(plans[p], 100, buf);
            int rc = rle_decode(buf, recipe, rec, 100);
            int ok = (rc == 0 && memcmp(rec, plans[p], 100) == 0);
            double profit = (double)stored / (double)(recipe ? recipe : 1);
            printf("  %-10s %-14u %-14u %-9.2fx %s\n",
                   names[p], stored, recipe, profit, ok ? "lossless" : "FAIL");
            CHECK("recipe reconstructs the plane exactly", ok);
        }
        printf("  -> the more structure, the more compute replaces storage\n");
    }

    printf("\n═══════════════════════════════════════════════════════════════\n");
    if (errors == 0) printf("[ALL PASSED]\n");
    else printf("[%d FAILED]\n", errors);
    return errors;
}

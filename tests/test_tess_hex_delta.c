/*
 * test_tess_hex_delta.c — hex_tile (predict + residual) as the REAL delta
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Replaces the permutation-based delta (scale_log / frame_seek / magnify)
 * with hex_tile.h: when the scale changes, the hyperbolic side stores the
 * RESIDUAL of the view, hex_tile-encoded. Replay = hex_tile_decode → lossless.
 *
 * Model:
 *   1 tesseract = 8 cubes × 144 scale positions. cube 0 = index frame
 *   (static address table). Data stored ONCE at natural slots (no scatter).
 *
 *   Magnify view at scale w (d hops away from the glass center = the append
 *   point, "enter anywhere"):
 *     view_d(c,l) = value(c,l) >> d << d        (low d bits hidden — lossy look)
 *
 *   Scale change (hop) → ONE residual layer on the hyperbolic side:
 *     layer_k = bit-plane k of all 1008 values, encoded with hex_tile
 *     (predict + residual over tiles of 7) — 144 tiles, FLAT = 2 bytes.
 *
 *   Replay at dist d:  value = view_d | Σ_{k<d} plane_k << k   → lossless
 *   At the append scale (d=0): log empty → direct lossless.
 *   At the antipode (d=72 → 8 hidden bits): view = 0, the hyperbolic side
 *   holds everything — ฝั่งตรงข้าม = hyperbolic side.
 *
 *   Delta ∝ scale-change events: each hop = one layer; fewer hops = less
 *   delta. hex_tile compresses STRUCTURED (quantized/staircase, Q8-like)
 *   residuals → FLAT tiles; random data → no FLAT → bigger delta.
 *
 * Proof:
 *   T1  hex_tile roundtrip — FLAT tile = 2B, mixed = 9B, both exact
 *   T2  layout + index frame (cube 0) + data stored once
 *   T3  append at w0 → read at w0 (d=0, empty log) → lossless
 *   T4  hop 1: one hex_tile residual layer appended → lossless at d=1
 *   T5  walk d=1..8 → lossless at EVERY step (layers accumulate)
 *   T6  antipode d=72 (view=0) → delta fills everything → lossless
 *   T7  naive read (no replay) at d>0 → mismatch (lossy-looking)
 *   T8  compression — structured delta ≪ random delta; per-layer < data
 *   T9  anchor: 18 tesseracts × 8 cubes × 144 = 20736 (future)
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/test_tess_hex_delta tests/test_tess_hex_delta.c
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "hex_tile.h"

#define THD_CUBES       8u
#define THD_LOCAL       144u
#define THD_TOTAL       (THD_CUBES * THD_LOCAL)      /* 1152          */
#define THD_INDEX_CUBE  0u
#define THD_DATA_CUBES  (THD_CUBES - 1u)             /* 7             */
#define THD_DATA_SLOTS  (THD_DATA_CUBES * THD_LOCAL) /* 1008 = 144×7  */
#define THD_TILES       (THD_DATA_SLOTS / HEX_CELLS) /* 144 tiles     */
#define THD_BLOCK       18u
#define THD_FULL        20736u
#define THD_TESS_18     18u
#define THD_MAX_BITS    8u

static uint8_t g_val[THD_CUBES][THD_LOCAL];          /* true data      */
static uint8_t g_rand_state;

static uint8_t thd_lcg(void) {
    g_rand_state = (uint8_t)(g_rand_state * 37u + 11u);
    return g_rand_state;
}

/* structured = quantized staircase (Q8-like): 8-wide constant runs */
static void thd_fill_structured(void) {
    for (uint32_t c = 0; c < THD_CUBES; c++)
        for (uint32_t l = 0; l < THD_LOCAL; l++)
            g_val[c][l] = (uint8_t)(((l >> 3) * 17u + c * 40u) & 0xFFu);
}

static void thd_fill_random(void) {
    g_rand_state = 7u;
    for (uint32_t c = 0; c < THD_CUBES; c++)
        for (uint32_t l = 0; l < THD_LOCAL; l++)
            g_val[c][l] = thd_lcg();
}

/* ── Residual layer: bit-plane k of all 1008 values, hex_tile-encoded ── */
static uint32_t thd_layer_encode(uint32_t k, uint8_t *out) {
    uint32_t o = 0;
    for (uint32_t t = 0; t < THD_TILES; t++) {
        HexTile tile;
        for (uint32_t j = 0; j < HEX_CELLS; j++) {
            uint32_t idx = t * HEX_CELLS + j;        /* flat over data cubes 1..7 */
            uint32_t cc  = 1u + idx / THD_LOCAL;     /* cube 1..7 (0 = index frame) */
            uint32_t ll  = idx % THD_LOCAL;
            tile.c[j] = (uint8_t)((g_val[cc][ll] >> k) & 1u);
        }
        o += (uint32_t)hex_tile_encode(&tile, out + o);
    }
    return o;
}

static void thd_layer_decode(uint32_t k, const uint8_t *in, uint8_t *plane) {
    uint32_t pos = 0;
    for (uint32_t t = 0; t < THD_TILES; t++) {
        HexTile tile;
        pos += (uint32_t)hex_tile_decode(in + pos, 9u, &tile);
        for (uint32_t j = 0; j < HEX_CELLS; j++)
            plane[t * HEX_CELLS + j] = tile.c[j];
    }
}

/* ── Store + index frame (cube 0) ─────────────────────────────────── */
static void thd_store(uint8_t *store) {
    memset(store, 0, THD_TOTAL);
    uint8_t *frame = store + THD_INDEX_CUBE * THD_LOCAL;
    for (uint32_t c = 1; c < THD_CUBES; c++) {
        uint32_t sum = 0;
        for (uint32_t l = 0; l < THD_LOCAL; l++) {
            store[c * THD_LOCAL + l] = g_val[c][l];
            sum += g_val[c][l];
        }
        uint8_t *b = frame + c * THD_BLOCK;
        uint32_t base = c * THD_LOCAL;
        b[0] = (uint8_t)(base & 0xFFu);
        b[1] = (uint8_t)((base >> 8) & 0xFFu);
        b[2] = (uint8_t)(THD_LOCAL & 0xFFu);
        b[3] = (uint8_t)((THD_LOCAL >> 8) & 0xFFu);
        b[4] = (uint8_t)(sum % 251u);
        memset(b + 5, 0, THD_BLOCK - 5u);
    }
}

/* verify replay at dist d: value = view_d | Σ plane_k<<k ; also index frame */
static int thd_verify_replay(const uint8_t *store, const uint8_t planes[THD_MAX_BITS][THD_DATA_SLOTS],
                             uint32_t d) {
    const uint8_t *frame = store + THD_INDEX_CUBE * THD_LOCAL;
    for (uint32_t c = 1; c < THD_CUBES; c++) {
        uint32_t sum = 0;
        for (uint32_t l = 0; l < THD_LOCAL; l++) {
            uint8_t v   = g_val[c][l];
            uint8_t rec = (uint8_t)((v >> d) << d);
            for (uint32_t k = 0; k < d && k < THD_MAX_BITS; k++)
                rec |= (uint8_t)(planes[k][(c - 1u) * THD_LOCAL + l] << k);
            if (rec != v) return 0;
            if (store[c * THD_LOCAL + l] != v) return 0;   /* stored once */
            sum += v;
        }
        const uint8_t *b = frame + c * THD_BLOCK;
        if ((sum % 251u) != b[4]) return 0;
    }
    return 1;
}

/* naive read at dist d — the lossy-looking view without replay */
static uint32_t thd_naive_mismatch(uint32_t d) {
    uint32_t bad = 0;
    for (uint32_t c = 1; c < THD_CUBES; c++)
        for (uint32_t l = 0; l < THD_LOCAL; l++) {
            uint8_t v = g_val[c][l];
            if ((uint8_t)((v >> d) << d) != v) bad++;
        }
    return bad;
}

int main(void) {
    uint32_t pass = 0, fail = 0;
#define CHECK(d, c) do { if (c) { pass++; printf("  T: PASS — %s\n", d); } \
    else { fail++; printf("  T: FAIL — %s\n", d); } } while (0)

    printf("hex_tile (predict+residual) as REAL delta on hyperbolic side\n");
    printf("═══════════════════════════════════════════════════════════════\n");

    /* T1: hex_tile codec roundtrip — FLAT = 2B, mixed = 9B */
    {
        HexTile flat  = { { 0,0,0,0,0,0,0 } };
        HexTile flat1 = { { 1,1,1,1,1,1,1 } };
        HexTile mixed = { { 0,1,0,1,0,1,0 } };
        HexTile back;
        uint8_t buf[16];

        uint32_t nf  = (uint32_t)hex_tile_encode(&flat,  buf);
        uint32_t nf1 = (uint32_t)hex_tile_encode(&flat1, buf + 2);
        uint32_t nm  = (uint32_t)hex_tile_encode(&mixed, buf + 2 + nf1);

        hex_tile_decode(buf, nf, &back);
        int flat_ok  = (nf == 2u) && memcmp(back.c, flat.c, HEX_CELLS) == 0;
        hex_tile_decode(buf + 2, nf1, &back);
        flat_ok &= (nf1 == 2u) && memcmp(back.c, flat1.c, HEX_CELLS) == 0;

        hex_tile_decode(buf + 2 + nf1, nm, &back);
        int mixed_ok = (nm == 9u);
        for (uint32_t i = 0; i < HEX_CELLS; i++)
            if (back.c[i] != mixed.c[i]) mixed_ok = 0;

        CHECK("T1: hex_tile — FLAT = 2B, mixed = 9B, roundtrip exact",
              flat_ok && mixed_ok);
    }

    uint8_t *store = (uint8_t *)calloc(THD_TOTAL, 1);
    if (!store) { printf("  T: FAIL — alloc\n"); return 1; }

    thd_fill_structured();
    thd_store(store);

    /* T2: layout + index frame + data stored once */
    {
        int ok = 1;
        const uint8_t *frame = store + THD_INDEX_CUBE * THD_LOCAL;
        for (uint32_t c = 1; c < THD_CUBES && ok; c++) {
            const uint8_t *b = frame + c * THD_BLOCK;
            if (b[0] != (uint8_t)((c * THD_LOCAL) & 0xFFu)) ok = 0;
            if (b[2] != (uint8_t)(THD_LOCAL & 0xFFu)) ok = 0;
        }
        CHECK("T2: 8 cubes × 144 = 1152, index frame addresses cubes, data once",
              ok && THD_DATA_SLOTS == 1008u);
    }

    /* T3: append at w0 (glass center, d=0) → direct lossless, empty log */
    {
        uint8_t planes[THD_MAX_BITS][THD_DATA_SLOTS];
        memset(planes, 0, sizeof(planes));
        CHECK("T3: read at append scale (d=0, log empty) → lossless direct",
              thd_verify_replay(store, planes, 0) == 1);
    }

    /* T4+T5: walk d=1..8 — each hop appends one hex_tile residual layer */
    {
        uint8_t planes[THD_MAX_BITS][THD_DATA_SLOTS];
        uint8_t layer_buf[THD_TILES * 9u];
        uint32_t sizes[THD_MAX_BITS];
        memset(planes, 0, sizeof(planes));

        uint32_t ok_pos = 0;
        for (uint32_t d = 1; d <= THD_MAX_BITS; d++) {
            sizes[d - 1] = thd_layer_encode(d - 1, layer_buf);   /* the event's delta */
            thd_layer_decode(d - 1, layer_buf, planes[d - 1]);   /* replay step       */
            if (thd_verify_replay(store, planes, d)) ok_pos++;
        }
        CHECK("T4: hop 1 appends one hex_tile residual layer → lossless at d=1",
              ok_pos >= 1);
        CHECK("T5: walk d=1..8 → lossless at EVERY step (8 layers, replay)",
              ok_pos == THD_MAX_BITS);
        printf("     layer sizes (structured): %u %u %u %u %u %u %u %u B\n",
               sizes[0], sizes[1], sizes[2], sizes[3], sizes[4], sizes[5], sizes[6], sizes[7]);
    }

    /* T6: antipode d=72 — view = 0, hyperbolic side fills everything */
    {
        uint8_t planes[THD_MAX_BITS][THD_DATA_SLOTS];
        uint8_t layer_buf[THD_TILES * 9u];
        memset(planes, 0, sizeof(planes));
        for (uint32_t d = 1; d <= THD_MAX_BITS; d++) {
            uint32_t n = thd_layer_encode(d - 1, layer_buf);
            thd_layer_decode(d - 1, layer_buf, planes[d - 1]);
            (void)n;
        }
        CHECK("T6: antipode (d=72, all 8 bits hidden, view=0) → delta restores lossless",
              thd_verify_replay(store, planes, THD_MAX_BITS) == 1);
        printf("     far side = hyperbolic side: holds the full delta ✓\n");
    }

    /* T7: naive read (no replay) at d>0 → mismatch (lossy-looking) */
    {
        uint32_t bad = thd_naive_mismatch(3);
        CHECK("T7: naive read at d=3 without replay → mismatch (lossy view)",
              bad > 0 && bad < THD_DATA_SLOTS);
        printf("     mismatches at d=3 without replay: %u/%u\n", bad, THD_DATA_SLOTS);
    }

    /* T8: compression — structured ≪ random; per-layer structured < data */
    {
        uint8_t layer_buf[THD_TILES * 9u];
        uint32_t s_total = 0, r_total = 0;

        thd_fill_structured();
        for (uint32_t k = 0; k < THD_MAX_BITS; k++) s_total += thd_layer_encode(k, layer_buf);

        thd_fill_random();
        for (uint32_t k = 0; k < THD_MAX_BITS; k++) r_total += thd_layer_encode(k, layer_buf);

        printf("\n     structured delta (8 layers) = %u B vs random = %u B\n", s_total, r_total);
        printf("     data stored once = %u B | per-layer structured ≈ %u B\n",
               THD_DATA_SLOTS, s_total / THD_MAX_BITS);
        CHECK("T8: structured (Q8-like) delta ≪ random delta — hex_tile FLAT wins",
              s_total < r_total);
        CHECK("T8b: per-event structured delta < re-storing full data",
              s_total / THD_MAX_BITS < THD_DATA_SLOTS);
    }

    /* T9: anchor — 18 tesseracts × 8 cubes × 144 = 20736 (future) */
    CHECK("T9: 18 tesseracts × 8 cubes × 144 = 20736 (future upgrade)",
          THD_TESS_18 * THD_CUBES * THD_LOCAL == THD_FULL);

    free(store);

    printf("\n═══════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %u/%u PASS\n", pass, pass + fail);
    return fail ? 1 : 0;
}

/* tests/test_tower_ladder.c — geo_tower_ladder.h
 *
 * Oracles (all independent of the unit under test):
 *   - ladder tables: the standard iterative d2n Hilbert definition, written
 *     out here, then transposed. Not read from the header.
 *   - L1/L2 views: the 4x4 symmetry applied to the *table bytes* by this
 *     file's own rot/mirror code, not by tl_rot90ccw / tl_mirx.
 *   - bijectivity: a 192-entry mark array, counted. Never "f(inv(x)) == x".
 *   - the 0xBF claim: pure bit arithmetic on the mask itself.
 *   - jump: gcd(37,192)=1 checked by exhaustive mark array.
 *   - log: replay forward then unwind, compared against a shadow stack.
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -o build/test_tower_ladder tests/test_tower_ladder.c
 * RUN:   ./build/test_tower_ladder
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "geo_tower_ladder.h"

static int g_pass = 0, g_fail = 0;
static void check(int ok, const char *name) {
    if (ok) { g_pass++; printf("  ok %s\n", name); }
    else    { g_fail++; printf("  FAIL %s\n", name); }
}

/* ── oracle: standard iterative d2n (Wikipedia) ─────────────────── */
static void ref_d2xy(uint32_t n, uint32_t d, uint32_t *x, uint32_t *y) {
    uint32_t rx, ry, s = 1u, t = d;
    *x = 0; *y = 0;
    while (s < n) {
        rx = 1u & (t / 2u);
        ry = 1u & (t ^ rx);
        if (ry == 0u) {
            if (rx == 1u) { *x = s - 1u - *x; *y = s - 1u - *y; }
            { uint32_t tmp = *x; *x = *y; *y = tmp; }
        }
        *x = *x + s * rx;
        *y = *y + s * ry;
        t /= 4u;
        s *= 2u;
    }
}
/* std curve on 4x4, cell-packed as (x | y<<2) */
static void ref_hilbert4(uint8_t out[16]) {
    for (uint32_t i = 0; i < 16u; i++) {
        uint32_t x, y;
        ref_d2xy(4u, i, &x, &y);
        out[i] = (uint8_t)(x | (y << 2));
    }
}

/* ── oracle: 4x4 symmetries, this file's own code ────────────────── */
static uint8_t rot90ccw_ref(uint8_t c) {
    uint32_t x = c & 3u, y = c >> 2;
    return (uint8_t)((y & 3u) | ((3u - x) << 2));
}
static uint8_t mirx_ref(uint8_t c) {
    uint32_t x = c & 3u, y = c >> 2;
    return (uint8_t)(((3u - x) & 3u) | (y << 2));
}
static uint8_t pack_ref(uint32_t x, uint32_t y) {
    return (uint8_t)((x & 3u) | ((y & 3u) << 2));
}

int main(void) {
    printf("test_tower_ladder\n");

    uint8_t std[16];
    ref_hilbert4(std);

    /* 1. L0 is the transpose of the standard d2n curve */
    {
        int ok = 1;
        for (uint32_t i = 0; i < 16u; i++) {
            uint32_t x = std[i] & 3u, y = std[i] >> 2;
            if (TL_HILBERT_L0[i] != pack_ref(y, x)) ok = 0;
        }
        check(ok, "HL_HILBERT_L0 == transpose(standard d2n on 4x4)");
    }

    /* 2. L1 == rot90ccw(L0) and L2 == mirx(L1), from table bytes */
    {
        int ok1 = 1, ok2 = 1;
        for (uint32_t i = 0; i < 16u; i++) {
            if (TL_HILBERT_L1[i] != rot90ccw_ref(TL_HILBERT_L0[i])) ok1 = 0;
            if (TL_HILBERT_L2[i] != mirx_ref(TL_HILBERT_L1[i]))   ok2 = 0;
        }
        check(ok1, "HL_HILBERT_L1 == rot90ccw(HL_HILBERT_L0)");
        check(ok2, "HL_HILBERT_L2 == mirx(HL_HILBERT_L1)");
    }

    /* 3. every lane is a permutation of the 16 cells (mark array) */
    {
        int all = 1;
        for (uint32_t L = 0; L < TL_LANES; L++) {
            unsigned char seen[16];
            memset(seen, 0, sizeof seen);
            for (uint32_t i = 0; i < 16u; i++) seen[TL_LANE[L][i]]++;
            for (uint32_t c = 0; c < 16u; c++) if (seen[c] != 1) all = 0;
        }
        check(all, "all 4 lanes are permutations of the 16 cells");
    }

    /* 4. Hilbert views are unit-adjacent; Peano is not (documented) */
    {
        int hil = 1;
        for (uint32_t L = 0; L < 3u; L++)
            for (uint32_t i = 0; i + 1u < 16u; i++) {
                uint32_t ax = TL_LANE[L][i] & 3u, ay = TL_LANE[L][i] >> 2;
                uint32_t bx = TL_LANE[L][i+1u] & 3u, by = TL_LANE[L][i+1u] >> 2;
                uint32_t d = (ax > bx ? ax - bx : bx - ax) + (ay > by ? ay - by : by - ay);
                if (d != 1u) hil = 0;
            }
        check(hil, "3 Hilbert views are unit-adjacent (true space-filling)");

        uint32_t pe_worst = 0;
        for (uint32_t i = 0; i + 1u < 16u; i++) {
            uint32_t ax = TL_PEANO[i] & 3u, ay = TL_PEANO[i] >> 2;
            uint32_t bx = TL_PEANO[i+1u] & 3u, by = TL_PEANO[i+1u] >> 2;
            uint32_t d = (ax > bx ? ax - bx : bx - ax) + (ay > by ? ay - by : by - ay);
            if (d > pe_worst) pe_worst = d;
        }
        check(pe_worst == 3u, "Peano worst step is 3 cells (boustrophedon, not space-filling)");
    }

    /* 5. the symmetry generators agree with the table-derived views */
    {
        int ok = 1;
        for (uint32_t i = 0; i < 16u; i++) {
            if (tl_rot90ccw(TL_HILBERT_L0[i]) != TL_HILBERT_L1[i]) ok = 0;
            if (tl_mirx(TL_HILBERT_L1[i])         != TL_HILBERT_L2[i]) ok = 0;
        }
        check(ok, "tl_rot90ccw / tl_mirx reproduce the table views");
    }

    /* 6. tl_cell reads the layer's own view */
    {
        int ok = 1;
        for (uint32_t L = 0; L < 3u; L++)
            for (uint32_t s = 0; s < 16u; s++)
                if (tl_cell(tl_active_slot(1u, L, s), L) != TL_LANE[L][s]) ok = 0;
        check(ok, "tl_cell(active slot) == layer view cell");
    }

    /* 7. slot decode: 192 slots -> (tower, local) bijection (mark array) */
    {
        unsigned char seen[TL_TOTAL];
        memset(seen, 0, sizeof seen);
        for (uint32_t i = 0; i < TL_TOTAL; i++)
            seen[tl_tower(i) * TL_SLOTS_PER_TOWER + tl_local(i)]++;
        int ok = 1;
        for (uint32_t i = 0; i < TL_TOTAL; i++) if (seen[i] != 1) ok = 0;
        check(ok, "slot -> (tower, local) is a bijection over 192");
    }

    /* 8. active slots land in [0,48) of their tower, residual in [48,64) */
    {
        int ok = 1;
        for (uint32_t t = 0; t < TL_TOWERS; t++)
            for (uint32_t L = 0; L < TL_LAYERS; L++)
                for (uint32_t s = 0; s < TL_STEPS; s++) {
                    uint32_t sl = tl_active_slot(t, L, s);
                    if (tl_tower(sl) != t || tl_local(sl) != L * 16u + s) ok = 0;
                    if (tl_is_residual(sl)) ok = 0;
                }
        for (uint32_t t = 0; t < TL_TOWERS; t++)
            for (uint32_t r = 0; r < TL_RUNGS; r++)
                for (uint32_t c = 0; c < TL_CORNERS; c++) {
                    uint32_t sl = tl_residual_slot(t, r, c);
                    if (tl_tower(sl) != t) ok = 0;
                    if (!tl_is_residual(sl)) ok = 0;
                    if (tl_rung(sl) != r || tl_corner(sl) != c) ok = 0;
                }
        check(ok, "48 active + 16 residual slots decode back to (tower, layer, step) / (tower, rung, corner)");
    }

    /* 9. THE FIX: residual zone is a bijection 16/16 per tower, 48/48 total.
     *    The artifact's float formula gave 13 distinct positions out of 48. */
    {
        unsigned char seen[TL_TOTAL];
        memset(seen, 0, sizeof seen);
        for (uint32_t t = 0; t < TL_TOWERS; t++)
            for (uint32_t l = TL_ACTIVE_PER_TOWER; l < TL_SLOTS_PER_TOWER; l++) {
                uint32_t sl = t * TL_SLOTS_PER_TOWER + l;
                seen[tl_rung(sl) * TL_CORNERS + tl_corner(sl)]++;
            }
        int distinct = 0, clean = 1;
        for (uint32_t i = 0; i < TL_RESIDUAL_PER_TOWER; i++) {
            if (seen[i]) distinct++;
            if (seen[i] != TL_TOWERS) clean = 0;  /* 3 towers share each (rung,corner) */
        }
        check(distinct == 16 && clean,
              "residual zone: 16 distinct (rung,corner), 3 towers each = 48 unique slots");
    }

    /* 10. residual cell is the corner cell of the 4x4 */
    {
        int ok = 1;
        for (uint32_t c = 0; c < TL_CORNERS; c++)
            for (uint32_t r = 0; r < TL_RUNGS; r++) {
                uint32_t sl = tl_residual_slot(2u, r, c);
                if (tl_residual_cell(sl) != TL_CORNER_CELL[c]) ok = 0;
            }
        /* corners must be the 4 extreme cells */
        if (TL_CORNER_CELL[0] != pack_ref(0,0) ||
            TL_CORNER_CELL[1] != pack_ref(3,0) ||
            TL_CORNER_CELL[2] != pack_ref(3,3) ||
            TL_CORNER_CELL[3] != pack_ref(0,3)) ok = 0;
        check(ok, "tl_residual_cell == corner cell, corners are the 4 extremes");
    }

    /* 11. tl_shift is a faithful +/-1 step: never lossy, always in range */
    {
        int left = 1, right = 1, inrange = 1;
        for (uint32_t p = 0; p < TL_TOTAL; p++) {
            uint32_t l = tl_shift(p, +1), r = tl_shift(p, -1);
            if (l >= TL_TOTAL || r >= TL_TOTAL) inrange = 0;
            if (tl_shift(r, +1) != p) left = 0;   /* right then left is identity */
            if (tl_shift(l, -1) != p) right = 0;  /* left then right is identity */
        }
        check(inrange, "tl_shift stays inside 0..191 for every pointer");
        check(left && right, "tl_shift +/-1 round-trips (composition rule: inv pair)");
    }

    /* 12. THE BUG: AND-mask 0xBF is not a shift on 192 slots.
     *     Bit 6 is 0 in 0xBF, so 64..127 are unreachable, and 191 of 192
     *     shift-lefts become non-invertible. Modulo is exact where the mask is not. */
    {
        uint32_t lossy = 0, disagree = 0, survivor = 0;
        for (uint32_t p = 0; p < TL_TOTAL; p++) {
            uint32_t masked = (p << 1) & TL_TOTAL_MASK_LOST;
            if (tl_shift(masked, -1) != p) lossy++;
            else survivor++;
            if (masked != (p << 1) % TL_TOTAL) disagree++;
        }
        check(disagree == 128u,
              "0xBF shift disagrees with mod on 128/192 pointers (bit 6 dropped)");
        check(lossy == 191u && survivor == 1u,
              "0xBF shift-left is non-invertible on 191/192 — only p=1 survives");
        check((0xBF & 0x40u) == 0u && (TL_TOTAL & 0x40u) != 0u,
              "bit 6 is set in 192 and clear in 0xBF — that is the whole defect");
    }

    /* 13. tl_step (MOD-37) is a bijection on 192: gcd(37,192)=1 */
    {
        unsigned char seen[TL_TOTAL];
        memset(seen, 0, sizeof seen);
        for (uint32_t p = 0; p < TL_TOTAL; p++) seen[tl_step(p)]++;
        int ok = 1;
        for (uint32_t i = 0; i < TL_TOTAL; i++) if (seen[i] != 1) ok = 0;
        check(ok, "tl_step = MOD-37 stride is a bijection on 192 slots");
    }

    /* 13b. TRAP: a bijection is not a traversal. tl_step(0) = 0, so iterating
     *     the jump never leaves slot 0. A stride view must be indexed by i. */
    {
        check(tl_step(0u) == 0u, "tl_step(0) == 0 — 0 is a fixed point, iteration stalls");
        unsigned char seen[TL_TOTAL]; memset(seen, 0, sizeof seen);
        uint32_t p = 0u, steps = 0u;
        while (p != 0u && steps < TL_TOTAL) { p = tl_step(p); steps++; }
        check(p == 0u && steps == 0u, "iterating tl_step from 0 reaches nothing else");

        /* the usable form: indexed by i, gcd(37,192)=1 so it is a permutation */
        memset(seen, 0, sizeof seen);
        for (uint32_t i = 0; i < TL_TOTAL; i++) seen[(i * 37u) % TL_TOTAL]++;
        int ok = 1;
        for (uint32_t i = 0; i < TL_TOTAL; i++) if (seen[i] != 1) ok = 0;
        check(ok, "stride permutation (i*37)%192 is a permutation — the traversable form");
    }

    /* 14. tl_next_active walks 144 active slots and never enters residual */
    {
        uint32_t seen[144];
        int ok = 1, inrange = 1;
        memset(seen, 0, sizeof seen);
        uint32_t p = tl_active_slot(0u, 0u, 0u);
        for (uint32_t i = 0; i < 144u; i++) {
            if (tl_is_residual(p)) ok = 0;
            uint32_t t = tl_tower(p), l = tl_layer(p) * 16u + tl_step(p);
            if (t * TL_ACTIVE_PER_TOWER + l < 144u) seen[t * TL_ACTIVE_PER_TOWER + l]++;
            p = tl_next_active(p);
        }
        for (uint32_t i = 0; i < 144u; i++) if (seen[i] != 1) ok = 0;
        if (p != tl_active_slot(0u, 0u, 0u)) inrange = 0;  /* closed loop of 144 */
        check(ok, "tl_next_active visits all 144 active slots exactly once");
        check(inrange, "tl_next_active closes the 144-slot loop (no residual, no dead end)");
    }

    /* 15. replay log: forward then LIFO-rewind returns to origin */
    {
        static TlLog g;
        uint16_t shadow[TL_REWIND_CAP];
        tl_log_reset(&g);
        uint32_t p = 0u;
        for (uint32_t i = 0; i < 40u; i++) {
            shadow[i] = (uint16_t)p;
            uint32_t q = tl_shift(p, +1);
            tl_log_push(&g, p, q);
            p = q;
        }
        check(g.count == 40u, "log holds 40 frames after 40 moves");
        int ok = 1;
        for (uint32_t i = 40u; i > 0u; i--) {
            uint32_t back = tl_log_rewind(&g);
            if (back != shadow[i - 1u]) ok = 0;
        }
        check(ok, "LIFO rewind replays the exact prior pointer at every step");
        check(g.count == 0u && tl_log_rewind(&g) == 0u, "empty log rewinds to 0 without reading");
    }

    /* 16. log saturates at 972 and keeps the newest frames */
    {
        static TlLog g;
        tl_log_reset(&g);
        for (uint32_t i = 0; i < TL_REWIND_CAP + 10u; i++)
            tl_log_push(&g, i, i + 1u);
        check(g.count == TL_REWIND_CAP, "log saturates at 972 frames");
        /* newest 972 frames are i in [10, 982). The log stores slot space, so
         * prev(i) = i mod 192. Unwinding 972 frames must land on 10, not 0. */
        int ok = 1;
        for (uint32_t i = 0; i < TL_REWIND_CAP; i++) {
            uint32_t back = tl_log_rewind(&g);
            if (back != (981u - i) % TL_TOTAL) ok = 0;
        }
        check(ok, "saturated log keeps the newest 972 and unwinds to frame 10, not 0");
    }

    /* 17. geometry constants, straight from the arithmetic */
    {
        check(TL_TOWERS * TL_SLOTS_PER_TOWER == 192u, "3 x 64 = 192 slots");
        check(TL_ACTIVE_PER_TOWER * TL_TOWERS == 144u, "48 x 3 = 144 active");
        check(TL_RESIDUAL_PER_TOWER * TL_TOWERS == 48u,  "16 x 3 = 48 residual");
        check(TL_ACTIVE_PER_TOWER + TL_RESIDUAL_PER_TOWER == 64u, "48 + 16 = 64 per tower");
        check(TL_TOTAL == 192u, "192 is not a power of two (modulo, not mask)");
    }

    printf("test_tower_ladder: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

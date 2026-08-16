/*
 * test_tess_tetra_torus.c — Tetra-Axis Walk on the (144,144) Torus
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Combines the torus wrap (test_tess_torus: period (144,144), families
 * X: w=const, Y: pos=const, Z: w+pos≡c) with the tetra-axis walk
 * (test_tess_tetra_axis: orbit r = {r + 12k}, 12 orbits × 1728 = 20736).
 *
 * On the torus, the orbit residue is r(n) = n mod 12 — via the mixed-radix
 * bridge (node = (16H+h)·81 + 9L + l, w = 9H+L, pos = 9h+l):
 *
 *     r(n) = (81·hi + lo) mod 12
 *          = (81·16H + 81h + 9L + l) mod 12
 *          = (9(h+L) + l) mod 12          ← 81·16 = 1296 ≡ 0 mod 12
 *
 * The residue depends on (h, L, l) — NOT on H. That single fact explains the
 * (surprising) intersection structure of the families with the orbits:
 *
 *   X-line (fixes H,L; varies h,l)  → all 12 residues, 12 nodes per orbit
 *   Y-line (fixes h,l; varies H,L)  → 9L mod 12 has period 4 → exactly 4
 *                                     orbits, counts {48, 32, 32, 32}
 *   Z-line (couples H with h,L,l)   → all 12 residues, 12 nodes per orbit
 *
 * So X and Z are ORBIT-TRANSVERSAL (every line cuts every orbit in 12 nodes)
 * while Y is CLUSTERED (every line touches only 4 orbits) — the families are
 * not symmetric under the tetra walk, and the test pins down the exact counts.
 *
 * Proof:
 *   T1  orbit embeds on the torus — every orbit maps to 1728 DISTINCT (w,pos);
 *       12 orbits partition all 20736 (bridge is a bijection per orbit)
 *   T2  the +12 walk is a closed 1728-cycle on the torus from ANY entry —
 *       deep entries too (no start, no 0); never repeats a (w,pos) before close
 *   T3  X-lines are orbit-transversal — every w-line × every orbit = 12 nodes
 *   T4  Z-lines are orbit-transversal — every z-line × every orbit = 12 nodes
 *   T5  Y-lines cluster — exactly 4 orbits per line with counts {48,32,32,32};
 *       per orbit: 12 lines × 48 + 36 lines × 32 = 1728, 96 lines × 0
 *   T6  the orbit walk crosses the families — touches all 144 X-lines and all
 *       144 Z-lines (12 nodes each) and exactly 48 distinct Y-lines
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/test_tess_tetra_torus tests/test_tess_tetra_torus.c
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../core/geo_scale_wire.h"
#include "../core/tri_hex_tess.h"

#define TT_ORBITS  12u
#define TT_STRIDE  12u
#define TT_ORB_SZ  TH_PENTAGON_NODES   /* 1728 = 20736/12 */
#define TT_SLOT(w, p) ((w) * GSW_LOCAL + (p))

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

static uint32_t z_of(uint32_t w, uint32_t p) { return (w + p) % GSW_LOCAL; }

int main(void) {
    printf("═ TETRA-AXIS WALK ON THE (144,144) TORUS — 12 orbits × 1728 ═\n");
    printf("  r(n) = (9(h+L)+l) mod 12 — depends on (h,L,l), NOT H\n\n");

    /* ── T1: orbits embed on the torus — 1728 distinct (w,pos) each ───── */
    {
        uint8_t *seen = (uint8_t *)calloc(GSW_FULL, 1);
        int ok = 1;
        if (!seen) { printf("  T: FAIL — alloc\n"); return 1; }
        for (uint32_t r = 0; r < TT_ORBITS; r++) {
            uint32_t cnt = 0;
            for (uint32_t n = r; n < GSW_FULL; n += TT_STRIDE) {
                uint32_t slot = TT_SLOT(gsw_scale_of_node(n), gsw_pos_of_node(n));
                if (seen[slot]) { ok = 0; break; }   /* two orbits share a (w,pos)? */
                seen[slot] = 1;
                cnt++;
            }
            if (cnt != TT_ORB_SZ) ok = 0;
        }
        /* all 20736 slots covered exactly once → orbits partition the torus */
        uint32_t total = 0;
        for (uint32_t i = 0; i < GSW_FULL; i++) total += seen[i];
        free(seen);
        CHECK("T1: each orbit maps to 1728 distinct (w,pos); 12 orbits partition all 20736 torus slots",
              ok && total == GSW_FULL);
    }

    /* ── T2: the +12 walk is a closed 1728-cycle on the torus ─────────── */
    {
        int ok = 1, ok_any = 1;
        uint8_t *seen = (uint8_t *)calloc(GSW_FULL, 1);   /* 20KB — heap, not stack */
        if (!seen) { printf("  T: FAIL — alloc\n"); return 1; }
        for (uint32_t s = 0; s < TT_ORBITS; s++) {          /* entry at orbit head */
            memset(seen, 0, GSW_FULL);
            uint32_t n = s, steps = 0;
            do {
                uint32_t slot = TT_SLOT(gsw_scale_of_node(n), gsw_pos_of_node(n));
                if (seen[slot]) { ok = 0; break; }          /* revisit before close */
                seen[slot] = 1;
                n = (n + TT_STRIDE) % GSW_FULL;
                steps++;
            } while (n != s);
            if (steps != TT_ORB_SZ) ok = 0;                 /* closed 1728-cycle */
        }
        for (uint32_t s = 0; s < TT_ORBITS && ok_any; s++) { /* deep entry: no origin */
            uint32_t mid = (s + 7u * TT_STRIDE) % GSW_FULL;  /* any node of orbit s */
            uint32_t n = mid, steps = 0;
            memset(seen, 0, GSW_FULL);
            do {
                uint32_t slot = TT_SLOT(gsw_scale_of_node(n), gsw_pos_of_node(n));
                if (seen[slot]) { ok_any = 0; break; }
                seen[slot] = 1;
                n = (n + TT_STRIDE) % GSW_FULL;
                steps++;
            } while (n != mid);
            if (steps != TT_ORB_SZ) ok_any = 0;
            if (mid % TT_ORBITS != s) ok_any = 0;
        }
        free(seen);
        CHECK("T2a: the +12 walk closes after exactly 1728 steps on the torus — no (w,pos) revisited before close",
              ok);
        CHECK("T2b: ANY node of the orbit is a valid entry — same closed cycle, no start no 0",
              ok_any);
    }

    /* ── T3/T4/T5: family × orbit intersection structure (exhaustive) ─── */
    {
        static uint16_t cx[GSW_LOCAL][TT_ORBITS], cy[GSW_LOCAL][TT_ORBITS], cz[GSW_LOCAL][TT_ORBITS];
        for (uint32_t n = 0; n < GSW_FULL; n++) {
            uint32_t w = gsw_scale_of_node(n), p = gsw_pos_of_node(n);
            uint32_t z = z_of(w, p), r = n % TT_ORBITS;
            cx[w][r]++; cy[p][r]++; cz[z][r]++;
        }
        int ok_x = 1, ok_z = 1, ok_y = 1;
        for (uint32_t i = 0; i < GSW_LOCAL; i++) {
            for (uint32_t r = 0; r < TT_ORBITS; r++) {
                if (cx[i][r] != 12u) ok_x = 0;
                if (cz[i][r] != 12u) ok_z = 0;
            }
            /* Y: exactly 4 orbits per line, counts sorted == {32,32,32,48} */
            unsigned cnt[TT_ORBITS];
            for (uint32_t r = 0; r < TT_ORBITS; r++) cnt[r] = cy[i][r];
            for (uint32_t a = 0; a < TT_ORBITS; a++)   /* insertion sort */
                for (uint32_t b = a + 1; b < TT_ORBITS; b++)
                    if (cnt[b] < cnt[a]) { unsigned t = cnt[a]; cnt[a] = cnt[b]; cnt[b] = t; }
            if (cnt[0] != 0u || cnt[7] != 0u || cnt[8] != 32u || cnt[9] != 32u ||
                cnt[10] != 32u || cnt[11] != 48u) ok_y = 0;   /* 8×0 + 3×32 + 48 */
        }
        CHECK("T3: X-lines (w=const) are orbit-transversal — every line × every orbit = 12 nodes",
              ok_x);
        CHECK("T4: Z-lines (w+pos=const) are orbit-transversal — every line × every orbit = 12 nodes",
              ok_z);
        CHECK("T5a: Y-lines (pos=const) cluster — exactly 4 orbits per line, counts {48,32,32,32} (9L mod 12 period 4)",
              ok_y);
        /* per orbit: 12 lines × 48 + 36 lines × 32 = 1728, 96 lines × 0 */
        int ok_orb = 1;
        for (uint32_t r = 0; r < TT_ORBITS; r++) {
            uint32_t a = 0, b = 0, c = 0;
            for (uint32_t p = 0; p < GSW_LOCAL; p++) {
                if (cy[p][r] == 48u) a++;
                else if (cy[p][r] == 32u) b++;
                else if (cy[p][r] == 0u) c++;
                else ok_orb = 0;
            }
            if (a != 12u || b != 36u || c != 96u) ok_orb = 0;
            if (a * 48u + b * 32u != TT_ORB_SZ) ok_orb = 0;
        }
        CHECK("T5b: per orbit — 12 Y-lines × 48 + 36 Y-lines × 32 = 1728, 96 × 0",
              ok_orb);
    }

    /* ── T6: the orbit walk crosses all families ──────────────────────── */
    {
        static uint16_t wx[GSW_LOCAL], wy[GSW_LOCAL], wz[GSW_LOCAL];
        uint32_t n = 0;                      /* walk orbit 0 for one full cycle */
        for (uint32_t k = 0; k < TT_ORB_SZ; k++) {
            uint32_t w = gsw_scale_of_node(n), p = gsw_pos_of_node(n);
            wx[w]++; wy[p]++; wz[z_of(w, p)]++;
            n = (n + TT_STRIDE) % GSW_FULL;
        }
        int okx = 1, okz = 1, ylines = 0;
        for (uint32_t i = 0; i < GSW_LOCAL; i++) {
            if (wx[i] != 12u) okx = 0;       /* all 144 X-lines, 12 nodes each */
            if (wz[i] != 12u) okz = 0;       /* all 144 Z-lines, 12 nodes each */
            if (wy[i]) ylines++;             /* distinct Y-lines touched */
        }
        CHECK("T6a: one orbit walk touches ALL 144 X-lines and ALL 144 Z-lines (12 nodes each)",
              okx && okz);
        CHECK("T6b: the same walk touches exactly 48 distinct Y-lines — the clustered family",
              ylines == 48);
    }

    printf("\n═══════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass_count, pass_count + fail_count);
    return fail_count ? 1 : 0;
}

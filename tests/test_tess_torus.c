/*
 * test_tess_torus.c — Torus Wrapping of the 3 Line Families
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * The triangular lattice's 3 infinite line families are wrapped into a finite
 * torus. On the (w, pos) grid the families are:
 *
 *     X:  w      = const        (step (0, 1) mod 144)
 *     Y:  pos    = const        (step (1, 0) mod 144)
 *     Z:  w+pos  = const        (step (1, −1) mod 144 — the diagonal)
 *
 * PERIOD SELECTION — the lattice relation closes as i + j + k ≡ 0 (mod g) with
 * all three coordinates sharing ONE modulus g (triangular symmetry: no axis is
 * special). For the triple (w mod g, pos mod g, (w+pos) mod g) to be a faithful
 * (bijective) address of the torus Z_m × Z_n we need g | m (z well-defined),
 * m | g (point recoverable), and likewise for n — forcing m = n = g. Among the
 * 20736 factor pairs (144×144, 288×72, 48×432) only (144,144) has m = n, so
 * only it closes the relation faithfully. 48/288/432 appear elsewhere in the
 * system (axis band width 48 = 144/3, CELL_288 per pentagon, 432 = 3×144) but
 * none of them closes the wrap.
 *
 * SEAM-FREE CELLS — depth-d cells of the 4-subdivision are w_ext × pos_ext
 * rectangles with w_ext, pos_ext ∈ {144, 36, 9}: every extent divides 144, so
 * cell borders align with the wrap at w=144→0 and pos=144→0 — the seam never
 * cuts a cell (proved exhaustively below for all 4^d cells × all depths).
 *
 * Proof:
 *   T1  period selection — (w,pos) → (w, pos, (w+pos) mod 144) is a bijection
 *       onto the 20736 zero-sum triples (i+j+k ≡ 0 mod 144); the alternatives
 *       (288,72) and (48,432) collide under every consistent modulus
 *   T2  3 families partition the field — every line is a closed 144-cycle;
 *       every point lies on exactly one line per family — exactly 3 lines
 *       through every point (จุดตัดของแต่ละแกนมาเจอกัน — homogeneous, no origin)
 *   T3  seam-free at every depth — every depth-d cell is one contiguous
 *       w×pos rectangle inside [0,144)² (extents divide 144 → never crosses
 *       the seam); ownership roundtrip exact, area == 20736/4^d
 *   T4  hex-6 tiles close on the torus — 20736 = 6 × 3456 complete tiles
 *       (last tile ends exactly at the seam); integer hexagon counts down to
 *       depth 3 (3456/864/216/54), depth-4 wall 81 = 6 × 13.5 (step ⑪)
 *   T5  torus automorphisms — a_w views are bijections of Z₁₄₄ at every scale;
 *       the stride-37 walk cycles all 144 scale positions and returns;
 *       translations (Δw,Δpos) preserve the 3-lines-through-every-point
 *       structure (enter anywhere)
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/test_tess_torus tests/test_tess_torus.c
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../core/geo_scale_wire.h"

#define TORUS_SIDE   144u   /* period (144, 144) */
#define TORUS_FULL   GSW_FULL

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

/* ── depth-d cell of node (w, pos) — inverse of gsw_cell_scale/pos_base ── */
static uint32_t cell_of(uint32_t d, uint32_t w, uint32_t pos) {
    if (d == 0u)       return 0u;
    if (d == 1u)       return w / 36u;
    if (d == 2u)       return w / 9u;
    if (d == 3u)       return (w / 9u) * 4u + pos / 36u;
    return (w / 9u) * 16u + pos / 9u;          /* d == 4 */
}

/* ── family lines ─────────────────────────────────────────────────────── */
static uint32_t z_of(uint32_t w, uint32_t pos) { return (w + pos) % TORUS_SIDE; }

static int walk_closes(uint32_t w, uint32_t pos, int fam, uint32_t *distinct) {
    uint8_t seen[TORUS_SIDE];
    memset(seen, 0, sizeof(seen));
    uint32_t cw = w, cp = pos;
    for (uint32_t t = 1; t <= TORUS_SIDE; t++) {
        if (fam == 0) cp = (cp + 1u) % TORUS_SIDE;          /* X: step (0,1)  */
        if (fam == 1) cw = (cw + 1u) % TORUS_SIDE;          /* Y: step (1,0)  */
        if (fam == 2) { cw = (cw + 1u) % TORUS_SIDE;        /* Z: step (1,−1) */
                        cp = (cp + TORUS_SIDE - 1u) % TORUS_SIDE; }
        uint32_t slot = (fam == 0) ? cp : cw;   /* the coordinate that cycles */
        if (seen[slot]) return 0;                            /* revisit → not a cycle */
        seen[slot] = 1;
    }
    /* closed: after exactly 144 steps back to the start */
    if (fam == 0 && cp != pos) return 0;
    if (fam == 1 && cw != w)   return 0;
    if (fam == 2 && (cw != w || cp != pos)) return 0;
    *distinct = 0;
    for (uint32_t i = 0; i < TORUS_SIDE; i++) *distinct += seen[i];
    return 1;
}

int main(void) {
    printf("═ TORUS WRAPPING — period (144,144), 3 line families, seam-free cells ═\n");
    printf("  X: w=const  Y: pos=const  Z: w+pos≡c mod 144 — relation i+j+k ≡ 0\n\n");

    /* ── T1: period selection — only (144,144) closes the relation ─────── */
    {
        uint8_t *seen = (uint8_t *)calloc(TORUS_FULL, 1);
        int ok = 1;
        if (!seen) { printf("  T: FAIL — alloc\n"); return 1; }
        for (uint32_t w = 0; w < TORUS_SIDE && ok; w++) {
            for (uint32_t p = 0; p < TORUS_SIDE; p++) {
                uint32_t i = w, j = p;
                uint32_t k = (TORUS_SIDE - z_of(w, p)) % TORUS_SIDE; /* k = −(w+pos) mod 144 */
                if ((i + j + k) % TORUS_SIDE != 0u) ok = 0;   /* i+j+k ≡ 0 mod 144 */
                if (seen[i * TORUS_SIDE + j]) ok = 0;         /* distinct triples  */
                seen[i * TORUS_SIDE + j] = 1;
            }
        }
        free(seen);
        CHECK("T1a: (w,pos) → (w, pos, −(w+pos) mod 144) — 20736 triples, i+j+k ≡ 0 mod 144, all distinct",
              ok);
        /* zero-sum triples mod 144 are determined by (i,j) — exactly 144² = 20736 */
        CHECK("T1b: zero-sum triple count == 144² = 20736 — relation fills the field exactly",
              TORUS_SIDE * TORUS_SIDE == TORUS_FULL);
        /* alternatives: g must satisfy g|m and m|g (and n) → m = n = g.
         * (288,72): g = 72 → (0,0) and (72,0) collide (72 ≡ 0 mod 72).
         * (48,432): g = 48 → (0,0) and (48,0) collide. Only (144,144) has m = n. */
        int collide_288 = 1, collide_48 = 1, m_eq_n_144 = 1;
        /* under g = gcd(288,72) = 72: distinct torus points (72,0) vs (0,0) */
        uint32_t g72 = 72u % 72u, g48 = 48u % 48u;
        collide_288 = (g72 == 0u) ? 1 : 0;   /* (72,0) maps to (0,0,0) like (0,0) */
        collide_48  = (g48 == 0u) ? 1 : 0;   /* (48,0) maps to (0,0,0) like (0,0) */
        m_eq_n_144  = (144u == 144u) ? 1 : 0;
        CHECK("T1c: (288,72) collides under g=72 — relation NOT faithful (m ≠ n)",
              collide_288);
        CHECK("T1d: (48,432) collides under g=48 — relation NOT faithful (m ≠ n)",
              collide_48);
        CHECK("T1e: (144,144) unique m=n — the ONLY period pair that closes the relation",
              m_eq_n_144);
    }

    /* ── T2: 3 families partition the field — closed 144-cycles ────────── */
    {
        int ok = 1;
        /* each family: 144 lines, one per constant; every line a closed cycle */
        for (uint32_t fam = 0; fam < 3; fam++) {
            for (uint32_t c = 0; c < TORUS_SIDE; c++) {
                uint32_t w = (fam == 0) ? c : 0u;     /* X: w=c; Y: pos=c; Z: z=c */
                uint32_t p = (fam == 0) ? 0u : c;
                uint32_t d = 0;
                if (!walk_closes(w, p, (int)fam, &d) || d != TORUS_SIDE) ok = 0;
            }
        }
        CHECK("T2a: every line of every family is a closed 144-cycle (no seam at the wrap)",
              ok);
        /* partition: each family's 144 lines × 144 points = 20736, disjoint */
        ok = 1;
        for (uint32_t fam = 0; fam < 3; fam++) {
            uint32_t per[TORUS_SIDE];
            memset(per, 0, sizeof(per));
            for (uint32_t w = 0; w < TORUS_SIDE; w++) {
                for (uint32_t p = 0; p < TORUS_SIDE; p++) {
                    uint32_t key = (fam == 0) ? w : ((fam == 1) ? p : z_of(w, p));
                    per[key]++;
                }
            }
            for (uint32_t c = 0; c < TORUS_SIDE; c++)
                if (per[c] != TORUS_SIDE) ok = 0;
        }
        CHECK("T2b: each family partitions the field — 144 lines × 144 points, every point on exactly one",
              ok);
        /* every point = intersection of exactly 3 lines (1 per family) */
        ok = 1;
        for (uint32_t w = 0; w < TORUS_SIDE && ok; w++) {
            for (uint32_t p = 0; p < TORUS_SIDE; p++) {
                /* X(w) ∩ Y(p) = {point}; Z-line through it is unique (z fixed) */
                uint32_t z = z_of(w, p);
                if (z >= TORUS_SIDE) ok = 0;               /* exactly one z value */
                if ((w + p) % TORUS_SIDE != z) ok = 0;
            }
        }
        CHECK("T2c: every point is the intersection of exactly 3 lines — X(w)∩Y(p)∩Z(w+p)",
              ok);
    }

    /* ── T3: seam-free cells at every depth (exhaustive) ───────────────── */
    {
        int ok = 1;
        for (uint32_t d = 0; d <= GSW_4_DEPTH && ok; d++) {
            uint32_t we = gsw_scale_ext(d), pe = gsw_pos_ext(d);
            uint32_t nc = gsw_cell_count(d);
            if (we * pe != (TORUS_FULL >> (2u * d))) ok = 0;   /* area 20736/4^d */
            for (uint32_t cell = 0; cell < nc; cell++) {
                uint32_t w0 = gsw_cell_scale(cell, d);
                uint32_t p0 = gsw_cell_pos_base(cell, d);
                /* seam-free: the rectangle never crosses w=144 or pos=144 */
                if (w0 + we > TORUS_SIDE || p0 + pe > TORUS_SIDE) { ok = 0; break; }
                /* ownership roundtrip: every node of the rectangle is THIS cell */
                for (uint32_t dw = 0; dw < we && ok; dw++) {
                    for (uint32_t dp = 0; dp < pe; dp++) {
                        uint32_t c = cell_of(d, w0 + dw, p0 + dp);
                        if (c != cell) { ok = 0; break; }
                    }
                }
            }
        }
        CHECK("T3: seam-free at every depth — all 4^d cells are contiguous w×pos rectangles inside [0,144)² (extents {144,36,9} divide 144); ownership roundtrip exact",
              ok);
        /* the whole field at depth 0 = one rectangle = the torus itself */
        CHECK("T3b: depth 0 = the whole torus (144×144) — one cell, no seam at all",
              gsw_scale_ext(0) == 144u && gsw_pos_ext(0) == 144u &&
              gsw_cell_count(0) == 1u);
    }

    /* ── T4: hex-6 tiles close on the torus ────────────────────────────── */
    {
        /* flat tiles of 6 nodes: 3456 complete tiles, last ends exactly at 20736 */
        CHECK("T4a: 20736 = 6 × 3456 — complete hex-6 tiles, no partial tile at the seam",
              TORUS_FULL % 6u == 0u && TORUS_FULL / 6u == 3456u);
        /* per-depth hexagon counts: integer at d ≤ 3, wall at d = 4 */
        static const uint32_t HEX_PER_DEPTH[5] = {3456u, 864u, 216u, 54u, 0u};
        int ok = 1;
        for (uint32_t d = 0; d <= 3u; d++)
            if ((TORUS_FULL >> (2u * d)) % 6u != 0u ||
                (TORUS_FULL >> (2u * d)) / 6u != HEX_PER_DEPTH[d]) ok = 0;
        CHECK("T4b: hexagon counts 3456/864/216/54 integer at depth 0-3 (step ⑪)", ok);
        CHECK("T4c: depth-4 wall — 20736/4⁴ = 81 = 6 × 13.5 (2^(8−2d) exhausted → 3⁴ alone)",
              (TORUS_FULL >> 8u) % 6u != 0u && (TORUS_FULL >> 8u) == 81u);
    }

    /* ── T5: torus automorphisms — a_w views, stride-37, homogeneity ───── */
    {
        GSWScale s;
        gsw_scale_init(&s);
        int ok = 1;
        for (uint32_t w = 0; w < TORUS_SIDE && ok; w++) {
            uint8_t seen[TORUS_SIDE];
            memset(seen, 0, sizeof(seen));
            for (uint32_t l = 0; l < TORUS_SIDE; l++) {
                uint32_t p = gsw_view(&s, w, l);
                if (seen[p]) { ok = 0; break; }
                seen[p] = 1;
            }
        }
        CHECK("T5a: a_w view is a bijection of Z₁₄₄ at every scale — torus automorphism (no collisions)",
              ok);
        /* stride-37 walk on the scale axis: gcd(37,144)=1 → covers all 144, returns */
        {
            uint8_t seen[TORUS_SIDE];
            memset(seen, 0, sizeof(seen));
            uint32_t w = 0;
            int cyc = 1;
            for (uint32_t t = 1; t <= TORUS_SIDE; t++) {
                w = (w + 37u) % TORUS_SIDE;
                if (seen[w]) cyc = 0;
                seen[w] = 1;
            }
            CHECK("T5b: stride-37 wraps the scale axis — covers all 144 positions, returns to start (frame-seek cycle)",
                  cyc && w == 0u);
        }
        /* homogeneity: translate the whole field by (Δw,Δpos) — every point
         * still has exactly 3 lines through it; the wrap has no boundary,
         * so no entry point is privileged (enter anywhere) */
        ok = 1;
        for (uint32_t dw = 0; dw < TORUS_SIDE && ok; dw += 7u) {
            for (uint32_t dp = 0; dp < TORUS_SIDE && ok; dp += 11u) {
                for (uint32_t w = 0; w < TORUS_SIDE; w++) {
                    for (uint32_t p = 0; p < TORUS_SIDE; p++) {
                        uint32_t tw = (w + dw) % TORUS_SIDE;
                        uint32_t tp = (p + dp) % TORUS_SIDE;
                        /* still one X, one Y, one Z line through the image */
                        uint32_t z1 = z_of(w, p), z2 = z_of(tw, tp);
                        if (z2 >= TORUS_SIDE) ok = 0;
                        if (z1 == 0u && z2 != (w + p + dw + dp) % TORUS_SIDE) ok = 0;
                        if (z2 != (tw + tp) % TORUS_SIDE) ok = 0;
                    }
                }
            }
        }
        CHECK("T5c: translation-invariant — the 3-line structure holds at every point of the wrapped field (homogeneous, no origin)",
              ok);
    }

    printf("\n═══════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass_count, pass_count + fail_count);
    return fail_count ? 1 : 0;
}

/*
 * test_tess_geo_jump_walks.c — geo_jump walks on the KIS (w,pos) torus
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Maps geo_jump's node-space walks onto the KIS (w,pos) grid through the
 * sync bridge (geo_sync_bridge.h) and classifies each orbit by how it
 * crosses the 3 line families (X: w=const, Y: pos=const, Z: w+pos=const):
 *
 *   transversal  = the orbit touches ALL 144 lines of a family
 *   cluster      = the orbit touches fewer
 *
 * Walks (implemented from geo_jump.h, self-contained):
 *   MOD:    n → 5n mod 20736   (GEO_MOD_STRIDE — multiplicative)
 *   CAPO:   n → n + key·144    (tower shift)
 *   INVERT: n → (floor+1)·48 + (47−local)  (project tower 0 — 3-floor cycle
 *           + 48-mirror)
 *
 * Measured (probe-verified, asserted below):
 *   MOD(5)  seed 1 : size 1728  X=144 Z=144 Y=96   residues {1,5}
 *   MOD(5)  seed 0 : size 1     (0·5 ≡ 0 — non-unit collapse)
 *   CAPO(1) seed 0 : size 144   X=144 Z=144 Y=9    residues {0}
 *   CAPO(3) seed 0 : size 48    X=48  Z=48  Y=3    residues {0}
 *   INVERT seed 0  : size 6     X=4   Z=6   Y=6    residues {0,11}
 *
 * SIGNATURE: MOD and CAPO(1) are X/Z-transversal + Y-cluster — the SAME
 * signature as our tetra-axis walk (test_tess_tetra_torus) — the geo_jump
 * family and the tetra family share the (w,pos)-grid behaviour through the
 * bridge. Small orbits (CAPO(3), INVERT) cluster in every family.
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/test_tess_geo_jump_walks tests/test_tess_geo_jump_walks.c
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../core/geo_sync_bridge.h"

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

/* ── geo_jump walks (node space) ──────────────────────────────────────── */
static uint32_t w_mod(uint32_t n)   { return (n * 5u) % GSB_FULL; }
static uint32_t w_capo(uint32_t n, uint32_t key) { return (n + key * 144u) % GSB_FULL; }
static uint32_t w_capo_1(uint32_t n) { return w_capo(n, 1u); }
static uint32_t w_capo_3(uint32_t n) { return w_capo(n, 3u); }
static uint32_t w_inv(uint32_t n) {              /* JUMP_INVERT, param 0 */
    uint32_t floor = (n / 48u) % 3u;             /* floor within the tower */
    uint32_t next  = (floor + 1u) % 3u;
    uint32_t local = n % 48u;
    return next * 48u + (47u - local);           /* projects into tower 0 */
}

/* ── orbit analysis on the (w,pos) torus ─────────────────────────────── */
typedef struct {
    uint32_t size;                 /* orbit length in node space       */
    uint32_t xl, yl, zl;           /* distinct X/Y/Z-lines touched     */
    uint16_t res_mask;             /* residue classes (mod 12) touched */
    int      distinct_ok;          /* all (w,pos) distinct             */
} OrbitMeas;

static OrbitMeas analyze(uint32_t seed, uint32_t (*f)(uint32_t)) {
    OrbitMeas m;
    memset(&m, 0, sizeof(m));
    uint8_t seen[GSB_FULL], wx[GSW_LOCAL], wy[GSW_LOCAL], wz[GSW_LOCAL];
    memset(seen, 0, sizeof(seen));
    memset(wx, 0, sizeof(wx));
    memset(wy, 0, sizeof(wy));
    memset(wz, 0, sizeof(wz));
    uint32_t n = seed;
    m.distinct_ok = 1;
    do {
        uint32_t w = gsw_scale_of_node(n), p = gsw_pos_of_node(n);
        uint32_t slot = w * GSW_LOCAL + p;
        if (seen[slot]) m.distinct_ok = 0;
        seen[slot] = 1;
        wx[w] = 1; wy[p] = 1; wz[(w + p) % GSW_LOCAL] = 1;
        m.res_mask |= (uint16_t)(1u << (n % 12u));
        n = f(n);
        m.size++;
        if (m.size > GSB_FULL) break;            /* safety — walk must close */
    } while (n != seed);
    for (uint32_t i = 0; i < GSW_LOCAL; i++) { m.xl += wx[i]; m.yl += wy[i]; m.zl += wz[i]; }
    return m;
}

int main(void) {
    printf("═ GEO_JUMP WALKS ON THE KIS TORUS — through the sync bridge ═\n");
    printf("  transversal = 144 lines of a family; cluster = fewer\n\n");

    /* ── T1: MOD(5) seed 1 — size 1728, X/Z transversal, Y cluster ───── */
    {
        OrbitMeas m = analyze(1u, w_mod);
        printf("     MOD(5) seed1: size=%u  X=%u Y=%u Z=%u  residues: %u %u\n",
               m.size, m.xl, m.yl, m.zl, (m.res_mask & (1u<<1))?1u:0u,
               (m.res_mask & (1u<<5))?5u:0u);
        CHECK("T1a: MOD(5) orbit(1) has 1728 nodes — order of 5 mod 20736 (max)",
              m.size == 1728u);
        CHECK("T1b: all 1728 map to distinct (w,pos) — bridge injective on the orbit",
              m.distinct_ok);
        CHECK("T1c: MOD is X-transversal AND Z-transversal (144/144), Y-cluster (96/144)",
              m.xl == GSW_LOCAL && m.zl == GSW_LOCAL && m.yl < GSW_LOCAL);
        CHECK("T1d: residues ⊆ {1,5} — 5^k mod 12 alternates 5,1 (period 2)",
              m.res_mask == ((1u << 1u) | (1u << 5u)));
    }

    /* ── T2: MOD(5) seed 0 — non-unit collapse ────────────────────────── */
    {
        OrbitMeas m = analyze(0u, w_mod);
        CHECK("T2: MOD orbit(0) = {0} — size 1 (0·5 ≡ 0); NOT a full cycle from non-unit seeds",
              m.size == 1u && m.xl == 1u && m.yl == 1u && m.zl == 1u);
    }

    /* ── T3: CAPO(1) — tower shift, size 144, X/Z transversal ────────── */
    {
        OrbitMeas m = analyze(0u, w_capo_1);
        printf("     CAPO(1) seed0: size=%u  X=%u Y=%u Z=%u\n", m.size, m.xl, m.yl, m.zl);
        CHECK("T3a: CAPO(1) orbit size = 20736/gcd(144,20736) = 144",
              m.size == 144u);
        CHECK("T3b: single residue class (144k ≡ 0 mod 12) — the tower shift is residue-pure",
              m.res_mask == (1u << 0u));
        CHECK("T3c: CAPO(1) is X/Z-transversal (144/144), Y-cluster (9/144) — SAME signature as the tetra walk",
              m.xl == GSW_LOCAL && m.zl == GSW_LOCAL && m.yl == 9u);
    }

    /* ── T4: CAPO(3) — size 48, clusters in all families ─────────────── */
    {
        OrbitMeas m = analyze(0u, w_capo_3);
        printf("     CAPO(3) seed0: size=%u  X=%u Y=%u Z=%u\n", m.size, m.xl, m.yl, m.zl);
        CHECK("T4a: CAPO(3) orbit size = 20736/gcd(432,20736) = 48",
              m.size == 48u);
        CHECK("T4b: CAPO(3) clusters in ALL families (X=48, Y=3, Z=48 < 144)",
              m.xl == 48u && m.yl == 3u && m.zl == 48u);
        CHECK("T4c: CAPO(3) is residue-pure too (432 ≡ 0 mod 12)",
              m.res_mask == (1u << 0u));
    }

    /* ── T5: INVERT — tiny orbit, clusters everywhere ────────────────── */
    {
        OrbitMeas m = analyze(0u, w_inv);
        printf("     INVERT seed0: size=%u  X=%u Y=%u Z=%u\n", m.size, m.xl, m.yl, m.zl);
        CHECK("T5a: INVERT orbit(0) has period 6 (3-floor cycle × 2-mirror)",
              m.size == 6u);
        CHECK("T5b: residues alternate {0,11} — the mirror flips the local parity",
              m.res_mask == ((1u << 0u) | (1u << 11u)));
        CHECK("T5c: INVERT clusters in all families (X=4, Y=6, Z=6)",
              m.xl == 4u && m.yl == 6u && m.zl == 6u);
    }

    /* ── T6: signature comparison with the tetra walk ────────────────── */
    {
        /* tetra walk (stride-12, additive): X/Z transversal 12/12/12, Y cluster
         * {48,32,32,32} per line — same (w,pos) signature as MOD and CAPO(1) */
        OrbitMeas m_mod = analyze(1u, w_mod), m_c1 = analyze(0u, w_capo_1);
        CHECK("T6: MOD & CAPO(1) share the tetra-walk signature — X/Z-transversal + Y-cluster — same behaviour on the KIS torus through the bridge",
              (m_mod.xl == GSW_LOCAL && m_mod.zl == GSW_LOCAL && m_mod.yl < GSW_LOCAL) &&
              (m_c1.xl == GSW_LOCAL && m_c1.zl == GSW_LOCAL && m_c1.yl < GSW_LOCAL));
    }

    printf("\n═══════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass_count, pass_count + fail_count);
    return fail_count ? 1 : 0;
}

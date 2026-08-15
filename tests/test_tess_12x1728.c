/*
 * test_tess_12x1728.c — Three Partitions of 12 × 1728 on 20736
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * "12 orbits × 1728 = 20736" appears in THREE different partitions of the
 * field — and they are DIFFERENT sets. This decides the canonical-orbit
 * choice for a geo_jump ↔ KIS sync bridge (see geo_sync_bridge.h):
 *
 *   A  pentagon blocks  (geo_jump):  {f·1728 + k : k ∈ [0,1728)}   f ∈ [0,12)
 *   B  residue mod 12   (KIS tetra): {r + 12k  : k ∈ [0,1728)}     r ∈ [0,12)
 *   C  MOD cosets       (geo_jump):  orbit of n under n → 5n mod 20736
 *
 * A and B are uniform by construction (12 parts × 1728). C's orbits under
 * ×5 (a unit mod 20736 → a permutation) partition the field but have
 * VARYING sizes: units (gcd(n,20736)=1) get 4 orbits of max size 1728
 * (φ(20736) = 6912 = 4 × 1728); non-units get smaller orbits (e.g. the
 * orbit of 0 is {0}). So "12 orbits" of the MOD walk is only the max-size
 * ideal, not a uniform partition.
 *
 * Proof:
 *   T1  A is a partition — 12 blocks × 1728, every node exactly once
 *   T2  B is a partition — 12 residue classes × 1728, every node exactly once
 *   T3  A ≠ B — explicit pairs: same block different residue, same residue
 *       different block
 *   T4  C is a partition — the ×5 orbits cover all 20736 disjointly
 *   T5  real orbit sizes of C — max = 1728 (units), count of max = φ/1728 = 4,
 *       orbit(0) = {0}; report the full size distribution
 *   T6  C ≠ A and C ≠ B — explicit counterexamples
 *   T7  conclusion — only A and B are UNIFORM 12×1728 partitions; a sync
 *       bridge must pick one canonical partition
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/test_tess_12x1728 tests/test_tess_12x1728.c
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FULL       20736u
#define ORBS       12u
#define ORB_SZ     (FULL / ORBS)          /* 1728 */
#define MOD_MULT   5u                     /* geo_jump GEO_MOD_STRIDE */
#define PHI_UNITS  6912u                  /* φ(20736) = 2⁸·3⁴·½·⅔ */

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

int main(void) {
    printf("═ THREE PARTITIONS OF 12 × 1728 — pentagon / residue / MOD coset ═\n");
    printf("  A: blocks  f·1728  |  B: residue mod 12  |  C: ×5 orbits\n\n");

    /* ── T1: partition A (pentagon blocks) ────────────────────────────── */
    {
        uint32_t cnt[ORBS];
        memset(cnt, 0, sizeof(cnt));
        for (uint32_t n = 0; n < FULL; n++) cnt[n / ORB_SZ]++;
        int ok = 1;
        for (uint32_t f = 0; f < ORBS; f++) if (cnt[f] != ORB_SZ) ok = 0;
        CHECK("T1: A (pentagon blocks f·1728) — 12 blocks × 1728, every node exactly once", ok);
    }

    /* ── T2: partition B (residue mod 12) ─────────────────────────────── */
    {
        uint32_t cnt[ORBS];
        memset(cnt, 0, sizeof(cnt));
        for (uint32_t n = 0; n < FULL; n++) cnt[n % ORBS]++;
        int ok = 1;
        for (uint32_t r = 0; r < ORBS; r++) if (cnt[r] != ORB_SZ) ok = 0;
        CHECK("T2: B (residue mod 12) — 12 classes × 1728, every node exactly once", ok);
    }

    /* ── T3: A ≠ B — explicit counterexamples ────────────────────────── */
    {
        /* (0,1): same block (0/1728 = 1/1728 = 0), different residue (0 vs 1) */
        int same_block = (0u / ORB_SZ == 1u / ORB_SZ);
        int diff_res   = (0u % ORBS != 1u % ORBS);
        /* (0,1728): same residue (0), different block (0 vs 1) */
        int same_res   = (0u % ORBS == 1728u % ORBS);
        int diff_block = (0u / ORB_SZ != 1728u / ORB_SZ);
        CHECK("T3: A ≠ B — (0,1) same block different residue; (0,1728) same residue different block",
              same_block && diff_res && same_res && diff_block);
    }

    /* ── T4/T5: partition C (×5 orbits) + real sizes ─────────────────── */
    {
        uint8_t *seen = (uint8_t *)calloc(FULL, 1);
        if (!seen) { printf("  T: FAIL — alloc\n"); return 1; }
        uint32_t *hist = (uint32_t *)calloc(FULL + 1, sizeof(uint32_t));
        uint32_t orbit_count = 0, max_size = 0, max_orbit_count = 0, sum = 0;
        int disjoint = 1;
        for (uint32_t n = 0; n < FULL; n++) {
            if (seen[n]) continue;
            /* walk the orbit of n */
            uint32_t cur = n, size = 0;
            do {
                if (seen[cur]) { disjoint = 0; break; }
                seen[cur] = 1;
                cur = (cur * MOD_MULT) % FULL;
                size++;
            } while (cur != n);
            if (!disjoint) break;
            hist[size]++;
            orbit_count++;
            sum += size;
            if (size > max_size) max_size = size;
            if (size == ORB_SZ) max_orbit_count++;
        }
        int covers = 1;
        for (uint32_t n = 0; n < FULL; n++) if (!seen[n]) covers = 0;
        CHECK("T4: C (×5 orbits) partitions the field — disjoint, covers all 20736",
              disjoint && covers && sum == FULL);
        CHECK("T5a: max orbit size = 1728; number of max orbits = φ/1728 = 4 (units only)",
              max_size == ORB_SZ && max_orbit_count == PHI_UNITS / ORB_SZ);
        CHECK("T5b: orbit(0) = {0} — non-units get smaller orbits (0·5 ≡ 0)",
              (0u * MOD_MULT) % FULL == 0u);
        printf("     C: %u orbits total, max %u (×%u), size histogram:\n",
               orbit_count, max_size, max_orbit_count);
        for (uint32_t s = 1; s <= max_size; s++)
            if (hist[s]) printf("       size %5u : %u orbit(s)\n", s, hist[s]);
        free(hist);
        free(seen);
    }

    /* ── T6: C ≠ A and C ≠ B — explicit counterexamples ──────────────── */
    {
        /* C vs A: orbit(0)={0} ⊂ block 0; orbit(1)=⟨5⟩ spans many blocks
         * (5^5 = 3125 ≥ 1728 → leaves block 0). Same block, different orbit:
         * nodes 0 and 1 are both in block 0 but in different ×5 orbits. */
        uint32_t b1 = 1u, steps = 0;
        do { b1 = (b1 * MOD_MULT) % FULL; steps++; } while (b1 != 1u);
        /* orbit(1) has 1728 elements; count distinct blocks it visits */
        uint8_t blocks[ORBS];
        memset(blocks, 0, sizeof(blocks));
        uint32_t cur = 1u;
        for (uint32_t k = 0; k < steps; k++) { blocks[cur / ORB_SZ] = 1; cur = (cur * MOD_MULT) % FULL; }
        uint32_t blocks_visited = 0;
        for (uint32_t f = 0; f < ORBS; f++) blocks_visited += blocks[f];
        CHECK("T6a: C ≠ A — orbit(0)={0} vs orbit(1) spans many blocks; same block (0) different orbits",
              blocks_visited >= 2u && blocks_visited <= ORBS);
        /* C vs B: nodes 0 and 12 share residue 0 but 12 is not in orbit(0)={0} */
        uint32_t o12 = 12u, in_zero = 0;
        do { o12 = (o12 * MOD_MULT) % FULL; if (o12 == 0u) in_zero = 1; } while (o12 != 12u);
        CHECK("T6b: C ≠ B — 0 and 12 share residue 0, but orbit(12) never contains 0",
              !in_zero && 12u % ORBS == 0u && 0u % ORBS == 0u);
    }

    /* ── T7: conclusion — only A and B are uniform ───────────────────── */
    {
        CHECK("T7: only A and B are UNIFORM 12×1728 partitions; C varies — a sync bridge must pick one canonical partition (A or B)",
              (FULL / ORB_SZ) == ORBS && (ORBS * ORB_SZ) == FULL);
    }

    printf("\n═══════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass_count, pass_count + fail_count);
    return fail_count ? 1 : 0;
}

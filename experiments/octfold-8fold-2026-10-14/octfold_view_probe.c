/*
 * octfold_view_probe.c — 10th language candidate: OCTFOLD (8-fold ladder)
 * ════════════════════════════════════════════════════════════════════════
 * Origin: regolo54 2018 — 8-fold Islamic star (square+octagon, 4.8.8 tiling).
 * Derived ladder (docs/DWGL_8fold_ladder.md):
 *   r(n) = R0 * 2^(n/2)      (ratio sqrt2 per level, EXACT)
 *   type(n) = square if n even else octagon
 *   addr = (n+32)*8 + s      (depth | 3-bit sector)
 * Ring-24 embed (A2): 8-fold sector = STRIDE-3 decimation of the rim:
 *   ring tooth k = 3*s   <=>   s = 3*k mod 8   (3*3 = 9 = 1 mod 8, unit)
 *
 * Carrier view: the 60 RID slots unfold into the 8 sectors. Slot w belongs
 * to sector s(w) = 3*(w mod 8) mod 8 ; reading order is SECTOR-MAJOR with
 * depth (w/8) ascending inside each sector — the 8-fold "fan out".
 *
 * Gates (LANGUAGES.md G1-G5 here; G6 = tools/octfold_roundtrip.c):
 *   G1 bijection sweep (bitset)
 *   G2 inverse deterministic (fwd o inv = id = inv o fwd)
 *   G3 int-only (domain/range in [0,60))
 *   G4 independent oracles: hand head/tail + sector-count law + unit-mod-8
 *      identity + grammar sort property (never "expected from impl")
 *   G5 mutation (raw mod-8 relabel, drops stride-3) must turn suite red
 */
#define __USE_MINGW_ANSI_STDIO 1
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define SLOTS 60
#define SECT  8

/* ── THE grammar (pure int) ─────────────────────────────────────────────
 * 8-fold sector of a carrier slot via the ring-24 stride-3 embed. */
static int sector_of(int w) { return (3 * (w % SECT)) % SECT; }

static void build(int8_t *v) {
    int n = 0;
    for (int s = 0; s < SECT; s++)
        for (int w = 0; w < SLOTS; w++)
            if (sector_of(w) == s) v[n++] = (int8_t)w;
}
/* MUTATION: drop the stride-3 relabel (raw w%8) -> sector grouping changes */
static void build_mut(int8_t *v) {
    int n = 0;
    for (int s = 0; s < SECT; s++)
        for (int w = 0; w < SLOTS; w++)
            if ((w % SECT) == s) v[n++] = (int8_t)w;
}

static int failc = 0;
#define CHECK(name, cond) do { \
    if (cond) printf("  PASS %s\n", name); \
    else { printf("  FAIL %s\n", name); failc++; } } while (0)

int main(void) {
    printf("=== octfold_view_probe — 10th language: 8-fold ladder (sector-major unfold) ===\n");

    int8_t v[SLOTS], inv[SLOTS];
    build(v);

    printf("  view: [%d,%d,%d,%d,%d ... %d,%d,%d]\n",
           v[0], v[1], v[2], v[3], v[4], v[57], v[58], v[59]);

    /* G1: bijection */
    {
        uint8_t hit[SLOTS]; memset(hit, 0, sizeof(hit));
        int ok = 1, n = 0;
        for (int p = 0; p < SLOTS; p++) {
            if (hit[(uint8_t)v[p]]) ok = 0;
            hit[(uint8_t)v[p]] = 1;
        }
        for (int w = 0; w < SLOTS; w++) n += hit[w];
        CHECK("G1 bijection sweep 60/60", ok && n == SLOTS);
    }

    /* G2: inverse roundtrip both directions */
    {
        memset(inv, -1, sizeof(inv));
        for (int p = 0; p < SLOTS; p++) inv[v[p]] = (int8_t)p;
        int ok = 1;
        for (int p = 0; p < SLOTS; p++) if (inv[v[p]] != p) ok = 0;
        for (int w = 0; w < SLOTS; w++) if (v[inv[w]] != w) ok = 0;
        CHECK("G2 inverse deterministic (fwd o inv = id = inv o fwd)", ok);
    }

    /* G3: int-only grammar */
    {
        int ok = 1;
        for (int p = 0; p < SLOTS; p++)
            if (v[p] < 0 || v[p] >= SLOTS) ok = 0;
        CHECK("G3 pure-int grammar (domain/range in [0,60))", ok);
    }

    /* G4a: hand-computed head (sector 0 = slots with w%8==0) + first of sector 1 */
    CHECK("G4a hand-computed head [0,8,16,24,32] & v[8]==3",
          v[0] == 0 && v[1] == 8 && v[2] == 16 && v[3] == 24 && v[4] == 32
          && v[8] == 3);

    /* G4b: hand-computed tail (sector 7 = slots with w%8==5) */
    CHECK("G4b hand-computed tail [...,37,45,53]",
          v[57] == 37 && v[58] == 45 && v[59] == 53);

    /* G4c: sector-count law (hand-computed): 60 = 4*8 + 4*7 */
    {
        static const int want[SECT] = { 8, 8, 7, 8, 7, 7, 8, 7 };
        int cnt[SECT]; memset(cnt, 0, sizeof(cnt));
        for (int p = 0; p < SLOTS; p++) cnt[sector_of(v[p])]++;
        int ok = 1;
        for (int s = 0; s < SECT; s++) if (cnt[s] != want[s]) ok = 0;
        CHECK("G4c sector-count law {8,8,7,8,7,7,8,7}", ok);
    }

    /* G4d: number-theory oracle — 3 is a unit mod 8 (3*3=9=1), so the
     * stride-3 relabel r -> 3r is a bijection of Z8 (independent of impl) */
    {
        int ok = 1;
        uint8_t seen[SECT]; memset(seen, 0, sizeof(seen));
        for (int r = 0; r < SECT; r++) {
            int s = (3 * r) % SECT;
            if (seen[s]) ok = 0;
            seen[s] = 1;
        }
        CHECK("G4d stride-3 is a unit mod 8 (bijection of Z8)", ok);
    }

    /* G4e: grammar sort property — view is non-decreasing in sector, and
     * within a sector strictly increasing in w (depth order) */
    {
        int ok = 1;
        for (int p = 1; p < SLOTS; p++) {
            int sp = sector_of(v[p - 1]), sc = sector_of(v[p]);
            if (sc < sp) ok = 0;
            if (sc == sp && v[p] <= v[p - 1]) ok = 0;
        }
        CHECK("G4e sorted by sector then depth (unfold property)", ok);
    }

    /* G5: mutation (raw mod-8 relabel) must go red */
    {
        int8_t m[SLOTS]; build_mut(m);
        int red = (m[8] != v[8]) || (m[59] != v[59]);
        CHECK("G5 mutation (raw mod-8 relabel) turns suite red", red);
    }

    printf("\n%s (fail=%d)\n", failc == 0 ? "RESULT: ALL GREEN" : "RESULT: FAILED", failc);
    return failc ? 1 : 0;
}

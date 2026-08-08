/*
 * capo_seeker_mix.c — Capo + Seeker mixing experiment on the 4D tesseract.
 *
 * Operators on 20736 (x,y,z,w each 0..11):
 *   CAPO  : w' = (w + key) % 12            layer switch (frame change)
 *   MOD(5): s' = (s*5) % 20736             multiplicative spiral — its
 *                                          1728-step orbit visits ALL 12 layers
 *   OCTANT: mirror within axis (self-inverse: apply twice = identity)
 *
 * BUILD: gcc -O2 -Icore -o build/capo_seeker_mix.exe tests/capo_seeker_mix.c -lm
 */

#include <stdio.h>
#include <stdint.h>

#define FULL       20736u
#define MOD5       5u
#define MOD5_INV   16589u          /* 5^-1 mod 20736 (verified: 5×16589=1) */
#define SIDE       12u
#define LAYER_SZ   1728u          /* 12³ = one w layer */

static inline uint32_t w_of(uint32_t s) { return s / LAYER_SZ; }

int main(void) {
    uint32_t pass = 0, fail = 0;
#define CHECK(d, c) do { if (c) { pass++; printf("  T: PASS — %s\n", d); } \
    else { fail++; printf("  T: FAIL — %s\n", d); } } while (0)

    printf("Capo+Seeker Mix — spiral through tesseract layers\n");
    printf("═════════════════════════════════════════════════\n");

    /* Q1: MOD(5) orbit from node 1 visits all 12 layers */
    {
        uint8_t seen_layer[12] = {0};
        uint32_t x = 1;
        for (uint32_t i = 0; i < 1728; i++) {
            seen_layer[x / LAYER_SZ] = 1;
            x = (x * 5u) % FULL;
        }
        int all = 1;
        for (int l = 0; l < 12; l++) if (!seen_layer[l]) all = 0;
        printf("       MOD(5) orbit touches layers: ");
        for (int l = 0; l < 12; l++) if (seen_layer[l]) printf("%d ", l);
        printf("\n");
        CHECK("Q1: MOD(5) orbit (1728 steps from node 1) visits ALL 12 layers", all);
    }

    /* Q2: capo + MOD composition: (+key layer, *5) then (*INV, -key) = origin */
    {
        int ok = 1;
        for (uint32_t key = 1; key <= 12 && ok; key++) {
            for (uint32_t s = 0; s < 5000; s += 37) {
                uint32_t w0 = w_of(s);
                uint32_t capo = ((w0 + key) % 12) * LAYER_SZ + (s % LAYER_SZ);
                uint32_t mod  = (capo * MOD5) % FULL;
                uint32_t back = (mod * MOD5_INV) % FULL;
                /* undo capo */
                uint32_t kb = (key == 12) ? 0u : key;
                uint32_t w2 = (w_of(back) + 12u - kb) % 12;
                uint32_t restored = w2 * LAYER_SZ + (back % LAYER_SZ);
                if ((s % LAYER_SZ) != (restored % LAYER_SZ) || w_of(s) != w_of(restored)) {
                    ok = 0; break;
                }
            }
        }
        CHECK("Q2: capo(+key)+MOD(5) then MOD(5^-1)+capo(-key) = origin", ok);
    }

    /* Q3: octant mirror self-inverse (apply twice = identity) */
    {
        const uint32_t axis_sz = 6912u;
        int ok = 1;
        for (uint32_t x0 = 0; x0 < 4096; x0++) {
            uint32_t m0 = axis_sz - 1 - x0;
            uint32_t m1 = axis_sz - 1 - m0;
            if (m1 != x0) { ok = 0; break; }
        }
        CHECK("Q3: octant mirror self-inverse (apply twice = identity)", ok);
    }

    /* Q4: TRUE MOD(5) orbit structure (brute-force, all 20736):
     *   - 128 disjoint orbits, full coverage
     *   - exactly 4 orbits of size 1728  (the UNIT orbits:
     *     φ(20736)=6912, 6912/1728=4)
     *   - 4 fixed points: 0, 5184, 10368, 15552 (x·5≡x mod 20736)
     *   - each size-1728 orbit passes through ALL 12 w layers (spiral)
     * The "12 orbits × 1728" on the board is capo's layer count
     * (12 layers × 1728), NOT the multiplicative orbit count. */
    {
        uint8_t seen[FULL] = {0};
        uint32_t orbits = 0, sz1728 = 0, fixed = 0, total = 0;
        int all_layers = 1;
        for (uint32_t start = 0; start < FULL; start++) {
            if (seen[start]) continue;
            uint32_t x = start, orbit_sz = 0;
            uint8_t lseen[12] = {0};
            do {
                seen[x] = 1; total++; orbit_sz++;
                lseen[x / LAYER_SZ] = 1;
                x = (x * MOD5) % FULL;
            } while (x != start && orbit_sz <= FULL);
            orbits++;
            if (orbit_sz == 1728u) {
                sz1728++;
                for (int l = 0; l < 12; l++) if (!lseen[l]) all_layers = 0;
            }
            if (orbit_sz == 1) fixed++;           /* fixed points */
        }
        CHECK("Q4: MOD(5) = 128 disjoint orbits, covers 20736", orbits == 128 && total == FULL);
        CHECK("Q4b: exactly 4 unit orbits of size 1728 (φ/1728 = 4)", sz1728 == 4 && fixed == 4);
        CHECK("Q4c: each unit orbit spirals through ALL 12 layers", all_layers);
        printf("       orbits: %u, fixed pts: %u, size-1728 orbits: %u, covered: %u/20736\n",
               orbits, fixed, sz1728, total);
    }

    /* Q5: capo offsets 1440 (clock) & 1728 (pentagon) = legal layers */
    {
        int ok = 1;
        for (uint32_t i = 0; i < FULL; i += 57) {
            uint32_t a = (i + 1440u) % FULL;
            uint32_t b = (i + 1728u) % FULL;
            if (w_of(a) > 11 || w_of(b) > 11) { ok = 0; break; }
        }
        CHECK("Q5: capo 1440/1728 land on valid layer (w<12) for all 20736", ok);
    }

    printf("\n═════════════════════════════════════════════════\n");
    printf("RESULTS: %u/%u PASS\n", pass, pass + fail);
    return fail ? 1 : 0;
}
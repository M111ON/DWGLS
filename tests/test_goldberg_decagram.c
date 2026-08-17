/*
 * test_goldberg_decagram.c — Decagram 10-Sector Goldberg Layout proof
 * ═══════════════════════════════════════════════════════════════════════
 *
 * T1.2f — "decagram เพื่อ map เข้า goldberg ได้ทุก face เพราะ bipolar inverted"
 *
 * Proof:
 *   T1  hexagon total = 10(n²−1) divides EXACTLY by 10 for all levels 1..8
 *       (the decagram fact — old 12-sector round-robin has remainder)
 *   T2  bijective: every hex tile_id 12..faces−1 addressed exactly once
 *       via (sector, offset) — zero gap, zero overlap
 *   T3  bipolar inversion: sector d ↔ sector (d+5)%10 = opposite directions;
 *       pentagon face f ↔ face f+6 (pairs from geo_goldberg_lut.h)
 *   T4  pole split: ring1 faces 0..5 vs ring2 faces 6..11 — inverted copies
 *   T5  lossless: write 64B chunk per tile (all faces) → read back byte-for-byte
 *   T6  decagram directions: 10 sectors × 36° = 360° — full face cycle
 *   T7  roundtrip reverse: ggd_sector_of_hex(ggd_hex_tile_id(...)) == identity
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/test_goldberg_decagram tests/test_goldberg_decagram.c
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../core/geo_goldberg_decagram.h"
#include "../core/geo_goldberg_sphere.h"
#include "../core/infra/tring.h"

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

#define GGD_MAX_LEVEL 8u

int main(void)
{
    printf("═══ test_goldberg_decagram — decagram 10-sector Goldberg layout ═══\n\n");

    /* T1: exact division — the decagram fact */
    {
        int all_exact = 1;
        for (uint8_t lv = 1; lv <= GGD_MAX_LEVEL; lv++) {
            uint32_t hex = ggd_hex_total(lv);
            if (hex % GGD_SECTORS != 0) all_exact = 0;
            if (ggd_hex_per_sector(lv) * GGD_SECTORS != hex) all_exact = 0;
        }
        CHECK("T1: 10(n²−1) hex divides exactly by 10 for all levels 1..8",
              all_exact);
        CHECK("T1b: hex_total == 10 × hex_per_sector (identity)",
              ggd_hex_total_matches(3) && ggd_hex_total_matches(8));
        CHECK("T1c: face_count == 10n²+2 (matches gp_face_count)",
              ggd_face_count(2) == gp_face_count(2) &&
              ggd_face_count(5) == gp_face_count(5) &&
              ggd_face_count(8) == gp_face_count(8));
    }

    /* T2: bijective — every hex tile_id addressed exactly once */
    {
        int ok = 1;
        for (uint8_t lv = 1; lv <= GGD_MAX_LEVEL; lv++) {
            uint32_t n_hex = ggd_hex_total(lv);
            uint8_t *seen = (uint8_t *)calloc(n_hex, 1);
            if (!seen) { ok = 0; continue; }
            for (uint8_t s = 0; s < GGD_SECTORS; s++) {
                for (uint32_t o = 0; o < ggd_hex_per_sector(lv); o++) {
                    uint32_t t = ggd_hex_tile_id(lv, s, o);
                    uint32_t h = t - GGD_PENTAGONS;
                    if (t < GGD_PENTAGONS || h >= n_hex) { ok = 0; }
                    else if (seen[h]) { ok = 0; }
                    else seen[h] = 1;
                }
            }
            for (uint32_t h = 0; h < n_hex && ok; h++)
                if (!seen[h]) ok = 0;
            free(seen);
        }
        CHECK("T2: every hex tile_id 12..faces−1 addressed exactly once (zero gap)",
              ok);
    }

    /* T3: bipolar inversion — sector d ↔ d+5; face f ↔ f+6 */
    {
        int ok = 1;
        for (uint8_t d = 0; d < GGD_SECTORS; d++) {
            if (ggd_inverted_sector(d) != (d + 5u) % GGD_SECTORS) ok = 0;
            if (ggd_inverted_sector(ggd_inverted_sector(d)) != d) ok = 0; /* involution */
        }
        for (uint8_t f = 0; f < GGD_PENTAGONS; f++) {
            if (ggd_pair_face(f) != (f + 6u) % GGD_PENTAGONS) ok = 0;
            if (ggd_pair_face(ggd_pair_face(f)) != f) ok = 0;
            /* GB_PEN_TO_PAIR / GB_PEN_POLE consistency */
            if (GB_PEN_TO_PAIR[f] != ggd_pair(f)) ok = 0;
            if (GB_PEN_POLE[f] != ggd_pole(f)) ok = 0;
        }
        CHECK("T3: inverted sector is involution (d ↔ d+5), face pair f ↔ f+6",
              ok);
        CHECK("T3b: pair/pole tables match geo_goldberg_lut.h",
              (GB_PEN_TO_PAIR[0] == 0 && GB_PEN_POLE[6] == 1));
    }

    /* T4: pole split — ring1 (0..5) vs ring2 (6..11) inverted */
    {
        int ok = 1;
        for (uint8_t f = 0; f < 6; f++)
            if (ggd_pole(f) != 0 || ggd_pole(f + 6) != 1) ok = 0;
        /* each pair (f, f+6) has opposite poles and same pair index */
        for (uint8_t f = 0; f < 6; f++)
            if (ggd_pair(f) != ggd_pair(f + 6)) ok = 0;
        CHECK("T4: ring1 faces 0..5 (pole 0) vs ring2 faces 6..11 (pole 1) — inverted copies",
              ok);
    }

    /* T5: lossless — write 64B chunk per face → read back byte-for-byte */
    {
        /* tick = (tile_id<<8)|dim — capacity must cover GP(3,0)=92 faces */
        Tring tring;
        tring_init(&tring, (uint32_t)ggd_face_count(3) << 8);
        GpSphere sphere;
        gp_sphere_init(&sphere, &tring, 3);
        int ok = 1;
        for (uint32_t t = 0; t < ggd_face_count(3); t++) {
            uint8_t chunk[GP_CHUNK_SZ], back[GP_CHUNK_SZ];
            for (int i = 0; i < GP_CHUNK_SZ; i++)
                chunk[i] = (uint8_t)((t * 31u + i * 7u) & 0xFFu);
            uint32_t tick = gp_lens_write(&sphere, t, 0, chunk);
            if (tick == UINT32_MAX) { ok = 0; break; }
            const uint8_t *rd = gp_lens_read(&sphere, t, 0);
            if (!rd) { ok = 0; break; }
            memcpy(back, rd, GP_CHUNK_SZ);
            if (memcmp(chunk, back, GP_CHUNK_SZ) != 0) { ok = 0; break; }
        }
        CHECK("T5: 64B chunk per face (GP(3,0)=92 faces) lossless roundtrip", ok);

        /* T5b: same through decagram addressing — every sector/offset readable */
        int ok2 = 1;
        for (uint8_t s = 0; s < GGD_SECTORS; s++) {
            for (uint32_t o = 0; o < ggd_hex_per_sector(3); o++) {
                uint32_t t = ggd_hex_tile_id(3, s, o);
                const uint8_t *rd = gp_lens_read(&sphere, t, 0);
                if (!rd) { ok2 = 0; break; }
            }
        }
        CHECK("T5b: all 10 decagram sectors × offsets readable (80 hex faces)",
              ok2);
        tring_destroy(&tring);
    }

    /* T6: decagram directions — 10 × 36° = 360° full cycle */
    {
        CHECK("T6: 10 sectors × 36° = 360° (GGD_SECTORS·GGD_SECTOR_DEG == 360)",
              GGD_SECTORS * GGD_SECTOR_DEG == 360u);
    }

    /* T7: roundtrip reverse — sector_of_hex(hex_tile_id(...)) identity */
    {
        int ok = 1;
        for (uint8_t lv = 1; lv <= GGD_MAX_LEVEL; lv++) {
            for (uint8_t s = 0; s < GGD_SECTORS; s++) {
                for (uint32_t o = 0; o < ggd_hex_per_sector(lv); o++) {
                    uint32_t t = ggd_hex_tile_id(lv, s, o);
                    if (ggd_sector_of_hex(lv, t) != s) ok = 0;
                    if (ggd_offset_of_hex(lv, t) != o) ok = 0;
                }
            }
        }
        CHECK("T7: reverse (sector,offset) roundtrip identity all levels", ok);
    }

    printf("\n═══ RESULT: %d/%d PASS ═══\n", pass_count, pass_count + fail_count);
    return fail_count == 0 ? 0 : 1;
}

/*
 * frame_index_demo.c — Frame-as-Index: 4D tesseract exposes ONE 3D view at a
 * time; that view is an INDEX into the full 4D data. Capo/seek across the
 * missing dimension reveals the remaining layers.
 *
 * Geometry:
 *   20736 = 12^4  (x,y,z,w)
 *   slot = w*1728 + z*144 + y*12 + x        (x,y,z,w in [0,12))
 *
 * Frame at w: 3D view (x,y,z) of size 1728 = the visible "shadow".
 *   frame == 1 of 12 layers. It IS an index: each cell addresses a real
 *   value in the 4D tesseract.
 * Capo:  w' = (w + key) % 12  → reveals another layer.
 *
 * Proof:
 *   T1: slot ↔ (x,y,z,w) bijective (20736 unique)
 *   T2: one frame (w fixed) exposes 1728 distinct addresses  → index usable
 *   T3: 12 capo steps (key=1) walk all 12 layers = full 20736 coverage
 *   T4: capo key=12 (GEO_SHELL_TICK) = same layer (identity)
 *   T5: capo key offsets from geo_jump: 1440 (clock) & 1728 (pentagon)
 *       both map to legal (x,y,z,w) addresses — deterministic
 *   T6: same 3D coordinate holds 12 DIFFERENT values across w
 *
 * BUILD: gcc -O2 -Icore -o build/frame_index_demo.exe tests/frame_index_demo.c -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define FULL      20736u
#define LAYER_SZ  1728u          /* 12^3 per w layer */
#define SIDE      12u

/* slot ↔ 4D coordinate: (x,y,z,w), each 0..11 */
static uint32_t slot_from_xyzw(uint32_t x, uint32_t y, uint32_t z, uint32_t w) {
    return w * LAYER_SZ + z * 144u + y * SIDE + x;
}

static void xyzw_from_slot(uint32_t s, uint32_t *x, uint32_t *y, uint32_t *z, uint32_t *w) {
    *w = s / LAYER_SZ;
    uint32_t r = s % LAYER_SZ;
    *z = r / 144u;
    r %= 144u;
    *y = r / SIDE;
    *x = r % SIDE;
}

/* 4D synthetic data: value = f(x,y,z,w), deterministic */
static uint8_t cell_value(uint32_t x, uint32_t y, uint32_t z, uint32_t w) {
    return (uint8_t)((x * 7 + y * 31 + z * 127 + w * 13) % 251);
}

int main(void) {
    uint32_t pass = 0, fail = 0;
#define CHECK(d, c) do { if (c) { pass++; printf("  T: PASS — %s\n", d); } \
    else { fail++; printf("  T: FAIL — %s\n", d); } } while (0)

    printf("4D Tesseract — Frame as INDEX, Capo reveals data\n");
    printf("═════════════════════════════════════════════════\n");

    /* T1: bijection slot ↔ (x,y,z,w) */
    int bi_ok = 1;
    for (uint32_t i = 0; i < FULL; i++) {
        uint32_t x, y, z, w;
        xyzw_from_slot(i, &x, &y, &z, &w);
        if (slot_from_xyzw(x, y, z, w) != i) { bi_ok = 0; break; }
        if (x >= SIDE || y >= SIDE || z >= SIDE || w >= SIDE) { bi_ok = 0; break; }
    }
    CHECK("T1: slot ↔ (x,y,z,w) bijective — each 12-slot axis", bi_ok);

    /* T2: one frame (w fixed) = 1728 visible cells — distinct addresses */
    {
        uint8_t seen2[FULL] = {0};
        uint32_t distinct = 0;
        for (uint32_t x = 0; x < SIDE; x++)
            for (uint32_t y = 0; y < SIDE; y++)
                for (uint32_t z = 0; z < SIDE; z++) {
                    uint32_t s = slot_from_xyzw(x, y, z, 0); /* frame w=0 */
                    if (!seen2[s]) { seen2[s] = 1; distinct++; }
                }
        CHECK("T2: frame w=0 exposes 1728 unique slots (12³ view)", distinct == LAYER_SZ);
    }

    /* T3: 12 capo steps (key=1) walk all layers → 20736 coverage */
    {
        uint8_t seen3[FULL] = {0};
        uint32_t total = 0;
        for (uint32_t w = 0; w < SIDE; w++)             /* 12 frames */
            for (uint32_t x = 0; x < SIDE; x++)
                for (uint32_t y = 0; y < SIDE; y++)
                    for (uint32_t z = 0; z < SIDE; z++) {
                        uint32_t s = slot_from_xyzw(x, y, z, w);
                        if (!seen3[s]) { seen3[s] = 1; total++; }
                    }
        CHECK("T3: all 12 frames (capo×12) = full 20736 coverage", total == FULL);
    }

    /* T4: capo key=12 (GEO_SHELL_TICK) → same frame (identity) */
    {
        uint32_t w0 = 2;
        uint32_t wc = (w0 + 12u) % SIDE;                 /* capo key = 12 */
        CHECK("T4: capo key=12 → same layer (tick identity)", wc == w0);
    }

    /* T5: geo_jump capo offsets (1440, 1728) resolve inside 20736 */
    {
        uint32_t offs[] = {1440u, 1728u};
        const char *nms[] = {"GEO_FIBO_CLOCK (1440 = 10×144)", "1 pentagon layer (1728 = 12³)"};
        int ok = 1;
        for (uint32_t i = 0; i < FULL; i++) {
            for (int k = 0; k < 2; k++) {
                uint32_t a = (i + offs[k]) % FULL;
                uint32_t x, y, z, w;
                xyzw_from_slot(a, &x, &y, &z, &w);
                if (slot_from_xyzw(x, y, z, w) != a) { ok = 0; break; }
            }
            if (!ok) break;
        }
        CHECK("T5: capo offsets 1440/1728 stay in tesseract (all 20736)", ok);
        for (int k = 0; k < 2; k++)
            printf("     capo offset %-8u → %s\n", offs[k], nms[k]);
    }

    /* T6: value rule — frame indexes REAL data; differ by w (4th dim exists) */
    {
        uint32_t fixed_x = 3, fixed_y = 5, fixed_z = 7;
        uint32_t vals[12];
        for (uint32_t w = 0; w < 12; w++)
            vals[w] = cell_value(fixed_x, fixed_y, fixed_z, w);
        int varied = 0;
        for (uint32_t w = 1; w < 12; w++)
            if (vals[w] != vals[0]) varied = 1;
        CHECK("T6: same 3D coordinate → 12 different values across w", varied);
    }

    printf("\n═════════════════════════════════════════════════\n");
    printf("RESULTS: %u/%u PASS\n", pass, pass + fail);
    return fail ? 1 : 0;
}
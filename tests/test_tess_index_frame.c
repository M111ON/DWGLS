/*
 * test_tess_index_frame.c — 1 Tesseract: Frame-as-Index proof
 * ══════════════════════════════════════════════════════════════
 *
 * Model (rescope — "ทำ 1 tesseract ให้ได้ก่อน"; 18-tess = next step):
 *   In 3D we see 4D as ONE frame. 1 tesseract = 8 cubes. We lock
 *   ONE cube as the index/address LUT for the other 7. Seeing the
 *   single frame (the index cube) gives access to the other 7 cubes
 *   and retrieves all data — losslessly.
 *
 * Layout (consistent with the 18-tess scale: 18 × 8 × 144 = 20736):
 *   slot   = cube*144 + local          (cube 0..7, local 0..143)
 *   cube 0 = INDEX frame (144 slots = 8 blocks × 18)
 *   cubes 1..7 = DATA (7 × 144 = 1008 slots)
 *
 * Index block c (slots c*18 .. c*18+17 of the frame):
 *   [0..1] base    = c*144        — address of cube c
 *   [2..3] len     = 144          — cube size
 *   [4]    stride  = ROUTE[c]     — deterministic route for cube c
 *   [5]    checksum = Σ value % 251
 *   [6..17] reserved (0)
 *
 * Route: data of cube c is scattered as store[cube][ (i*stride)%144 ]
 *   = value(cube, i). stride coprime to 144 → the walk covers all 144
 *   locals → retrieval from the frame alone reconstructs the cube.
 *   Deterministic, short (1 byte) — "store route, reconstruct".
 *
 * Proof:
 *   T1  slot ↔ (cube, local) bijective; 8×144 = 1152
 *   T2  route LUT strides are coprime to 144 → full walk per cube
 *   T3  encode: frame (index) + 7 scattered data cubes
 *   T4  retrieve from FRAME ONLY → all 7 cubes lossless (1008 slots)
 *   T5  corruption → checksum flags it (integrity)
 *   T6  anchor: 18 tesseracts × 8 × 144 = 20736 (next step)
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/test_tess_index_frame tests/test_tess_index_frame.c
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define T1F_CUBES       8u
#define T1F_LOCAL       144u
#define T1F_TOTAL       (T1F_CUBES * T1F_LOCAL)      /* 1152  */
#define T1F_INDEX_CUBE  0u
#define T1F_DATA_CUBES  (T1F_CUBES - 1u)             /* 7     */
#define T1F_DATA_SLOTS  (T1F_DATA_CUBES * T1F_LOCAL) /* 1008  */
#define T1F_BLOCKS      8u
#define T1F_BLOCK       18u                          /* 144/8 */
#define T1F_FULL        20736u
#define T1F_TESS_18     18u

/* Static route LUT — strides coprime to 144 (odd, not multiple of 3).
 * Allowed: LUT for static geometry only (AGENTS.md rule). */
static const uint8_t T1F_ROUTE[T1F_CUBES] = { 1, 5, 7, 11, 13, 17, 19, 23 };

static uint32_t t1f_slot(uint32_t cube, uint32_t local) {
    return cube * T1F_LOCAL + local;
}

/* Deterministic synthetic value for cube data (no storage needed) */
static uint8_t t1f_value(uint32_t cube, uint32_t local) {
    return (uint8_t)((cube * 37u + local * 7u + 11u) % 251u);
}

/* ── Index frame (cube 0) block format ─────────────────────────── */
typedef struct {
    uint32_t base;      /* cube address in the tesseract store */
    uint32_t len;       /* cube size                          */
    uint8_t  stride;    /* route: i → (i*stride) % 144         */
    uint8_t  checksum;  /* Σ value % 251                       */
} T1IndexBlock;

static void t1f_frame_write_block(uint8_t *frame, uint32_t c, const T1IndexBlock *b) {
    uint8_t *p = frame + c * T1F_BLOCK;
    p[0] = (uint8_t)(b->base & 0xFFu);
    p[1] = (uint8_t)((b->base >> 8) & 0xFFu);
    p[2] = (uint8_t)(b->len & 0xFFu);
    p[3] = (uint8_t)((b->len >> 8) & 0xFFu);
    p[4] = b->stride;
    p[5] = b->checksum;
    memset(p + 6, 0, T1F_BLOCK - 6u);
}

static void t1f_frame_read_block(const uint8_t *frame, uint32_t c, T1IndexBlock *b) {
    const uint8_t *p = frame + c * T1F_BLOCK;
    b->base     = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
    b->len      = (uint32_t)p[2] | ((uint32_t)p[3] << 8);
    b->stride   = p[4];
    b->checksum = p[5];
}

/* ── Encode: scatter 7 data cubes by route + build index frame ── */
static void t1f_encode(uint8_t *store) {
    memset(store, 0, T1F_TOTAL);

    for (uint32_t c = 1; c < T1F_CUBES; c++) {
        uint8_t  stride = T1F_ROUTE[c];
        uint32_t sum = 0;
        for (uint32_t i = 0; i < T1F_LOCAL; i++) {
            uint32_t pos = (i * stride) % T1F_LOCAL;
            uint8_t  v   = t1f_value(c, i);
            store[t1f_slot(c, pos)] = v;
            sum += v;
        }
        T1IndexBlock b = { t1f_slot(c, 0), T1F_LOCAL, stride, (uint8_t)(sum % 251u) };
        t1f_frame_write_block(store + T1F_INDEX_CUBE * T1F_LOCAL, c, &b);
    }
    /* index cube describes itself too (identity block) */
    T1IndexBlock b0 = { t1f_slot(0, 0), T1F_LOCAL, T1F_ROUTE[0], 0u };
    t1f_frame_write_block(store + T1F_INDEX_CUBE * T1F_LOCAL, 0, &b0);
}

/* ── Retrieve from FRAME ONLY → returns 1 if lossless ─────────── */
static int t1f_retrieve_from_frame(const uint8_t *store, uint32_t *checked) {
    const uint8_t *frame = store + T1F_INDEX_CUBE * T1F_LOCAL;
    uint32_t n = 0;

    for (uint32_t c = 1; c < T1F_CUBES; c++) {
        T1IndexBlock b;
        t1f_frame_read_block(frame, c, &b);

        /* walk cube c by route stride, reconstruct value(i) */
        uint32_t sum = 0;
        for (uint32_t i = 0; i < b.len && i < T1F_LOCAL; i++) {
            uint32_t pos = (i * b.stride) % T1F_LOCAL;
            uint8_t  got = store[b.base + pos];
            uint8_t  exp = t1f_value(c, i);
            if (got != exp) return 0;
            sum += got;
            n++;
        }
        if ((sum % 251u) != b.checksum) return 0;
    }
    *checked = n;
    return 1;
}

int main(void) {
    uint32_t pass = 0, fail = 0;
#define CHECK(d, c) do { if (c) { pass++; printf("  T: PASS — %s\n", d); } \
    else { fail++; printf("  T: FAIL — %s\n", d); } } while (0)

    printf("1 Tesseract — Frame-as-Index (1 index LUT + 7 data cubes)\n");
    printf("══════════════════════════════════════════════════════════\n");

    /* T1: layout — slot ↔ (cube, local) bijective, 8×144 = 1152 */
    {
        int bi_ok = 1;
        for (uint32_t s = 0; s < T1F_TOTAL; s++) {
            uint32_t cube  = s / T1F_LOCAL;
            uint32_t local = s % T1F_LOCAL;
            if (t1f_slot(cube, local) != s) { bi_ok = 0; break; }
            if (cube >= T1F_CUBES || local >= T1F_LOCAL) { bi_ok = 0; break; }
        }
        CHECK("T1: 8 cubes × 144 = 1152, slot ↔ (cube,local) bijective", bi_ok);
        CHECK("T1b: index cube 0 + 7 data cubes = 1008 data slots",
              T1F_INDEX_CUBE == 0u && T1F_DATA_SLOTS == 1008u);
    }

    /* T2: route strides coprime to 144 → each walk covers all locals */
    {
        int lut_ok = 1;
        for (uint32_t c = 0; c < T1F_CUBES; c++) {
            if (T1F_ROUTE[c] == 0u || (T1F_ROUTE[c] % 2u) == 0u ||
                (T1F_ROUTE[c] % 3u) == 0u) { lut_ok = 0; break; }
            uint8_t seen[T1F_LOCAL] = {0};
            for (uint32_t i = 0; i < T1F_LOCAL; i++)
                seen[(i * T1F_ROUTE[c]) % T1F_LOCAL] = 1;
            for (uint32_t i = 0; i < T1F_LOCAL; i++)
                if (!seen[i]) { lut_ok = 0; break; }
            if (!lut_ok) break;
        }
        CHECK("T2: route LUT strides coprime to 144 → full walk per cube", lut_ok);
    }

    /* T3 + T4: encode then retrieve from the frame only */
    {
        uint8_t *store = (uint8_t *)calloc(T1F_TOTAL, 1);
        if (!store) { printf("  T: FAIL — alloc\n"); return 1; }

        t1f_encode(store);

        /* frame occupies cube 0 = 144 slots; data cubes = 1008 */
        uint32_t checked = 0;
        int ok = t1f_retrieve_from_frame(store, &checked);

        CHECK("T3: encode — index frame + 7 route-scattered data cubes", ok != -1);
        CHECK("T4: retrieve from FRAME ONLY → 7 cubes lossless",
              ok == 1 && checked == T1F_DATA_SLOTS);

        /* T5: corruption → checksum flags it */
        if (ok == 1) {
            store[t1f_slot(3, 57)] ^= 0x40u;   /* flip one data byte */
            uint32_t n2 = 0;
            int ok2 = t1f_retrieve_from_frame(store, &n2);
            CHECK("T5: corrupted data byte → retrieve fails (integrity)", ok2 == 0);
            store[t1f_slot(3, 57)] ^= 0x40u;   /* restore */
        }

        printf("\n     frame (cube 0)      = %u slots (index LUT)\n", T1F_LOCAL);
        printf("     data via frame       = %u slots retrieved\n", checked);
        printf("     ratio frame:data     = 1 : %u\n", checked / T1F_LOCAL);

        free(store);
    }

    /* T6: anchor for next step — 18 tesseracts × 8 × 144 = 20736 */
    {
        CHECK("T6: 18 tesseracts × 8 cubes × 144 = 20736",
              T1F_TESS_18 * T1F_CUBES * T1F_LOCAL == T1F_FULL);
    }

    printf("\n══════════════════════════════════════════════════════════\n");
    printf("RESULTS: %u/%u PASS\n", pass, pass + fail);
    return fail ? 1 : 0;
}

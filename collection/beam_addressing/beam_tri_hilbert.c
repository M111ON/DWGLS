/*
 * beam_tri_hilbert.c — Triangle Tower + Hilbert Tessellation
 *
 * Structure from geo_jump.h:
 *   tower (144) = 3 blocks (48) = 3 vertices of triangle
 *   block (48) = 3 floors (16) = 3 levels
 *   floor (16) = 4×4 metatron grid = Hilbert tessellation
 *
 * 3 towers at 3 vertices of triangle:
 *   vertex 0: block 0 (floors 0,1,2)
 *   vertex 1: block 1 (floors 0,1,2)
 *   vertex 2: block 2 (floors 0,1,2)
 *
 * Hilbert curve tessellates naturally on 4×4 grid.
 * Each tower gives 144 positions (3 blocks × 3 floors × 16 cells).
 *
 * Flow:
 *   weight → tessellation node (20736 grid)
 *   node → tower (0..143) → block (0..2) → floor (0..2) → metatron cell (0..15)
 *   Hilbert index on 4×4 grid → frame_seek tile
 *
 * Storage: tile per weight (11 bits for 1440 tiles)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* ══════════════════════════════════════════════════════════════
   CONSTANTS
   ══════════════════════════════════════════════════════════════ */

#define GEO_FULL        20736u
#define GEO_PENTAGONS   12u
#define GEO_TOWER       144u
#define GEO_SHELL_TICK  12u
#define GEO_FIBO_CLOCK  1440u
#define FACE_STRIDE     (GEO_FULL / GEO_PENTAGONS)  /* 1728 */

#define GEO_METATRON_COLS   4u
#define GEO_METATRON_ROWS   4u
#define GEO_METATRON_FLOORS 3u
#define GEO_METATRON_CELLS  (GEO_METATRON_COLS * GEO_METATRON_ROWS)  /* 16 */
#define GEO_BLOCK           (GEO_METATRON_CELLS * GEO_METATRON_FLOORS)  /* 48 */

#define FRAME_CYCLE     1440u
#define FRAME_STRIDE    37u

/* ══════════════════════════════════════════════════════════════
   HILBERT CURVE (from geo_jump.h)
   ══════════════════════════════════════════════════════════════ */

static inline uint32_t hilbert_idx(uint32_t x, uint32_t y, uint32_t n) {
    uint32_t d = 0;
    for (uint32_t s = n >> 1; s > 0; s >>= 1) {
        uint32_t rx = (x & s) > 0;
        uint32_t ry = (y & s) > 0;
        d = (d << 2) | (((uint32_t)(3u * rx)) ^ ry);
        if (ry == 0) {
            if (rx == 1) { x = n - 1u - x; y = n - 1u - y; }
            uint32_t t = x; x = y; y = t;
        }
    }
    return d;
}

/* ══════════════════════════════════════════════════════════════
   TESSELLATION (from geo_jump.h)
   ══════════════════════════════════════════════════════════════ */

static inline uint32_t tess_face(uint32_t node)  { return node / FACE_STRIDE; }
static inline uint32_t tess_shell(uint32_t node) { return (node / GEO_TOWER) % GEO_SHELL_TICK; }
static inline uint32_t tess_local(uint32_t node) { return node % GEO_TOWER; }
static inline uint32_t tess_node(uint32_t face, uint32_t shell, uint32_t local) {
    return face * FACE_STRIDE + shell * GEO_TOWER + local;
}

/* Tower decomposition: node → (tower, block, floor, cell) */
static inline uint32_t node_tower(uint32_t node) { return node / GEO_TOWER; }
static inline uint32_t node_block(uint32_t node) { return (node % GEO_TOWER) / GEO_BLOCK; }
static inline uint32_t node_floor(uint32_t node) { return ((node % GEO_TOWER) % GEO_BLOCK) / GEO_METATRON_CELLS; }
static inline uint32_t node_cell(uint32_t node)  { return (node % GEO_TOWER) % GEO_METATRON_CELLS; }

/* Recompose: (tower, block, floor, cell) → node */
static inline uint32_t tri_node(uint32_t tower, uint32_t block, uint32_t floor, uint32_t cell) {
    return tower * GEO_TOWER + block * GEO_BLOCK + floor * GEO_METATRON_CELLS + cell;
}

/* ══════════════════════════════════════════════════════════════
   FRAME SEEK (from geo_frame_seek.h)
   ══════════════════════════════════════════════════════════════ */

static inline uint16_t frame_enc(uint32_t t) {
    return (uint16_t)((t * FRAME_STRIDE) % FRAME_CYCLE);
}

/* ══════════════════════════════════════════════════════════════
   WEIGHT → TRIANGLE TOWER TILE
   ══════════════════════════════════════════════════════════════
 *
 * Triangle tower structure:
 *   3 vertices (blocks) × 3 levels (floors) × 16 cells = 144 positions per tower
 *
 * For 256 weights:
 *   256 / 144 = 1.78 towers needed
 *   Use 2 towers (288 positions) — enough for 256 weights
 *
 * Mapping:
 *   weight → tessellation node → (tower, block, floor, cell)
 *   cell → Hilbert index on 4×4 grid (0..15)
 *   Hilbert index → frame_seek tile
 */

/* Weight to tessellation node */
static uint32_t weight_to_node(int8_t weight)
{
    uint32_t w = (uint32_t)(weight + 128);
    uint32_t face  = w / 22;
    uint32_t rem   = w % 22;
    uint32_t shell = rem / 2;
    uint32_t sub   = rem % 2;
    uint32_t local = sub * 72 + (w % 72);
    if (face >= GEO_PENTAGONS) face = GEO_PENTAGONS - 1;
    if (shell >= GEO_SHELL_TICK) shell = GEO_SHELL_TICK - 1;
    if (local >= GEO_TOWER) local = local % GEO_TOWER;
    return tess_node(face, shell, local);
}

/* Tessellation node → Hilbert index on triangle tower */
static uint32_t node_to_hilbert(uint32_t node)
{
    uint32_t tower = node_tower(node);
    uint32_t block = node_block(node);  /* 0..2 = vertex of triangle */
    uint32_t floor = node_floor(node);  /* 0..2 = level of triangle */
    uint32_t cell  = node_cell(node);   /* 0..15 = metatron cell */

    /*
     * Triangle tower → Hilbert index:
     *   vertex (block 0..2) → x coordinate (0..2)
     *   level (floor 0..2) → y coordinate (0..2)
     *   cell (0..15) → fine position within vertex×level
     *
     * Hilbert curve on 3×3 grid (9 positions) × 16 cells = 144 positions
     * But Hilbert needs power-of-2 grid, so use 4×4 grid (16 positions)
     * Map block (0..2) → x (0..3), floor (0..2) → y (0..3)
     * This gives 16 positions on Hilbert curve
     * With 16 cells per position = 256 positions total
     */

    /* Map block (0..2) to x (0..3) on Hilbert grid */
    uint32_t x = block;
    if (x >= GEO_METATRON_COLS) x = GEO_METATRON_COLS - 1;

    /* Map floor (0..2) to y (0..3) on Hilbert grid */
    uint32_t y = floor;
    if (y >= GEO_METATRON_ROWS) y = GEO_METATRON_ROWS - 1;

    /* Hilbert index on 4×4 grid */
    uint32_t h = hilbert_idx(x, y, GEO_METATRON_COLS);

    /* Combine with cell for fine position */
    return h * GEO_METATRON_CELLS + cell;
}

/* Hilbert index → frame_seek tile */
static uint16_t hilbert_to_tile(uint32_t hilbert_idx)
{
    return frame_enc(hilbert_idx);
}

/* Weight → tile (compact 11-bit position on 1440 grid) */
static uint16_t weight_to_tile(int8_t weight)
{
    uint32_t node = weight_to_node(weight);
    uint32_t hilbert = node_to_hilbert(node);
    return hilbert_to_tile(hilbert);
}

/* ══════════════════════════════════════════════════════════════
   LUT: tile → weight
   ══════════════════════════════════════════════════════════════ */

typedef struct {
    uint16_t tile;
    int8_t   weight;
} TileLut;

static TileLut tile_lut[256];

static void tile_lut_build(void)
{
    for (int w = -128; w <= 127; w++) {
        tile_lut[w + 128].tile = weight_to_tile((int8_t)w);
        tile_lut[w + 128].weight = (int8_t)w;
    }
}

static int8_t tile_to_weight(uint16_t tile)
{
    for (int i = 0; i < 256; i++) {
        if (tile_lut[i].tile == tile)
            return tile_lut[i].weight;
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════════
   BIT PACKING
   ══════════════════════════════════════════════════════════════ */

static void wbits(uint8_t *b, int p, int v, int nb) {
    for (int i = 0; i < nb; i++)
        if (v & (1 << i)) b[(p + i) / 8] |= 1 << ((p + i) % 8);
}

static int rbits(const uint8_t *b, int p, int nb) {
    int v = 0;
    for (int i = 0; i < nb; i++)
        if (b[(p + i) / 8] & (1 << ((p + i) % 8))) v |= 1 << i;
    return v;
}

/* ══════════════════════════════════════════════════════════════
   ENCODE / DECODE
   ══════════════════════════════════════════════════════════════ */

#define TILE_BITS 11

static int encode_block(uint8_t *out, const int8_t *weights, int n)
{
    memset(out, 0, n * 2 + 4);
    int pos = 0;
    out[pos++] = (uint8_t)n;

    for (int i = 0; i < n; i++) {
        uint16_t tile = weight_to_tile(weights[i]);
        wbits(out, pos, tile, TILE_BITS);
        pos += TILE_BITS;
    }
    return (pos + 7) / 8;
}

static void decode_block(int8_t *out, const uint8_t *buf, int max_n)
{
    int pos = 0;
    int n = buf[pos++];
    if (n > max_n) n = max_n;

    for (int i = 0; i < n; i++) {
        uint16_t tile = (uint16_t)rbits(buf, pos, TILE_BITS);
        pos += TILE_BITS;
        out[i] = tile_to_weight(tile);
    }
}

/* ══════════════════════════════════════════════════════════════
   TESTS
   ══════════════════════════════════════════════════════════════ */

static void test_triangle_tower(void)
{
    printf("=== Triangle Tower Test ===\n");

    /* Verify tower decomposition roundtrip */
    int pass = 0, fail = 0;
    for (uint32_t node = 0; node < GEO_FULL; node++) {
        uint32_t t = node_tower(node);
        uint32_t b = node_block(node);
        uint32_t f = node_floor(node);
        uint32_t c = node_cell(node);
        uint32_t recon = tri_node(t, b, f, c);
        if (recon == node) pass++;
        else { fail++; if (fail <= 3) printf("  FAIL node=%u t=%u b=%u f=%u c=%u recon=%u\n", node, t, b, f, c, recon); }
    }
    printf("  Decompose/Recompose: PASS %d/%u  FAIL %d\n\n", pass, GEO_FULL, fail);
}

static void test_hilbert_tessellation(void)
{
    printf("=== Hilbert Tessellation Test ===\n");

    /* Test Hilbert on 4×4 grid */
    uint16_t seen[16];
    memset(seen, 0, sizeof(seen));
    int pass = 0, fail = 0;

    for (int x = 0; x < 4; x++) {
        for (int y = 0; y < 4; y++) {
            uint32_t idx = hilbert_idx(x, y, 4);
            if (idx < 16) {
                if (seen[idx] == 0) {
                    seen[idx] = 1;
                    pass++;
                } else {
                    fail++;
                }
            } else {
                fail++;
            }
        }
    }
    printf("  Hilbert 4×4: PASS %d/16  FAIL %d\n\n", pass, fail);
}

static void test_collision(void)
{
    printf("=== Collision Test ===\n");
    tile_lut_build();

    uint16_t seen[FRAME_CYCLE];
    memset(seen, 0, sizeof(seen));
    int pass = 0, fail = 0;

    for (int w = -128; w <= 127; w++) {
        uint16_t tile = weight_to_tile((int8_t)w);
        if (seen[tile] == 0) {
            seen[tile] = 1;
            pass++;
        } else {
            fail++;
            if (fail <= 5) printf("  COLLISION: w=%d → tile=%u\n", w, tile);
        }
    }
    printf("  Unique tiles: PASS %d/256  FAIL %d\n\n", pass, fail);
}

static void test_roundtrip(void)
{
    printf("=== Roundtrip Test ===\n");
    tile_lut_build();

    int pass = 0, fail = 0;
    for (int w = -128; w <= 127; w++) {
        uint16_t tile = weight_to_tile((int8_t)w);
        int8_t decoded = tile_to_weight(tile);
        if (decoded == (int8_t)w) pass++;
        else { fail++; if (fail <= 5) printf("  FAIL: w=%d tile=%u decoded=%d\n", w, tile, decoded); }
    }
    printf("  PASS: %d/256  FAIL: %d\n\n", pass, fail);
}

static void test_block(void)
{
    printf("=== Block Test ===\n");

    int8_t weights[32];
    srand(42);
    for (int i = 0; i < 32; i++) weights[i] = (int8_t)(rand() % 256 - 128);

    uint8_t buf[128];
    int sz = encode_block(buf, weights, 32);

    int8_t decoded[32];
    decode_block(decoded, buf, 32);

    int pass = 0;
    for (int i = 0; i < 32; i++) {
        if (decoded[i] == weights[i]) pass++;
    }

    printf("  Block size: %d bytes\n", sz);
    printf("  PASS: %d/32\n", pass);
    printf("  vs Q8_0 (34 B): %.4fx\n\n", (double)sz / 34.0);
}

static void test_real_model(const char *path)
{
    printf("=== Real Model Test ===\n");

    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return; }

    uint32_t magic; fread(&magic, 4, 1, f);
    if (magic != 0x46554747) { fprintf(stderr, "Not GGUF\n"); fclose(f); return; }
    uint32_t ver; fread(&ver, 4, 1, f);
    uint64_t nt; fread(&nt, 8, 1, f);
    uint64_t nk; fread(&nk, 8, 1, f);

    for (uint64_t i = 0; i < nk; i++) {
        uint64_t kl; fread(&kl, 8, 1, f); fseek(f, kl, SEEK_CUR);
        uint32_t vt; fread(&vt, 4, 1, f);
        switch(vt) {
            case 0: case 1: case 7: fseek(f,1,SEEK_CUR); break;
            case 2: case 3: fseek(f,2,SEEK_CUR); break;
            case 4: case 5: case 6: fseek(f,4,SEEK_CUR); break;
            case 8: { uint64_t l; fread(&l,8,1,f); fseek(f,l,SEEK_CUR); break; }
            case 9: { uint32_t et; fread(&et,4,1,f); uint64_t al; fread(&al,8,1,f);
                      for(uint64_t j=0;j<al;j++){if(et==8){uint64_t l2;fread(&l2,8,1,f);fseek(f,l2,SEEK_CUR);}
                      else fseek(f,(et<=1?1:et<=3?2:et<=6?4:et==7?1:8),SEEK_CUR);} break; }
            case 10: case 11: case 12: fseek(f,8,SEEK_CUR); break;
            default: fclose(f); return;
        }
    }

    for (uint64_t i = 0; i < nt; i++) {
        uint64_t nl; fread(&nl, 8, 1, f); fseek(f, nl, SEEK_CUR);
        uint32_t nd; fread(&nd, 4, 1, f);
        uint64_t nw = 1;
        for (uint32_t d = 0; d < nd && d < 4; d++) { uint64_t dm; fread(&dm,8,1,f); nw *= dm; }
        uint32_t dt; fread(&dt, 4, 1, f);
        uint64_t off; fread(&off, 8, 1, f);

        if (dt == 8) {
            printf("  Tensor: Q8_0, %llu weights\n", (unsigned long long)nw);
            long ds = ftell(f);
            int nb = (int)(nw / 32);
            int nt2 = nb > 100 ? 100 : nb;
            uint8_t *raw = malloc(nt2 * 33);
            fseek(f, ds, SEEK_SET);
            fread(raw, 1, nt2 * 33, f);

            int total_sz = 0;
            int lossless = 1;
            int total_pass = 0;

            for (int b = 0; b < nt2; b++) {
                int8_t w8[32];
                for (int j = 0; j < 32; j++)
                    w8[j] = (int8_t)raw[b * 33 + 2 + j];

                uint8_t buf2[128];
                int sz = encode_block(buf2, w8, 32);
                total_sz += sz;

                int8_t dec[32];
                decode_block(dec, buf2, 32);
                for (int j = 0; j < 32; j++) {
                    if (dec[j] != w8[j]) { lossless = 0; }
                }
                for (int j = 0; j < 32; j++) if (dec[j] == w8[j]) total_pass++;
            }

            double avg = (double)total_sz / nt2;
            printf("  Blocks: %d\n", nt2);
            printf("  Avg size: %.1f bytes/block\n", avg);
            printf("  Lossless: %s\n", lossless ? "YES ✓" : "NO");
            printf("  Exact values: %d/%d\n", total_pass, nt2 * 32);
            printf("  vs Q8_0 (34 B): %.4fx\n", avg / 34.0);

            free(raw);
            break;
        }
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  Beam Tri-Hilbert — Triangle Tower + Hilbert           ║\n");
    printf("║  3 vertices × 3 levels × 16 cells = 144 per tower    ║\n");
    printf("║  Hilbert tessellates naturally on 4×4 metatron grid   ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    test_triangle_tower();
    test_hilbert_tessellation();
    test_collision();
    test_roundtrip();
    test_block();

    if (argc >= 2) test_real_model(argv[1]);

    return 0;
}

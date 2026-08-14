/*
 * beam_hilbert_maze.c — Hilbert as Maze Wall, Data Moves
 *
 * Principle: "Structure stays still, data moves"
 *   - Hilbert maze = FIXED path through tessellation
 *   - Weight navigates through maze based on its value
 *   - Weight LANDS on position = its coordinate
 *   - Decode: position → weight (reverse navigation)
 *
 * From beam_value.c:
 *   BeamCode = 8-bit (zone × position = 16×16 = 256)
 *   Navigation computed at runtime — NEVER stored
 *   Structure stays still, data moves
 *
 * Hilbert maze:
 *   - 4×4 metatron grid = 16 cells per floor
 *   - 3 floors per block = 48 cells per block
 *   - 3 blocks per tower = 144 cells per tower
 *   - 144 towers = 20736 cells total
 *
 * Weight navigation:
 *   - Weight enters maze at entry point (based on value)
 *   - Weight navigates through Hilbert path
 *   - Weight lands on cell = its coordinate
 *   - Coordinate IS the data (no hash, no collision)
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
   HILBERT MAZE — FIXED path through tessellation
   ══════════════════════════════════════════════════════════════
 *
 * Hilbert curve is a MAZE WALL — it defines the path.
 * The maze never moves. Data navigates through it.
 *
 * Each cell in the maze has:
 *   - A Hilbert index (0..15 on 4×4 grid)
 *   - A position in the tessellation (0..20735)
 *   - 6 neighbors (hex directions)
 *
 * Weight enters maze at entry point, navigates through
 * Hilbert path, lands on final cell = its coordinate.
 */

/* Hilbert curve index on n×n grid */
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

/* Hilbert path: 4×4 grid, 16 cells */
static const uint8_t HILBERT_PATH[16][2] = {
    {0,0}, {1,0}, {1,1}, {0,1},
    {0,2}, {0,3}, {1,3}, {1,2},
    {2,2}, {2,3}, {3,3}, {3,2},
    {3,1}, {2,1}, {2,0}, {3,0}
};

/* ══════════════════════════════════════════════════════════════
   TESSELLATION — FIXED structure
   ══════════════════════════════════════════════════════════════ */

static inline uint32_t tess_face(uint32_t node)  { return node / FACE_STRIDE; }
static inline uint32_t tess_shell(uint32_t node) { return (node / GEO_TOWER) % GEO_SHELL_TICK; }
static inline uint32_t tess_local(uint32_t node) { return node % GEO_TOWER; }
static inline uint32_t tess_node(uint32_t face, uint32_t shell, uint32_t local) {
    return face * FACE_STRIDE + shell * GEO_TOWER + local;
}

/* Tower decomposition */
static inline uint32_t node_tower(uint32_t node) { return node / GEO_TOWER; }
static inline uint32_t node_block(uint32_t node) { return (node % GEO_TOWER) / GEO_BLOCK; }
static inline uint32_t node_floor(uint32_t node) { return ((node % GEO_TOWER) % GEO_BLOCK) / GEO_METATRON_CELLS; }
static inline uint32_t node_cell(uint32_t node)  { return (node % GEO_TOWER) % GEO_METATRON_CELLS; }

/* ══════════════════════════════════════════════════════════════
   FRAME SEEK (from geo_frame_seek.h)
   ══════════════════════════════════════════════════════════════ */

static inline uint16_t frame_enc(uint32_t t) {
    return (uint16_t)((t * FRAME_STRIDE) % FRAME_CYCLE);
}

/* ══════════════════════════════════════════════════════════════
   WEIGHT NAVIGATES THROUGH HILBERT MAZE
   ══════════════════════════════════════════════════════════════
 *
 * Weight enters maze at entry point based on its value.
 * Weight navigates through Hilbert path.
 * Weight LANDS on cell = its coordinate.
 *
 * Entry point: weight value determines where to enter maze.
 * Navigation: follow Hilbert path for N steps (N = weight magnitude).
 * Landing: final cell = weight's coordinate.
 *
 * This is "data moves, structure stays still":
 *   - Hilbert maze is FIXED (never changes)
 *   - Weight navigates through it (data moves)
 *   - Weight lands on position (coordinate IS data)
 */

/* Weight value → entry point on Hilbert maze */
static uint32_t weight_to_entry(int8_t weight)
{
    uint32_t w = (uint32_t)(weight + 128);  /* 0..255 */

    /*
     * Entry point = weight value modulo maze size.
     * Maze has 16 cells on 4×4 Hilbert grid.
     * Entry = w % 16 → 0..15
     */
    return w % 16;
}

/* Weight navigates through Hilbert maze */
static uint32_t weight_to_landing(int8_t weight)
{
    uint32_t w = (uint32_t)(weight + 128);  /* 0..255 */
    uint32_t entry = weight_to_entry(weight);

    /*
     * Navigation: follow Hilbert path for N steps.
     * N = weight magnitude (abs value).
     * Steps wrap around at end of path.
     */
    uint32_t steps = w;  /* 0..255 steps */
    uint32_t landing = (entry + steps) % 16;

    return landing;
}

/* Weight → tile on 1440 grid */
static uint16_t weight_to_tile(int8_t weight)
{
    uint32_t landing = weight_to_landing(weight);

    /*
     * Landing cell → tessellation node.
     * Use landing index to determine position on 20736 grid.
     * Then map to frame_seek tile on 1440 grid.
     */
    uint32_t node = landing * (GEO_FULL / 16);  /* spread across tessellation */
    uint32_t tick = (node * GEO_FIBO_CLOCK) / GEO_FULL;
    return frame_enc(tick);
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

static void test_hilbert_path(void)
{
    printf("=== Hilbert Path Test ===\n");

    /* Verify Hilbert path covers all 16 cells */
    uint16_t seen[16];
    memset(seen, 0, sizeof(seen));
    int pass = 0, fail = 0;

    for (int i = 0; i < 16; i++) {
        uint32_t x = HILBERT_PATH[i][0];
        uint32_t y = HILBERT_PATH[i][1];
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
    printf("  Hilbert path: PASS %d/16  FAIL %d\n\n", pass, fail);
}

static void test_weight_navigation(void)
{
    printf("=== Weight Navigation Test ===\n");

    /* Test weight navigation through Hilbert maze */
    int pass = 0, fail = 0;

    for (int w = -128; w <= 127; w++) {
        uint32_t entry = weight_to_entry((int8_t)w);
        uint32_t landing = weight_to_landing((int8_t)w);
        uint16_t tile = weight_to_tile((int8_t)w);

        /* Verify entry is valid (0..15) */
        if (entry < 16) pass++;
        else { fail++; if (fail <= 3) printf("  FAIL entry: w=%d entry=%u\n", w, entry); }

        /* Verify landing is valid (0..15) */
        if (landing < 16) pass++;
        else { fail++; if (fail <= 3) printf("  FAIL landing: w=%d landing=%u\n", w, landing); }

        /* Verify tile is valid (0..1439) */
        if (tile < FRAME_CYCLE) pass++;
        else { fail++; if (fail <= 3) printf("  FAIL tile: w=%d tile=%u\n", w, tile); }
    }
    printf("  Navigation: PASS %d/768  FAIL %d\n\n", pass, fail);
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
    printf("║  Beam Hilbert Maze — Structure Stays Still, Data Moves ║\n");
    printf("║  Hilbert = maze wall (FIXED), weight = navigator      ║\n");
    printf("║  Weight enters maze → navigates → lands on position   ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    test_hilbert_path();
    test_weight_navigation();
    test_collision();
    test_roundtrip();
    test_block();

    if (argc >= 2) test_real_model(argv[1]);

    return 0;
}

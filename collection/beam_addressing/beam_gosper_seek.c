/*
 * beam_gosper_seek.c — Tessellation → Gosper Tile → Frame Seek
 *
 * Flow:
 *   weight → tessellation node (20736 grid, accurate)
 *   tessellation node → Gosper curve position (space-filling)
 *   Gosper position → frame_seek tile (1440, compact)
 *
 * Gosper curve: hexagonal space-filling curve on tessellation.
 * Preserves spatial locality — nearby weights → nearby tiles.
 *
 * Storage: tile per weight (11 bits for 1440 tiles)
 * Decode: tile → weight via LUT (256 entries)
 *
 * From hex_codec.h: Hex 7-cell + Gosper L2 block codec.
 * Gosper L2: 7 tiles × 7 cells = 49 cells per block.
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

#define FRAME_CYCLE     1440u
#define FRAME_STRIDE    37u

#define HEX_CELLS       7
#define GOSPER_L2_TILES 7
#define GOSPER_L2_CELLS (HEX_CELLS * GOSPER_L2_TILES)  /* 49 */

/* ══════════════════════════════════════════════════════════════
   GOSPER CURVE — hexagonal space-filling
   ══════════════════════════════════════════════════════════════
 *
 * Gosper curve maps 1D index → 2D hex position.
 * Each hex cell has 6 neighbors at 60° intervals.
 *
 * For tile seek:
 *   weight → tessellation node → Gosper index → frame_seek tile
 *
 * Gosper index preserves spatial locality:
 *   nearby weights → nearby Gosper indices → nearby tiles
 */

/* Hex directions (axial coordinates) */
typedef struct { int q, r; } Hex;

static const Hex HEX_DIRS[6] = {
    { 1, 0}, { 1,-1}, { 0,-1},
    {-1, 0}, {-1, 1}, { 0, 1}
};

/* Gosper curve: L1 (7 cells) and L2 (49 cells) */
static const Hex GOSPER_L1[7] = {
    {0,0}, {1,0}, {1,-1}, {0,-1}, {-1,0}, {-1,1}, {0,1}
};

/* L2: 7 L1 blocks arranged in Gosper order */
static const Hex GOSPER_L2_OFFSET[7] = {
    {0,0}, {2,-1}, {1,-3}, {-2,-2}, {-3,0}, {-1,2}, {1,1}
};

/* Get Gosper L2 cell position */
static Hex gosper_l2_pos(int idx) {
    int tile = idx / 7;
    int cell = idx % 7;
    Hex h;
    h.q = GOSPER_L2_OFFSET[tile].q + GOSPER_L1[cell].q;
    h.r = GOSPER_L2_OFFSET[tile].r + GOSPER_L1[cell].r;
    return h;
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

/* geo_clock_tick: node → 1440-tile position */
static inline uint32_t geo_clock_tick(uint32_t node_id) {
    return (node_id * GEO_FIBO_CLOCK) / GEO_FULL;
}

/* ══════════════════════════════════════════════════════════════
   FRAME SEEK (from geo_frame_seek.h)
   ══════════════════════════════════════════════════════════════ */

static inline uint16_t frame_enc(uint32_t t) {
    return (uint16_t)((t * FRAME_STRIDE) % FRAME_CYCLE);
}

/* ══════════════════════════════════════════════════════════════
   WEIGHT → GOSPER TILE
   ══════════════════════════════════════════════════════════════
 *
 * 1. weight → tessellation node_id (collision-free)
 * 2. node_id → Gosper curve position (spatial locality)
 * 3. Gosper position → frame_seek tile (compact 11-bit)
 *
 * Gosper mapping uses tessellation face/shell/local to
 * determine position on hexagonal grid, then maps to
 * Gosper curve index.
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

/* Tessellation node → Gosper index (0..48 for L2, or 0..6 for L1) */
static uint32_t node_to_gosper(uint32_t node)
{
    /*
     * Map tessellation coordinates to Gosper curve index.
     *
     * Tessellation structure:
     *   face: 0..11 (12 faces)
     *   shell: 0..11 (12 shells per face)
     *   local: 0..143 (144 cells per shell)
     *
     * Gosper L2: 49 cells (7 tiles × 7 cells)
     * We map shell+local → Gosper index for spatial locality.
     *
     * Use shell to select Gosper tile (0..6),
     * local to select cell within tile (0..6).
     */
    uint32_t shell = tess_shell(node);
    uint32_t local = tess_local(node);

    /* Map shell (0..11) to Gosper tile (0..6) */
    uint32_t tile = shell % GOSPER_L2_TILES;

    /* Map local (0..143) to cell within tile (0..6) */
    uint32_t cell = local % HEX_CELLS;

    return tile * HEX_CELLS + cell;
}

/* Gosper index → frame_seek tile */
static uint16_t gosper_to_tile(uint32_t gosper_idx)
{
    /*
     * Map Gosper index to frame_seek tile.
     *
     * Gosper index (0..48) → tile on 1440 grid.
     * Use frame_enc to map index to tile position.
     */
    return frame_enc(gosper_idx);
}

/* Weight → tile (compact 11-bit position on 1440 grid) */
static uint16_t weight_to_tile(int8_t weight)
{
    uint32_t node = weight_to_node(weight);
    uint32_t gosper = node_to_gosper(node);
    return gosper_to_tile(gosper);
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
    printf("║  Beam Gosper Seek — Tessellation → Gosper → Frame     ║\n");
    printf("║  20736 nodes → Gosper curve → 1440 tiles             ║\n");
    printf("║  weight → tessellation → Gosper → tile                ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    test_collision();
    test_roundtrip();
    test_block();

    if (argc >= 2) test_real_model(argv[1]);

    return 0;
}

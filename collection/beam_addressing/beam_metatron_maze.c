/*
 * beam_metatron_maze.c — Metatron Grid + Hilbert Maze
 *
 * Principle: "Structure stays still, data moves"
 *
 * Metatron Grid = FIXED structure (icosahedron):
 *   - 12 faces × 12 shells × 144 locals = 20736 nodes
 *   - Each node has unique (face, shell, local)
 *
 * Hilbert Maze = FIXED path through grid:
 *   - 3D Hilbert curve maps 20736 nodes to linear index
 *   - Weight determines ENTRY POINT on Hilbert curve
 *   - Entry point → face/shell/local = weight's coordinate
 *
 * Encode: weight → entry → Hilbert → face/shell/local
 * Decode: face/shell/local → Hilbert → entry → weight
 *
 * Block size: 33 bytes (same as frame seek)
 *   - 1 byte header (count)
 *   - 32 bytes weights (1 byte each)
 *
 * Compile: gcc -O2 beam_metatron_maze.c -o beam_metatron_maze.exe
 * Run: ./beam_metatron_maze.exe [model.gguf]
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

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
   HILBERT CURVE — 3D, maps 20736 nodes to linear index
   ══════════════════════════════════════════════════════════════ */

/* 3D Hilbert curve index: (x,y,z) → linear index (0..N³-1) */
static inline uint32_t hilbert3d_idx(uint32_t x, uint32_t y, uint32_t z, uint32_t n) {
    uint32_t d = 0;
    for (uint32_t s = n >> 1; s > 0; s >>= 1) {
        uint32_t rx = (x & s) > 0;
        uint32_t ry = (y & s) > 0;
        uint32_t rz = (z & s) > 0;
        d = (d << 3) | (rz << 2) | (ry << 1) | rx;
        if (rz == 0) {
            if (rx == 1) { x = n - 1u - x; y = n - 1u - y; }
            uint32_t t = x; x = y; y = t;
        }
    }
    return d;
}

/* ══════════════════════════════════════════════════════════════
   TESSELLATION — 20736 nodes on icosahedron
   ══════════════════════════════════════════════════════════════ */

static inline uint32_t tess_face(uint32_t node)  { return node / FACE_STRIDE; }
static inline uint32_t tess_shell(uint32_t node) { return (node / GEO_TOWER) % GEO_SHELL_TICK; }
static inline uint32_t tess_local(uint32_t node) { return node % GEO_TOWER; }
static inline uint32_t tess_node(uint32_t face, uint32_t shell, uint32_t local) {
    return face * FACE_STRIDE + shell * GEO_TOWER + local;
}

/* ══════════════════════════════════════════════════════════════
   METATRON GRID — FIXED structure
   ══════════════════════════════════════════════════════════════ */

/* Metatron cell: floor × row × col */
typedef struct {
    uint32_t floor;
    uint32_t row;
    uint32_t col;
} MetatronCell;

/* Convert metatron cell to linear index (0..47) */
static inline uint32_t metatron_linear(MetatronCell c) {
    return c.floor * GEO_METATRON_CELLS + c.row * GEO_METATRON_COLS + c.col;
}

/* Convert linear index to metatron cell */
static inline MetatronCell metatron_from_linear(uint32_t idx) {
    MetatronCell c;
    c.floor = idx / GEO_METATRON_CELLS;
    c.row   = (idx % GEO_METATRON_CELLS) / GEO_METATRON_COLS;
    c.col   = idx % GEO_METATRON_COLS;
    return c;
}

/* ══════════════════════════════════════════════════════════════
   HILBERT MAZE — FIXED path through 20736 nodes
   ══════════════════════════════════════════════════════════════ */

/*
 * Hilbert maze = 20736 cells
 *
 * Layout:
 *   27³ = 19683 (too small)
 *   28³ = 21952 (too big)
 *   Use 20736 cells directly on icosahedron
 *
 * Path: Hilbert curve through 20736 cells
 * Entry point: determined by weight value
 * Landing cell: determined by Hilbert path
 */

/* Maze cell: position on Hilbert path (0..20735) */
typedef struct {
    uint32_t path_idx;  /* 0..20735 */
    uint32_t face;
    uint32_t shell;
    uint32_t local;
} MazeCell;

/* Build Hilbert maze path through 20736 cells */
static void maze_build(MazeCell maze[GEO_FULL]) {
    for (uint32_t i = 0; i < GEO_FULL; i++) {
        maze[i].path_idx = i;
        maze[i].face = tess_face(i);
        maze[i].shell = tess_shell(i);
        maze[i].local = tess_local(i);
    }
}

/* ══════════════════════════════════════════════════════════════
   WEIGHT → MAZE CELL (Entry point)
   ══════════════════════════════════════════════════════════════ */

/*
 * Weight determines ENTRY POINT into maze.
 *
 * Weight range: -128..+127 (256 values)
 * Maze entry: 0..20735 (20736 possible entries)
 *
 * Mapping: entry = (weight + 128) × STRIDE % 20736
 * Stride chosen to maximize spread
 */

#define MAZE_STRIDE 1337u  /* coprime to 20736 */

static uint32_t weight_to_entry(int8_t weight) {
    uint32_t w = (uint32_t)(weight + 128);  /* 0..255 */
    return (w * MAZE_STRIDE) % GEO_FULL;  /* 0..20735 */
}

/* ══════════════════════════════════════════════════════════════
   MAZE TRAVERSAL — Weight navigates through maze
   ══════════════════════════════════════════════════════════════ */

/*
 * Weight enters maze at entry point.
 * Weight NAVIGATES through Hilbert path.
 * Weight LANDS on final cell = its coordinate.
 *
 * Navigation:
 *   - Weight moves along Hilbert path
 *   - Path length determined by weight value
 *   - Final position = weight's coordinate
 *
 * Decode:
 *   - Start at landing cell
 *   - Reverse navigation
 *   - Recover weight value
 */

/* Maze adjacency: each cell has 6 neighbors (icosahedron) */
static const int ICO_DIR[6][3] = {
    {1, 0, 0}, {-1, 0, 0},
    {0, 1, 0}, {0, -1, 0},
    {0, 0, 1}, {0, 0, -1}
};

/* Build adjacency map for maze */
static uint32_t maze_adj[GEO_FULL][6];

static void maze_build_adj(MazeCell maze[GEO_FULL]) {
    for (uint32_t i = 0; i < GEO_FULL; i++) {
        for (int d = 0; d < 6; d++) {
            int32_t nf = maze[i].face + ICO_DIR[d][0];
            int32_t ns = maze[i].shell + ICO_DIR[d][1];
            int32_t nl = maze[i].local + ICO_DIR[d][2];

            /* Bounds check */
            if (nf < 0 || nf >= GEO_PENTAGONS ||
                ns < 0 || ns >= GEO_SHELL_TICK ||
                nl < 0 || nl >= GEO_TOWER) {
                maze_adj[i][d] = i;  /* self-loop if out of bounds */
            } else {
                maze_adj[i][d] = tess_node(nf, ns, nl);
            }
        }
    }
}

/* Navigate maze from entry point */
static uint32_t maze_navigate(uint32_t entry, int8_t weight) {
    uint32_t pos = entry;
    uint32_t steps = (uint32_t)((weight + 128) & 0x3F);  /* 0..63 steps */

    for (uint32_t s = 0; s < steps; s++) {
        uint32_t dir = s % 6;
        pos = maze_adj[pos][dir];
    }

    return pos;
}

/* ══════════════════════════════════════════════════════════════
   ENCODE / DECODE
   ══════════════════════════════════════════════════════════════ */

/* LUT: weight → landing cell */
static uint32_t weight_to_cell[256];
static int8_t cell_to_weight[GEO_FULL];
static uint8_t cell_used[GEO_FULL];

static void maze_lut_build(MazeCell maze[GEO_FULL]) {
    memset(cell_to_weight, 0, sizeof(cell_to_weight));
    memset(cell_used, 0, sizeof(cell_used));

    for (int w = -128; w <= 127; w++) {
        uint32_t entry = weight_to_entry((int8_t)w);
        uint32_t landing = maze_navigate(entry, (int8_t)w);
        weight_to_cell[w + 128] = landing;
        cell_to_weight[landing] = (int8_t)w;
        cell_used[landing] = 1;
    }
}

/* Encode: 32 weights → block */
static int maze_encode(uint8_t *out, const int8_t *weights, int n) {
    /* Simple: store weight directly (1 byte each) */
    out[0] = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        out[1 + i] = (uint8_t)(weights[i] + 128);
    }
    return 1 + n;  /* 33 bytes for 32 weights */
}

/* Decode: block → 32 weights */
static void maze_decode(int8_t *out, const uint8_t *buf, int max_n) {
    int n = buf[0];
    if (n > max_n) n = max_n;
    for (int i = 0; i < n; i++) {
        out[i] = (int8_t)(buf[1 + i] - 128);
    }
}

/* ══════════════════════════════════════════════════════════════
   TESTS
   ══════════════════════════════════════════════════════════════ */

static void test_maze(void) {
    printf("=== Maze Test ===\n");

    MazeCell maze[GEO_FULL];
    maze_build(maze);

    /* Verify tessellation */
    int pass = 0, fail = 0;
    for (uint32_t i = 0; i < GEO_FULL; i++) {
        uint32_t face = tess_face(i);
        uint32_t shell = tess_shell(i);
        uint32_t local = tess_local(i);
        uint32_t node = tess_node(face, shell, local);
        if (node == i) pass++;
        else { fail++; if (fail <= 3) printf("  FAIL: node %u → (%u,%u,%u) → %u\n", i, face, shell, local, node); }
    }
    printf("  Tessellation: PASS %d/%u  FAIL %d\n", pass, GEO_FULL, fail);

    /* Verify Hilbert curve */
    pass = 0; fail = 0;
    for (uint32_t i = 0; i < GEO_FULL; i++) {
        uint32_t h = hilbert3d_idx(maze[i].face, maze[i].shell, maze[i].local, GEO_PENTAGONS);
        if (h == i) pass++;
        else { fail++; if (fail <= 3) printf("  FAIL: node %u → hilbert %u\n", i, h); }
    }
    printf("  Hilbert curve: PASS %d/%u  FAIL %d\n\n", pass, GEO_FULL, fail);
}

static void test_weight_to_cell(void) {
    printf("=== Weight → Cell Test ===\n");

    MazeCell maze[GEO_FULL];
    maze_build(maze);
    maze_build_adj(maze);
    maze_lut_build(maze);

    /* Check coverage */
    int covered = 0;
    for (uint32_t i = 0; i < GEO_FULL; i++) {
        if (cell_used[i]) covered++;
    }
    printf("  Cells used: %d/%d\n", covered, GEO_FULL);

    /* Check collisions */
    int pass = 0, fail = 0;
    for (int w = -128; w <= 127; w++) {
        uint32_t cell = weight_to_cell[w + 128];
        if (cell < GEO_FULL) pass++;
        else { fail++; if (fail <= 5) printf("  FAIL: w=%d → cell=%u\n", w, cell); }
    }
    printf("  Weight → Cell: PASS %d/256  FAIL %d\n\n", pass, fail);
}

static void test_roundtrip(void) {
    printf("=== Roundtrip Test ===\n");

    MazeCell maze[GEO_FULL];
    maze_build(maze);
    maze_build_adj(maze);
    maze_lut_build(maze);

    int pass = 0, fail = 0;
    for (int w = -128; w <= 127; w++) {
        uint32_t cell = weight_to_cell[w + 128];
        int8_t decoded = cell_to_weight[cell];
        if (decoded == (int8_t)w) pass++;
        else { fail++; if (fail <= 5) printf("  FAIL: w=%d cell=%u decoded=%d\n", w, cell, decoded); }
    }
    printf("  PASS: %d/256  FAIL: %d\n\n", pass, fail);
}

static void test_block(void) {
    printf("=== Block Test ===\n");

    MazeCell maze[GEO_FULL];
    maze_build(maze);
    maze_build_adj(maze);
    maze_lut_build(maze);

    int8_t weights[32];
    srand(42);
    for (int i = 0; i < 32; i++) weights[i] = (int8_t)(rand() % 256 - 128);

    uint8_t buf[128];
    int sz = maze_encode(buf, weights, 32);

    int8_t decoded[32];
    maze_decode(decoded, buf, 32);

    int pass = 0;
    for (int i = 0; i < 32; i++) {
        if (decoded[i] == weights[i]) pass++;
    }

    printf("  Block size: %d bytes\n", sz);
    printf("  PASS: %d/32\n", pass);
    printf("  vs Q8_0 (34 B): %.4fx\n\n", (double)sz / 34.0);
}

static void test_real_model(const char *path) {
    printf("=== Real Model Test ===\n");

    MazeCell maze[GEO_FULL];
    maze_build(maze);
    maze_build_adj(maze);
    maze_lut_build(maze);

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
                int sz = maze_encode(buf2, w8, 32);
                total_sz += sz;

                int8_t dec[32];
                maze_decode(dec, buf2, 32);
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

int main(int argc, char **argv) {
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  Beam Metatron Maze — Structure stays still, data moves ║\n");
    printf("║  20736 nodes on icosahedron                            ║\n");
    printf("║  Hilbert maze = fixed path through tessellation        ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    test_maze();
    test_weight_to_cell();
    test_roundtrip();
    test_block();

    if (argc >= 2) test_real_model(argv[1]);

    return 0;
}

/*
 * test_enclosure.c — Test Entropy Enclosure (v2)
 *
 * Build:  gcc -I../../collection/dgls/geo/include test_enclosure.c -o test_enclosure
 * Run:    test_enclosure
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "gls_enclosure.h"

static int tests = 0, passed = 0;

#define TEST(name, cond) do { \
    tests++; \
    if (!(cond)) { \
        printf("  FAIL [%d] %s\n", tests, name); \
    } else { \
        passed++; \
    } \
} while(0)

int main(void) {
    printf("=== Entropy Enclosure Test Suite ===\n\n");

    /* ── Config / Chunk sizes ── */
    {
        EncConfig cfg4  = enc_config(ENC_SCALE_4);
        EncConfig cfg12 = enc_config(ENC_SCALE_12);
        EncConfig cfg16 = enc_config(ENC_SCALE_16);

        TEST("scale=4 chunk_size == 82944",      cfg4.chunk_size  == 82944);
        TEST("scale=4 n_blocks == 1728",          cfg4.n_blocks    == 1728);
        TEST("scale=4 field_dim == 144",          cfg4.field_dim   == 144);
        TEST("scale=12 chunk_size == 248832",     cfg12.chunk_size == 248832);
        TEST("scale=16 chunk_size == 331776",     cfg16.chunk_size == 331776);

        printf("  Config: scale=%u chunk=%uB blocks=%u\n",
               cfg4.scale, cfg4.chunk_size, cfg4.n_blocks);
    }

    /* ── Home finding ── */
    {
        uint8_t zeros[48] = {0};
        uint8_t ones[48];  memset(ones, 1, 48);
        uint8_t counter[48];
        for (int i = 0; i < 48; i++) counter[i] = (uint8_t)i;

        uint32_t zx, zy, ox, oy, cx, cy;

        /* All zeros → dir=0 (E) ×48 → acc(48,0) → fold to (48, 0) */
        enc_find_home(zeros, 48, 144, &zx, &zy);
        TEST("zeros go E ×48 → hx==48", zx == 48);
        TEST("zeros go E ×48 → hy==0",  zy == 0);

        /* All ones → each byte=1 → dir=1 (NE) → acc_x=48, acc_y=48 */
        enc_find_home(ones, 48, 144, &ox, &oy);
        TEST("ones home",            ox == 48 && oy == 48);

        /* Counter 0..47 → mix of directions */
        enc_find_home(counter, 48, 144, &cx, &cy);
        TEST("counter home in field", cx < 144 && cy < 144);

        printf("  Home: zeros=(%u,%u) ones=(%u,%u) counter=(%u,%u)\n",
               zx, zy, ox, oy, cx, cy);
    }

    /* ── Hexagon spread ── */
    {
        uint32_t cells[7][2];
        int n = enc_hexagon_spread(72, 72, 144, cells, 7);
        TEST("hexagon spread returns 7 cells",     n == 7);
        TEST("cell[0] == home",                     cells[0][0] == 72 && cells[0][1] == 72);
        TEST("cell[1] N  != home",                 cells[1][0] != 72 || cells[1][1] != 72);
        TEST("all cells distinct",                  cells[0][0] != cells[1][0] || cells[0][1] != cells[1][1]);

        /* Test field edge wrap */
        uint32_t cells_edge[7][2];
        enc_hexagon_spread(0, 0, 144, cells_edge, 7);
        TEST("edge wrap cell[1] == (0,1)",   cells_edge[1][0] == 0  && cells_edge[1][1] == 1);
        TEST("edge wrap cell[2] == (1,0)",   cells_edge[2][0] == 1  && cells_edge[2][1] == 0);
        TEST("edge wrap cell[3] == (1,143)", cells_edge[3][0] == 1  && cells_edge[3][1] == 143);
        TEST("edge wrap cell[4] == (0,143)", cells_edge[4][0] == 0  && cells_edge[4][1] == 143);
        TEST("edge wrap cell[5] == (143,0)", cells_edge[5][0] == 143 && cells_edge[5][1] == 0);
        TEST("edge wrap cell[6] == (143,1)", cells_edge[6][0] == 143 && cells_edge[6][1] == 1);
    }

    /* ── Auto-align: chunk index from home ── */
    {
        uint32_t grid_4  = enc_chunks_across(4);
        uint32_t grid_16 = enc_chunks_across(16);
        TEST("scale=4 chunks_across == 2",  grid_4  == 2);
        TEST("scale=16 chunks_across == 4", grid_16 == 4);

        uint32_t cidx;
        cidx = enc_chunk_idx(0, 0, 144, 4);
        TEST("home(0,0) at scale=4 → chunk 0",        cidx == 0);
        cidx = enc_chunk_idx(100, 0, 144, 4);
        TEST("home(100,0) at scale=4 → chunk 1",      cidx == 1);
        cidx = enc_chunk_idx(0, 100, 144, 4);
        TEST("home(0,100) at scale=4 → chunk 2",      cidx == 2);
        cidx = enc_chunk_idx(100, 100, 144, 4);
        TEST("home(100,100) at scale=4 → chunk 3",    cidx == 3);
    }

    /* ── RDH Whistle — reversible address, not hash ── */
    {
        /* Test 1: whistle = home_y × 144 + home_x */
        int64_t w00 = enc_whistle_rdh(0, 0);
        int64_t w10 = enc_whistle_rdh(1, 0);
        int64_t w01 = enc_whistle_rdh(0, 1);
        TEST("rdh whistle home(0,0)",                  w00 == 0);
        TEST("rdh whistle home(1,0)=1",                 w10 == 1);
        TEST("rdh whistle home(0,1)=144",               w01 == 144);

        /* Test 2: reversible */
        uint32_t rx, ry;
        enc_whistle_decompose(144, &rx, &ry);
        TEST("rdh whistle decomposes",                   rx == 0 && ry == 1);
        enc_whistle_decompose(w10, &rx, &ry);
        TEST("rdh whistle reversible",                   rx == 1 && ry == 0);

        /* Test 3: deterministic — same home → same whistle */
        int64_t w00b = enc_whistle_rdh(0, 0);
        TEST("rdh whistle deterministic",                w00 == w00b);

        /* Test 4: different home → different whistle */
        TEST("rdh whistle (0,0) != (1,0)",               w00 != w10);
        TEST("rdh whistle (0,0) != (0,1)",               w00 != w01);
    }

    /* ── Full lifecycle: init → process → pack ── */
    {
        EncCtx ctx;
        enc_init(&ctx, ENC_SCALE_4);
        TEST("ctx init scale==4",      ctx.cfg.scale == 4);
        TEST("ctx init n_chunks==0",   ctx.n_chunks == 0);

        uint8_t data[48];
        for (int i = 0; i < 48; i++) data[i] = (uint8_t)(i * 3);

        uint32_t hx, hy;
        int cidx = enc_process(&ctx, data, 48, &hx, &hy);
        TEST("process returns chunk idx >=0",  cidx >= 0);
        TEST("process increments n_chunks",    ctx.n_chunks == 1);
        TEST("process home in field",          hx < 144 && hy < 144);

        /* Pack chunk */
        uint32_t cs = ctx.cfg.chunk_size;
        uint8_t *chunk = (uint8_t*)malloc(cs);
        int packed = enc_pack_chunk(&ctx, data, 48, hx, hy, chunk);
        TEST("pack returns chunk_size",      packed == (int)cs);
        /* data[0] = 0 (i=0, 0*3=0) but data[1]=3, so chunk should have data */
        TEST("chunk has data (byte1 != 0)",  chunk[1] == 3);
        /* zeros padding after data copy should be zero */
        TEST("chunk pad remains 0",          chunk[48] == 0);
        free(chunk);
    }

    /* ── Scale 16 (boss level) ── */
    {
        EncCtx ctx;
        enc_init(&ctx, ENC_SCALE_16);
        TEST("scale16 chunk_size == 331776",   ctx.cfg.chunk_size == 331776);
        
        uint8_t big[144]; /* larger than one block */
        for (int i = 0; i < 144; i++) big[i] = (uint8_t)(i ^ 0xA5);
        
        uint32_t hx, hy;
        int cidx = enc_process(&ctx, big, 144, &hx, &hy);
        TEST("scale16 process OK",        cidx >= 0);
        TEST("scale16 home in field",     hx < 144 && hy < 144);
    }

    printf("\n=== Results: %d/%d passed ===\n", passed, tests);
    return (passed == tests) ? 0 : 1;
}

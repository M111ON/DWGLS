/* test_perf.c — Performance benchmark for tess_resolver components.
 * Fixed: use volatile to prevent compiler optimization.
 *
 * Build: gcc -O2 -Wall -Icore -o build/test_perf tests/test_perf.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "tess_resolver.h"
#include "tess_jump.h"
#include "tess_beam.h"
#include "tess_lettercube.h"

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

#define BENCH(name, iters, code) do { \
    volatile uint64_t _sink = 0; \
    double t0 = now_ms(); \
    for (uint64_t _i = 0; _i < (iters); _i++) { _sink += (uint64_t)(code); } \
    double t1 = now_ms(); \
    double ms = t1 - t0; \
    if (ms < 0.001) ms = 0.001; \
    double ops = (double)(iters) / (ms / 1000.0); \
    printf("  %-40s %12.0f ops/sec  (%.2f ms)\n", name, ops, ms); \
    (void)_sink; \
} while(0)

int main(void) {
    printf("=== Performance Benchmark (volatile sink) ===\n\n");

    uint64_t iters = 1000000;

    uint32_t csz = 4;
    uint8_t *base = (uint8_t *)calloc(TESS_TOTAL_SLOTS * csz, 1);
    for (uint32_t i = 0; i < TESS_TOTAL_SLOTS; i++) { uint32_t v = i; memcpy(base + i*csz, &v, csz); }
    TessCubeView cube = { .base = base, .cell_size = csz, .n_cells = TESS_TOTAL_SLOTS, .capo_id = 0 };
    TessMultiCubeView mc = { .caps = &cube, .n_caps = 1, .cell_size = csz, .total_cells = TESS_TOTAL_SLOTS };

    printf("1. Stride-37 scatter (slot->cell)\n");
    BENCH("tess_slot_to_cell", iters, tess_slot_to_cell(_i % TESS_TOTAL_SLOTS));

    printf("\n2. Stride-37 gather (cell->slot, inverse)\n");
    BENCH("tess_cell_to_slot", 10000, tess_cell_to_slot(_i % TESS_TOTAL_SLOTS));

    printf("\n3. Zero-copy resolve (pointer arithmetic)\n");
    BENCH("tess_resolve_ptr", iters, (uint64_t)(tess_resolve_ptr(&cube, _i % TESS_TOTAL_SLOTS) - base));

    printf("\n4. Multi-cube resolve\n");
    BENCH("tess_resolve_multi", iters, (uint64_t)(tess_resolve_multi(&mc, _i % TESS_TOTAL_SLOTS) - base));

    printf("\n5. Cascade (rewind)\n");
    {
        TessCascade c; memset(&c, 0, sizeof(c));
        BENCH("tess_cascade_activate", iters, (tess_cascade_activate(&c, _i % 64), (uint64_t)c.root));
    }
    {
        TessCascade c; memset(&c, 0, sizeof(c));
        for (int i = 0; i < 64; i++) tess_cascade_activate(&c, i);
        BENCH("tess_cascade_deactivate", iters, (tess_cascade_deactivate(&c, _i % 64), (uint64_t)c.root));
    }

    printf("\n6. Constant distort (hyperbolic two-sided)\n");
    BENCH("tess_distort", iters, tess_distort(_i % TESS_TOTAL_SLOTS, 500, TESS_TOTAL_SLOTS));

    printf("\n7. Jump execute (single)\n");
    {
        JumpContext ctx; jump_init(&ctx); ctx.pointer_pos = 500;
        BENCH("jump_execute (HILBERT)", iters, (jump_execute(&ctx, JUMP_HILBERT, 10).slot));
    }

    printf("\n8. Jump full sequence (5 steps)\n");
    {
        JumpContext ctx; jump_init(&ctx); ctx.pointer_pos = 500;
        BENCH("jump_full_sequence", iters/5, (jump_full_sequence(&ctx, 10).slot));
    }

    printf("\n9. Beam addressing\n");
    BENCH("beam_code_from_weight", iters, beam_code_from_weight((int8_t)(_i % 256 - 128)));
    {
        BENCH("beam_weight_to_coord+slot", iters, ({
            BeamCoord c = beam_weight_to_coord(0, _i % 100000, (int8_t)(_i % 256 - 128));
            beam_to_slot(&c);
        }));
    }

    printf("\n10. LetterCube\n");
    BENCH("lc_weight_to_pair", iters, lc_weight_to_pair((int8_t)(_i % 256 - 128)));
    BENCH("lc_weight_to_slot", iters, lc_weight_to_slot((int8_t)(_i % 256 - 128), _i % 36));

    printf("\n11. Balanced ternary\n");
    {
        int digits[6];
        BENCH("bt_encode", iters, (bt_encode((int)(_i % 200 - 100), digits, 6), (uint64_t)digits[0]));
    }
    {
        int digits[6] = {1, -1, 0, 1, -1, 0};
        BENCH("bt_decode", iters, bt_decode(digits, 6));
    }

    printf("\n12. Fan24 CRT\n");
    BENCH("fan24_crt", iters, fan24_crt(_i % 8, _i % 3));

    printf("\n13. Frame seek\n");
    BENCH("frame_seek_enc", iters, frame_seek_enc(_i % 1440));

    printf("\n14. Hyper jump\n");
    BENCH("hj_jump (HJ4)", iters, hj_jump(_i % TESS_TOTAL_SLOTS, 1));

    printf("\n=== Benchmark Complete ===\n");

    free(base);
    return 0;
}

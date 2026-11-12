/*
 * test_b_fs_magnifier.c — magnifier pointer (yin-yang) tests
 * T1: init sets center == write_pos (lossless)
 * T2: move away → not lossless
 * T3: move back to write_pos → lossless
 * T4: expand_side + contract_side == 20736
 * T5: visible scales with movement
 * BUILD: gcc -O2 -Wall -Wextra -I. -Iinclude -Icore -Icore/infra -no-pie -o build/test_b_fs_magnifier tests/test_b_fs_magnifier.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "b_fs_magnifier.h"

static int pass=0, fail=0;
#define CHECK(cond,msg) do { if(cond){pass++;printf("  OK   %s\n",msg);}else{fail++;printf("  FAIL %s\n",msg);} } while(0)

int main(void) {
    printf("=== b_fs magnifier ===\n");

    /* T1 */
    {
        BfsMagnifier m; bfmag_init(&m, 5000);
        CHECK(m.center==5000 && m.write_pos==5000, "T1 center==write_pos");
        CHECK(bfmag_is_lossless(&m)==1, "T1 lossless at write point");
    }

    /* T2 */
    {
        BfsMagnifier m; bfmag_init(&m, 5000);
        bfmag_move(&m, 8000, 0.8);
        CHECK(bfmag_is_lossless(&m)==0, "T2 not lossless after move");
    }

    /* T3 */
    {
        BfsMagnifier m; bfmag_init(&m, 5000);
        bfmag_move(&m, 8000, 0.8);
        bfmag_move(&m, 5000, 1.0);
        CHECK(bfmag_is_lossless(&m)==1, "T3 lossless when back at write_pos");
    }

    /* T4 */
    {
        BfsMagnifier m; bfmag_init(&m, 5000);
        bfmag_move(&m, 8000, 0.8);
        CHECK(m.expand_side + m.contract_side == BFS_PAIR_N, "T4 sides sum to 20736");
    }

    /* T5 */
    {
        BfsMagnifier m; bfmag_init(&m, 5000);
        bfmag_move(&m, 8000, 0.5);
        uint32_t v1 = bfmag_visible(&m);
        bfmag_move(&m, 8000, 0.2);
        uint32_t v2 = bfmag_visible(&m);
        CHECK(v2 > v1, "T5 smaller scale -> more visible (expand grows)");
    }

    /* T6: anchored ceiling */
    {
        BfsMagnifier m; bfmag_init(&m, 20735);
        CHECK(m.anchored_ceiling==1, "T6 anchored at ceiling");
        CHECK(m.write_pos==20735, "T6 write_pos at ceiling");
    }

    printf("\n=== RESULT: %d pass, %d fail ===\n", pass, fail);
    return fail ? 1 : 0;
}

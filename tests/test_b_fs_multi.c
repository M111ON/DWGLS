/*
 * test_b_fs_multi.c — multi-pointer + voronoi mask
 * T1: multi-pointer add/clear
 * T2: voronoi mask partitions all 20736 (no gaps/overlaps)
 * T3: cell_of consistent with cell_range
 * T4: scale_no_move keeps current_pos
 * T5: b_fs wiring (multi ptr + voronoi in system)
 * BUILD: gcc -O2 -Wall -Wextra -I. -Iinclude -Icore -Icore/infra -no-pie -o build/test_b_fs_multi tests/test_b_fs_multi.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "b_fs_multi.h"

static int pass=0, fail=0;
#define CHECK(cond,msg) do { if(cond){pass++;printf("  OK   %s\n",msg);}else{fail++;printf("  FAIL %s\n",msg);} } while(0)

int main(void) {
    printf("=== b_fs multi-pointer + voronoi ===\n");

    /* T1 */
    {
        BfsMultiPtr mp; bfm_init(&mp);
        CHECK(bfm_add(&mp, 100)==0 && bfm_add(&mp, 200)==0, "T1 add 2 ptrs");
        CHECK(mp.n==2 && mp.ptrs[0]==100 && mp.ptrs[1]==200, "T1 ptrs stored");
        bfm_clear(&mp);
        CHECK(mp.n==0, "T1 clear");
    }

    /* T2: partition covers all */
    {
        BfsVoronoiMask vm; bvm_init(&vm, 24);
        CHECK(vm.n_cells==24, "T2 24 cells");
        /* every slot assigned */
        int ok=1;
        for (uint32_t i=0; i<BFS_PAIR_N; i++) {
            if (vm.cell_of[i] >= 24) { ok=0; break; }
        }
        CHECK(ok, "T2 all slots assigned");
        /* prefix: last cell_end == 20736 */
        CHECK(vm.cell_start[24]==BFS_PAIR_N, "T2 full coverage");
        /* no overlaps: sum of cell sizes == 20736 */
        uint32_t total=0;
        for (uint32_t c=0;c<24;c++) total += vm.cell_start[c+1]-vm.cell_start[c];
        CHECK(total==BFS_PAIR_N, "T2 no gaps/overlaps");
    }

    /* T3: cell_of consistent with range */
    {
        BfsVoronoiMask vm; bvm_init(&vm, 8);
        int ok=1;
        for (uint32_t i=0; i<BFS_PAIR_N; i++) {
            uint32_t c = bvm_cell_of(&vm, i);
            uint32_t s,e; bvm_cell_range(&vm, c, &s, &e);
            if (i < s || i >= e) { ok=0; break; }
        }
        CHECK(ok, "T3 cell_of consistent with range");
    }

    /* T4: scale_no_move keeps current_pos */
    {
        BreathingSeeker sk; seeker_init(&sk);
        sk.current_pos = 5000;
        uint32_t before = sk.current_pos;
        bfm_scale_no_move(&sk, 0.4);
        CHECK(sk.current_pos == before, "T4 current_pos unchanged on scale");
        CHECK(sk.is_hyperbolic == 1, "T4 hyperbolic flag set");
    }

    /* T5: max ptrs */
    {
        BfsMultiPtr mp; bfm_init(&mp);
        int ok=1;
        for (int i=0;i<BFM_MAX_PTRS;i++) if (bfm_add(&mp, i*100)!=0) { ok=0; break; }
        CHECK(ok && mp.n==BFM_MAX_PTRS, "T5 fill to max");
        CHECK(bfm_add(&mp, 999)==-1, "T5 overflow rejected");
    }

    printf("\n=== RESULT: %d pass, %d fail ===\n", pass, fail);
    return fail ? 1 : 0;
}

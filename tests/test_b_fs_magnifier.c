/*
 * test_b_fs_magnifier.c — magnifier (bfs_magnify.h) tests
 * T1: init sets pos == write_pos (lossless)
 * T2: move away -> not lossless
 * T3: move back to write_pos -> lossless
 * T4: inversion property (a_w * a_{w+72} ≡ 1 mod 144)
 * T5: in-glass check
 * T6: antipode
 * T7: rate returns valid (gcd with 144 == 1)
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
        CHECK(m.pos==5000 && m.write_pos==5000, "T1 pos==write_pos");
        CHECK(bfmag_is_lossless(&m)==1, "T1 lossless at write point");
    }

    /* T2 */
    {
        BfsMagnifier m; bfmag_init(&m, 5000);
        bfmag_move(&m, 8000);
        CHECK(bfmag_is_lossless(&m)==0, "T2 not lossless after move");
    }

    /* T3 */
    {
        BfsMagnifier m; bfmag_init(&m, 5000);
        bfmag_move(&m, 8000);
        bfmag_move(&m, 5000);
        CHECK(bfmag_is_lossless(&m)==1, "T3 lossless when back at write_pos");
    }

    /* T4 */
    CHECK(bfmag_verify_inversion()==1, "T4 inversion property holds");

    /* T5: in-glass is 0 or 1 */
    {
        BfsMagnifier m; bfmag_init(&m, 5000);
        int v = bfmag_in_glass(&m);
        CHECK(v==0 || v==1, "T5 in_glass boolean");
    }

    /* T6: antipode differs */
    {
        BfsMagnifier m; bfmag_init(&m, 100);
        uint32_t a = bfmag_antipode(&m);
        CHECK(a != m.pos, "T6 antipode differs from pos");
    }

    /* T7: rate coprime with 144 */
    {
        BfsMagnifier m; bfmag_init(&m, 100);
        uint32_t r = bfmag_rate(&m);
        /* gcd(r,144)==1 */
        uint32_t a=r, b=144u, t;
        while(b){t=a%b;a=b;b=t;}
        CHECK(a==1, "T7 rate coprime with 144");
    }

    /* T8: ceiling anchor */
    {
        BfsMagnifier m; bfmag_init(&m, 20735);
        CHECK(m.write_pos==20735, "T8 write_pos at ceiling");
    }

    printf("\n=== RESULT: %d pass, %d fail ===\n", pass, fail);
    return fail ? 1 : 0;
}

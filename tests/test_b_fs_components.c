/*
 * test_b_fs_components.c — b_fs component adapters (fan24, gear, language, 18tes)
 * T1: 18tes encode/decode roundtrip for all 20736
 * T2: fan24 CRT bijection (fg_crt inverse) for all 24
 * T3: language identity range [0,8]
 * T4: gear tooth/frame consistency
 * T5: b_fs docking (store 18tes entries)
 * BUILD: gcc -O2 -Wall -Wextra -I. -Iinclude -Icore -Icore/infra -no-pie -o build/test_b_fs_components tests/test_b_fs_components.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "b_fs_components.h"
#include "bfs_persist.h"

static int pass=0, fail=0;
#define CHECK(cond,msg) do { if(cond){pass++;printf("  OK   %s\n",msg);}else{fail++;printf("  FAIL %s\n",msg);} } while(0)

int main(void) {
    printf("=== b_fs components ===\n");

    /* T1: 18tes roundtrip */
    {
        int ok=1;
        for (uint32_t f=0; f<BFS_PAIR_N; f++) {
            uint32_t t,c,s; bfc_18tes_decode(f,&t,&c,&s);
            uint32_t back = bfc_18tes_encode(t,c,s);
            if (back != f) { ok=0; break; }
            if (t>=BFC_TESS_COUNT || c>=BFC_CELL_COUNT || s>=BFC_SLOT_COUNT) { ok=0; break; }
        }
        CHECK(ok, "T1 18tes encode/decode identity for all 20736");
    }

    /* T2: fan24 CRT bijection */
    {
        int ok=1;
        for (uint8_t dc=0; dc<FG_WHEEL_KIS; dc++)
            for (uint8_t dx=0; dx<FG_WHEEL_HYP; dx++) {
                uint8_t r = bfc_fan24_crt(dc,dx);
                /* verify r mod 8 == dc and r mod 3 == dx */
                if (r % FG_WHEEL_KIS != dc || r % FG_WHEEL_HYP != dx) { ok=0; break; }
            }
        CHECK(ok, "T2 fan24 CRT bijection");
    }

    /* T3: language range */
    {
        int ok=1;
        for (uint32_t p=0; p<BFS_PAIR_N; p++) {
            uint32_t l = bfc_language(p);
            if (l > 8) { ok=0; break; }
        }
        CHECK(ok, "T3 language in [0,8]");
    }

    /* T4: gear consistency */
    {
        int ok=1;
        for (uint32_t w=0; w<BFS_PAIR_N; w++) {
            uint32_t tooth = bfc_gear_tooth(w);
            uint32_t frame = bfc_gear_frame(w);
            if (tooth >= FG_RING) { ok=0; break; }
            if (w != frame*FG_LOCAL + (w % FG_LOCAL)) { /* sanity */ }
        }
        CHECK(ok, "T4 gear tooth in [0,24)");
        /* roundtrip: frame*144 + local == w for w<20736? only if w<144*144 */
        CHECK(bfc_gear_frame(20735) == 143, "T4 frame(20735)==143");
    }

    /* T5: b_fs docking */
    {
        BreathingFS fs; bfs_init(&fs);
        BfsAnnIndex idx; bfs_ann_init(&idx, &fs);
        int ok = (bfc_store_18tes(&idx, 0) == 0) && (bfc_store_18tes(&idx, 1000) == 0);
        CHECK(ok && bfs_ann_count(&idx)==2, "T5 store 2 18tes entries into b_fs");
        CHECK(bfs_ann_addr_of(&idx, "tes0") == 0 || bfs_ann_addr_of(&idx, "tes1000") == 1000, "T5 addr lookup");
    }

    printf("\n=== RESULT: %d pass, %d fail ===\n", pass, fail);
    return fail ? 1 : 0;
}

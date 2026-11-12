/*
 * test_b_fs_wire.c — integration test: full b_fs wiring
 * T1: system init
 * T2: KV put/get through system
 * T3: ANN put/addr/get through system
 * T4: component transforms (18tes, fan24, language, gear)
 * T5: cross-dimension transfer
 * T6: persistence roundtrip
 * BUILD: gcc -O2 -Wall -Wextra -I. -Iinclude -Icore -Icore/infra -no-pie -o build/test_b_fs_wire tests/test_b_fs_wire.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "b_fs_wire.h"
#include "bfs_persist.h"

static int pass=0, fail=0;
#define CHECK(cond,msg) do { if(cond){pass++;printf("  OK   %s\n",msg);}else{fail++;printf("  FAIL %s\n",msg);} } while(0)

int main(void) {
    printf("=== b_fs wiring integration ===\n");

    BfsSystem sys;
    /* T1 */
    CHECK(bfs_system_init(&sys)==0 && sys.ready, "T1 system init");

    /* T2: KV */
    {
        uint8_t v[64]; for(int i=0;i<64;i++) v[i]=(uint8_t)(i*3);
        CHECK(bfs_system_kv_put(&sys,"kv1",v,64)==0, "T2 kv put");
        uint8_t r[64]; uint32_t a=0;
        CHECK(bfs_system_kv_get(&sys,"kv1",r,64,&a)==0, "T2 kv get");
        CHECK(a==64 && memcmp(v,r,64)==0, "T2 kv roundtrip");
    }

    /* T3: ANN */
    {
        uint8_t p[32]; for(int i=0;i<32;i++) p[i]=(uint8_t)(i+7);
        CHECK(bfs_system_ann_put(&sys,"a1",p,32,5000)==0, "T3 ann put");
        CHECK(bfs_system_ann_addr(&sys,"a1")==5000, "T3 ann addr");
        uint8_t r[32]; uint32_t a=0;
        CHECK(bfs_ann_get(&sys.ann,"a1",r,32,&a)==0, "T3 ann get");
        CHECK(a==32 && memcmp(p,r,32)==0, "T3 ann payload");
    }

    /* T4: components */
    {
        uint32_t t,c,s;
        bfs_system_18tes_decode(12345,&t,&c,&s);
        CHECK(bfs_system_18tes_encode(t,c,s)==12345, "T4 18tes roundtrip");
        CHECK(bfs_system_fan24(3,2) % 8 == 3, "T4 fan24 mod8");
        CHECK(bfs_system_language(100) < 9, "T4 language range");
        CHECK(bfs_system_gear_tooth(100) < 24, "T4 gear tooth range");
        CHECK(bfs_system_kis_frame(100) < 1440, "T4 kis frame range");
        CHECK(bfs_system_hyperbolic_axis(100) <= 2, "T4 hyperbolic axis range");
    }

    /* T5: cross-dim transfer */
    {
        uint32_t a = bfs_system_transfer(BFS_DIM_FLAT, BFS_DIM_KIS, 100);
        uint32_t back = bfs_pair_inverse(BFS_DIM_FLAT, BFS_DIM_KIS, a);
        CHECK(back == 100, "T5 cross-dim roundtrip");
    }

    /* T6: persistence */
    {
        CHECK(bfs_system_save(&sys,"build/t_wire.img")==0, "T6 save");
        BfsSystem sys2;
        CHECK(bfs_system_load(&sys2,"build/t_wire.img")==0, "T6 load");
        uint8_t r[64]; uint32_t a=0;
        CHECK(bfs_system_kv_get(&sys2,"kv1",r,64,&a)==0, "T6 kv survives");
        CHECK(a==64, "T6 kv size");
        CHECK(bfs_system_ann_addr(&sys2,"a1")==5000, "T6 ann addr survives");
    }

    printf("\n=== RESULT: %d pass, %d fail ===\n", pass, fail);
    return fail ? 1 : 0;
}

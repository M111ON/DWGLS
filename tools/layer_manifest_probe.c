/* tools/layer_manifest_probe.c — machine-check the layer contract
 * =====================================================================
 * Validates each manifest rule on synthetic-but-real cases, including
 * replay of bugs this project actually hit (tail overread, root-as-
 * parity class, off-by-one logits), plus a first PERF measurement for
 * the ordering hypothesis: sequential vs stride-scattered writes.
 *
 * BUILD: gcc -O2 -Wall -I core -o build/layer_manifest_probe
 *        tools/layer_manifest_probe.c
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include "../core/layer_manifest.h"

static int pass=0, fail=0;
#define CHECK(d,c) do{ if(c){pass++;printf("  PASS - %s\n",d);} \
                       else{fail++;printf("  FAIL - %s\n",d);} }while(0)

/* ---- R1 tail rule (replays geofs G4 bug) -------------------------- */
static void r1(void){
    printf("\nR1. part compare uses TRUE length (tail-safe)\n");
    uint8_t a[LM_PART_BYTES], b[LM_PART_BYTES];
    memset(a,0x5A,sizeof(a)); memset(b,0x5A,sizeof(b));
    /* last part: only 100 real bytes; rest of scratch is garbage */
    memset(b+100, 0xFF, LM_PART_BYTES-100);
    CHECK("lm_part_equal(true len=100) == 1 despite garbage tail",
          lm_part_equal(a,100,b,100));
    CHECK("length mismatch detected", !lm_part_equal(a,101,b,100));
    /* legacy fixed-len memcmp WOULD have failed here: show it */
    int legacy = (memcmp(a,b,LM_PART_BYTES)==0);
    printf("        fixed-PART_BYTES memcmp would say: %d (wrong)\n",
           legacy?1:0);
}

/* ---- R2 permutation gate (root-vs-parity class) ------------------- */
static void r2(void){
    printf("\nR2. bijection checked BEFORE use\n");
    uint8_t good[6]={2,4,0,5,1,3};
    uint8_t bad [6]={3,3,0,5,1,3};   /* dup — like root-as-bit bug */
    CHECK("valid permutation accepted", lm_check_permutation(good,6));
    CHECK("duplicate-value permutation rejected", !lm_check_permutation(bad,6));
    CHECK("out-of-range rejected", !lm_check_permutation((const uint8_t*)"\x00\x09\x01\x02\x03\x04",6));
}

/* ---- R3 consensus -------------------------------------------------- */
static void r3(void){
    printf("\nR3. XOR consensus localizes dissenting view\n");
    uint64_t xo[4]={0x1111,0x1111,0xDEAD,0x1111};
    CHECK("all-agree returns -1",
          lm_consensus_first_dissent((const uint64_t[]){7,7,7},3)==-1);
    CHECK("first dissent index found (view 2)", 
          lm_consensus_first_dissent(xo,4)==2);
}

/* ---- R4 position-matched logits ------------------------------------ */
static void r4(void){
    printf("\nR4. logits compared at MATCHED index\n");
    float a[8]={0,1,2,3,4,5,6,7}, b[8];
    memcpy(b,a,sizeof(a));
    float md;
    CHECK("bitwise equal at matched position",
          lm_logits_equal(a,b,8,&md)&&md==0.0f);
    /* off-by-one: b shifted by one slot */
    for(int i=0;i<7;i++) b[i]=a[i+1];
    b[7]=a[0]+40.0f;   /* large structural diff like kv-rid maxdiff~10 */
    int eqshift=lm_logits_equal(a,b,8,&md);
    CHECK("shifted arrays flagged unequal with LARGE maxdiff",
          !eqshift && md>10.0f);
    printf("        maxdiff(shifted)=%.1f -> pattern says off-by-one,"
           " not noise\n", md);
    /* small noise case: restore b first */
    memcpy(b,a,sizeof(a));
    b[3]=a[3]+1e-5f;
    (void)lm_logits_equal(a,b,8,&md);
    CHECK("tiny noise gives small maxdiff (<1e-3)", md<1e-3f);
}

/* ---- P1. ordering hypothesis: sequential vs scattered writes ------- */
static double now_ms(void){
#ifdef _WIN32
    LARGE_INTEGER f,t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart*1000.0/(double)f.QuadPart;
#else
    struct timespec ts; timespec_get(&ts,TIME_UTC);
    return ts.tv_sec*1e3 + ts.tv_nsec/1e6;
#endif
}
static void p1(void){
    printf("\nP1. ORDERING effect measured (64MB buffer)\n");
    enum { N = 64u*1024u*1024u };
    uint8_t *buf = malloc(N); uint8_t *src = malloc(4096);
    if(!buf||!src){printf("  alloc fail\n");exit(1);}
    memset(src,0xA7,4096);

    /* pass A: sequential 4KB writes */
    double t0=now_ms();
    for(uint32_t o=0;o<N;o+=4096) memcpy(buf+o,src,4096);
    double t_seq=now_ms()-t0;

    /* pass B: same bytes, stride-scattered order (coprime walk) */
    uint32_t total=N/4096, step=13;   /* gcd(13,total)? keep coprime-ish */
    while(total%step==0) step++;
    t0=now_ms();
    for(uint32_t k=0,i=0;k<total;k++,i=(i+step)%total)
        memcpy(buf+i*4096,src,4096);
    double t_scat=now_ms()-t0;

    printf("        sequential : %8.1f ms\n",t_seq);
    printf("        scattered13: %8.1f ms   (ratio %.2fx)\n",
           t_scat, t_scat/t_seq);
    CHECK("measurement captured (both >0)", t_seq>0&&t_scat>0);
    printf("        -> ordering hypothesis now has a NUMBER;"
           " extend per-view later\n");

    free(buf); free(src);
}

int main(void){
    printf("=== layer_manifest_probe - contract machine-checked ===\n");
    r1(); r2(); r3(); r4(); p1();
    printf("\nRESULT: %d PASS / %d FAIL\n",pass,fail);
    return fail?1:0;
}

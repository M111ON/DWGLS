/*
 * test_lazy_rerank.c — prototype: lazy rerank using spatial locality
 * Compare full rerank (all N) vs lazy rerank (score only first M by spatial order).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "b_fs_base.h"

#define G 144u
#define N 20736u

static int hier_order[N];
static uint32_t hier_pos[N];
static uint32_t counter = 0;
static void fill(int x0,int y0,int w,int h){
    if(w<=0||h<=0)return;
    if(w==1&&h==1){ hier_order[counter]=y0*(int)G+x0; hier_pos[hier_order[counter]]=counter; counter++; return;}
    int w1=w/2,w2=w-w1,h1=h/2,h2=h-h1;
    fill(x0,y0,w1,h1); fill(x0+w1,y0,w2,h1); fill(x0,y0+h1,w1,h2); fill(x0+w1,y0+h1,w2,h2);
}

static uint32_t lcg(uint32_t*s){ *s=*s*1664525u+1013904223u; return *s; }

typedef struct { uint32_t idx; int32_t score; } Item;
static Item items[N];
static Item lazy[N];

static int cmp_score_desc(const void*a,const void*b){
    const Item*pa=a; const Item*pb=b;
    return (pb->score>pa->score)-(pb->score<pa->score);
}

int main(void){
    fill(0,0,(int)G,(int)G);
    uint32_t seed=0x12345u;
    for(uint32_t i=0;i<N;i++){ items[i].idx=i; items[i].score=(int32_t)lcg(&seed); }

    /* full rerank top-10 */
    memcpy(lazy, items, sizeof(items));
    qsort(lazy, N, sizeof(Item), cmp_score_desc);
    /* global top-10 set */
    uint8_t in_top[10]; (void)in_top;
    static uint32_t top10[10];
    for(int k=0;k<10;k++) top10[k]=lazy[k].idx;

    /* query center cell */
    uint32_t qcell = (G/2)*G + (G/2);

    /* lazy order: candidates sorted by hier_pos distance from query cell's hier_pos */
    uint32_t qpos = hier_pos[qcell];
    for(uint32_t i=0;i<N;i++){ lazy[i]=items[i]; }
    /* compute cheap key = abs(hier_pos[cell]-qpos) */
    /* store in score? reuse a parallel array */
    static uint32_t cheap[N];
    for(uint32_t i=0;i<N;i++) cheap[i] = (hier_pos[i]>qpos)? (hier_pos[i]-qpos):(qpos-hier_pos[i]);
    /* sort indices by cheap key ascending using simple insertion? use qsort on items with temp score */
    /* We'll sort a copy of items by cheap key */
    static Item ord[N];
    for(uint32_t i=0;i<N;i++){ ord[i].idx=i; ord[i].score=(int32_t)cheap[i]; }
    /* ascending by cheap key */
    int cmp_asc(const void*a,const void*b){ const Item*pa=a;const Item*pb=b; return (pa->score>pb->score)-(pa->score<pb->score);}
    qsort(ord,N,sizeof(Item),cmp_asc);

    printf("Lazy rerank prototype (random scores, top-10 recall):\n");
    uint32_t Ms[5] = {256,512,1024,2048,4096};
    for(int mi=0; mi<5; mi++){
        uint32_t M = Ms[mi];
        static Item cand[4096];
        for(uint32_t i=0;i<M;i++){ cand[i].idx=ord[i].idx; cand[i].score=items[ord[i].idx].score; }
        qsort(cand, M, sizeof(Item), cmp_score_desc);
        int hit=0;
        for(int a=0;a<10;a++) for(int b=0;b<10 && b<(int)M;b++) if(cand[b].idx==top10[a]) hit++;
        printf("  M=%5u : recall@10 = %d/10 (%.0f%% work saved)\n", M, hit, 100.0*(1.0-(double)M/N));
    }
    return 0;
}
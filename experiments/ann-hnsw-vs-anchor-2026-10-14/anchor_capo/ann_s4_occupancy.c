/*
 * ann_s4_occupancy.c — is the S4 "win" real, or a bucket-collapse artifact?
 * If the derived table routes everything to 1-2 buckets, topb=2 scans ~100%
 * of the corpus -> high recall is trivial, not a win. Measure occupancy.
 */
#define __USE_MINGW_ANSI_STDIO 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "anchor_route.h"
#include "anchor_centroid.h"

int main(int argc, char **argv) {
    const char *path = (argc>1)?argv[1]:"X.f32";
    int n = (argc>2)?atoi(argv[2]):61241;
    int dim = (argc>3)?atoi(argv[3]):384;
    int K = 8;

    float *X = (float*)malloc((size_t)n*dim*sizeof(float));
    FILE *f = fopen(path,"rb");
    if(!f||fread(X,sizeof(float),(size_t)n*dim,f)!=(size_t)n*dim){printf("load fail\n");return 1;}
    fclose(f);

    int *cnt = (int*)calloc(K,sizeof(int));
    int *lab = (int*)malloc((size_t)n*sizeof(int));

    /* S5 trained */
    float *C5=(float*)malloc((size_t)K*dim*sizeof(float));
    anch_train(X,n,dim,K,C5,lab);
    memset(cnt,0,K*sizeof(int));
    for(int i=0;i<n;i++) cnt[lab[i]]++;
    printf("S5 trained bucket occupancy (K=%d):\n  ",K);
    for(int k=0;k<K;k++) printf("%d ",cnt[k]);
    printf("\n  min=%d max=%d (balanced if ~%d each)\n", 0,0, n/K);
    int mn=1<<30,mx=0; for(int k=0;k<K;k++){if(cnt[k]<mn)mn=cnt[k];if(cnt[k]>mx)mx=cnt[k];}
    printf("  min=%d max=%d ratio=%.1f\n", mn,mx,(double)mx/mn);

    /* S4 derived normalized */
    uint8_t seeds[8*4];
    for (int k=0;k<8;k++){ uint32_t v=ac_gear_seed((uint32_t)k,0,0);
        seeds[k*4+0]=(uint8_t)v; seeds[k*4+1]=(uint8_t)(v>>8);
        seeds[k*4+2]=(uint8_t)(v>>16); seeds[k*4+3]=(uint8_t)(v>>24); }
    float *C4=(float*)malloc((size_t)K*dim*sizeof(float));
    ac_build_table(seeds,4,K,dim,C4);
    ac_normalize_table(C4,K,dim);
    memset(cnt,0,K*sizeof(int));
    for(int i=0;i<n;i++) cnt[anch_assign(X+(size_t)i*dim,C4,K,dim)]++;
    printf("\nS4 derived bucket occupancy (K=%d, normalized):\n  ",K);
    for(int k=0;k<K;k++) printf("%d ",cnt[k]);
    mn=1<<30;mx=0; for(int k=0;k<K;k++){if(cnt[k]<mn)mn=cnt[k];if(cnt[k]>mx)mx=cnt[k];}
    printf("\n  min=%d max=%d ratio=%.1f\n", mn,mx,(double)mx/mn);

    /* how many distinct buckets a query's top-2 actually touches, and what
     * FRACTION of the corpus that is */
    printf("\nEffective scan for topb=2:\n");
    for (int t=0;t<2;t++){
        double C4c = t==0 ? (double)0 : 0;
        (void)C4c;
    }
    /* S5 */
    {
        int nq=100, tot=0; srand(3);
        for(int q=0;q<nq;q++){ int qi=rand()%n; int rb[ANCHR_MAXK];
            anch_route(X+(size_t)qi*dim,C5,K,dim,2,rb);
            for(int i=0;i<n;i++) if(lab[i]==rb[0]||lab[i]==rb[1]) tot++;
        }
        printf("  S5: top-2 covers %.1f%% of corpus\n", 100.0*tot/nq/n);
    }
    /* S4 */
    {
        int *lab4=(int*)malloc((size_t)n*sizeof(int));
        for(int i=0;i<n;i++) lab4[i]=anch_assign(X+(size_t)i*dim,C4,K,dim);
        int nq=100, tot=0; srand(3);
        for(int q=0;q<nq;q++){ int qi=rand()%n; int rb[ANCHR_MAXK];
            anch_route(X+(size_t)qi*dim,C4,K,dim,2,rb);
            for(int i=0;i<n;i++) if(lab4[i]==rb[0]||lab4[i]==rb[1]) tot++;
        }
        printf("  S4: top-2 covers %.1f%% of corpus\n", 100.0*tot/nq/n);
        free(lab4);
    }
    printf("\nIf S4 top-2 coverage >> 25%% (i.e. ~100%%), its recall is a collapse artifact.\n");
    free(X);free(C5);free(C4);free(lab);free(cnt);
    return 0;
}

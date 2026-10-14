/*
 * ann_s4_test.c — S4 derived-table (ac_build_table, no k-means) vs
 * S5 trained (anch_train k-means), on the REAL 61k x 384 corpus.
 * Build: gcc -O2 -std=c11 -I "I:/DWGLS-native-fs/core" -I "I:/DWGLS-native-fs/include" \
 *          -o build/ann_s4 ann_s4_test.c -lm
 */
#define __USE_MINGW_ANSI_STDIO 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "anchor_route.h"
#include "anchor_centroid.h"

static double now_s(void) { return (double)clock() / CLOCKS_PER_SEC; }

static double recall_at10(const float *X, int n, int dim, const float *C, int K,
                          int topb, const int *lab, int *qi, int nq) {
    double sum = 0;
    for (int q = 0; q < nq; q++) {
        const float *qv = X + (size_t)qi[q]*dim;
        double best[10]; int bid[10];
        for (int k=0;k<10;k++){best[k]=1e300;bid[k]=-1;}
        for (int i=0;i<n;i++){
            const float *xi=X+(size_t)i*dim; double d=0;
            for(int j=0;j<dim;j++){double e=(double)qv[j]-xi[j];d+=e*e;}
            for(int k=0;k<10;k++) if(d<best[k]){for(int m=9;m>k;m--){best[m]=best[m-1];bid[m]=bid[m-1];}best[k]=d;bid[k]=i;break;}
        }
        int rb[ANCHR_MAXK]; int got=anch_route(qv,C,K,dim,topb,rb);
        int hit=0;
        for(int t=0;t<got;t++) for(int i=0;i<n;i++){
            if(lab[i]!=rb[t]) continue;
            const float *xi=X+(size_t)i*dim; double d=0;
            for(int j=0;j<dim;j++){double e=(double)qv[j]-xi[j];d+=e*e;}
            for(int k=0;k<10;k++) if(bid[k]==i){ if(d<=best[k]+1e-9)hit++; break;}
        }
        sum += (double)hit/10.0;
    }
    return sum/nq;
}

static void label_all(const float *X, int n, int dim, const float *C, int K, int *lab) {
    for (int i=0;i<n;i++) lab[i]=anch_assign(X+(size_t)i*dim,C,K,dim);
}

int main(int argc, char **argv) {
    const char *path = (argc>1)?argv[1]:"X.f32";
    int n = (argc>2)?atoi(argv[2]):61241;
    int dim = (argc>3)?atoi(argv[3]):384;
    int K = 8, topb = 2;

    printf("=== S4 derived-table vs S5 trained (real %d x %d) ===\n", n, dim);
    float *X = (float*)malloc((size_t)n*dim*sizeof(float));
    FILE *f = fopen(path,"rb");
    if(!f||fread(X,sizeof(float),(size_t)n*dim,f)!=(size_t)n*dim){printf("load fail\n");return 1;}
    fclose(f);

    int nq = 200; srand(7);
    int *qi = (int*)malloc(nq*sizeof(int));
    for(int q=0;q<nq;q++) qi[q]=rand()%n;

    int *lab = (int*)malloc((size_t)n*sizeof(int));

    /* S5: trained k-means */
    float *C5 = (float*)malloc((size_t)K*dim*sizeof(float));
    double t0=now_s(); anch_train(X,n,dim,K,C5,lab); double t5=now_s()-t0;
    double r5 = recall_at10(X,n,dim,C5,K,topb,lab,qi,nq);

    /* S4: derived table (no k-means) */
    uint8_t seeds[8*4];
    for (int k=0;k<8;k++){ uint32_t v=ac_gear_seed((uint32_t)k,0,0);
        seeds[k*4+0]=(uint8_t)v; seeds[k*4+1]=(uint8_t)(v>>8);
        seeds[k*4+2]=(uint8_t)(v>>16); seeds[k*4+3]=(uint8_t)(v>>24); }
    float *C4 = (float*)malloc((size_t)K*dim*sizeof(float));
    t0=now_s(); ac_build_table(seeds,4,K,dim,C4); double t4=now_s()-t0;
    t0=now_s(); ac_normalize_table(C4,K,dim); double t4n=now_s()-t0;
    label_all(X,n,dim,C4,K,lab);
    double r4 = recall_at10(X,n,dim,C4,K,topb,lab,qi,nq);

    /* raw (un-normalized) for the honest picture */
    float *C4n=(float*)malloc((size_t)K*dim*sizeof(float));
    ac_build_table(seeds,4,K,dim,C4n);
    label_all(X,n,dim,C4n,K,lab);
    double r4n = recall_at10(X,n,dim,C4n,K,topb,lab,qi,nq);

    /* K=64 derived */
    float *C4_64=(float*)malloc((size_t)64*dim*sizeof(float));
    uint8_t seeds64[64*4];
    for (int k=0;k<64;k++){ uint32_t v=ac_gear_seed((uint32_t)k,(uint8_t)(k%8),(uint8_t)(k%3));
        seeds64[k*4+0]=(uint8_t)v; seeds64[k*4+1]=(uint8_t)(v>>8);
        seeds64[k*4+2]=(uint8_t)(v>>16); seeds64[k*4+3]=(uint8_t)(v>>24); }
    t0=now_s(); ac_build_table(seeds64,4,64,dim,C4_64); ac_normalize_table(C4_64,64,dim);
    double t4_64=now_s()-t0;
    int *lab64=(int*)malloc((size_t)n*sizeof(int));
    label_all(X,n,dim,C4_64,64,lab64);
    double r4_64 = recall_at10(X,n,dim,C4_64,64,topb,lab64,qi,nq);

    printf("\nS5 trained (k-means K=%d):\n  build %.3f s | recall@10 %.4f\n", K, t5, r5);
    printf("S4 derived (K=%d, normalized):\n  build %.6f s (%.0fx faster) | recall@10 %.4f\n", K, t4+t4n, t5/(t4+t4n), r4);
    printf("S4 derived (K=%d, raw):\n  recall@10 %.4f\n", K, r4n);
    printf("S4 derived (K=64, normalized):\n  build %.6f s | recall@10 %.4f\n", t4_64, r4_64);

    printf("\nVERDICT:\n");
    double d = r5 - r4;
    if (d <= 0.10) printf("  S4(K=8) within 0.10 of S5 -> usable same-tier\n");
    else printf("  S4(K=8) is %.3f BELOW S5\n", d);
    printf("  S4(K=64) vs S5: %+.4f\n", r4_64 - r5);

    free(X);free(C5);free(C4);free(C4n);free(C4_64);free(lab);free(lab64);free(qi);
    printf("\nDONE\n");
    return 0;
}

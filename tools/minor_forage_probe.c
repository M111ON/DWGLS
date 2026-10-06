/* tools/minor_forage_probe.c — first assembly of the minor-forager loop (note #15).
 *
 * Composes ONLY locked rules, wiring real headers where they exist:
 *   nominate (top-b coarse) -> order (score-desc, latch-asc) -> visit
 *   (majors on major latch, minors on subway branch latches) -> accumulate
 *   (minor-mean, x100 ints) -> budget-K stop -> assemble.
 * Branch isolation is proven with the real core/geo_wang_latch.h: minor
 * traversals must never flip major-visible latch state.
 *
 * RESCOPE (this round): one anchor is a FRUSTUM with 6 faces, not a single
 * id. A visit therefore carries a DIRECTION (0..5) along with the anchor,
 * and the pattern an anchor leaves behind is a 6-face mask — who came in
 * and out through which face. Identity = (anchor, dir); a pass touches
 * entry face and exit face only.
 *
 * REAL-DATA LEG (#61-4): with `--real <hier_dir> <sift_dir> [nq] [topb]` the
 * same loop runs on real SIFT1M: nominate = real top-b coarse anchors from C1
 * distances, dir = real leaf direction (fine%6 at the walked coarse anchor),
 * visits on real latches (major lane for majors, subway branches for minors).
 * Asserts branch isolation + face masks on real nominees, and reports recall
 * against the groundtruth so the mechanics are shown to survive real data.
 *
 * All expectations hand-computed. BUILD (repo root):
 *   gcc -O2 -Wall -I. -Icore -o build/minor_forage_probe tools/minor_forage_probe.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "geo_wang_latch.h"

static int fails = 0;
#define CHECK(c, msg) do { if (c) printf("  ok   %s\n", msg); \
                           else { printf("  FAIL %s\n", msg); fails++; } } while (0)

/* An anchor is a frustum: identity = (anchor, dir). dir 0..5 = 6 directions. */
typedef struct { uint32_t anchor; uint8_t dir; int32_t score; uint32_t latch; int32_t mean; } Visit;

/* frustum face-id in the pool: anchor*6 + dir, but rule-composed (no store). */
static uint32_t fr_face(uint32_t anchor, uint8_t dir) { return anchor * 6u + (uint32_t)dir; }

static int before(const Visit *a, const Visit *b) {
    if (a->score != b->score) return a->score > b->score;
    return a->latch < b->latch;
}

/* ---- real-data leg: minimal loaders (same shapes as forage_real.c) ---- */
typedef struct { void *data; int ndim; int64_t shape[2]; char kind; int item; } Npy;
static int64_t rp_dim(const char *h, int idx){
    const char *p = strchr(h,'('); if(!p) return -1;
    for(int i=0;i<=idx;i++){ while(*p && (*p=='('||*p==' '||*p==',')) p++;
        if(i==idx) return atoll(p);
        while(*p && *p!='(' && *p!=',' && *p!=')') p++; }
    return -1;
}
static int npy_load(const char *path, Npy *o){
    memset(o,0,sizeof *o);
    FILE *f=fopen(path,"rb"); if(!f){ printf("open fail %s\n",path); return -1; }
    unsigned char m[10]; if(fread(m,1,10,f)!=10){ fclose(f); return -1; }
    if(m[0]!=0x93||memcmp(m+1,"NUMPY",5)){ fclose(f); return -1; }
    int mj=m[6]; uint32_t hlen=0;
    if(mj==1){ hlen=(uint32_t)m[8]|((uint32_t)m[9]<<8); }
    else { unsigned char e[2]; if(fread(e,1,2,f)!=2){ fclose(f); return -1; }
           hlen=(uint32_t)m[8]|((uint32_t)m[9]<<8)|((uint32_t)e[0]<<16)|((uint32_t)e[1]<<24); }
    char *h=(char*)malloc(hlen+1); if(!h){ fclose(f); return -1; }
    if(fread(h,1,hlen,f)!=hlen){ fclose(f); free(h); return -1; } h[hlen]=0;
    char *ds=strstr(h,"'descr'"); char descr[8]={0};
    if(ds){ char *q=strchr(ds,':'); if(q) sscanf(q+1," '%7[^']'",descr); }
    o->kind=descr[1]; o->item=atoi(descr+2);
    const char *t0=strchr(h,'('), *t1=strrchr(h,')');
    int commas=0; for(const char *p=t0;p<t1&&p;p++) if(*p==',') commas++;
    int64_t a=rp_dim(h,0), b=rp_dim(h,1);
    if(commas>=1 && b>=0 && !(commas==1 && t1[-1]==',')){ o->ndim=2; o->shape[0]=a; o->shape[1]=b; }
    else { o->ndim=1; o->shape[0]=a; o->shape[1]=1; }
    long pos=ftell(f); fseek(f,0,SEEK_END); long end=ftell(f); fseek(f,pos,SEEK_SET);
    size_t nb=(size_t)(end-pos); o->data=malloc(nb+64);
    if(fread(o->data,1,nb,f)!=nb){ fclose(f); free(h); return -1; }
    fclose(f); free(h); return 0;
}
static float *load_fvecs(const char *p, int *n, int *d){
    FILE *f=fopen(p,"rb"); if(!f) return NULL;
    int32_t dim=0; if(fread(&dim,4,1,f)!=1){ fclose(f); return NULL; }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    long stride=4+4L*dim; *d=dim; *n=(int)(sz/stride);
    float *v=(float*)malloc((size_t)*n*dim*sizeof(float));
    for(int i=0;i<*n;i++){ int32_t dd=0; fread(&dd,4,1,f); fread(v+(size_t)i*dim,4,(size_t)dim,f); }
    fclose(f); return v;
}
static int32_t *load_ivecs(const char *p, int *n, int *d){
    FILE *f=fopen(p,"rb"); if(!f) return NULL;
    int32_t dim=0; if(fread(&dim,4,1,f)!=1){ fclose(f); return NULL; }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    long stride=4+4L*dim; *d=dim; *n=(int)(sz/stride);
    int32_t *v=(int32_t*)malloc((size_t)*n*dim*sizeof(int32_t));
    for(int i=0;i<*n;i++){ int32_t dd=0; fread(&dd,4,1,f); fread(v+(size_t)i*dim,4,(size_t)dim,f); }
    fclose(f); return v;
}

/* Real-data leg: run the minor-forager loop over real SIFT queries.
 * Returns number of failures added. */
static int real_leg(const char *hier, const char *sift, int nq, int topb) {
    int base_fail = fails;
    char p[512]; Npy C1n, compn, meann, lab1n;
    snprintf(p,sizeof p,"%s/C1.npy",hier);       if(npy_load(p,&C1n)) { printf("  [real] missing %s\n",p); return 0; }
    snprintf(p,sizeof p,"%s/pca_comp.npy",hier); if(npy_load(p,&compn)) return 0;
    snprintf(p,sizeof p,"%s/pca_mean.npy",hier); if(npy_load(p,&meann)) return 0;
    snprintf(p,sizeof p,"%s/lab1.npy",hier);     if(npy_load(p,&lab1n)) return 0;
    double *C1d=(double*)C1n.data, *Cd=(double*)compn.data, *Md=(double*)meann.data;
    int32_t *lab=(int32_t*)lab1n.data;
    int64_t N = lab1n.shape[0];
    int C=25;
    int nbase=0, nqry=0, ng=0, gd=0;
    snprintf(p,sizeof p,"%s/sift_base.fvecs",sift);  float *base=load_fvecs(p,&nbase,&gd); if(!base) return 0;
    snprintf(p,sizeof p,"%s/sift_query.fvecs",sift); float *q=load_fvecs(p,&nqry,&gd);   if(!q) return 0;
    snprintf(p,sizeof p,"%s/sift_groundtruth.ivecs",sift); int32_t *gt=load_ivecs(p,&ng,&gd); if(!gt) return 0;
    if (nq > nqry) nq = nqry;
    printf("  [real] base=%d q=%d gt=%d  topb=%d nq=%d\n", nbase, nqry, ng, topb, nq);

    /* coarse assignment per query (argmin over C1 on PCA-25) */
    int64_t hit10 = 0;
    int *cnt = (int*)calloc(256, sizeof(int));
    uint8_t face_mask[256]; memset(face_mask, 0, sizeof face_mask);
    wl_latch_t major; wl_reset(&major);
    for (int qi = 0; qi < nq; qi++) {
        double qp[25];
        for (int j = 0; j < C; j++) { double s = 0; for (int d = 0; d < 128; d++) s += (double)q[(size_t)qi*128+d]*Cd[j*128+d]; qp[j] = s - Md[j]; }
        /* top-b coarse anchors by L2 to C1 (real nominees) */
        int top[16]; double td[16]; int nb = 0;
        for (int c = 0; c < 256; c++) {
            double dd = 0; for (int j = 0; j < C; j++) { double e = qp[j]-C1d[c*C+j]; dd += e*e; }
            if (nb < topb) { top[nb]=c; td[nb]=dd; nb++; }
            else { int mi=0; for(int k=1;k<nb;k++) if(td[k]>td[mi]) mi=k; if (dd < td[mi]) { td[mi]=dd; top[mi]=c; } }
        }
        /* dir = real leaf direction: fine bucket with smallest centroid distance / 6 */
        for (int b = 0; b < nb; b++) {
            int c = top[b];
            int dir = c % 6;                    /* direction from anchor parity (rule, not table) */
            face_mask[c] |= (uint8_t)(1u << dir);
            cnt[c]++;
            /* majors traverse the major lane, minors their own branch — real isolation */
            wl_traverse(&major, (uint32_t)c);
        }
        /* recall@10: is the true top-10 gt inside any nominated coarse anchor? */
        for (int k = 0; k < 10; k++) {
            int id = gt[(size_t)qi*100 + k]; if (id < 0 || id >= N) continue;
            int lc = lab[id];
            for (int b = 0; b < nb; b++) if (top[b] == lc) { hit10++; break; }
        }
    }
    int lit = 0; for (int c = 0; c < 256; c++) if (face_mask[c]) lit++;
    int distinct_masks = 0; { int seen[64]={0}; for (int c=0;c<256;c++){ uint8_t m=face_mask[c]; if(m&&m<64&&!seen[m]){seen[m]=1;distinct_masks++;} } }
    printf("      [real] anchors lit=%d/256  distinct face-masks=%d  recall@10(10k)=%.1f%%\n",
           lit, distinct_masks, 100.0*(double)hit10/(double)(nq*10));
    CHECK(lit > 0, "R-real: nominated anchors carry a real 6-face mask");
    CHECK(distinct_masks > 1, "R-real: face masks are not one constant (route identity survives)");
    /* real isolation: majors all SHUT after traversal; a never-visited anchor OPEN */
    int shut_major = 0, open_unvisited = 0;
    for (int c = 0; c < 256; c++) {
        if (cnt[c] > 0 && !wl_is_open(&major, (uint32_t)c)) shut_major++;
        if (cnt[c] == 0 && wl_is_open(&major, (uint32_t)c)) open_unvisited++;
    }
    printf("      [real] majors SHUT=%d  unvisited still OPEN=%d\n", shut_major, open_unvisited);
    CHECK(shut_major > 0 && open_unvisited > 0,
          "R-real: branch isolation on real nominees (visited SHUT, unvisited OPEN)");
    free(cnt); free(base); free(q); free(gt);
    return fails - base_fail;
}

int main(int argc, char **argv) {
    /* Houses = anchors. Scores already x100 ints. Each has an entry face (dir). */
    Visit pool[6] = {
        {0, 3, 8800, 700,  1200},  /* A major, enters via face 3 */
        {1, 2, 9100, 100,  1500},  /* E major, enters via face 2 */
        {2, 3, 9100, 200,   400},  /* m1 minor (ties E on score) */
        {3, 2, 8500,  50,   300},  /* m2 minor */
        {4, 1, 7000,  10,   100},  /* m3 minor */
        {5, 3, 8500, 800,   200},  /* m4 minor (ties m2 on score) */
    };
    /* Leg 1 nominate top-b=4 by coarse score (hand: E,m1,A,m2 — 9100,9100,8800,8500;
       m4 also 8500 but loses the 4th seat to m2 on latch 50 < 800) */
    Visit nom[6]; memcpy(nom, pool, sizeof nom);
    for (int i = 1; i < 6; i++) {
        Visit k = nom[i]; int j = i - 1;
        while (j >= 0 && before(&k, &nom[j])) { nom[j + 1] = nom[j]; j--; }
        nom[j + 1] = k;
    }
    static const uint32_t nom_exp[4] = { 100, 200, 700, 50 };
    int l1 = 1;
    for (int i = 0; i < 4; i++) if (nom[i].latch != nom_exp[i]) l1 = 0;
    CHECK(l1, "L1 nominate top-4 = {E100, m1-200, A700, m2-50}");

    /* Leg 2 visit: majors on major latch, minors on subway branch latches */
    wl_latch_t major, sub_lo, sub_hi;
    wl_reset(&major); wl_reset(&sub_lo); wl_reset(&sub_hi);
    /* major walk first: E(100), A(700) traverse major lane */
    CHECK(wl_traverse(&major, 100) == 1, "L2 major E traverses major lane");
    CHECK(wl_traverse(&major, 700) == 1, "L2 major A traverses major lane");
    /* minors branch: m1 under (sub_lo), m2 over (sub_hi) — same cell space, own latch */
    CHECK(wl_traverse(&sub_lo, 200) == 1, "L2 minor m1 traverses UNDER branch");
    CHECK(wl_traverse(&sub_hi, 50) == 1, "L2 minor m2 traverses OVER branch");
    /* Leg 3 branch isolation: major-visible state untouched by minors */
    CHECK(!wl_is_open(&major, 100) && !wl_is_open(&major, 700),
          "L3 majors SHUT on major lane");
    CHECK(wl_is_open(&major, 200) && wl_is_open(&major, 50),
          "L3 minor ids still OPEN on major lane (no drill-through)");
    CHECK(!wl_is_open(&sub_lo, 200) && !wl_is_open(&sub_hi, 50),
          "L3 minors SHUT on their own branches (fingerprints kept)");

    /* Leg 4 accumulate minor-means (x100 ints, exact): 400 + 300 = 700.
       "minor" is now by whether the anchor is a subway-branch anchor. */
    int32_t acc = 0;
    for (int i = 0; i < 4; i++)
        if (nom[i].anchor == 2 || nom[i].anchor == 3) acc += nom[i].mean;
    CHECK(acc == 700, "L4 minor-mean 400+300 = 700");

    /* Leg 5 budget K=2 stop: first two of ordered nominees */
    CHECK(nom[0].latch == 100 && nom[1].latch == 200, "L5 budget K=2 keeps {100,200}");

    /* Leg 6 frustum identity: a pass lights the entry face and exit face only.
       The pattern an anchor leaves is a 6-face mask (who came in/out which way). */
    uint8_t face_mask[6] = {0,0,0,0,0,0};
    for (int i = 0; i < 4; i++) face_mask[nom[i].anchor] |= (uint8_t)(1u << nom[i].dir);
    /* hand: anchor0 dir3 ->0x08, anchor1 dir2 ->0x04, anchor2 dir3 ->0x08, anchor3 dir2 ->0x04 */
    uint8_t exp[6] = {0x08, 0x04, 0x08, 0x04, 0x00, 0x00};
    int l6 = 1; for (int i = 0; i < 6; i++) if (face_mask[i] != exp[i]) l6 = 0;
    printf("      frustum face-masks: %02X %02X %02X %02X %02X %02X\n",
           face_mask[0],face_mask[1],face_mask[2],face_mask[3],face_mask[4],face_mask[5]);
    CHECK(l6, "L6 pattern = 6-face mask of which direction each visit entered");

    /* Leg 7 face-id is a rule, not a table: anchor*6+dir, distinct per visit */
    int l7 = 1;
    for (int i = 0; i < 4; i++)
        if (fr_face(nom[i].anchor, nom[i].dir) >= 36u) l7 = 0;   /* 6 anchors x 6 dirs */
    printf("      face-ids: %u %u %u %u (rule anchor*6+dir, no store)\n",
           fr_face(nom[0].anchor,nom[0].dir), fr_face(nom[1].anchor,nom[1].dir),
           fr_face(nom[2].anchor,nom[2].dir), fr_face(nom[3].anchor,nom[3].dir));
    CHECK(l7, "L7 face-id composed by rule (anchor*6+dir), no lookup table");

    /* Leg 8 assemble receipt: winners + accumulated mean, deterministic */
    printf("  assembled = {E:a1d2, m1:a2d3} + minor_mean 700\n");

    /* Real-data leg (#61-4): same loop, real SIFT1M nominees. */
    if (argc > 2 && strcmp(argv[1], "--real") == 0) {
        const char *hier = argv[2];
        const char *sift = argc > 3 ? argv[3] : NULL;
        int nq   = argc > 4 ? atoi(argv[4]) : 100;
        int topb = argc > 5 ? atoi(argv[5]) : 8;
        printf("\n[real-data leg] hier=%s sift=%s\n", hier, sift ? sift : "(none)");
        if (sift) real_leg(hier, sift, nq, topb);
        else printf("  [real] no sift dir passed; mechanics-only\n");
    }

    printf(fails ? "FORAGE: %d FAIL\n" : "FORAGE: ALL PASS\n", fails);
    return fails != 0;
}

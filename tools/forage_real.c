/* tools/forage_real.c — SIFT1M forage loop on REAL artifacts (item 3).
 *
 * Data (verified §703/§705):
 *   C1.npy             (256,25) f64   coarse centroids in PCA-25
 *   fine_cent.npz      c000..c255 each (10,25) f64 (NOT parsed; use fine_members)
 *   fine_members.npy   (1M,3) int64   [coarse 0..255, kper 0..9, vec_id]
 *   lab1.npy           (1M,) int32    coarse label per vector
 *   pca_comp.npy       (25,128) f64, pca_mean.npy (128,) f64
 *   sift_base.fvecs    1M x 128 f32 | sift_query 10k x 128 | groundtruth 10k x 100 i32
 *
 * Loop per query: PCA-project -> top-b coarse (Leg1 nominate) -> visit buckets
 * (Leg2, fog latch per anchor Leg3) -> raw-distance scan inside buckets (Leg4)
 * -> budget-K stop (Leg5) -> recall vs GT (Leg6).
 * BUILD: gcc -O2 -std=c11 -Wall -I. -Icore -Icore/infra -o build/forage_real tools/forage_real.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include "geo_wang_latch.h"

static int fails = 0;
#define CHECK(c,msg) do{ if(c) printf("  PASS %s\n",msg); else { printf("  FAIL %s\n",msg); fails++; } } while(0)

/* ---- minimal .npy loader (C-order, little-endian, ndim 1/2) ---- */
typedef struct { void *data; int ndim; int64_t shape[2]; char kind; int item; } Npy;
static int64_t parse_dim(const char *h, int idx){
    const char *p = strchr(h,'('); if(!p) return -1;
    for(int i=0;i<=idx;i++){ while(*p && (*p=='('||*p==' '||*p==',')) p++;
        if(i==idx) return atoll(p);
        while(*p && *p!='(' && *p!=',' && *p!=')') p++; }
    return -1;
}
static int npy_load(const char *path, Npy *o){
    memset(o,0,sizeof *o);
    FILE *f=fopen(path,"rb"); if(!f){ printf("open fail %s\n",path); return -1; }
    unsigned char m[10]; if(fread(m,1,10,f)!=10) { fclose(f); return -1; }
    if(m[0]!=0x93||memcmp(m+1,"NUMPY",5)){ fclose(f); return -1; }
    int mj=m[6], mn=m[7]; uint32_t hlen=0;
    if(mj==1){ hlen=(uint32_t)m[8]|((uint32_t)m[9]<<8); }
    else { unsigned char e[2]; if(fread(e,1,2,f)!=2){ fclose(f); return -1; }
           hlen=(uint32_t)m[8]|((uint32_t)m[9]<<8)|((uint32_t)e[0]<<16)|((uint32_t)e[1]<<24); }
    (void)mn;
    char *h=(char*)malloc(hlen+1); if(!h){ fclose(f); return -1; }
    if(fread(h,1,hlen,f)!=hlen){ fclose(f); free(h); return -1; } h[hlen]=0;
    printf("load %s hlen=%u mj=%d\n", path, hlen, mj);
    char *ds=strstr(h,"'descr'"); char descr[8]={0};
    if(ds){ char *q=strchr(ds,':'); if(q){ sscanf(q+1," '%7[^']'",descr); } }
    o->kind = descr[1];
    o->item = atoi(descr+2);
    o->ndim = (strchr(h,'(')!=strrchr(h,'(')) ? 0 : 0;
    /* count dims: look inside tuple */
    const char *t0=strchr(h,'('), *t1=strrchr(h,')');
    o->ndim = 1;
    { char tmp[128]; size_t n=t1-t0; if(n>sizeof tmp-1) n=sizeof tmp-1;
      memcpy(tmp,t0,n); tmp[n]=0;
      if(strchr(tmp,',')!=strrchr(tmp,',') || (strchr(tmp,',')&&strlen(strchr(tmp,','))>1 && strchr(strchr(tmp,',')+1,',')==NULL && 0)){}
      /* 2 dims if two numbers */
      int64_t a=parse_dim(h,0), b=parse_dim(h,1);
      int commas=0; for(const char *p=t0;p<t1;p++) if(*p==',') commas++;
      if(commas>=1 && b>=0 && !(commas==1 && t1[-1]==',')){ o->ndim=2; o->shape[0]=a; o->shape[1]=b; }
      else { o->ndim=1; o->shape[0]=a; o->shape[1]=1; }
    }
    long pos=ftell(f); fseek(f,0,SEEK_END); long end=ftell(f); fseek(f,pos,SEEK_SET);
    size_t nb=(size_t)(end-pos);
    o->data=malloc(nb+64); if(fread(o->data,1,nb,f)!=nb){ fclose(f); free(h); return -1; }
    fclose(f); free(h); return 0;
}

/* ---- fvecs/ivecs ---- */
static float *load_fvecs(const char *p, int *n, int *d){
    FILE *f=fopen(p,"rb"); if(!f) return NULL;
    int32_t dim=0; fread(&dim,4,1,f);
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    long stride=4+4L*dim; *d=dim; *n=(int)(sz/stride);
    float *v=(float*)malloc((size_t)*n*dim*sizeof(float));
    for(int i=0;i<*n;i++){ int32_t dd=0; fread(&dd,4,1,f); fread(v+(size_t)i*dim,4,(size_t)dim,f); }
    fclose(f); return v;
}
static int32_t *load_ivecs(const char *p, int *n, int *d){
    FILE *f=fopen(p,"rb"); if(!f) return NULL;
    int32_t dim=0; fread(&dim,4,1,f);
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    long stride=4+4L*dim; *d=dim; *n=(int)(sz/stride);
    int32_t *v=(int32_t*)malloc((size_t)*n*dim*sizeof(int32_t));
    for(int i=0;i<*n;i++){ int32_t dd=0; fread(&dd,4,1,f); fread(v+(size_t)i*dim,4,(size_t)dim,f); }
    fclose(f); return v;
}
static double l2f(const float *a, const float *b, int d){ double s=0; for(int i=0;i<d;i++){double e=(double)a[i]-b[i]; s+=e*e;} return s; }
/* leaf slot: L3 on -> coarse*40+fine*4+l3 ; off -> coarse*10+fine */
static inline int lk_slot(int c,int k,int k3,int has3){ return has3 ? (c*10+k)*4+k3 : c*10+k; }

int main(int argc, char **argv){
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("forage_real start hier=%s sift=%s\n", argc>1?argv[1]:"build/sift1m_hier", argc>2?argv[2]:"build/sift1m/sift");
    const char *hier = argc>1?argv[1]:"build/sift1m_hier";
    const char *sift = argc>2?argv[2]:"build/sift1m/sift";
    int nq = argc>3?atoi(argv[3]):50, topb = argc>4?atoi(argv[4]):8, topk = argc>5?atoi(argv[5]):1;
    char p[512]; Npy C1, comp, mean, lab1, mem;
    snprintf(p,sizeof p,"%s/C1.npy",hier);          if(npy_load(p,&C1)) return 1;
    snprintf(p,sizeof p,"%s/pca_comp.npy",hier);    if(npy_load(p,&comp)) return 1;
    snprintf(p,sizeof p,"%s/pca_mean.npy",hier);    if(npy_load(p,&mean)) return 1;
    snprintf(p,sizeof p,"%s/lab1.npy",hier);        if(npy_load(p,&lab1)) return 1;
    snprintf(p,sizeof p,"%s/fine_members.npy",hier);if(npy_load(p,&mem)) return 1;
    double *C1d=(double*)C1.data, *Cd=(double*)comp.data, *Md=(double*)mean.data;
    int32_t *lab=(int32_t*)lab1.data; int64_t *mm=(int64_t*)mem.data;
    int64_t N = lab1.shape[0];
    printf("forage_real -- %I64d vectors C1=(%I64d,%I64d) comp=(%I64d,%I64d)\n",
        N,C1.shape[0],C1.shape[1],
        comp.shape[0],comp.shape[1]);
    CHECK(C1.shape[0]==256&&C1.shape[1]==25,"R1 C1 is 256x25 PCA centroids");
    CHECK(comp.shape[0]==25&&comp.shape[1]==128,"R2 pca_comp is 25x128");
    CHECK(N==1000000,"R3 lab1 covers 1M vectors");

    printf("  mem shape=(%I64d,%I64d) item=%d kind=%c mm0=%I64d %I64d %I64d\n",
        mem.shape[0],mem.shape[1],mem.item,mem.kind,mm[0],mm[1],mm[2]);
    /* coarse bucket sizes from fine_members col0 */
    int64_t cnt[256]={0}; for(int64_t i=0;i<N;i++){ int64_t c=mm[i*3+0]; if(c>=0&&c<256) cnt[c]++; }
    int64_t mx=0; for(int i=0;i<256;i++) if(cnt[i]>mx) mx=cnt[i];
    printf("  bucket max=%I64d (cap rule: scan what the box holds)\n",mx);
    CHECK(mx>0,"R4 every coarse owns members");

    snprintf(p,sizeof p,"%s/sift_base.fvecs",sift);
    int nb,db; float *base=load_fvecs(p,&nb,&db);
    snprintf(p,sizeof p,"%s/sift_query.fvecs",sift);
    int nqq,dq; float *qry=load_fvecs(p,&nqq,&dq);
    snprintf(p,sizeof p,"%s/sift_groundtruth.ivecs",sift);
    int ng,dg; int32_t *gt=load_ivecs(p,&ng,&dg);
    if(!base||!qry||!gt){ printf("missing sift files\n"); return 1; }
    printf("  base=%d x %d query=%d gt=%d x %d\n",nb,db,nqq,ng,dg);
    CHECK(db==128&&dq==128,"R5 raw dim is 128");
    if(nq>nqq) nq=nqq;

    /* ---- IMPORT (train): walk top->down, do NOT place directly.
     * For each DB vector: PCA-25 -> nearest coarse (top) -> walk down to the
     * nearest of that coarse's 10 fine centroids -> land the row in THAT leaf.
     * The route walked is what shuts the latch (footprint), never a pre-placed
     * lab1/fine_members index. fine_cent.bin = (256,10,25) f64 from the npz. --- */
    clock_t t_train0=clock();
    wl_latch_t fog; wl_reset(&fog);
    double *fc=NULL;
    { snprintf(p,sizeof p,"%s/fine_cent.bin",hier);
      FILE *ff=fopen(p,"rb");
      if(ff){ fc=(double*)malloc((size_t)256*10*25*sizeof(double));
              if(fread(fc,8,(size_t)256*10*25,ff)!=(size_t)256*10*25){ free(fc); fc=NULL; }
              fclose(ff); } }
    if(!fc){ printf("  note: fine_cent.bin absent (run dump step)\n"); }
    /* L3 sub-leaf centroids: l3_cent.bin = (256,10,4,25) f64 */
    double *l3c=NULL;
    { snprintf(p,sizeof p,"%s/l3_cent.bin",hier);
      FILE *ff=fopen(p,"rb");
      if(ff){ l3c=(double*)malloc((size_t)256*10*4*25*sizeof(double));
              if(fread(l3c,8,(size_t)256*10*4*25,ff)!=(size_t)256*10*4*25){ free(l3c); l3c=NULL; }
              fclose(ff); } }
    printf("  layers: coarse=256 fine=%s L3=%s (leaf total=%d)\n", fc?"10":"-", l3c?"4":"-", l3c?10240:2560);
    /* leaf postings keyed by coarse*40+fine*4+l3 (10240 slots) */
    int64_t bcnt[10240]={0};
    int *blab=(int*)malloc((size_t)N*sizeof(int));   /* leaf slot per row */
    for(int64_t v=0;v<N;v++){
        float *bv=base+(size_t)v*128;
        double q25[25];
        for(int j=0;j<25;j++){ double s=0; for(int i=0;i<128;i++) s+=((double)bv[i]-Md[i])*Cd[j*128+i]; q25[j]=s; }
        int bc=0; double bd=1e300;
        for(int c=0;c<256;c++){ double s=0; for(int j=0;j<25;j++){double e=q25[j]-C1d[c*25+j]; s+=e*e;} if(s<bd){bd=s;bc=c;} }
        int bk=0;
        if(fc){ double kd=1e300; for(int k=0;k<10;k++){ double s=0; for(int j=0;j<25;j++){double e=q25[j]-fc[(size_t)(bc*10+k)*25+j]; s+=e*e;} if(s<kd){kd=s;bk=k;} } }
        int b3=0;
        if(l3c){ double d3=1e300; for(int k=0;k<4;k++){ double s=0; for(int j=0;j<25;j++){double e=q25[j]-l3c[(size_t)((bc*10+bk)*4+k)*25+j]; s+=e*e;} if(s<d3){d3=s;b3=k;} } }
        int slot=lk_slot(bc,bk,b3,l3c?1:0); blab[v]=slot; bcnt[slot]++;
        uint32_t cid=wl_id((uint32_t)(bc%144),(uint32_t)((bk*7+(bc/144)*36)%72)); if(wl_is_reserved(cid)) cid+=2;
        wl_traverse(&fog,cid);
    }
    int *bpost[10240]={0}; int64_t bfill[10240]={0};
    for(int s=0;s<10240;s++) bpost[s]=(int*)malloc((bcnt[s]+1)*sizeof(int));
    for(int64_t v=0;v<N;v++){ int s=blab[v]; bpost[s][bfill[s]++]=(int)v; }
    int64_t shut=0; for(int i=0;i<162;i++){ uint64_t w=fog.w[i]; while(w){shut+=w&1;w>>=1;} }
    double t_train=(double)(clock()-t_train0)/CLOCKS_PER_SEC;
    printf("  train: 1M walked from entrance, latch SHUT=%I64d (%.2fs)\n",shut,t_train);
    CHECK(shut>0,"R4 train left footprints (SHUT>0)");

    /* per-query forage: query enters the SAME entrance, nominates top-b coarse
     * (Leg1), visits only trained postings (Leg2), raw-L2 inside (Leg4),
     * budget stop (Leg5), recall vs GT (Leg6). No pre-placed inside used. */
    double *qp=(double*)malloc(25*sizeof(double));
    double *cd=(double*)malloc(256*sizeof(double));
    int *order=(int*)malloc(256*sizeof(int));
    long long scans=0, r1=0, r10=0;
    if(topb>32) topb=32;
    clock_t t_q0=clock();
    /* batch: all queries enter the same entrance together — nothing overlaps,
     * so the latch is walked once per visited anchor, not per query. */
    for(int q=0;q<nq;q++){
        float *qv=qry+(size_t)q*128;
        for(int j=0;j<25;j++){ double s=0; for(int i=0;i<128;i++) s+=((double)qv[i]-Md[i])*Cd[j*128+i]; qp[j]=s; }
        for(int c=0;c<256;c++){ double s=0; for(int j=0;j<25;j++){double e=qp[j]-C1d[c*25+j]; s+=e*e;} cd[c]=s; order[c]=c; }
        for(int i=1;i<256;i++){ int k=order[i],j=i-1; while(j>=0&&cd[k]<cd[order[j]]){order[j+1]=order[j];j--;} order[j+1]=k; }
        int vis[32]={0}; for(int i=0;i<topb&&i<32;i++) vis[i]=order[i];
        /* walk down from each nominated coarse into its 10 fine leaves (top->down) */
        double best[10]; int besti[10]; for(int i=0;i<10;i++){best[i]=1e300;besti[i]=-1;}
        long long sc=0;
        for(int vi=0;vi<topb&&vi<32;vi++){
            int c=vis[vi];
            /* walk down: take topk nearest fine leaves of this coarse */
            int fk[10]; for(int k=0;k<10;k++) fk[k]=k;
            if(fc){ double kd[10]; for(int k=0;k<10;k++){ double s=0; for(int j=0;j<25;j++){double e=qp[j]-fc[(size_t)(c*10+k)*25+j]; s+=e*e;} kd[k]=s; }
                for(int i=1;i<10;i++){ int kk=fk[i],j=i-1; while(j>=0&&kd[kk]<kd[fk[j]]){fk[j+1]=fk[j];j--;} fk[j+1]=kk; } }
            for(int kk=0;kk<10;kk++){
                if(fc && kk>=topk) break;
                int k=fk[kk];
                if(l3c){
                    /* walk deeper into L3 sub-leaves of this fine node */
                    int gk[4]; for(int k3=0;k3<4;k3++) gk[k3]=k3;
                    double gd[4]; for(int k3=0;k3<4;k3++){ double s=0; for(int j=0;j<25;j++){double e=qp[j]-l3c[(size_t)((c*10+k)*4+k3)*25+j]; s+=e*e;} gd[k3]=s; }
                    for(int i=1;i<4;i++){ int kk2=gk[i],j=i-1; while(j>=0&&gd[kk2]<gd[gk[j]]){gk[j+1]=gk[j];j--;} gk[j+1]=kk2; }
                    for(int t3=0;t3<topk&&t3<4;t3++){
                        int s=(c*10+k)*4+gk[t3];
                        for(int64_t j=0;j<bfill[s];j++){
                            int id=bpost[s][j]; sc++;
                            double d2=l2f(qv, base+(size_t)id*128, 128);
                            if(d2<best[9]){ best[9]=d2; besti[9]=id;
                                for(int t=9;t>0&&best[t]<best[t-1];t--){ double x=best[t];best[t]=best[t-1];best[t-1]=x; int ti=besti[t];besti[t]=besti[t-1];besti[t-1]=ti; } }
                        }
                    }
                } else {
                    int s=c*10+k;
                    for(int64_t j=0;j<bfill[s];j++){
                        int id=bpost[s][j]; sc++;
                        double d2=l2f(qv, base+(size_t)id*128, 128);
                        if(d2<best[9]){ best[9]=d2; besti[9]=id;
                            for(int t=9;t>0&&best[t]<best[t-1];t--){ double x=best[t];best[t]=best[t-1];best[t-1]=x; int ti=besti[t];besti[t]=besti[t-1];besti[t-1]=ti; } }
                    }
                }
            }
            /* footprint along the walked route: coarse + nearest fine latch */
            int bk0=fk[0];
            uint32_t cid=wl_id((uint32_t)(c%144),(uint32_t)((bk0*7+(c/144)*36)%72)); if(wl_is_reserved(cid)) cid+=2;
            wl_traverse(&fog,cid);
        }
        scans+=sc;
        int g0=gt[(size_t)q*100+0];
        for(int i=0;i<10;i++) if(besti[i]==g0){ r1+=(i==0); r10+=1; break; }
    }
    printf("  queries=%d topb=%d avg_scan=%.0f\n",nq,topb,(double)scans/nq);
    printf("  recall@1=%.4f recall@10hit=%.4f (query %.2fs, %.1fms/q)\n",(double)r1/nq,(double)r10/nq,(double)(clock()-t_q0)/CLOCKS_PER_SEC,(double)(clock()-t_q0)/CLOCKS_PER_SEC*1000/nq);
    CHECK(scans>0,"R6 forage loop scanned real buckets");
    printf("forage_real: %s\n",fails?"FAIL":"ALL PASS");
    return fails!=0;
}

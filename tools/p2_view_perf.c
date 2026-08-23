/* tools/p2_view_perf.c — per-view bake/readback timing on real GGUF
 * =====================================================================
 * P2 gate of the layer manifest: ordering/view choice must be MEASURED.
 * Bakes the full model file into a 60-slot region through each language
 * view, times write + readback separately, min of 3 passes.
 * Two media: RAM buffer (pure locality) and twin mmap (disk-backed,
 * our persistence path).
 *
 * BUILD: gcc -O2 -Wall -I core -o build/p2_view_perf tools/p2_view_perf.c
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
#include "../core/infra/dramtile_store.h"

static double now_ms(void){
#ifdef _WIN32
    LARGE_INTEGER f,t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart*1000.0/(double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC,&ts);
    return ts.tv_sec*1e3 + ts.tv_nsec/1e6;
#endif
}

/* ---- RID geometry + views (pattern from gguf_roundtrip.c) --------- */
typedef struct{int64_t x,y,z;}V3;
static V3 V(int64_t x,int64_t y,int64_t z){V3 v={x,y,z};return v;}
static V3 vsub(V3 a,V3 b){return V(a.x-b.x,a.y-b.y,a.z-b.z);}
static int64_t dist2(V3 a,V3 b){V3 d=vsub(a,b);return d.x*d.x+d.y*d.y+d.z*d.z;}
static V3 verts[20]; static uint32_t n_verts=0;
static uint32_t faces[16][5]; static uint32_t n_faces=0;
static uint32_t edges[40][2]; static uint32_t n_edges=0;
static void build_dodeca(void){
    const int64_t S=104,H=64,P=169;const int sg[2]={1,-1};
    for(int sx=0;sx<2;sx++)for(int sy=0;sy<2;sy++)for(int sz=0;sz<2;sz++)
        verts[n_verts++]=V(S*sg[sx],S*sg[sy],S*sg[sz]);
    for(int sy=0;sy<2;sy++)for(int sz=0;sz<2;sz++){
        verts[n_verts++]=V(0,H*sg[sy],P*sg[sz]);
        verts[n_verts++]=V(H*sg[sy],P*sg[sz],0);
        verts[n_verts++]=V(P*sg[sz],0,H*sg[sy]);
    }
}
#define FACE_POS(f,u) ({int _p=-1; \
    for(int _q=0;_q<5;_q++) if(faces[(f)][_q]==(u)){_p=_q;break;} _p;})
static int32_t rid_of[320];
static uint32_t rf_v[64],rf_f[64],nrf=0;
static uint32_t sqc[40][4]; static uint8_t radj[64][64];
static int32_t uf_p[40]; static uint8_t uf_par[40];
static int uf_find(int x,uint8_t*par){
    if(uf_p[x]<0){*par=0;return x;}
    uint8_t pp;int r=uf_find(uf_p[x],&pp);
    uf_par[x]^=pp;uf_p[x]=r;*par=uf_par[x];return r;
}
static int uf_merge(int i,int j,uint8_t d){
    uint8_t pi,pj;int ri=uf_find(i,&pi),rj=uf_find(j,&pj);
    if(ri==rj)return (pi^pj)==d;
    uf_p[ri]=rj;uf_par[ri]=(uint8_t)(pi^d^pj);return 1;
}
static int build_rid(uint8_t vw[4][60]){
    build_dodeca();
    int64_t emin=0;
    for(uint32_t i=0;i<n_verts;i++)for(uint32_t j=i+1;j<n_verts;j++){
        int64_t d2=dist2(verts[i],verts[j]);
        if(d2&&(!emin||d2<emin))emin=d2;
    }
    int64_t elo=emin,ehi=emin+emin/16;
#define ADJ(i,j) ({int64_t d2=dist2(verts[i],verts[j]);d2&&d2>=elo&&d2<=ehi;})
    for(uint32_t i=0;i<n_verts;i++)
        for(uint32_t j=i+1;j<n_verts;j++)
            if(ADJ(i,j)&&n_edges<40){edges[n_edges][0]=i;edges[n_edges][1]=j;n_edges++;}
    for(uint32_t a=0;a<n_verts;a++)
    for(uint32_t b=0;b<n_verts;b++){
        if(b==a||!ADJ(a,b))continue;
        for(uint32_t c=0;c<n_verts;c++){
            if(c==a||c==b||!ADJ(b,c))continue;
            for(uint32_t dd=0;dd<n_verts;dd++){
                if(dd==a||dd==b||dd==c||!ADJ(c,dd))continue;
                for(uint32_t e=0;e<n_verts;e++){
                    if(e==a||e==b||e==c||e==dd)continue;
                    if(!ADJ(dd,e)||!ADJ(e,a))continue;
                    uint32_t mn=a;if(b<mn)mn=b;if(c<mn)mn=c;
                    if(dd<mn)mn=dd;if(e<mn)mn=e;
                    if(a!=mn||b>e)continue;
                    uint32_t cl[5]={a,b,c,dd,e};
                    V3 u=vsub(verts[cl[1]],verts[cl[0]]);
                    V3 w=vsub(verts[cl[2]],verts[cl[0]]);
                    int planar=1;
                    for(int k=3;k<5&&planar;k++){
                        V3 r2=vsub(verts[cl[k]],verts[cl[0]]);
                        int64_t dx=u.y*w.z-u.z*w.y,dy=u.z*w.x-u.x*w.z,
                                dz=u.x*w.y-u.y*w.x;
                        if(labs((long)(r2.x*dx+r2.y*dy+r2.z*dz))>100000L)planar=0;
                    }
                    if(!planar)continue;
                    int dupf=0;
                    for(uint32_t fc=0;fc<n_faces&&!dupf;fc++){
                        int same=1;
                        for(int p=0;p<5&&same;p++){
                            int hit=0;
                            for(int q=0;q<5;q++)
                                if(faces[fc][q]==cl[p]){hit=1;break;}
                            if(!hit)same=0;
                        }
                        if(same)dupf=1;
                    }
                    if(dupf||n_faces>=16)continue;
                    memcpy(faces[n_faces],cl,sizeof(cl));n_faces++;
                }
                if(n_faces>=16)break;
            }
            if(n_faces>=16)break;
        }
        if(n_faces>=16)break;
    }
    memset(rid_of,0xFF,sizeof(rid_of));
    for(uint32_t f=0;f<n_faces;f++)
        for(int p=0;p<5;p++){
            uint32_t u=faces[f][p];
            if(rid_of[u*16+f]<0){rid_of[u*16+f]=(int32_t)nrf;
                rf_v[nrf]=u;rf_f[nrf]=f;nrf++;}
        }
    memset(radj,0,sizeof(radj));
    for(uint32_t i=0;i<nrf;i++){
        uint32_t u=rf_v[i],f=rf_f[i];int pos=-1;
        for(int p=0;p<5;p++)if(faces[f][p]==u){pos=p;break;}
        uint32_t pv=faces[f][(pos+4)%5],nx=faces[f][(pos+1)%5];
        radj[i][rid_of[pv*16+f]]=radj[rid_of[pv*16+f]][i]=1;
        radj[i][rid_of[nx*16+f]]=radj[rid_of[nx*16+f]][i]=1;
        for(uint32_t g=0;g<n_faces;g++){
            if(g==f)continue;
            for(int p2=0;p2<5;p2++)if(faces[g][p2]==u){
                radj[i][rid_of[u*16+g]]=radj[rid_of[u*16+g]][i]=1;break;}
        }
    }
    uint32_t deg_ok=1,e2=0;
    for(uint32_t i=0;i<nrf;i++){uint32_t d=0;
        for(uint32_t j=0;j<nrf;j++)d+=radj[i][j];
        if(d!=4)deg_ok=0;}
    for(uint32_t i=0;i<nrf;i++)for(uint32_t j=i+1;j<nrf;j++)e2+=radj[i][j];
    if(nrf!=60||e2!=120||!deg_ok)return -1;
    static int32_t vsq[64][2];memset(vsq,0xFF,sizeof(vsq));
    for(uint32_t k=0;k<n_edges;k++){
        uint32_t a=edges[k][0],b=edges[k][1],ff[2],nf=0;
        for(uint32_t g=0;g<n_faces&&nf<2;g++)
            if(FACE_POS(g,a)>=0&&FACE_POS(g,b)>=0)ff[nf++]=g;
        sqc[k][0]=(uint32_t)rid_of[a*16+ff[0]];
        sqc[k][1]=(uint32_t)rid_of[b*16+ff[0]];
        sqc[k][2]=(uint32_t)rid_of[b*16+ff[1]];
        sqc[k][3]=(uint32_t)rid_of[a*16+ff[1]];
        for(int c=0;c<4;c++){
            uint32_t w=sqc[k][c];
            if(vsq[w][0]<0)vsq[w][0]=(int32_t)k;
            else if(vsq[w][1]<0&&vsq[w][0]!=(int32_t)k)vsq[w][1]=(int32_t)k;
        }
    }
    memset(uf_p,0xFF,sizeof(uf_p));memset(uf_par,0,sizeof(uf_par));
    for(uint32_t w=0;w<nrf;w++){
        int32_t i=vsq[w][0],j=vsq[w][1];
        uint8_t c0=(w==sqc[i][0]||w==sqc[i][2])?0:1;
        uint8_t c1=(w==sqc[j][0]||w==sqc[j][2])?0:1;
        if(!uf_merge((int)i,(int)j,(uint8_t)(c0^c1^1)))return -1;
    }
    {uint8_t bits[40];memset(bits,0,sizeof(bits));
     for(uint32_t k=0;k<n_edges;k++){uint8_t p;uf_find((int)k,&p);bits[k]=p;}
     {uint32_t n=0;
      for(uint32_t f=0;f<n_faces;f++)
          for(int p=0;p<5;p++)vw[0][n++]=(uint8_t)rid_of[faces[f][p]*16+f];}
     {uint32_t n=0;
      for(uint32_t u=0;u<n_verts;u++)
          for(uint32_t g=0;g<n_faces;g++)
              if(FACE_POS(g,u)>=0)vw[1][n++]=(uint8_t)rid_of[u*16+g];}
     {uint32_t n=0;
      for(uint32_t k=0;k<n_edges;k++){
          uint32_t a,b;
          if(bits[k]==0){a=sqc[k][0];b=sqc[k][2];}
          else{a=sqc[k][1];b=sqc[k][3];}
          vw[2][n++]=(uint8_t)a;vw[2][n++]=(uint8_t)b;}
      n=0;
      for(uint32_t k=0;k<n_edges;k++){
          uint32_t a,b;
          if(bits[k]==0){a=sqc[k][1];b=sqc[k][3];}
          else{a=sqc[k][0];b=sqc[k][2];}
          vw[3][n++]=(uint8_t)a;vw[3][n++]=(uint8_t)b;}
     }
    }
    for(int v=0;v<4;v++){
        uint8_t hit[64];memset(hit,0,sizeof(hit));
        for(uint32_t p=0;p<60;p++){if(hit[vw[v][p]])return -1;hit[vw[v][p]]=1;}
    }
    return 0;
}
static inline uint32_t fwd_view(const uint8_t vw[60],uint32_t f){
    uint32_t w=f%60,l=f/60,pos=60;
    for(uint32_t p=0;p<60;p++)if(vw[p]==w){pos=p;break;}
    return l*60+pos;
}

#define PART_BYTES (128u*1024u)
#define NV 6

int main(int argc,char**argv){
    setvbuf(stdout,NULL,_IONBF,0);
    const char*path=argc>1?argv[1]:"I:\\model\\Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char*twinp=argc>2?argv[2]:"build\\p2_perf.twin";
    printf("=== p2_view_perf - per-view timing on real model ===\n");
    static uint8_t vw[4][60];
    if(build_rid(vw)!=0){printf("FAIL rid\n");return 1;}
    /* extra views: hosoya stride, zeck reversed-code */
    uint8_t vws[NV][60];
    for(int i=0;i<4;i++)memcpy(vws[i],vw[i],60);
    for(uint32_t p=0;p<60;p++)vws[4][p]=(uint8_t)((p*13u)%60u);
    {static const uint64_t ZF[12]={0,1,1,2,3,5,8,13,21,34,55,89};
     uint32_t key[60],ord[60];
     for(int n=0;n<60;n++){
         uint64_t rem=(uint64_t)(n+1),mask=0;
         for(int i=9;i>=0;i--)
             if(ZF[i+2]<=rem){mask|=1ull<<i;rem-=ZF[i+2];}
         uint64_t rev=0;
         for(int b=0;b<10;b++)if(mask&(1ull<<b))rev|=1ull<<(9-b);
         key[n]=(uint32_t)rev;ord[n]=(uint32_t)n;}
     for(int i=1;i<60;i++){uint32_t v=ord[i];int j=i-1;
         while(j>=0&&key[ord[j]]>key[v]){ord[j+1]=ord[j];j--;}
         ord[j+1]=v;}
     for(int i=0;i<60;i++)vws[5][i]=(uint8_t)ord[i];}
    const char*lname[NV]={"pent","tri","snubL","snubR","hosoya","zeck"};

    FILE*fp=fopen(path,"rb");
    if(!fp){printf("FAIL open %s\n",path);return 1;}
    fseek(fp,0,SEEK_END);long fsz=ftell(fp);fseek(fp,0,SEEK_SET);
    uint8_t*src=malloc((size_t)fsz);
    if(fread(src,1,(size_t)fsz,fp)!=(size_t)fsz){printf("FAIL read\n");return 1;}
    fclose(fp);
    uint32_t total=(uint32_t)(((size_t)fsz+PART_BYTES-1)/PART_BYTES);
    uint32_t nslots=((total+59)/60)*60;
    printf("file %.1f MB · %u parts · %u slots · min of 3 passes\n\n",
           fsz/1e6,total,nslots);

    int media_ram=argc>3?atoi(argv[3]):0;   /* 1 = pure RAM mode */
    printf("[mode: %s]\n",media_ram?"RAM buffer":"disk twin mmap");
    printf("%-7s %10s %10s %12s\n","view","bake ms","read ms","MB/s bake");
    double best_bake[NV],best_read[NV];
    for(int v=0;v<NV;v++){best_bake[v]=1e18;best_read[v]=1e18;}

    for(int run=0;run<3;run++){
        void*regmem=NULL;DtSlotRegion reg;uint8_t*ram=NULL;
        if(media_ram){
            ram=malloc((size_t)nslots*PART_BYTES);
            memset(ram,0,(size_t)nslots*PART_BYTES);
        }else{
            remove(twinp);
            if(dt_slot_init_twin(&reg,twinp,nslots,PART_BYTES)!=0){
                printf("FAIL twin\n");return 1;}
        }
        for(int v=0;v<NV;v++){
            double t0=now_ms();
            for(uint32_t f=0;f<total;f++){
                uint32_t addr=fwd_view(vws[v],f);
                uint32_t off=f*PART_BYTES,len=PART_BYTES;
                if((size_t)off+len>(size_t)fsz)len=(uint32_t)((size_t)fsz-off);
                if(media_ram)memcpy(ram+(size_t)addr*PART_BYTES,src+off,len);
                else dt_slot_put(&reg,addr,src+off,len);
            }
            double tb=now_ms()-t0;
            t0=now_ms();
            volatile uint64_t sink=0;
            for(uint32_t f=0;f<total;f++){
                uint32_t addr=fwd_view(vws[v],f);
                const uint8_t*p=media_ram
                    ?ram+(size_t)addr*PART_BYTES
                    :dt_slot_ptr(&reg,addr);
                if(p)sink^=*(const uint64_t*)p;
            }
            double tr=now_ms()-t0;
            (void)sink;
            if(tb<best_bake[v])best_bake[v]=tb;
            if(tr<best_read[v])best_read[v]=tr;
        }
        if(media_ram)free(ram);else dt_slot_destroy(&reg);
    }

    /* sequential baseline (identity order) */
    {double tb_best=1e18,tr_best=1e18;
     for(int run=0;run<3;run++){
         void*regmem=NULL;DtSlotRegion reg;uint8_t*ram=NULL;
         if(media_ram){
             ram=malloc((size_t)nslots*PART_BYTES);
             memset(ram,0,(size_t)nslots*PART_BYTES);
         }else{
             remove(twinp);
             dt_slot_init_twin(&reg,twinp,nslots,PART_BYTES);
         }
         double t0=now_ms();
         for(uint32_t f=0;f<total;f++){
             uint32_t len=PART_BYTES;
             if((size_t)f*len+len>(size_t)fsz)
                 len=(uint32_t)((size_t)fsz-(size_t)f*PART_BYTES);
             if(media_ram)memcpy(ram+(size_t)f*PART_BYTES,src+f*PART_BYTES,len);
             else dt_slot_put(&reg,f,src+f*PART_BYTES,len);
         }
         double tb=now_ms()-t0;t0=now_ms();
         volatile uint64_t sink=0;
         for(uint32_t f=0;f<total;f++){
             const uint8_t*p=media_ram
                 ?ram+(size_t)f*PART_BYTES:dt_slot_ptr(&reg,f);
             if(p)sink^=*(const uint64_t*)p;
         }
         double tr=now_ms()-t0;(void)sink;
         if(tb<tb_best)tb_best=tb;if(tr<tr_best)tr_best=tr;
         if(media_ram)free(ram);else dt_slot_destroy(&reg);
     }
     printf("%-7s %10.1f %10.1f %12.0f\n","seq-base",
            tb_best,tr_best,(double)fsz/1e6/(tb_best/1000.0));}

    for(int v=0;v<NV;v++)
        printf("%-7s %10.1f %10.1f %12.0f\n",lname[v],
               best_bake[v],best_read[v],
               (double)fsz/1e6/(best_bake[v]/1000.0));

    printf("\nnote: single-box numbers, disk-backed mode includes NVMe;\n"
           "      extend with llama-throughput-per-view next.\n");
    free(src);
    return 0;
}

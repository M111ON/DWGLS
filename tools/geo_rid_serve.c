/*
 * tools/geo_rid_serve.c — bake REAL GGUF into RID slots, serve through
 * ════════════════════════════════════════════════════════════════════════
 * three languages of the same graph. Data-plane companion of
 * geo_archimedean_test.c (RID base) + geo_snub_test.c (chiral diagonals).
 *
 *   RID field : V=60 R(u,F) slots · E=120+30 snub · F=62→92 split
 *   part id f → layer l = f / 60 · slot w = f % 60   (closed form)
 *              byte offset = (l*60 + w) * 128KB
 *              slot w IS the R(u,F) label built purely from int dodeca.
 *
 *   3 language views, each a STRUCTURAL bijection over the 60 slots:
 *     pent-view : 12 faces × 5-cycle corners      (dodeca language)
 *     tri-view  : 20 vertex stars × 3 corners     (icosa language)
 *     snub-view : 30 squares × diagonal endpoints (chiral matching:
 *                 every RID vertex is endpoint of EXACTLY one diagonal,
 *                 proven unique-pair by geo_snub_test parity solve)
 *   XOR checksum over all parts must be IDENTICAL in every language
 *   and match the zero-padded source stream (order-invariance oracle).
 *
 *   damage drill : flip one window byte -> detect · localize slot via
 *   per-part XOR vs source · restore by re-bake -> lossless again.
 *
 * BUILD: gcc -O2 -Wall -D__USE_MINGW_ANSI_STDIO=1 -Icore \
 *        -o build/geo_rid_serve tools/geo_rid_serve.c -lm
 * RUN:   ./build/geo_rid_serve [model.gguf]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "../core/gguf_reader.h"

/* ── int dodecahedron (phi = 13/8 x104, same as geo_archimedean_test) ── */
typedef struct { int64_t x, y, z; } V3;
static V3 V(int64_t x,int64_t y,int64_t z){ V3 v={x,y,z}; return v; }
static V3 vsub(V3 a,V3 b){ return V(a.x-b.x,a.y-b.y,a.z-b.z); }
static int64_t vdot(V3 a,V3 b){ return a.x*b.x+a.y*b.y+a.z*b.z; }
static int64_t dist2(V3 a,V3 b){ V3 d=vsub(a,b); return vdot(d,d); }

static V3 verts[20]; static uint32_t n_verts=0;
static uint32_t faces[16][5]; static uint32_t n_faces=0;
static uint32_t edges[40][2]; static uint32_t n_edges=0;

static void build_dodeca(void){
    const int64_t S=104,H=64,P=169; const int sg[2]={1,-1};
    for(int sx=0;sx<2;sx++)for(int sy=0;sy<2;sy++)for(int sz=0;sz<2;sz++)
        verts[n_verts++]=V(S*sg[sx],S*sg[sy],S*sg[sz]);
    for(int sy=0;sy<2;sy++)for(int sz=0;sz<2;sz++){
        verts[n_verts++]=V(0,H*sg[sy],P*sg[sz]);
        verts[n_verts++]=V(H*sg[sy],P*sg[sz],0);
        verts[n_verts++]=V(P*sg[sz],0,H*sg[sy]);
    }
}

/* ── RID state ──────────────────────────────────────────────────────── */
static int32_t rid_of[320];
static uint32_t rf_v[64], rf_f[64], nrf=0;
static uint8_t radj[64][64];       /* RID base adjacency (deg 4) */
#define FACE_POS(f,u) ({int _p=-1; \
    for(int _q=0;_q<5;_q++) if(faces[(f)][_q]==(u)){_p=_q;break;} _p;})

/* square corners of edge k: q0=R(a,P) q1=R(b,P) q2=R(b,Q) q3=R(a,Q) */
static uint32_t sqc[40][4];

/* ── union-find with parity (snub diagonal solve) ───────────────────── */
static int32_t uf_p[40]; static uint8_t uf_par[40];
static int uf_find(int x, uint8_t *par){
    if(uf_p[x]<0){ *par=0; return x; }
    uint8_t pp; int r=uf_find(uf_p[x],&pp);
    uf_par[x]^=pp; uf_p[x]=r; *par=uf_par[x]; return r;
}
static int uf_merge(int i,int j,uint8_t d){
    uint8_t pi,pj; int ri=uf_find(i,&pi), rj=uf_find(j,&pj);
    if(ri==rj) return (pi^pj)==d;
    uf_p[ri]=rj; uf_par[ri]=(uint8_t)(pi^d^pj);
    return 1;
}

#define PART_BYTES   (128u * 1024u)
#define RID_SLOTS    60u

/* XOR fold over 64-byte chunks; zero tail contributes nothing */
static uint64_t xor64(const uint8_t *p, uint32_t n) {
    uint64_t x = 0;
    for (uint32_t b = 0; b + 64 <= n; b += 64)
        x ^= *(const uint64_t *)(p + b)     ^ *(const uint64_t *)(p + b + 8)
           ^ *(const uint64_t *)(p + b + 16)^ *(const uint64_t *)(p + b + 24)
           ^ *(const uint64_t *)(p + b + 32)^ *(const uint64_t *)(p + b + 40)
           ^ *(const uint64_t *)(p + b + 48)^ *(const uint64_t *)(p + b + 56);
    return x;
}

int main(int argc, char **argv) {
    /* ── geometry gate ──────────────────────────────────────────────── */
    build_dodeca();
    int64_t emin=0;
    for(uint32_t i=0;i<n_verts;i++)for(uint32_t j=i+1;j<n_verts;j++){
        int64_t d2=dist2(verts[i],verts[j]);
        if(d2&&(!emin||d2<emin)) emin=d2;
    }
    int64_t elo=emin, ehi=emin+emin/16;
    #define ADJ(i,j) ({int64_t d2=dist2(verts[i],verts[j]); d2&&d2>=elo&&d2<=ehi;})
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
                    uint32_t mn5=a;
                    if(b<mn5)mn5=b;
                    if(c<mn5)mn5=c;
                    if(dd<mn5)mn5=dd;
                    if(e<mn5)mn5=e;
                    if(a!=mn5||b>e)continue;
                    uint32_t cl[5]={a,b,c,dd,e};
                    V3 u=vsub(verts[cl[1]],verts[cl[0]]);
                    V3 w=vsub(verts[cl[2]],verts[cl[0]]);
                    int planar=1;
                    for(int k=3;k<5&&planar;k++){
                        V3 r2=vsub(verts[cl[k]],verts[cl[0]]);
                        int64_t dx=u.y*w.z-u.z*w.y, dy=u.z*w.x-u.x*w.z,
                                dz=u.x*w.y-u.y*w.x;
                        if(labs((long)(r2.x*dx+r2.y*dy+r2.z*dz))>100000L) planar=0;
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
                    memcpy(faces[n_faces],cl,sizeof(cl));
                    n_faces++;
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
            if(rid_of[u*16+f]<0){
                rid_of[u*16+f]=(int32_t)nrf;
                rf_v[nrf]=u; rf_f[nrf]=f; nrf++;
            }
        }
    memset(radj,0,sizeof(radj));
    for(uint32_t i=0;i<nrf;i++){
        uint32_t u=rf_v[i], f=rf_f[i];
        int pos=-1;
        for(int p=0;p<5;p++) if(faces[f][p]==u){pos=p;break;}
        uint32_t pv=faces[f][(pos+4)%5], nx=faces[f][(pos+1)%5];
        int32_t jp=rid_of[pv*16+f], jn=rid_of[nx*16+f];
        radj[i][jp]=radj[jp][i]=1;
        radj[i][jn]=radj[jn][i]=1;
        for(uint32_t g=0;g<n_faces;g++){
            if(g==f)continue;
            for(int p2=0;p2<5;p2++) if(faces[g][p2]==u){
                int32_t j=rid_of[u*16+g];
                radj[i][j]=radj[j][i]=1; break;
            }
        }
    }
    uint32_t deg_ok=1, r_edges=0;
    for(uint32_t i=0;i<nrf;i++){
        uint32_t d=0;
        for(uint32_t j=0;j<nrf;j++) d+=radj[i][j];
        if(d!=4) deg_ok=0;
    }
    for(uint32_t i=0;i<nrf;i++)
        for(uint32_t j=i+1;j<nrf;j++) r_edges+=radj[i][j];
    printf("RID base: v=%u e=%u deg4=%s\n", nrf,r_edges,deg_ok?"ok":"FAIL");
    if(nrf!=60||r_edges!=120||!deg_ok){ printf("FAIL geometry\n"); return 1; }

    /* squares + membership */
    static uint8_t onsq[64][40]; static int32_t vsq[64][2];
    memset(onsq,0,sizeof(onsq)); memset(vsq,0xFF,sizeof(vsq));
    for(uint32_t k=0;k<n_edges;k++){
        uint32_t a=edges[k][0], b=edges[k][1], ff[2], nf=0;
        for(uint32_t g=0;g<n_faces&&nf<2;g++)
            if(FACE_POS(g,a)>=0&&FACE_POS(g,b)>=0) ff[nf++]=g;
        uint32_t P=ff[0], Q=ff[1];
        sqc[k][0]=(uint32_t)rid_of[a*16+P]; sqc[k][1]=(uint32_t)rid_of[b*16+P];
        sqc[k][2]=(uint32_t)rid_of[b*16+Q]; sqc[k][3]=(uint32_t)rid_of[a*16+Q];
        for(int c=0;c<4;c++){
            uint32_t w=sqc[k][c];
            onsq[w][k]=1;
            if(vsq[w][0]<0) vsq[w][0]=(int32_t)k;
            else if(vsq[w][1]<0 && vsq[w][0]!=(int32_t)k) vsq[w][1]=(int32_t)k;
        }
    }

    /* snub diagonal solve (parity system, proven unique pair) */
    memset(uf_p,0xFF,sizeof(uf_p)); memset(uf_par,0,sizeof(uf_par));
    for(uint32_t w=0;w<nrf;w++){
        int32_t i=vsq[w][0], j=vsq[w][1];
        uint8_t cw0=(w==sqc[i][0]||w==sqc[i][2])?0:1;
        uint8_t cw1=(w==sqc[j][0]||w==sqc[j][2])?0:1;
        if(!uf_merge((int)i,(int)j,(uint8_t)(cw0^cw1^1))){
            printf("FAIL parity\n"); return 1;
        }
    }
    uint8_t bits[40]; uint8_t par_of[40]; uint32_t ncomp=0;
    {
        int32_t seen[40]; memset(seen,0xFF,sizeof(seen));
        for(uint32_t k=0;k<n_edges;k++){
            uint8_t p; int r=uf_find((int)k,&p);
            if(seen[r]<0) seen[r]=(int32_t)ncomp++;
            par_of[k]=p;
        }
        /* canonical solution: group root bit = 0 */
        memset(bits,0,sizeof(bits));
        for(uint32_t k=0;k<n_edges;k++) bits[k]=par_of[k];
    }
    printf("snub diagonals solved (%u groups)\n", ncomp);

    /* ── 3 language views (each must be a bijection over 60) ────────── */
    uint8_t vw[3][60]; uint8_t hit[64];
    uint32_t bijective=1;

    /* pent-view: face cycles */
    { uint32_t n=0;
      for(uint32_t f=0;f<n_faces;f++)
        for(int p=0;p<5;p++) vw[0][n++]=(uint8_t)rid_of[faces[f][p]*16+f];
    }
    /* tri-view: vertex stars */
    { uint32_t n=0;
      for(uint32_t u=0;u<n_verts;u++)
        for(uint32_t g=0;g<n_faces;g++)
            if(FACE_POS(g,u)>=0) vw[1][n++]=(uint8_t)rid_of[u*16+g];
    }
    /* snub-view: diagonal endpoints (perfect matching) */
    { uint32_t n=0;
      for(uint32_t k=0;k<n_edges;k++){
          uint32_t a,b;
          if(bits[k]==0){ a=sqc[k][0]; b=sqc[k][2]; }
          else          { a=sqc[k][1]; b=sqc[k][3]; }
          vw[2][n++]=(uint8_t)a; vw[2][n++]=(uint8_t)b;
      }
    }
    for(int v=0;v<3;v++){
        memset(hit,0,sizeof(hit));
        for(uint32_t p=0;p<60;p++){
            if(hit[vw[v][p]]) bijective=0;
            hit[vw[v][p]]=1;
        }
    }
    printf("views bijective over 60 slots: %s\n", bijective?"ok":"FAIL");
    if(!bijective){ printf("FAIL views\n"); return 1; }

    /* ── open GGUF ──────────────────────────────────────────────────── */
    const char *path = argc > 1 ? argv[1]
        : "I:\\model\\Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    GgufReader r;
    if (gguf_open((char *)path, &r) != 0) { printf("FAIL open %s\n", path); return 1; }
    printf("\n=== geo_rid_serve — %s ===\n", path);
    printf("tensors %u · %.1f MB · window 60 slots x layers x 128KB\n\n",
           r.n_tensors, (double)r.base_sz / 1e6);

    /* count parts */
    uint64_t total_bytes = 0; uint32_t total_parts = 0;
    for (uint32_t i = 0; i < r.n_tensors; i++) {
        total_bytes += r.sizes[i];
        total_parts += (r.sizes[i] + PART_BYTES - 1) / PART_BYTES;
    }
    uint32_t layers = (total_parts + RID_SLOTS - 1) / RID_SLOTS;
    printf("parts: %u (%.1f MB) · layers: %u\n",
           total_parts, (double)total_bytes / 1e6, layers);

    /* sparse window */
    uint8_t *win = (uint8_t *)VirtualAlloc(NULL,
        (size_t)layers * RID_SLOTS * PART_BYTES, MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE);
    if (!win) { printf("FAIL VirtualAlloc\n"); return 1; }

    /* job table */
    typedef struct { const uint8_t *src; uint8_t *dst; uint32_t len; } Job;
    Job *jobs = (Job *)malloc(sizeof(Job) * total_parts);
    if (!jobs) { printf("FAIL malloc jobs\n"); return 1; }
    {
        uint32_t f = 0;
        for (uint32_t i = 0; i < r.n_tensors; i++) {
            const uint8_t *src = r.base + r.data_offset + r.offsets[i];
            uint32_t np = (r.sizes[i] + PART_BYTES - 1) / PART_BYTES;
            for (uint32_t p = 0; p < np && f < total_parts; p++, f++) {
                uint32_t off = p * PART_BYTES;
                uint32_t len = r.sizes[i] - off; if (len > PART_BYTES) len = PART_BYTES;
                uint32_t w = f % RID_SLOTS, l = f / RID_SLOTS;
                jobs[f].src = src + off;
                jobs[f].dst = win + ((size_t)l * RID_SLOTS + w) * PART_BYTES;
                jobs[f].len = len;
            }
        }
    }

    /* ── BAKE ───────────────────────────────────────────────────────── */
    for (uint32_t f = 0; f < total_parts; f++)
        memcpy(jobs[f].dst, jobs[f].src, jobs[f].len);

    /* ── VERIFY byte-identical vs source ────────────────────────────── */
    uint32_t bad = 0;
    for (uint32_t f = 0; f < total_parts; f++)
        if (memcmp(jobs[f].dst, jobs[f].src, jobs[f].len) != 0) bad++;
    printf("VERIFY %u parts byte-identical · %u bad (%s)\n\n",
           total_parts, bad, bad ? "LOSSLESS BROKEN" : "lossless");
    if (bad) return 1;

    /* ── per-part XOR expected table (for damage localization) ──────── */
    uint64_t *pxor = (uint64_t *)malloc(sizeof(uint64_t) * total_parts);
    for (uint32_t f = 0; f < total_parts; f++)
        pxor[f] = xor64(jobs[f].src, jobs[f].len);

    /* ── 3-VIEW SWEEP — XOR per language == source XOR ──────────────── */
    uint64_t src_xor = 0;
    for (uint32_t f = 0; f < total_parts; f++) src_xor ^= pxor[f];

    printf("%10s %12s %10s\n", "language", "xor", "match");
    int views_ok = 1;
    for (int v = 0; v < 3; v++) {
        const char *name = v==0 ? "pent" : v==1 ? "tri" : "snub";
        uint64_t xo = 0;
        for (uint32_t l = 0; l < layers; l++)
            for (uint32_t p = 0; p < RID_SLOTS; p++) {
                uint32_t f = l * RID_SLOTS + vw[v][p];
                if (f >= total_parts) continue;
                xo ^= xor64(win + ((size_t)l * RID_SLOTS + vw[v][p]) * PART_BYTES,
                            PART_BYTES);
            }
        int ok = (xo == src_xor);
        if (!ok) views_ok = 0;
        printf("%10s %016llx %10s\n", name,
               (unsigned long long)xo, ok ? "yes" : "NO");
    }
    printf("\nthree-language lossless: %s\n",
           views_ok ? "ALL LANGUAGES MATCH SOURCE" : "MISMATCH");

    /* ── damage drill: flip one byte → detect · localize · restore ──── */
    if (total_parts > 7) {
        uint32_t fp = total_parts / 2;
        jobs[fp].dst[17] ^= 0x80;
        /* detect + localize: per-part XOR vs expected */
        uint32_t found = total_parts;
        for (uint32_t f = 0; f < total_parts; f++)
            if (xor64(jobs[f].dst, jobs[f].len) != pxor[f]) { found = f; break; }
        uint32_t w = found % RID_SLOTS, l = found / RID_SLOTS;
        printf("\nDAMAGE  flipped 1 byte @ part %u -> slot R(v%u,f%u) layer %u\n",
               found, rf_v[w], rf_f[w], l);
        printf("LOCATE  detected at part %u (%s)\n",
               found, found<total_parts ? "localized" : "NOT FOUND");
        /* restore: re-bake that one part */
        memcpy(jobs[found].dst, jobs[found].src, jobs[found].len);
        uint32_t bad2 = 0;
        for (uint32_t f = 0; f < total_parts; f++)
            if (memcmp(jobs[f].dst, jobs[f].src, jobs[f].len) != 0) bad2++;
        printf("RESTORE re-baked 1 part · %u bad (%s)\n",
               bad2, bad2 ? "STILL BROKEN" : "lossless again");
        views_ok = views_ok && (found < total_parts) && (bad2 == 0);
    }

    printf("\n%s\n", views_ok ? "RESULT: PASSED" : "RESULT: FAILED");
    free(pxor); free(jobs);
    VirtualFree(win, 0, MEM_RELEASE);
    gguf_close(&r);
    return views_ok ? 0 : 1;
}

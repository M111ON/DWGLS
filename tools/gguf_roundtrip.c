/*
 * tools/gguf_roundtrip.c — full GGUF file roundtrip through RID slot region
 * ════════════════════════════════════════════════════════════════════════
 * Pure file roundtrip (no inference): reads entire GGUF into memory,
 * bakes 128KB parts into DtSlotRegion twin via RID language views,
 * reads back, verifies byte-identical. Proves the full GGUF — every
 * tensor, every weight, every header byte — survives the slot path.
 *
 * Gates:
 *   R1  BAKE + READBACK  per-language: every part byte-identical vs source
 *   R2  FULL REBUILD     header+payload from region → memcmp vs original
 *   R3  DAMAGE           flip 1 byte → detect → localize → restore → R2
 *   R4  PERSISTENCE      twin file survives dt_slot_destroy (size check)
 *
 * BUILD: make gguf-roundtrip
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
#include "../core/infra/dramtile_store.h"

/* ── int dodecahedron + RID labeling + snub diagonals ──────────────── */
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
#define FACE_POS(f,u) ({int _p=-1; \
    for(int _q=0;_q<5;_q++) if(faces[(f)][_q]==(u)){_p=_q;break;} _p;})
static int32_t rid_of[320];
static uint32_t rf_v[64], rf_f[64], nrf=0;
static uint32_t sqc[40][4];
static uint8_t radj[64][64];
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
static int build_rid(uint8_t vw[4][60]) {
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
            if(rid_of[u*16+f]<0){ rid_of[u*16+f]=(int32_t)nrf;
                rf_v[nrf]=u; rf_f[nrf]=f; nrf++; }
        }
    memset(radj,0,sizeof(radj));
    for(uint32_t i=0;i<nrf;i++){
        uint32_t u=rf_v[i], f=rf_f[i];
        int pos=-1;
        for(int p=0;p<5;p++) if(faces[f][p]==u){pos=p;break;}
        uint32_t pv=faces[f][(pos+4)%5], nx=faces[f][(pos+1)%5];
        radj[i][rid_of[pv*16+f]]=radj[rid_of[pv*16+f]][i]=1;
        radj[i][rid_of[nx*16+f]]=radj[rid_of[nx*16+f]][i]=1;
        for(uint32_t g=0;g<n_faces;g++){
            if(g==f)continue;
            for(int p2=0;p2<5;p2++) if(faces[g][p2]==u){
                radj[i][rid_of[u*16+g]]=radj[rid_of[u*16+g]][i]=1; break;
            }
        }
    }
    uint32_t deg_ok=1, e2=0;
    for(uint32_t i=0;i<nrf;i++){ uint32_t d=0;
        for(uint32_t j=0;j<nrf;j++) d+=radj[i][j];
        if(d!=4) deg_ok=0; }
    for(uint32_t i=0;i<nrf;i++)for(uint32_t j=i+1;j<nrf;j++) e2+=radj[i][j];
    if(nrf!=60||e2!=120||!deg_ok) return -1;
    /* squares */
    static uint8_t onsq[64][40]; static int32_t vsq[64][2];
    memset(onsq,0,sizeof(onsq)); memset(vsq,0xFF,sizeof(vsq));
    for(uint32_t k=0;k<n_edges;k++){
        uint32_t a=edges[k][0], b=edges[k][1], ff[2], nf=0;
        for(uint32_t g=0;g<n_faces&&nf<2;g++)
            if(FACE_POS(g,a)>=0&&FACE_POS(g,b)>=0) ff[nf++]=g;
        sqc[k][0]=(uint32_t)rid_of[a*16+ff[0]]; sqc[k][1]=(uint32_t)rid_of[b*16+ff[0]];
        sqc[k][2]=(uint32_t)rid_of[b*16+ff[1]]; sqc[k][3]=(uint32_t)rid_of[a*16+ff[1]];
        for(int c=0;c<4;c++){
            uint32_t w=sqc[k][c]; onsq[w][k]=1;
            if(vsq[w][0]<0) vsq[w][0]=(int32_t)k;
            else if(vsq[w][1]<0 && vsq[w][0]!=(int32_t)k) vsq[w][1]=(int32_t)k;
        }
    }
    /* snub parity solve */
    memset(uf_p,0xFF,sizeof(uf_p)); memset(uf_par,0,sizeof(uf_par));
    for(uint32_t w=0;w<nrf;w++){
        int32_t i=vsq[w][0], j=vsq[w][1];
        uint8_t cw0=(w==sqc[i][0]||w==sqc[i][2])?0:1;
        uint8_t cw1=(w==sqc[j][0]||w==sqc[j][2])?0:1;
        if(!uf_merge((int)i,(int)j,(uint8_t)(cw0^cw1^1))) return -1;
    }
    /* views */
    { uint8_t bits[40]; memset(bits,0,sizeof(bits));
      for(uint32_t k=0;k<n_edges;k++){
          uint8_t p; uf_find((int)k,&p);
          bits[k]=p;
      }
      { uint32_t n=0;
        for(uint32_t f=0;f<n_faces;f++)
          for(int p=0;p<5;p++) vw[0][n++]=(uint8_t)rid_of[faces[f][p]*16+f];
      }
      { uint32_t n=0;
        for(uint32_t u=0;u<n_verts;u++)
          for(uint32_t g=0;g<n_faces;g++)
              if(FACE_POS(g,u)>=0) vw[1][n++]=(uint8_t)rid_of[u*16+g];
      }
      { uint32_t n=0;
        for(uint32_t k=0;k<n_edges;k++){
            uint32_t a,b;
            if(bits[k]==0){ a=sqc[k][0]; b=sqc[k][2]; }
            else          { a=sqc[k][1]; b=sqc[k][3]; }
            vw[2][n++]=(uint8_t)a; vw[2][n++]=(uint8_t)b;
        }
      }
      /* mirror enantiomorph: complement ALL diagonal bits — the parity
       * system has exactly 2 solutions (proven geo_snub_test.c T6);
       * flipping every bit yields the other one */
      { uint32_t n=0;
        for(uint32_t k=0;k<n_edges;k++){
            uint32_t a,b;
            if(bits[k]==0){ a=sqc[k][1]; b=sqc[k][3]; }
            else          { a=sqc[k][0]; b=sqc[k][2]; }
            vw[3][n++]=(uint8_t)a; vw[3][n++]=(uint8_t)b;
        }
      }
    }
    for(int v=0;v<4;v++){
        uint8_t hit[64]; memset(hit,0,sizeof(hit));
        for(uint32_t p=0;p<60;p++){
            if(hit[vw[v][p]]) return -1;
            hit[vw[v][p]]=1;
        }
    }
    return 0;
}

static inline uint32_t fwd_view(const uint8_t vw[60], uint32_t f) {
    uint32_t w = f % 60, l = f / 60, pos = 60;
    for(uint32_t p=0;p<60;p++) if(vw[p]==w){ pos=p; break; }
    return l * 60 + pos;
}
static inline uint32_t inv_view(const uint8_t vw[60], uint32_t addr,
                                uint32_t *w_out) {
    uint32_t l = addr / 60, pos = addr % 60, w = vw[pos];
    *w_out = w;
    return l * 60 + w;
}
static uint64_t xor64(const uint8_t *p, uint32_t n) {
    uint64_t x = 0;
    for (uint32_t b = 0; b + 64 <= n; b += 64)
        x ^= *(const uint64_t *)(p + b)     ^ *(const uint64_t *)(p + b + 8)
           ^ *(const uint64_t *)(p + b + 16)^ *(const uint64_t *)(p + b + 24)
           ^ *(const uint64_t *)(p + b + 32)^ *(const uint64_t *)(p + b + 40)
           ^ *(const uint64_t *)(p + b + 48)^ *(const uint64_t *)(p + b + 56);
    return x;
}

#define PART_BYTES   (128u * 1024u)
#define RID_SLOTS    60u

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *path = argc > 1 ? argv[1]
        : "I:\\model\\Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *twin_path = argc > 2 ? argv[2] : "build\\gguf_roundtrip.twin";

    printf("=== gguf_roundtrip — full file through RID slot region ===\n");

    /* ── RID geometry ────────────────────────────────────────────────── */
    static uint8_t vw[5][60];
    const char *lname[5] = { "pent", "tri", "snubL", "snubR", "hosoya" };
    if (build_rid(vw) != 0) { printf("FAIL rid geometry\n"); return 1; }
    if (memcmp(vw[2], vw[3], 60) == 0) {
        printf("FAIL enantiomorphs identical\n"); return 1;
    }
    printf("chiral: snubL != snubR over 30/30 squares (enantiomorph pair) ✓\n");
    /* 5th language: golden-spiral (phyllotaxis) — stride F(7)=13 mod 60
     * proven bijective in tools/hosoya_view_probe.c (gcd oracle + inverse 37) */
    for (uint32_t p = 0; p < 60; p++) vw[4][p] = (uint8_t)((p * 13u) % 60u);
    { uint8_t hit[60]; memset(hit, 0, sizeof(hit));
      for (uint32_t p = 0; p < 60; p++) {
          if (hit[vw[4][p]]) { printf("FAIL hosoya view not bijective\n"); return 1; }
          hit[vw[4][p]] = 1;
      } }

    /* ── read entire file ────────────────────────────────────────────── */
    FILE *fp = fopen(path, "rb");
    if (!fp) { printf("FAIL open %s\n", path); return 1; }
    fseek(fp, 0, SEEK_END);
    long file_sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *src = (uint8_t *)malloc((size_t)file_sz);
    size_t nr = fread(src, 1, (size_t)file_sz, fp);
    fclose(fp);
    if ((long)nr != file_sz) { printf("FAIL read %ld\n", file_sz); return 1; }

    uint32_t total_parts = (uint32_t)(((size_t)file_sz + PART_BYTES - 1) / PART_BYTES);
    uint32_t layers = (total_parts + RID_SLOTS - 1) / RID_SLOTS;
    printf("file: %s · %.1f MB · %u parts · %u layers\n\n",
           path, (double)file_sz / 1e6, total_parts, layers);

    /* part table + XOR oracle */
    typedef struct { uint32_t off; uint32_t len; } Part;
    Part *parts = (Part *)malloc(sizeof(Part) * total_parts);
    uint64_t *pxor = (uint64_t *)malloc(sizeof(uint64_t) * total_parts);
    for (uint32_t f = 0; f < total_parts; f++) {
        parts[f].off = f * PART_BYTES;
        parts[f].len = (uint32_t)((size_t)file_sz - parts[f].off);
        if (parts[f].len > PART_BYTES) parts[f].len = PART_BYTES;
        pxor[f] = xor64(src + parts[f].off, parts[f].len);
    }

    /* rebuild buffer (from region) */
    uint8_t *rebld = (uint8_t *)calloc(1, (size_t)layers * RID_SLOTS * PART_BYTES);

    int all_ok = 1;

    for (int lang = 0; lang < 5; lang++) {
        DtSlotRegion reg;
        remove(twin_path);
        if (dt_slot_init_twin(&reg, twin_path,
                              (size_t)layers * RID_SLOTS, PART_BYTES) != 0) {
            printf("FAIL twin init\n"); return 1;
        }

        /* ── R1: BAKE + READBACK ──────────────────────────────────── */
        int bake_bad = 0;
        for (uint32_t f = 0; f < total_parts; f++) {
            uint32_t addr = fwd_view(vw[lang], f);
            if (!dt_slot_put(&reg, addr, src + parts[f].off, parts[f].len))
                bake_bad++;
        }
        uint32_t rb_bad = 0;
        for (uint32_t f = 0; f < total_parts; f++) {
            uint8_t *p = dt_slot_ptr(&reg, fwd_view(vw[lang], f));
            if (!p || memcmp(p, src + parts[f].off, parts[f].len) != 0)
                rb_bad++;
        }
        printf("[%s] R1 BAKE+READ  %u parts · bake bad=%u · readback bad=%u (%s)\n",
               lname[lang], total_parts, bake_bad, rb_bad,
               (bake_bad || rb_bad) ? "FAIL" : "lossless");
        if (bake_bad || rb_bad) { all_ok = 0; dt_slot_destroy(&reg); continue; }

        /* ── R2: FULL REBUILD from region → byte-identical ─────────── */
        memset(rebld, 0, (size_t)layers * RID_SLOTS * PART_BYTES);
        for (uint32_t addr = 0; addr < layers * RID_SLOTS; addr++) {
            uint32_t w; uint32_t f = inv_view(vw[lang], addr, &w);
            if (f >= total_parts) continue;
            uint8_t *p = dt_slot_ptr(&reg, addr);
            if (!p) continue;
            memcpy(rebld + parts[f].off, p, parts[f].len);
        }
        int rebuild_ok = (memcmp(rebld, src, (size_t)file_sz) == 0);
        printf("[%s] R2 REBUILD   rebuilt %zu MB · byte-identical=%s\n",
               lname[lang], (size_t)(file_sz / (1024*1024)),
               rebuild_ok ? "YES" : "NO");
        if (!rebuild_ok) all_ok = 0;

        /* ── R3: DAMAGE DRILL ──────────────────────────────────────── */
        if (lang == 0 && total_parts > 9) {
            uint32_t fhit = total_parts / 2;
            uint32_t aha = fwd_view(vw[lang], fhit);
            dt_slot_ptr(&reg, aha)[17] ^= 0x80;
            uint32_t found = total_parts;
            for (uint32_t f = 0; f < total_parts; f++) {
                uint8_t *p = dt_slot_ptr(&reg, fwd_view(vw[lang], f));
                if (!p || xor64(p, parts[f].len) != pxor[f]) { found = f; break; }
            }
            int loc_ok = (found == fhit);
            if (loc_ok)
                dt_slot_put(&reg, aha, src + parts[fhit].off, parts[fhit].len);
            uint32_t still_bad = 0;
            for (uint32_t f = 0; f < total_parts; f++) {
                uint8_t *p = dt_slot_ptr(&reg, fwd_view(vw[lang], f));
                if (!p || memcmp(p, src + parts[f].off, parts[f].len) != 0)
                    still_bad++;
            }
            printf("[%s] R3 DAMAGE    flip@part %u → localized=%s · restored bad=%u (%s)\n",
                   lname[lang], fhit, loc_ok ? "YES" : "NO", still_bad,
                   still_bad ? "STILL BROKEN" : "lossless again");
            if (!loc_ok || still_bad) all_ok = 0;
        }

        dt_slot_destroy(&reg);
        printf("\n");
    }

    /* ── R4: persistence ────────────────────────────────────────────── */
    FILE *tf = fopen(twin_path, "rb");
    int persist_ok = 0;
    if (tf) {
        fseek(tf, 0, SEEK_END);
        long tsz = ftell(tf);
        fclose(tf);
        persist_ok = (tsz == (long)((size_t)layers * RID_SLOTS * PART_BYTES));
    }
    printf("R4 PERSIST    twin file: %s (%.1f MB)\n",
           persist_ok ? "YES" : "NO",
           persist_ok ? (double)layers * RID_SLOTS * PART_BYTES / 1e6 : 0.0);

    printf("\n%s\n", (all_ok && persist_ok)
        ? "RESULT: PASSED — full GGUF roundtrip through RID slots"
        : "RESULT: FAILED");

    free(src); free(parts); free(pxor); free(rebld);
    return (all_ok && persist_ok) ? 0 : 1;
}

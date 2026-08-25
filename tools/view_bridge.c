/*
 * tools/view_bridge.c — INTEROP BRIDGE: bake once, serve every language
 * ════════════════════════════════════════════════════════════════════════
 * Proves LANGUAGES.md rationale #1 for real: gguf_roundtrip re-bakes per
 * language; this probe bakes ONCE through 'pent' and serves the OTHER 8
 * languages by pure ADDRESS TRANSLATION — zero re-bake, zero copy:
 *
 *   part f lives at  pent addr  A0(f) = l*60 + vw0[w]
 *   lang L wants     AL(f)  = l*60 + vwL[w]
 *   bridge sigma_L:  vwL[w] -> vw0[w]  (permutation of 60, same all layers)
 *   => read at l*60 + sigma_L[vwL[w]] gives part f's bytes.
 *
 * Gates:
 *   B1 SINGLE BAKE    one write pass (count bytes)
 *   B2 CROSS-READ     8 languages x 5156 parts memcmp vs source (zero-copy)
 *   B3 XOR CONSENSUS  every view sees identical XOR == source
 *   B4 DAMAGE X-VIEW  flip 1 byte -> EACH language's own ordering
 *                     localizes the SAME part -> restore once
 *   B5 CYCLE CENSUS   sigma_L decomposition (fixed points = shared words)
 *   MUT               corrupted sigma -> cross-read MUST fail (red ok)
 *
 * BUILD: gcc -O2 -std=c11 -Wall -I core -o build/view_bridge tools/view_bridge.c -lm
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

/* ── int dodecahedron + RID labeling + snub diagonals (from gguf_roundtrip) */
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

#include "view_bridge_geom.inc"   /* build_rid(vw) — same as gguf_roundtrip */
#include "view_bridge_main.inc"   /* gates B1-B5 + MUT */

/*
 * tools/geo_archimedean_test.c — Rhombicosidodecahedron from int dodeca
 * ══════════════════════════════════════════════════════════════════
 * User's bridge data:
 *   RID  : 62 F (20 tri + 30 sq + 12 pent) · E=120 · V=60
 *          -> GEO_COMP_SPIKE_120 (E=120)
 *   Snub : 92 F (80 tri + 12 pent) · E=150 · V=60
 *          -> GEO_GOLDBERG_92
 *
 * Construction (combinatorial, int-only):
 *   cantellation of OUR integer dodecahedron (phi=13/8 x104):
 *   - RID vertex  = directed dodeca edge (u,v)          -> 60
 *   - triangle    = dodeca VERTEX v: {R(v,a),R(v,b),R(v,c)}  -> 20
 *   - pentagon    = dodeca FACE  f: every-other of decagon   -> 12
 *   - square      = dodeca EDGE   (u,v) 4-cycle              -> 30
 *   Euler gate: 60 - E + 62 == 2  =>  E == 120
 *
 * BUILD: gcc -O2 -Wall -D__USE_MINGW_ANSI_STDIO=1 -Icore \
 *        -o build/geo_archimedean_test tools/geo_archimedean_test.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

typedef struct { int64_t x, y, z; } V3;
static V3 V(int64_t x, int64_t y, int64_t z) { V3 v={x,y,z}; return v; }
static V3 vsub(V3 a,V3 b){ return V(a.x-b.x,a.y-b.y,a.z-b.z); }
static V3 vcross(V3 a,V3 b){ return V(a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x); }
static int64_t vdot(V3 a,V3 b){ return a.x*b.x+a.y*b.y+a.z*b.z; }
static int64_t det3(V3 a,V3 b,V3 c){
    return a.x*(b.y*c.z-b.z*c.y)-a.y*(b.x*c.z-b.z*c.x)+a.z*(b.x*c.y-b.y*c.x);
}
static int64_t dist2(V3 a,V3 b){ V3 d=vsub(a,b); return vdot(d,d); }

static V3 verts[20]; static uint32_t n_verts=0;
static uint32_t faces[16][5]; static V3 fnorm[16]; static uint32_t n_faces=0;
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

int main(void){
    build_dodeca();
    printf("=== geo_archimedean_test — RID (cantellated dodeca) ===\n");
    printf("dodeca: %u verts\n\n", n_verts);

    /* edge band (rational-phi aware) */
    int64_t emin=0;
    for(uint32_t i=0;i<n_verts;i++)for(uint32_t j=i+1;j<n_verts;j++){
        int64_t d2=dist2(verts[i],verts[j]);
        if(d2&&(!emin||d2<emin)) emin=d2;
    }
    int64_t elo=emin, ehi=emin+emin/16;
    #define ADJ(i,j) ({int64_t d2=dist2(verts[i],verts[j]); d2&&d2>=elo&&d2<=ehi;})

    /* undirected edges */
    for(uint32_t i=0;i<n_verts;i++)
        for(uint32_t j=i+1;j<n_verts;j++)
            if(ADJ(i,j)&&n_edges<40){edges[n_edges][0]=i;edges[n_edges][1]=j;n_edges++;}
    printf("dodeca edges: %u (expect 30)\n", n_edges);

    /* faces: planar 5-cycles */
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
                    if(b<mn5)mn5=b; if(c<mn5)mn5=c;
                    if(dd<mn5)mn5=dd; if(e<mn5)mn5=e;
                    if(a!=mn5||b>e)continue;
                    uint32_t cl[5]={a,b,c,dd,e};
                    V3 u=vsub(verts[cl[1]],verts[cl[0]]);
                    V3 w=vsub(verts[cl[2]],verts[cl[0]]);
                    int planar=1;
                    for(int k=3;k<5&&planar;k++){
                        V3 r2=vsub(verts[cl[k]],verts[cl[0]]);
                        if(labs((long)det3(u,w,r2))>100000L) planar=0;
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
                    fnorm[n_faces]=vcross(vsub(verts[cl[1]],verts[cl[0]]),
                                          vsub(verts[cl[2]],verts[cl[0]]));
                    n_faces++;
                }
            }
        }
    }
    printf("dodeca faces: %u (expect 12)\n\n", n_faces);


    /* ── RID combinatorics: (vertex, face) labeling ──────────────────
       P(u,F) = RID vertex at dodeca vertex u on face F.  20×3 = 60
       pent-side: consecutive corners within face F        (2 nbrs)
       tri-side : other faces G ∋ u                        (2 nbrs)
       degree 4 → E=120 · faces 20 tri + 30 sq + 12 pent · Euler=2 */
    uint32_t nrf=0;
    static uint32_t rf_v[64], rf_f[64];
    static int32_t rid_of[320];
    memset(rid_of,0xFF,sizeof(rid_of));
    for(uint32_t f=0;f<n_faces;f++)
        for(int p=0;p<5;p++){
            uint32_t u=faces[f][p];
            if(rid_of[u*16+f]<0){
                rid_of[u*16+f]=(int32_t)nrf;
                rf_v[nrf]=u; rf_f[nrf]=f; nrf++;
            }
        }
    printf("RID vertices (vertex,face): %u (expect 60)\n", nrf);

    static uint8_t radj[64][64]; memset(radj,0,sizeof(radj));
    for(uint32_t i=0;i<nrf;i++){
        uint32_t u=rf_v[i], f=rf_f[i];
        int pos=-1;
        for(int p=0;p<5;p++) if(faces[f][p]==u){pos=p;break;}
        uint32_t pv=faces[f][(pos+4)%5], nx=faces[f][(pos+1)%5];
        int32_t jp=rid_of[pv*16+f], jn=rid_of[nx*16+f];      /* pent-side */
        radj[i][jp]=radj[jp][i]=1;
        radj[i][jn]=radj[jn][i]=1;
        for(uint32_t g=0;g<n_faces;g++){                     /* tri-side */
            if(g==f) continue;
            for(int p2=0;p2<5;p2++) if(faces[g][p2]==u){
                int32_t j=rid_of[u*16+g];
                radj[i][j]=radj[j][i]=1; break;
            }
        }
    }

    uint32_t deg_ok=1;
    for(uint32_t i=0;i<nrf&&deg_ok;i++){
        uint32_t d=0;
        for(uint32_t j=0;j<nrf;j++) d+=radj[i][j];
        if(d!=4) deg_ok=0;
    }
    printf("degree==4 everywhere: %s\n", deg_ok?"PASS":"FAIL");

    uint32_t r_edges=0;
    for(uint32_t i=0;i<nrf;i++)
        for(uint32_t j=i+1;j<nrf;j++) r_edges+=radj[i][j];
    printf("RID edges: %u (expect 120)\n", r_edges);

    /* pentagons = original faces verified as 5-cycles in RID graph */
    uint32_t npent=0;
    for(uint32_t f=0;f<n_faces;f++){
        int okc=1;
        for(int p=0;p<5&&okc;p++){
            int32_t a=rid_of[faces[f][p]*16+f];
            int32_t b=rid_of[faces[f][(p+1)%5]*16+f];
            if(!radj[a][b]) okc=0;
        }
        npent+=okc;
    }
    printf("pentagons: %u (expect 12)\n", npent);

    /* triangles = per-vertex cliques {R(u,F): F∋u} */
    uint32_t ntri=0;
    for(uint32_t u=0;u<n_verts;u++){
        int32_t ids[8]; uint32_t nn=0;
        for(uint32_t g=0;g<n_faces;g++)
            for(int p2=0;p2<5;p2++)
                if(faces[g][p2]==u){ ids[nn++]=rid_of[u*16+g]; break; }
        if(nn==3&&radj[ids[0]][ids[1]]&&radj[ids[1]][ids[2]]&&radj[ids[0]][ids[2]])
            ntri++;
    }
    printf("triangles: %u (expect 20)\n", ntri);

    /* squares = per-edge 4-cycles across the two faces sharing it */
    uint32_t nsq=0;
    for(uint32_t k=0;k<n_edges;k++){
        uint32_t u=edges[k][0], v=edges[k][1], ff[4], nf=0;
        for(uint32_t g=0;g<n_faces&&nf<2;g++){
            int hu=0,hv=0;
            for(int p2=0;p2<5;p2++){
                if(faces[g][p2]==u)hu=1;
                if(faces[g][p2]==v)hv=1;
            }
            if(hu&&hv) ff[nf++]=g;
        }
        if(nf!=2) continue;
        int32_t q0=rid_of[u*16+ff[0]], q1=rid_of[v*16+ff[0]];
        int32_t q2=rid_of[v*16+ff[1]], q3=rid_of[u*16+ff[1]];
        if(radj[q0][q1]&&radj[q1][q2]&&radj[q2][q3]&&radj[q3][q0]) nsq++;
    }
    printf("squares: %u (expect 30)\n", nsq);

    {
        uint32_t F_total=npent+ntri+nsq;
        int32_t euler=(int32_t)nrf-(int32_t)r_edges+(int32_t)F_total;
        printf("Euler: %u - %u + %u = %d (expect 2)\n",
               nrf,r_edges,F_total,euler);
        int pass=(nrf==60)&&(r_edges==120)&&(npent==12)&&(ntri==20)&&
                 (nsq==30)&&(euler==2)&&deg_ok;
        printf("\n%s\n", pass?"RESULT: PASSED":"RESULT: FAILED");
        return pass?0:1;
    }
}

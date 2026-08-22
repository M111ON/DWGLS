/*
 * tools/geo_snub_test.c — Snub dodecahedron = chiral diagonal split of RID squares
 * ════════════════════════════════════════════════════════════════════════════════
 * User's bridge image ("shift square > rhombus"):
 *   RID  : V=60 · E=120 · F=62 (20 tri + 30 sq + 12 pent) · degree 4
 *   snub : split each square along ONE diagonal
 *          -> V=60 · E=150 · F=92 (80 tri + 12 pent) · degree 5 · 3.3.3.3.5
 *          -> GEO_GOLDBERG_92, chiral
 *
 * Proof method (constraint solve, no guessed local rule):
 *   Each square s has 2 diagonal choices -> binary var x_s.
 *   Vertex figure demand: every RID vertex must be a diagonal endpoint
 *   EXACTLY once among its 2 squares (degree 4 -> 5 uniformly).
 *   That is one XOR constraint per vertex linking the choices of its two
 *   squares -> parity system solved by union-find with path parity.
 *
 * Oracles:
 *   C1 : every RID vertex lies on EXACTLY 2 squares (vertex figure 3.4.5.4)
 *   C2 : parity system CONSISTENT (a perfect diagonal system exists)
 *   U1 : solution count == 2, and the two are bitwise complements
 *        -> unique up to mirror = CHIRAL (enantiomorph pair)
 *   G  : BOTH solutions pass full snub gate:
 *        E=150 · degree 5 everywhere · EXACTLY 80 triangles (20 clique +
 *        60 halves, none spurious) · per-vertex triangle incidence == 4
 *        (figure 3.3.3.3.5) · 12 pentagon cycles intact · Euler 60-150+92=2
 *   M1 : mutating ANY single choice breaks the degree gate (suite can go red)
 *
 * BUILD: gcc -O2 -Wall -D__USE_MINGW_ANSI_STDIO=1 -Icore \
 *        -o build/geo_snub_test tools/geo_snub_test.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

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

/* ── RID state ─────────────────────────────────────────────────────── */
static int32_t rid_of[320];
static uint32_t rf_v[64], rf_f[64], nrf=0;
static uint8_t radj[64][64];       /* RID base adjacency (deg 4) */
static uint8_t sradj[64][64];      /* candidate snub adjacency   */
#define FACE_POS(f,u) ({int _p=-1; \
    for(int _q=0;_q<5;_q++) if(faces[(f)][_q]==(u)){_p=_q;break;} _p;})

/* square corners of edge k: q0=R(a,P) q1=R(b,P) q2=R(b,Q) q3=R(a,Q)
   diagonal A = {q0,q2} (choice bit 0), B = {q1,q3} (bit 1) */
static uint32_t sqc[40][4];

/* ── union-find with parity ────────────────────────────────────────── */
static int32_t uf_p[40]; static uint8_t uf_par[40];
static int uf_find(int x, uint8_t *par){
    if(uf_p[x]<0){ *par=0; return x; }
    uint8_t pp; int r=uf_find(uf_p[x],&pp);
    uf_par[x]^=pp; uf_p[x]=r; *par=uf_par[x]; return r;
}
/* constraint: b_j = b_i XOR d ; returns 0 on contradiction */
static int uf_merge(int i,int j,uint8_t d){
    uint8_t pi,pj; int ri=uf_find(i,&pi), rj=uf_find(j,&pj);
    if(ri==rj) return (pi^pj)==d;
    uf_p[ri]=rj; uf_par[ri]=(uint8_t)(pi^d^pj);
    return 1;
}

int main(void){
    build_dodeca();

    /* ── dodeca edges ──────────────────────────────────────────────── */
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
    printf("T0 dodeca edges: %u (expect 30)\n", n_edges);

    /* ── dodeca faces: planar 5-cycles ─────────────────────────────── */
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
    printf("T0 dodeca faces: %u (expect 12)\n", n_faces);
    if(n_edges!=30||n_faces!=12){ printf("RESULT: FAILED\n"); return 1; }

    /* ── RID labeling R(u,F) + base adjacency ──────────────────────── */
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
    printf("T1 RID base: v=%u (60) e=%u (120) deg4=%s\n",
           nrf,r_edges,deg_ok?"PASS":"FAIL");
    if(nrf!=60||r_edges!=120||!deg_ok){ printf("RESULT: FAILED\n"); return 1; }

    /* ── squares + membership ──────────────────────────────────────── */
    for(uint32_t k=0;k<n_edges;k++){
        uint32_t a=edges[k][0], b=edges[k][1], ff[2], nf=0;
        for(uint32_t g=0;g<n_faces&&nf<2;g++)
            if(FACE_POS(g,a)>=0&&FACE_POS(g,b)>=0) ff[nf++]=g;
        uint32_t P=ff[0], Q=ff[1];
        sqc[k][0]=rid_of[a*16+P]; sqc[k][1]=rid_of[b*16+P];
        sqc[k][2]=rid_of[b*16+Q]; sqc[k][3]=rid_of[a*16+Q];
    }
    /* C1: each RID vertex on exactly 2 squares */
    uint32_t c1_ok=1;
    static uint8_t onsq[64][40]; static int32_t vsq[64][2];
    memset(onsq,0,sizeof(onsq)); memset(vsq,0xFF,sizeof(vsq));
    for(uint32_t k=0;k<n_edges;k++)
        for(int c=0;c<4;c++){
            uint32_t w=sqc[k][c];
            onsq[w][k]=1;
            if(vsq[w][0]<0) vsq[w][0]=(int32_t)k;
            else if(vsq[w][1]<0 && vsq[w][0]!=(int32_t)k) vsq[w][1]=(int32_t)k;
        }
    for(uint32_t w=0;w<nrf;w++){
        uint32_t cnt=0;
        for(uint32_t k=0;k<n_edges;k++) cnt+=onsq[w][k];
        if(cnt!=2) c1_ok=0;
        if(vsq[w][0]<0||vsq[w][1]<0) c1_ok=0;
    }
    printf("C1 vertex-on-2-squares: %s\n", c1_ok?"PASS":"FAIL");
    if(!c1_ok){ printf("RESULT: FAILED\n"); return 1; }

    /* ── C2: parity system ───────────────────────────────────────────
       cover(w | square k, bit t): t=0 -> {q0,q2}, t=1 -> {q1,q3}.
       Constraint per w: covered-by-vsq0 XOR covered-by-vsq1
         =>  b_j = b_i XOR d_w  where
             d_w = cw0 ^ cw1 ^ 1   (cw* = the bit that would cover w)     */
    memset(uf_p,0xFF,sizeof(uf_p)); memset(uf_par,0,sizeof(uf_par));
    uint32_t sys_ok=1;
    for(uint32_t w=0;w<nrf;w++){
        int32_t i=vsq[w][0], j=vsq[w][1];
        uint32_t q0=sqc[i][0],q2=sqc[i][2];
        uint8_t cw0=(w==q0||w==q2)?0:1;
        uint32_t r0=sqc[j][0],r2=sqc[j][2];
        uint8_t cw1=(w==r0||w==r2)?0:1;
        if(!uf_merge((int)i,(int)j,(uint8_t)(cw0^cw1^1))) sys_ok=0;
    }
    printf("C2 parity system consistent: %s\n", sys_ok?"PASS":"FAIL");
    if(!sys_ok){ printf("RESULT: FAILED\n"); return 1; }

    /* components */
    uint32_t ncomp=0; static int32_t root_of[40]; static uint8_t par_of[40];
    {
        static int32_t seen[64]; memset(seen,0xFF,sizeof(seen));
        for(uint32_t k=0;k<n_edges;k++){
            uint8_t p; int r=uf_find((int)k,&p);
            if(seen[r]<0) seen[r]=(int32_t)ncomp++;
            root_of[k]=seen[r];
            par_of[k]=p;
        }
    }
    printf("U0 independent choice groups: %u\n", ncomp);
    if(ncomp>6){ printf("too many solutions to enumerate\n"); return 1; }

    /* ── GATE on a full choice vector ──────────────────────────────── */
    #define GATE(label, resvar) do{ \
        uint32_t e2=0, dok=1; \
        for(uint32_t i=0;i<nrf;i++){ uint32_t d=0; \
            for(uint32_t j=0;j<nrf;j++) d+=sradj[i][j]; \
            if(d!=5) dok=0; } \
        for(uint32_t i=0;i<nrf;i++)for(uint32_t j=i+1;j<nrf;j++) e2+=sradj[i][j]; \
        uint32_t ntri=0; static uint32_t tinc[64]; \
        memset(tinc,0,sizeof(tinc)); \
        for(uint32_t i=0;i<nrf;i++)for(uint32_t j=i+1;j<nrf;j++){ \
            if(!sradj[i][j])continue; \
            for(uint32_t k=j+1;k<nrf;k++) \
                if(sradj[i][k]&&sradj[j][k]){ntri++;tinc[i]++;tinc[j]++;tinc[k]++;} \
        } \
        uint32_t incok=1; \
        for(uint32_t w=0;w<nrf;w++) if(tinc[w]!=4) incok=0; \
        uint32_t npent=0; \
        for(uint32_t f=0;f<n_faces;f++){ \
            int okc=1; \
            for(int p=0;p<5&&okc;p++){ \
                int32_t aa=rid_of[faces[f][p]*16+f]; \
                int32_t bb=rid_of[faces[f][(p+1)%5]*16+f]; \
                if(!sradj[aa][bb]) okc=0; \
            } \
            npent+=(uint32_t)okc; \
        } \
        int32_t euler=(int32_t)nrf-(int32_t)e2+(int32_t)(ntri+npent); \
        uint32_t gok=(e2==150)&&(dok)&&(ntri==80)&&(incok)&&(npent==12)&&(euler==2); \
        printf("%s e=%u(%s) deg5=%s tri=%u(%s) inc4=%s pent=%u(%s) euler=%d(%s)\n", \
            label,e2,"150",dok?"Y":"n",ntri,"80",incok?"Y":"n", \
            npent,"12",euler,"2"); \
        (resvar)=gok; \
    }while(0)

    #define BUILD_SNUB(ch) do{ \
        memcpy(sradj,radj,sizeof(radj)); \
        for(uint32_t k=0;k<n_edges;k++){ \
            int32_t a,c2_; \
            uint8_t t=(ch)[k]; \
            a   =(t==0)?sqc[k][0]:sqc[k][1]; \
            c2_ =(t==0)?sqc[k][2]:sqc[k][3]; \
            if(sradj[a][c2_]){ printf("BAD DIAGONAL\n"); return 1; } \
            sradj[a][c2_]=sradj[c2_][a]=1; \
        } \
    }while(0)

    /* ── enumerate all solutions (mask over components) ────────────── */
    uint32_t nsol_pass=0, sol_mask[64]={0}, n_sol=0;
    for(uint32_t mask=0; mask<(1u<<ncomp); mask++){
        uint8_t bits[40];
        for(uint32_t k=0;k<n_edges;k++)
            bits[k]=(uint8_t)(((mask>>root_of[k])&1u)^par_of[k]);
        BUILD_SNUB(bits);
        uint32_t g=0;
        GATE("  cand:", g);
        if(g&&n_sol<64){ sol_mask[n_sol++]=mask; }
        nsol_pass+=g;
    }
    printf("U1 solutions: %u total, %u pass full gate (of %u candidates)\n",
           1u<<ncomp, nsol_pass, 1u<<ncomp);

    /* chirality: the two expected solutions are bitwise complements */
    uint32_t chir=0;
    if(nsol_pass==2&&n_sol==2){
        uint8_t b1[40],b2[40]; uint32_t diff=0;
        for(uint32_t k=0;k<n_edges;k++){
            b1[k]=(uint8_t)(((sol_mask[0]>>root_of[k])&1u)^par_of[k]);
            b2[k]=(uint8_t)(((sol_mask[1]>>root_of[k])&1u)^par_of[k]);
            if(b1[k]!=b2[k]) diff++;
        }
        chir=(diff==n_edges);
        printf("T6 enantiomorphs differ on %u/%u squares (expect 30/30)\n",
               diff,n_edges);
    }

    /* ── M1: single-bit mutation must break degree gate ────────────── */
    uint8_t best[40];
    for(uint32_t k=0;k<n_edges;k++)
        best[k]=(uint8_t)(((sol_mask[0]>>root_of[k])&1u)^par_of[k]);
    uint32_t mok=1;
    for(uint32_t kk=0;kk<n_edges&&mok;kk++){
        uint8_t mut[40]; memcpy(mut,best,sizeof(mut)); mut[kk]^=1;
        BUILD_SNUB(mut);
        for(uint32_t i=0;i<nrf;i++){
            uint32_t d=0;
            for(uint32_t j=0;j<nrf;j++) d+=sradj[i][j];
            if(d!=5) goto mut_broken;
        }
        mok=0; /* a mutant still uniform = suite blind */
        mut_broken:;
    }
    printf("M1 all 30 single-bit mutants break degree gate: %s\n",
           mok?"PASS":"FAIL");

    uint32_t unique_pair=(nsol_pass==2)&&((1u<<ncomp)==2u);
    uint32_t pass=unique_pair&&chir&&mok;
    printf("\ngates: C1=%u C2=%u U1=%u T6=%u M1=%u\n",
           c1_ok,sys_ok,(uint32_t)(nsol_pass==2),chir,mok);
    printf("\n%s\n", pass?"RESULT: PASSED":"RESULT: FAILED");
    return pass?0:1;
}

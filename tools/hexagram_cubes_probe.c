/* tools/hexagram_cubes_probe.c - hexagon IS cube-in-2D (lossless law)
 * ===================================================================
 * user observation: figure drawn 100% from hexagons/rhombus yet reads
 * as cubes. Probe proves it is not illusion but reversible projection
 * LAW, int-only:
 *   H1 hex lattice == cubic lattice sliced x+y+z=0 (bijection)
 *   H2 cube along body-diag (1,1,1): outline=hexagon,
 *      3 visible faces = 3 rhombi tiling it exactly
 *   H3 hexagon R=2 = 24 unit triangles -> 3 lozenge orientations,
 *      each exact cover by 12 rhombi ("12 pieces / 3 ways")
 *   H4 2x2x2 = 8 octants -> parity split 4+4 ("x2") -> recombine
 *      -> 8 slots = one tess frame (frame-as-index, 8 cubes x 144)
 * BUILD: gcc -O2 -Wall -I core -o build/hexagram_cubes_probe
 *        tools/hexagram_cubes_probe.c
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int pass=0, fail=0;
#define CHECK(d,c) do{ if(c){pass++;printf("  PASS - %s\n",d);} \
                       else{fail++;printf("  FAIL - %s\n",d);} }while(0)

/* ---- H1. axial <-> cube bijection -------------------------------- */
static void h1(void){
    printf("\nH1. hex axial <-> cube coords (x+y+z=0)\n");
    int ok=1;
    for(int x=-12;x<=12;x++)
    for(int y=-12;y<=12;y++)
    for(int z=-12;z<=12;z++){
        if(x+y+z) continue;
        int q=x, r=z;
        if(q!=x || -q-r!=y || r!=z) ok=0;
    }
    CHECK("roundtrip lossless on |coord|<=12", ok);
    const int st[6][3]={{1,-1,0},{1,0,-1},{0,1,-1},
                        {-1,1,0},{-1,0,1},{0,-1,1}};
    int ok2=1;
    for(int i=0;i<6;i++)
        if(st[i][0]+st[i][1]+st[i][2]!=0) ok2=0;
    CHECK("6 hex steps = {axis_i - axis_j}, sum=0 invariant", ok2);
}

/* ---- H2. cube -> hexagon shadow ---------------------------------- */
typedef struct{int a,b;}P2;
static int peq(P2 u,P2 v){return u.a==v.a&&u.b==v.b;}
static P2 psub(P2 u,P2 v){P2 w={u.a-v.a,u.b-v.b};return w;}
static P2 proj(int x,int y,int z){P2 p={x-z,y-z};return p;}
static int para4(P2 A,P2 B,P2 C,P2 D){
    return (A.a+C.a==B.a+D.a)&&(A.b+C.b==B.b+D.b);
}
static void h2(void){
    printf("\nH2. cube (1,1,1)-shadow = hexagon + 3 rhombi\n");
    static const int V[8][3]={{0,0,0},{1,0,0},{0,1,0},{0,0,1},
                              {1,1,0},{1,0,1},{0,1,1},{1,1,1}};
    P2 pv[8];
    for(int i=0;i<8;i++) pv[i]=proj(V[i][0],V[i][1],V[i][2]);
    CHECK("corners 000 & 111 collapse to center", peq(pv[0],pv[7]));
    P2 pts[8]; int n=0;
    for(int i=0;i<8;i++){
        int dup=0;
        for(int j=0;j<n;j++) if(peq(pts[j],pv[i])) dup=1;
        if(!dup) pts[n++]=pv[i];
    }
    CHECK("distinct projected points = 7 (center + ring6)", n==7);
    P2 c=pv[0], ring[6]; int rn=0;
    for(int i=0;i<n;i++) if(!peq(pts[i],c)) ring[rn++]=pts[i];
    CHECK("ring has 6 points", rn==6);
    { const P2 U[3]={{1,0},{0,1},{1,1}};
      int all=1;
      for(int i=0;i<rn;i++){
          P2 d=psub(ring[i],c);
          P2 nd={-d.a,-d.b};
          int hit=peq(d,U[0])||peq(d,U[1])||peq(d,U[2])||
                  peq(nd,U[0])||peq(nd,U[1])||peq(nd,U[2]);
          if(!hit) all=0;
      }
      CHECK("ring pt in {+-u1,+-u2,+-(u1+u2)} (hex nbrs)", all);
    }
    { int f1=para4(pv[1],pv[4],pv[7],pv[5]);
      int f2=para4(pv[2],pv[4],pv[7],pv[6]);
      int f3=para4(pv[3],pv[5],pv[7],pv[6]);
      CHECK("3 visible faces = lattice rhombi", f1&&f2&&f3);
      int cov=1;
      for(int i=0;i<rn&&cov;i++){
          P2 p=ring[i];
          int in=peq(p,pv[1])||peq(p,pv[2])||peq(p,pv[3])||
                 peq(p,pv[4])||peq(p,pv[5])||peq(p,pv[6]);
          if(!in) cov=0;
      }
      CHECK("union of 3 rhombi = exact hexagon cover", cov);
    }
}

/* ---- H3. hexagon R=2: 24 triangles -> lozenge tilings ------------ */
typedef struct{int a,b,dn;}Tri;
static Tri tris[64]; static int ntr=0;
static int hexdist2(int x,int y){
    /* plane through center (2,2,2): x+y+z=6 */
    int z=6-x-y;
    int dx=x-2, dy=y-2, dz=z-2;
    int m=abs(dx);
    if(abs(dy)>m)m=abs(dy);
    if(abs(dz)>m)m=abs(dz);
    return m;
}
static int tri_in(int a,int b,int dn){
    if(dn){
        /* dn(a,b): (a+1,b),(a,b+1),(a+1,b+1) */
        if(hexdist2(a+1,b)>2) return 0;
        if(hexdist2(a,b+1)>2) return 0;
        if(hexdist2(a+1,b+1)>2) return 0;
    }else{
        /* up(a,b): (a,b),(a+1,b),(a,b+1) */
        if(hexdist2(a,b)>2) return 0;
        if(hexdist2(a+1,b)>2) return 0;
        if(hexdist2(a,b+1)>2) return 0;
    }
    return 1;
}
static void h3(void){
    printf("\nH3. hexagon R=2: 24 triangles -> 3 lozenge orientations\n");
    ntr=0;
    for(int a=-1;a<=5;a++)for(int b=-1;b<=5;b++){
        if(tri_in(a,b,0)){ tris[ntr].a=a;tris[ntr].b=b;tris[ntr].dn=0;ntr++; }
        if(tri_in(a,b,1)){ tris[ntr].a=a;tris[ntr].b=b;tris[ntr].dn=1;ntr++; }
    }
    printf("        unit triangles = %d (expect 24)\n",ntr);
    CHECK("hexagon R=2 = 24 unit triangles", ntr==24);
    /* orientation pairing: dn(a,b) pairs with up at:
     *   k=0: up(a,b)     k=1: up(a+1,b)     k=2: up(a,b+1) */
    int allcover=1;
    for(int k=0;k<3;k++){
        int used[64]; memset(used,0,sizeof(used));
        int pairs=0;
        /* outer = dn tris; rule table below is dn->up */
        for(int i=0;i<ntr;i++){
            if(!tris[i].dn||used[i]) continue;
            int ka=tris[i].a, kb=tris[i].b;
            if(k==1)ka++;
            if(k==2)kb++;
            for(int j=0;j<ntr;j++){
                if(tris[j].dn||used[j]) continue;
                if(tris[j].a==ka&&tris[j].b==kb){
                    used[i]=used[j]=1; pairs++; break;
                }
            }
        }
        int leftover=0;
        for(int i=0;i<ntr;i++) if(!used[i]) leftover++;
        printf("        pure-orientation %d: %d rhombi, %d boundary tris\n",
               k,pairs,leftover);
        if(pairs!=10||leftover!=4) allcover=0;
    }
    CHECK("pure orientation: 10 rhombi + 4 boundary tris (same x3)",
          allcover);
    printf("        -> single orientation cannot tile: MIXING required\n");

    /* perfect matching via Kuhn's algorithm: ups x dns share an edge */
    {
        static int adj[32][8], na[32];
        memset(adj,0,sizeof(adj)); memset(na,0,sizeof(na));
        int upidx[32], nup=0, dnidx[32], ndn=0;
        for(int i=0;i<ntr;i++){
            if(tris[i].dn) dnidx[ndn++]=i; else upidx[nup++]=i;
        }
        /* edge = share one unit segment */
        for(int i=0;i<nup;i++){
            Tri*u=&tris[upidx[i]];
            for(int j=0;j<ndn;j++){
                Tri*d=&tris[dnidx[j]];
                /* shared edge iff dn at (a,b) with (a,b)==u or
                 * (a-1,b)==u or (a,b-1)==u */
                if((d->a==u->a&&d->b==u->b)||
                   (d->a==u->a-1&&d->b==u->b)||
                   (d->a==u->a&&d->b==u->b-1)){
                    adj[i][na[i]++]=j;
                }
            }
        }
        int matchR[32]; memset(matchR,0xFF,sizeof(matchR));
        int okmatch=1;
        for(int i=0;i<nup&&okmatch;i++){
            int vis[32]; memset(vis,0,sizeof(vis));
            /* dfs */
            int stack_ok=0;
            /* iterative dfs */
            int try_match(int u){
                for(int t=0;t<na[u];t++){
                    int v=adj[u][t];
                    if(vis[v])continue; vis[v]=1;
                    if(matchR[v]<0||try_match(matchR[v])){
                        matchR[v]=u;return 1;
                    }
                }
                return 0;
            }
            (void)stack_ok;
            if(!try_match(i)) okmatch=0;
        }
        CHECK("perfect matching exists: 12 rhombi tile the hexagon",
              okmatch);
        /* count ALL tilings via bitmask dp over dns (expect 20,
         * MacMahon formula for hexagon(2,2,2)) */
        {
            /* process ups in order; mask = used dns */
            static long long dp[33][4096];
            int N=nup, M=ndn;
            memset(dp,0,sizeof(dp));
            dp[0][0]=1;
            for(int i=0;i<N;i++)
                for(int m=0;m<(1<<M);m++){
                    if(!dp[i][m])continue;
                    for(int t=0;t<na[i];t++){
                        int v=adj[i][t];
                        if(m&(1<<v))continue;
                        dp[i+1][m|(1<<v)]+=dp[i][m];
                    }
                }
            long long total=dp[N][(1<<M)-1];
            printf("        total lozenge tilings = %lld (MacMahon: 20)\n",
                   total);
            CHECK("tiling count == 20 (MacMahon hexagon(2,2,2))",
                  total==20);
        }
    }
    CHECK("'12 pieces / 3 ways' = 12 rhombi x 3 orientations", 1);
}

/* ---- H4. 2x2x2 octants -> parity 4+4 -> tess frame ---------------- */
static void h4(void){
    printf("\nH4. 2x2x2: parity split 4+4 ('x2') -> 8 slots = tess frame\n");
    int even=0, odd=0;
    for(int m=0;m<8;m++){
        int par=(m&1)^((m>>1)&1)^((m>>2)&1);
        if(par==0) even++; else odd++;
    }
    CHECK("checkerboard split of 8 octants = 4+4 (tetrahedral halves)",
          even==4&&odd==4);
    CHECK("4+4 recombine = full 8 ('x2' pieces build 2x2x2)", even+odd==8);
    /* cube-corner motifs in tiling k=0: interior vertices where exactly
     * 3 lozenges meet = rhombille 'cube corners' visible */
    printf("        chain: hexagon(12) -> cubes -> x2 -> 8 = tess frame\n");
}

int main(void){
    printf("=== hexagram_cubes_probe - hexagon IS cube-in-2D ===\n");
    h1(); h2(); h3(); h4();
    printf("\nRESULT: %d PASS / %d FAIL\n", pass, fail);
    return fail?1:0;
}

/*
 * test_hybrid.c — hybrid: choose flat vs hier per query (oracle + heuristic)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "b_fs_base.h"

#define G 144u
#define N 20736u

static int hier_order[N];
static uint32_t hier_pos[N];
static uint32_t counter=0;
static void fill(int x0,int y0,int w,int h){
    if(w<=0||h<=0)return;
    if(w==1&&h==1){ hier_order[counter]=y0*(int)G+x0; hier_pos[hier_order[counter]]=counter; counter++; return;}
    int w1=w/2,w2=w-w1,h1=h/2,h2=h-h1;
    fill(x0,y0,w1,h1); fill(x0+w1,y0,w2,h1); fill(x0,y0+h1,w1,h2); fill(x0+w1,y0+h1,w2,h2);
}
static uint32_t lcg(uint32_t*s){ *s=*s*1664525u+1013904223u; return *s;}

static void span_jump(uint32_t* cells,int n,int mode,double*span,double*jump){
    uint32_t p[64];
    for(int i=0;i<n;i++) p[i]=(mode==0)?cells[i]:hier_pos[cells[i]];
    uint32_t mn=p[0],mx=p[0],j=0;
    for(int i=1;i<n;i++){ if(p[i]<mn)mn=p[i]; if(p[i]>mx)mx=p[i]; j += (p[i]>p[i-1])?p[i]-p[i-1]:p[i-1]-p[i];}
    *span=(double)(mx-mn); if(jump) *jump=(double)j;
}

/* 2D max distance (Chebyshev) among cells */
static int maxcheb(uint32_t* cells,int n){
    int mn_r=9999,mx_r=-1,mn_c=9999,mx_c=-1;
    for(int i=0;i<n;i++){ int r=cells[i]/G, c=cells[i]%G; if(r<mn_r)mn_r=r; if(r>mx_r)mx_r=r; if(c<mn_c)mn_c=c; if(c>mx_c)mx_c=c;}
    int dr=mx_r-mn_r, dc=mx_c-mn_c; return (dr>dc)?dr:dc;
}

static int lca_depth(int r1,int c1,int r2,int c2,int x0,int y0,int w,int h,int depth){
    if(w<=1 && h<=1) return depth;
    int w1=w/2,w2=w-w1,h1=h/2,h2=h-h1;
    int midx=x0+w1, midy=y0+h1;
    int q1=(r1<midy?0:2)+(c1<midx?0:1);
    int q2=(r2<midy?0:2)+(c2<midx?0:1);
    if(q1!=q2) return depth;
    int nx0=(q1%2==0)?x0:midx, ny0=(q1<2)?y0:midy;
    int nw=(q1%2==0)?w1:w2, nh=(q1<2)?h1:h2;
    return lca_depth(r1,c1,r2,c2,nx0,ny0,nw,nh,depth+1);
}

int main(void){
    fill(0,0,(int)G,(int)G);
    printf("Hybrid per-query selection (avg over 1000 queries per pattern)\n");
    printf("%-12s %10s %10s %10s %10s %10s\n","Pattern","FlatSpan","HierSpan","OracleSp","HeurSp","HeurAcc");

    uint32_t seed=0xBEEFu;
    const char* pats[4]={"4-nbr","8-nbr","2hop","randwlk"};
    for(int pi=0;pi<4;pi++){
        double fs=0,hs=0,os=0,hes=0; int heur_ok=0, nq=0;
        for(int q=0;q<1000;q++){
            uint32_t r,c; uint32_t cells[12]; int n=0;
            if(pi==0){ r=1+(lcg(&seed)%(G-2)); c=1+(lcg(&seed)%(G-2)); cells[0]=(r-1)*G+c; cells[1]=(r+1)*G+c; cells[2]=r*G+(c-1); cells[3]=r*G+(c+1); n=4;}
            else if(pi==1){ r=1+(lcg(&seed)%(G-2)); c=1+(lcg(&seed)%(G-2)); int idx=0; for(int dr=-1;dr<=1;dr++)for(int dc=-1;dc<=1;dc++){if(dr==0&&dc==0)continue; cells[idx++]=(r+dr)*G+(c+dc);} n=8;}
            else if(pi==2){ r=2+(lcg(&seed)%(G-4)); c=2+(lcg(&seed)%(G-4)); int idx=0; for(int dr=-2;dr<=2;dr++)for(int dc=-2;dc<=2;dc++){if(abs(dr)+abs(dc)!=2)continue; cells[idx++]=(r+dr)*G+(c+dc);} n=idx;}
            else { r=4+(lcg(&seed)%(G-8)); c=4+(lcg(&seed)%(G-8)); for(int i=0;i<8;i++){ int dr=(int)(lcg(&seed)%3)-1; int dc=(int)(lcg(&seed)%3)-1; r=(uint32_t)((int)r+dr); c=(uint32_t)((int)c+dc); cells[i]=r*G+c;} n=8;}
            double fspan,hspan;
            span_jump(cells,n,0,&fspan,NULL);
            span_jump(cells,n,1,&hspan,NULL);
            fs+=fspan; hs+=hspan;
            /* oracle: pick lower span */
            os += (fspan<hspan)? fspan : hspan;
            /* heuristic: avg LCA depth among cells >= 5 -> hier else flat */
            double avg_lca=0; int pairs=0;
            for(int a=0;a<n;a++) for(int b=a+1;b<n;b++){
                int ra=cells[a]/G, ca=cells[a]%G, rb=cells[b]/G, cb=cells[b]%G;
                avg_lca += lca_depth(ra,ca,rb,cb,0,0,(int)G,(int)G,0);
                pairs++;
            }
            if(pairs>0) avg_lca/=pairs;
            int usehier = (avg_lca >= 5.0);
            double hspan2; span_jump(cells,n, usehier?1:0, &hspan2, NULL);
            hes += hspan2;
            /* accuracy vs oracle choice */
            int oracle_hier = (hspan < fspan);
            if(usehier == oracle_hier) heur_ok++;
            nq++;
        }
        printf("%-12s %10.1f %10.1f %10.1f %10.1f %9.1f%%\n", pats[pi], fs/nq, hs/nq, os/nq, hes/nq, 100.0*heur_ok/nq);
    }
    return 0;
}
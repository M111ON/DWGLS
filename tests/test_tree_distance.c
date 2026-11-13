/*
 * test_tree_distance.c — Linguistic-tree principle: LCA depth as locality metric
 * Compare how well Flat vs Hierarchical linear layout tracks tree distance.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
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

/* LCA depth in quadtree subdivision: root depth 0 */
static int lca_depth(int r1,int c1,int r2,int c2,int x0,int y0,int w,int h,int depth){
    if(w<=1 && h<=1) return depth;
    int w1=w/2,w2=w-w1,h1=h/2,h2=h-h1;
    /* determine child index 0..3 for each point */
    int midx = x0 + w1, midy = y0 + h1;
    int q1 = (r1 < midy ? 0 : 2) + (c1 < midx ? 0 : 1);
    int q2 = (r2 < midy ? 0 : 2) + (c2 < midx ? 0 : 1);
    if(q1 != q2) return depth;
    /* descend into chosen child bounds */
    int nx0 = (q1%2==0)? x0 : midx;
    int ny0 = (q1<2)? y0 : midy;
    int nw = (q1%2==0)? w1 : w2;
    int nh = (q1<2)? h1 : h2;
    return lca_depth(r1,c1,r2,c2,nx0,ny0,nw,nh,depth+1);
}

int main(void){
    fill(0,0,(int)G,(int)G);
    /* max depth */
    int maxdepth = lca_depth(0,0,0,0,0,0,(int)G,(int)G,0);
    /* sample pairs */
    #define S 2000
    static double td[S], fd[S], hd[S];
    uint32_t seed=0xABCDu;
    for(int i=0;i<S;i++){
        uint32_t a=lcg(&seed)%N, b=lcg(&seed)%N;
        int r1=a/G, c1=a%G, r2=b/G, c2=b%G;
        int ld = lca_depth(r1,c1,r2,c2,0,0,(int)G,(int)G,0);
        int tree_dist = maxdepth - ld;
        td[i]= (double)tree_dist;
        fd[i]= (double)( (a>b)? (int)(a-b):(int)(b-a) );
        hd[i]= (double)( (hier_pos[a]>hier_pos[b])? (int)(hier_pos[a]-hier_pos[b]) : (int)(hier_pos[b]-hier_pos[a]) );
    }
    /* Pearson correlation */
    /* flat */
    double mean_t=0,mean_f=0,mean_h=0;
    for(int i=0;i<S;i++){ mean_t+=td[i]; mean_f+=fd[i]; mean_h+=hd[i]; }
    mean_t/=S; mean_f/=S; mean_h/=S;
    double cov_tf=0,var_t=0,var_f=0,var_h=0,cov_th=0;
    for(int i=0;i<S;i++){
        double dt=td[i]-mean_t, df=fd[i]-mean_f, dh=hd[i]-mean_h;
        cov_tf+=dt*df; var_t+=dt*dt; var_f+=df*df;
        cov_th+=dt*dh; var_h+=dh*dh;
    }
    double r_flat = cov_tf / sqrt(var_t*var_f);
    double r_hier = cov_th / sqrt(var_t*var_h);
    printf("Tree-distance vs linear-distance correlation (Pearson r), %d sample pairs:\n", S);
    printf("  Flat Grid : r = %.4f  (1.0 = perfect locality tracking)\n", r_flat);
    printf("  Hier Tree : r = %.4f\n", r_hier);
    printf("  Max quadtree depth: %d\n\n", maxdepth);

    /* neighbor query: average LCA depth for 4 neighbors (higher = closer in tree) */
    double avg_lca_flat=0, avg_lca_hier=0; (void)avg_lca_flat;(void)avg_lca_hier;
    /* LCA depth is layout-independent; report for neighbors */
    double sum=0; int nq=0;
    for(uint32_t q=0;q<1000;q++){
        /* deterministic interior center */
        uint32_t seed2 = q*997+1;
        uint32_t r=1+(seed2%(G-2)); uint32_t c=1+((seed2*31)%(G-2));
        uint32_t n[4]={(r-1)*G+c,(r+1)*G+c,r*G+(c-1),r*G+(c+1)};
        for(int i=0;i<4;i++){
            int nr=n[i]/G, nc=n[i]%G;
            sum += lca_depth((int)r,(int)c,nr,nc,0,0,(int)G,(int)G,0);
            nq++;
        }
    }
    printf("Neighbor LCA depth (higher=closer in tree): avg = %.2f / %d max\n", sum/nq, maxdepth);
    return 0;
}
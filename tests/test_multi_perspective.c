/*
 * test_multi_perspective.c — multiple query perspectives on flat vs hier mapping
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

/* compute span & jump for a set of cell ids for a mapping (0=flat,1=hier) */
static void metrics(uint32_t* cells, int ncells, int mode, double* span, double* jump){
    uint32_t p[64];
    for(int i=0;i<ncells;i++){
        uint32_t cell=cells[i];
        p[i] = (mode==0)? cell : hier_pos[cell];
    }
    uint32_t mn=p[0],mx=p[0]; uint32_t j=0;
    for(int i=1;i<ncells;i++){ if(p[i]<mn)mn=p[i]; if(p[i]>mx)mx=p[i]; j += (p[i]>p[i-1])? p[i]-p[i-1] : p[i-1]-p[i]; }
    *span = (double)(mx-mn); *jump=(double)j;
}

int main(void){
    fill(0,0,(int)G,(int)G);
    printf("Multi-perspective locality: Flat vs Hier (avg over 1000 queries)\n");
    printf("%-22s %12s %12s %12s %12s\n", "Pattern", "Flat span", "Hier span", "Flat jump", "Hier jump");

    uint32_t seed=0xBEEFu;
    /* 1. 4-neighbor */
    {
        double fs=0,hs=0,fj=0,hj=0;
        for(int q=0;q<1000;q++){
            uint32_t r=1+(lcg(&seed)%(G-2)); uint32_t c=1+(lcg(&seed)%(G-2));
            uint32_t cells[4]={(r-1)*G+c,(r+1)*G+c,r*G+(c-1),r*G+(c+1)};
            double s,j;
            metrics(cells,4,0,&s,&j); fs+=s; fj+=j;
            metrics(cells,4,1,&s,&j); hs+=s; hj+=j;
        }
        printf("%-22s %12.1f %12.1f %12.1f %12.1f\n","4-neighbor",fs/1000,hs/1000,fj/1000,hj/1000);
    }
    /* 2. 8-neighbor (king) */
    {
        double fs=0,hs=0,fj=0,hj=0;
        for(int q=0;q<1000;q++){
            uint32_t r=1+(lcg(&seed)%(G-2)); uint32_t c=1+(lcg(&seed)%(G-2));
            uint32_t cells[8];
            int idx=0;
            for(int dr=-1;dr<=1;dr++) for(int dc=-1;dc<=1;dc++){
                if(dr==0&&dc==0) continue;
                cells[idx++] = (r+dr)*G + (c+dc);
            }
            double s,j;
            metrics(cells,8,0,&s,&j); fs+=s; fj+=j;
            metrics(cells,8,1,&s,&j); hs+=s; hj+=j;
        }
        printf("%-22s %12.1f %12.1f %12.1f %12.1f\n","8-neighbor",fs/1000,hs/1000,fj/1000,hj/1000);
    }
    /* 3. 2-hop Manhattan radius 2 (ring) */
    {
        double fs=0,hs=0,fj=0,hj=0;
        for(int q=0;q<1000;q++){
            uint32_t r=2+(lcg(&seed)%(G-4)); uint32_t c=2+(lcg(&seed)%(G-4));
            uint32_t cells[12]; int idx=0;
            /* points at Manhattan distance 2 */
            for(int dr=-2;dr<=2;dr++) for(int dc=-2;dc<=2;dc++){
                if(abs(dr)+abs(dc)!=2) continue;
                cells[idx++] = (r+dr)*G + (c+dc);
            }
            double s,j;
            metrics(cells,idx,0,&s,&j); fs+=s; fj+=j;
            metrics(cells,idx,1,&s,&j); hs+=s; hj+=j;
        }
        printf("%-22s %12.1f %12.1f %12.1f %12.1f\n","2-hop ring",fs/1000,hs/1000,fj/1000,hj/1000);
    }
    /* 4. Random walk 8 steps */
    {
        double fs=0,hs=0,fj=0,hj=0;
        for(int q=0;q<1000;q++){
            uint32_t r=4+(lcg(&seed)%(G-8)); uint32_t c=4+(lcg(&seed)%(G-8));
            uint32_t cells[8];
            for(int i=0;i<8;i++){
                int dr = (int)(lcg(&seed)%3)-1;
                int dc = (int)(lcg(&seed)%3)-1;
                r = (uint32_t)((int)r+dr); c=(uint32_t)((int)c+dc);
                cells[i]=r*G+c;
            }
            double s,j;
            metrics(cells,8,0,&s,&j); fs+=s; fj+=j;
            metrics(cells,8,1,&s,&j); hs+=s; hj+=j;
        }
        printf("%-22s %12.1f %12.1f %12.1f %12.1f\n","rand-walk 8",fs/1000,hs/1000,fj/1000,hj/1000);
    }
    return 0;
}
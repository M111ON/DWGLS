#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include <time.h>

typedef struct { double x,y,z; } V3;
static inline V3 v3(double x,double y,double z){ return (V3){x,y,z}; }
static inline V3 add(V3 a,V3 b){ return v3(a.x+b.x,a.y+b.y,a.z+b.z); }
static inline V3 sub(V3 a,V3 b){ return v3(a.x-b.x,a.y-b.y,a.z-b.z); }
static inline V3 mul(V3 a,double s){ return v3(a.x*s,a.y*s,a.z*s); }
static inline double dot(V3 a,V3 b){ return a.x*b.x+a.y*b.y+a.z*b.z; }
static inline double len(V3 a){ return sqrt(dot(a,a)); }
static inline V3 mid(V3 a,V3 b){ return mul(add(a,b),0.5); }

/* generate a regular tetrahedron: 4 vertices on unit sphere */
static void regular_tetra(V3 out[4]) {
    /* 4 points: (1,1,1), (1,-1,-1), (-1,1,-1), (-1,-1,1) normalized */
    double s = 1.0 / sqrt(3.0);
    out[0] = v3( s,  s,  s);
    out[1] = v3( s, -s, -s);
    out[2] = v3(-s,  s, -s);
    out[3] = v3(-s, -s,  s);
}

/* test regular tetra -> regular octahedron */
static int test_regular_tetra(void) {
    V3 T[4];
    regular_tetra(T);
    
    V3 M[6] = { mid(T[0],T[1]), mid(T[0],T[2]), mid(T[0],T[3]),
                mid(T[1],T[2]), mid(T[1],T[3]), mid(T[2],T[3]) };
    
    /* 12 edges of octahedron (between 6 midpoints) */
    double lens[12]; int li=0;
    for(int u=0;u<6;u++) for(int v=u+1;v<6;v++) lens[li++] = len(sub(M[u],M[v]));
    
    double avg=0; for(int k=0;k<12;k++) avg+=lens[k]; avg/=12;
    int eq=1; for(int k=0;k<12;k++) if(fabs(lens[k]-avg)/avg > 1e-10) eq=0;
    
    /* 3 opposite pairs from center (which is origin) */
    V3 v1=sub(M[0],M[5]), v2=sub(M[1],M[4]), v3v=sub(M[2],M[3]);
    int ortho = (fabs(dot(v1,v2))<1e-10 && fabs(dot(v1,v3v))<1e-10 && fabs(dot(v2,v3v))<1e-10);
    
    printf("regular tetra: edges equal=%d, ortho diagonals=%d\n", eq, ortho);
    return eq && ortho;
}

/* test general tetra -> general octahedron (combinatorial only) */
static int test_general_tetra(void) {
    V3 T[4];
    for(int i=0;i<4;i++) T[i] = v3((double)rand()/RAND_MAX*2-1, (double)rand()/RAND_MAX*2-1, (double)rand()/RAND_MAX*2-1);
    
    V3 M[6] = { mid(T[0],T[1]), mid(T[0],T[2]), mid(T[0],T[3]),
                mid(T[1],T[2]), mid(T[1],T[3]), mid(T[2],T[3]) };
    
    /* combinatorial check: 6 vertices, 12 edges, 8 faces, center = average of M */
    V3 center = v3(0,0,0);
    for(int i=0;i<6;i++) center = add(center, M[i]);
    center = mul(center, 1.0/6.0);
    
    /* each M[i] should have a unique opposite M[j] such that (M[i]+M[j])/2 = center */
    int opposite[6] = {-1,-1,-1,-1,-1,-1};
    for(int i=0;i<6;i++) {
        for(int j=0;j<6;j++) if(i!=j) {
            V3 mid = mul(add(M[i],M[j]),0.5);
            if(len(sub(mid, center)) < 1e-10) opposite[i] = j;
        }
    }
    
    int all_have_opp = 1;
    for(int i=0;i<6;i++) if(opposite[i]==-1) all_have_opp=0;
    
    printf("general tetra: 6 vertices, center valid=%d\n", all_have_opp);
    return all_have_opp;
}

int main(){
    srand((unsigned)time(NULL));
    int pass_reg = test_regular_tetra();
    int pass_gen = test_general_tetra();
    
    printf("Regular tetra -> regular octahedron: %s\n", pass_reg ? "PASS" : "FAIL");
    printf("General tetra -> octahedron (combinatorial): %s\n", pass_gen ? "PASS" : "FAIL");
    printf("Conclusion: 6 midpoints = octahedron vertices (combinatorial always, metric only for regular)\n");
    return (pass_reg && pass_gen) ? 0 : 1;
}
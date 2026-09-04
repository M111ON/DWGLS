#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#define FIELD 20736
#define MAX_FACTS 256

static const int PLATONIC[] = {4,6,8,12,18,24,36,48,72,96,144,288,576};
#define N_PLATONIC (sizeof(PLATONIC)/sizeof(PLATONIC[0]))

typedef struct { int a,b,c; } Triple;
typedef struct { int a,b,c,d; } Quad;
typedef struct { int a,b,c; double cache_eff; int align144; int align1152; } PipeRank;

static Triple triples[MAX_FACTS];
static Quad quads[MAX_FACTS];
static PipeRank pipes[MAX_FACTS];
static int n_triples=0, n_quads=0, n_pipes=0;

static int is_platonic(int v) {
    for (int i=0; i<(int)N_PLATONIC; i++)
        if (PLATONIC[i]==v) return 1;
    return 0;
}

static void find_triples(void) {
    for (int i=0; i<(int)N_PLATONIC; i++) {
        int a = PLATONIC[i];
        if (FIELD % a != 0) continue;
        int rem1 = FIELD / a;
        for (int j=i; j<(int)N_PLATONIC; j++) {
            int b = PLATONIC[j];
            if (rem1 % b != 0) continue;
            int c = rem1 / b;
            if (!is_platonic(c)) continue;
            if (c < b) continue;
            if (n_triples < MAX_FACTS) {
                triples[n_triples++] = (Triple){a,b,c};
            }
        }
    }
}

static void find_quads(void) {
    for (int i=0; i<(int)N_PLATONIC; i++) {
        int a = PLATONIC[i];
        if (FIELD % a != 0) continue;
        int r1 = FIELD / a;
        for (int j=i; j<(int)N_PLATONIC; j++) {
            int b = PLATONIC[j];
            if (r1 % b != 0) continue;
            int r2 = r1 / b;
            for (int k=j; k<(int)N_PLATONIC; k++) {
                int c = PLATONIC[k];
                if (r2 % c != 0) continue;
                int d = r2 / c;
                if (!is_platonic(d)) continue;
                if (d < c) continue;
                if (n_quads < MAX_FACTS) {
                    quads[n_quads++] = (Quad){a,b,c,d};
                }
            }
        }
    }
}

static void compute_pipes(void) {
    for (int i=0; i<n_triples; i++) {
        int a = triples[i].a, b = triples[i].b, c = triples[i].c;
        double pipe_size = (double)a;
        double cache_eff = pipe_size * sizeof(float) / 65536.0;
        int align144 = (a % 144 == 0) ? 1 : 0;
        int align1152 = (a % 1152 == 0) ? 1 : 0;
        if (n_pipes < MAX_FACTS) {
            pipes[n_pipes++] = (PipeRank){a,b,c,cache_eff,align144,align1152};
        }
    }
}

static int cmp_pipe(const void *x, const void *y) {
    const PipeRank *p1 = (const PipeRank*)x;
    const PipeRank *p2 = (const PipeRank*)y;
    if (p2->cache_eff > p1->cache_eff) return 1;
    if (p2->cache_eff < p1->cache_eff) return -1;
    return 0;
}

static int cmp_triple(const void *x, const void *y) {
    const Triple *t1 = (const Triple*)x;
    const Triple *t2 = (const Triple*)y;
    int prod1 = t1->a + t1->b + t1->c;
    int prod2 = t2->a + t2->b + t2->c;
    return prod1 - prod2;
}

static int cmp_quad(const void *x, const void *y) {
    const Quad *q1 = (const Quad*)x;
    const Quad *q2 = (const Quad*)y;
    int s1 = q1->a + q1->b + q1->c + q1->d;
    int s2 = q2->a + q2->b + q2->c + q2->d;
    return s1 - s2;
}

int main(void) {
    printf("=== DWGLS Factorization Pipes ===\n");
    printf("Field = %d\n\n", FIELD);

    find_triples();
    find_quads();
    compute_pipes();

    /* Sort triples and quads by sum (smaller = more balanced) */
    qsort(triples, n_triples, sizeof(Triple), cmp_triple);
    qsort(quads, n_quads, sizeof(Quad), cmp_quad);
    qsort(pipes, n_pipes, sizeof(PipeRank), cmp_pipe);

    /* 1. Triples */
    printf("--- Platonic Triples (a*b*c = 20736) ---\n");
    printf("  %-12s %-12s %-12s  %-10s\n", "a", "b", "c", "a*b*c");
    for (int i=0; i<n_triples; i++) {
        int prod = triples[i].a * triples[i].b * triples[i].c;
        printf("  %-12d %-12d %-12d  %-10d\n",
               triples[i].a, triples[i].b, triples[i].c, prod);
    }
    printf("  Total: %d triples\n\n", n_triples);

    /* 2. Quads */
    printf("--- Platonic Quads (a*b*c*d = 20736) ---\n");
    printf("  %-8s %-8s %-8s %-8s  %-10s\n", "a", "b", "c", "d", "product");
    for (int i=0; i<n_quads; i++) {
        int prod = quads[i].a * quads[i].b * quads[i].c * quads[i].d;
        printf("  %-8d %-8d %-8d %-8d  %-10d\n",
               quads[i].a, quads[i].b, quads[i].c, quads[i].d, prod);
    }
    printf("  Total: %d quads\n\n", n_quads);

    /* 3. Pipe ranking */
    printf("--- Pipe Ranking (by cache efficiency) ---\n");
    printf("  %-6s %-6s %-8s  %-12s %-14s %-8s %-8s\n",
           "pipe_a", "b", "b*c", "cache_eff%", "access_bits", "144-ok", "1152-ok");
    for (int i=0; i<n_pipes; i++) {
        double cache_pct = pipes[i].cache_eff * 100.0;
        double access = log2((double)(pipes[i].b * pipes[i].c)) + log2((double)pipes[i].a);
        printf("  %-6d %-6d %-8d  %-12.2f %-14.2f %-8s %-8s\n",
               pipes[i].a, pipes[i].b, pipes[i].b * pipes[i].c,
               cache_pct, access,
               pipes[i].align144 ? "YES" : "no",
               pipes[i].align1152 ? "YES" : "no");
    }
    printf("\n");

    /* 4. Tesseract packing */
    printf("--- Tesseract Packing ---\n");
    printf("  1 tesseract = 8 cubes x 144 = 1152 slots\n");
    printf("  18 tesseracts = 18 x 1152 = %d\n", 18*1152);
    printf("  Verify: 18 * 8 * 144 = %d %s\n\n",
           18*8*144, (18*8*144 == FIELD) ? "OK" : "FAIL");

    printf("  Factorizations aligned with tesseract boundary (a %% 144 == 0):\n");
    for (int i=0; i<n_triples; i++) {
        if (triples[i].a % 144 == 0) {
            printf("    %d x %d x %d  (pipe=%d = %d x 144)\n",
                   triples[i].a, triples[i].b, triples[i].c,
                   triples[i].a, triples[i].a / 144);
        }
    }
    printf("\n  Factorizations aligned with full tesseract (a %% 1152 == 0):\n");
    for (int i=0; i<n_triples; i++) {
        if (triples[i].a % 1152 == 0) {
            printf("    %d x %d x %d  (pipe=%d = %d x 1152)\n",
                   triples[i].a, triples[i].b, triples[i].c,
                   triples[i].a, triples[i].a / 1152);
        }
    }
    printf("\n");

    /* 5. E8 lattice */
    printf("--- E8 Lattice Analysis ---\n");
    printf("  E8 root system: 240 roots\n");
    printf("  20736 / 240 = %.1f %s\n", 20736.0/240.0,
           (20736 % 240 == 0) ? "(evenly divides)" : "(NOT even)");
    printf("  20736 / 24 = %d  (24 origins)\n", 20736 / 24);
    printf("  20736 / 240 = %d remainder %d\n", 20736 / 240, 20736 % 240);
    printf("  E8 = 24-gon x 10  =>  20736 / 24 = %d positions per hub\n", 20736/24);
    printf("  Interpretation: 24 hubs, each with %d positions\n", 20736/24);
    printf("  E8 root count 240 maps to hub=%d x factor=%d (24 x 10)\n\n",
           24, 240/24);

    /* 6. Special factorization */
    printf("--- DWGLS Core Factorization ---\n");
    printf("  20736 = 4 tetra x 3 axes x 8 pairs x 6 x 6\n");
    printf("        = 4x3 x 8 x 6x6\n");
    printf("        = 12 x 8 x 36\n");
    printf("        = 96 x 36 x 6\n");
    printf("  Verify: 96 * 36 * 6 = %d %s\n", 96*36*6,
           (96*36*6 == FIELD) ? "OK" : "FAIL");
    printf("  Verify: 12 * 8 * 216 = %d %s\n", 12*8*216,
           (12*8*216 == FIELD) ? "OK" : "FAIL");
    printf("  Note: 12x8=96, 96x36=3456, 3456x6=20736\n");

    printf("\n=== Done ===\n");
    return 0;
}

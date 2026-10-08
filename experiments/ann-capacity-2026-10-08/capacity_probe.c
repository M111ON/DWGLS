/* tools/capacity_probe.c — how far does the ANN index scale, and what does
 * more data cost in speed? (owner 2026-10-08)
 *
 * The shipped serve has two costs and one hard cap:
 *   (a) PARSE  — lz_index_load reads kv_index.jsonl once (cached by mtime).
 *   (b) SCAN   — every query scores the candidate set; budget auto-raises to
 *                N, so the walk is effectively O(N) at exact recall.
 *   (c) CAP    — LZ_ANCH_MAXK = 8 buckets. The anchor route can only cover the
 *                index with 8 buckets, so routing quality is fixed while N grows.
 *
 * This probe measures (a) and (b) as N grows, using SYNTHETIC entries of the
 * real dimension, so the curve is clean (no I/O noise from the live index).
 * It answers: at N entries, how long to parse, and how long to scan?
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -o build/capacity_probe tools/capacity_probe.c -lm
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define DIM 384          /* real embed dim of the live index */

typedef struct { float *vec; double norm; } Ent;

static double now_s(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* one query = cosine against every entry (the worst case the budget allows).
 * The result is written through a volatile sink so the compiler cannot elide
 * the loop (a plain return value was optimized away -> scan_ms read 0.000). */
static volatile double g_sink;
static void scan_once(const Ent *e, int n, const float *q, double qn) {
    double best = -2;
    for (int i = 0; i < n; i++) {
        double dot = 0;
        const float *v = e[i].vec;
        for (int d = 0; d < DIM; d++) dot += (double)q[d] * v[d];
        double sc = (e[i].norm > 0 && qn > 0) ? dot / (qn * e[i].norm) : -1;
        if (sc > best) best = sc;
    }
    g_sink = best;   /* observable side effect */
}

int main(void) {
    const int sizes[] = { 2881, 5762, 11524, 23048, 46096, 92192 };
    const int ns = (int)(sizeof sizes / sizeof sizes[0]);
    const int REPS = 20;

    static float q[DIM];
    srand(7);
    for (int d = 0; d < DIM; d++) q[d] = (float)(rand() % 1000) / 1000.0f;
    double qn = 0; for (int d = 0; d < DIM; d++) qn += (double)q[d]*q[d]; qn = sqrt(qn);

    printf("== ANN capacity / scaling probe (dim=%d) ==\n\n", DIM);
    printf("%8s %10s %10s %10s %10s %12s\n",
           "N", "parse_ms", "scan_ms", "us/item", "mem_MB", "MB_per_1k");

    for (int s = 0; s < ns; s++) {
        int n = sizes[s];

        /* (a) ALLOCATE + FILL: the parse/memory cost, synthetic but real dim */
        double t0 = now_s();
        Ent *e = (Ent *)malloc((size_t)n * sizeof(Ent));
        if (!e) { printf("  (alloc fail at N=%d)\n", n); break; }
        for (int i = 0; i < n; i++) {
            e[i].vec = (float *)malloc(DIM * sizeof(float));
            for (int d = 0; d < DIM; d++) e[i].vec[d] = (float)(rand() % 1000) / 1000.0f;
            double nn = 0; for (int d = 0; d < DIM; d++) nn += (double)e[i].vec[d]*e[i].vec[d];
            e[i].norm = sqrt(nn);
        }
        double t1 = now_s();

        /* (b) SCAN: time a query, take the min (least noise) */
        double best = 1e9;
        for (int r = 0; r < REPS; r++) {
            double a = now_s();
            scan_once(e, n, q, qn);
            double b = now_s();
            if (b - a < best) best = b - a;
        }

        double parse_ms = (t1 - t0) * 1e3;
        double scan_ms = best * 1e3;
        double us_item = best * 1e6 / n;
        /* resident estimate: vec floats + norm + pointer + per-entry overhead */
        double mem_mb = ((double)n * (DIM * sizeof(float) + sizeof(double) + sizeof(float*))) / (1024.0*1024.0);

        printf("%8d %10.1f %10.3f %10.3f %10.2f %12.3f\n",
               n, parse_ms, scan_ms, us_item, mem_mb, mem_mb * 1000.0 / n);

        for (int i = 0; i < n; i++) free(e[i].vec);
        free(e);
    }

    printf("\nRead: scan_ms grows ~linearly with N (O(N)); us/item is ~flat.\n");
    printf("So more data = proportionally slower per query, until the anchor\n");
    printf("route prunes. LZ_ANCH_MAXK=8 bounds that pruning.\n");
    return 0;
}

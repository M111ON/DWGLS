/*
 * test_bfs_hnsw.c — Deterministic HNSW on b_fs verification & benchmark
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <x86intrin.h>
#include "b_fs_base.h"
#include "bfs_hnsw.h"

#define K 10
#define Q 200
static volatile uint32_t g_sink = 0;

static uint32_t lcg(uint32_t*s){ *s=*s*1664525u+1013904223u; return *s;}

/* brute force top-k addresses by dist to query */
static void brute_topk(uint32_t q, uint32_t *out) {
    /* simple: collect all, sort first k */

    /* partial selection: sort by dist using insertion for k? N large; use qsort on struct */
    typedef struct { uint32_t id, d; } Item;
    static Item arr[20736];
    for (uint32_t i = 0; i < 20736; i++) { arr[i].id = i; arr[i].d = bfs_hnsw_dist(i, q); }
    /* partial sort: find k smallest via simple O(N*k) */
    for (int k = 0; k < K; k++) {
        int best = k;
        for (int i = k+1; i < 20736; i++) if (arr[i].d < arr[best].d) best = i;
        Item tmp = arr[k]; arr[k] = arr[best]; arr[best] = tmp;
    }
    for (int i = 0; i < K; i++) out[i] = arr[i].id;
}

/* quadtree branch-and-bound top-k */
static uint32_t qt_addr[K], qt_dist[K];
static int qt_n = 0;
static int qt_worst(void){
    int w=0; for(int i=1;i<qt_n;i++) if(qt_dist[i]>qt_dist[w]) w=i; return (qt_n?qt_dist[w]:0xFFFFFFFFu);
}
static void qt_insert(uint32_t addr, uint32_t d){
    if (qt_n < K) {
        qt_addr[qt_n]=addr; qt_dist[qt_n]=d; qt_n++;
    } else {
        int w=0; for(int i=1;i<qt_n;i++) if(qt_dist[i]>qt_dist[w]) w=i;
        if (d < qt_dist[w]) { qt_addr[w]=addr; qt_dist[w]=d; }
    }
}
static void qt_search(int x0,int y0,int w,int h,int qr,int qc){
    if (w <= 0 || h <= 0) return;
    int dx=0,dy=0;
    if (qr < x0) dx = x0 - qr; else if (qr >= x0 + w) dx = qr - (x0 + w - 1);
    if (qc < y0) dy = y0 - qc; else if (qc >= y0 + h) dy = qc - (y0 + h - 1);
    uint32_t lb = (uint32_t)(dx*dx + dy*dy);
    if (qt_n == K && lb >= (uint32_t)qt_worst()) return;
    if (w == 1 && h == 1) {
        uint32_t addr = (uint32_t)(y0 * 144 + x0);
        uint32_t d = bfs_hnsw_dist(addr, (uint32_t)(qr*144+qc));
        qt_insert(addr, d);
        return;
    }
    int w1 = w/2, w2 = w - w1;
    int h1 = h/2, h2 = h - h1;
    qt_search(x0, y0, w1, h1, qr, qc);
    qt_search(x0 + w1, y0, w2, h1, qr, qc);
    qt_search(x0, y0 + h1, w1, h2, qr, qc);
    qt_search(x0 + w1, y0 + h1, w2, h2, qr, qc);
}

static void quadtree_topk(uint32_t q, uint32_t *out) {
    qt_n = 0;
    int qr = q / 144, qc = q % 144;
    qt_search(0, 0, 144, 144, qr, qc);
    /* sort qt by dist */
    for (int a = 1; a < qt_n; a++) {
        uint32_t da=qt_dist[a], aa=qt_addr[a]; int b=a-1;
        while (b>=0 && qt_dist[b]>da){ qt_dist[b+1]=qt_dist[b]; qt_addr[b+1]=qt_addr[b]; b--; }
        qt_dist[b+1]=da; qt_addr[b+1]=aa;
    }
    for (int i = 0; i < K && i < qt_n; i++) out[i] = qt_addr[i];
}

int main(void) {
    printf("=== Deterministic HNSW (b_fs) ===\n"); fflush(stdout);
    BfsHnswGraph g;
    memset(&g, 0, sizeof(g));
    printf("Building graph...\n"); fflush(stdout);
    if (bfs_hnsw_build(&g) != 0) { printf("BUILD FAIL\n"); return 1; }
    printf("Sizes: L0=%u L1=%u L2=%u\n", g.sizes[0], g.sizes[1], g.sizes[2]); fflush(stdout);

    /* Correctness gate */
    int ok = 1;
    if (g.sizes[0] != 20736 || g.sizes[1] != 1296 || g.sizes[2] != 144) ok = 0;
    for (int L = 0; L < 3; L++) {
        for (uint32_t i = 0; i < g.sizes[L]; i++) {
            if (g.layers[L][i].count == 0) { ok = 0; break; }
            for (int e = 0; e < g.layers[L][i].count; e++) {
                uint32_t ed = g.layers[L][i].edges[e];
                if (ed >= 20736) { ok = 0; break; }
            }
            if (!ok) break;
        }
        if (!ok) break;
    }
    printf("Correctness gate: %s\n", ok ? "PASS" : "FAIL"); fflush(stdout);

    /* Recall gate */
    uint32_t seed = 0x777u;
    double recall_sum = 0;
    int nq = Q;
    for (int q = 0; q < nq; q++) {
        uint32_t query = lcg(&seed) % 20736;
        uint32_t res[K];
        uint32_t bf[K];
        bfs_hnsw_search(&g, query, K, res);
        brute_topk(query, bf);
        /* intersection */
        int hit = 0;
        for (int a = 0; a < K; a++)
            for (int b = 0; b < K; b++)
                if (res[a] == bf[b]) hit++;
        recall_sum += (double)hit / K;
    }
    double recall = recall_sum / nq;
    printf("Recall@%d (avg over %d queries): %.4f %s\n", K, nq, recall, (recall >= 0.95) ? "PASS" : "FAIL"); fflush(stdout); fflush(stdout);

    /* Performance gate */
    unsigned int aux = 0;
    uint32_t queries[1000];
    seed = 0x999u;
    for (int i = 0; i < 1000; i++) queries[i] = lcg(&seed) % 20736;

    /* Flat scan */
    uint64_t t0 = __rdtscp(&aux);
    for (int it = 0; it < 5; it++) {
        for (int i = 0; i < 1000; i++) {
            uint32_t q = queries[i];
            /* keep top-k min dist */
            uint32_t best_d[K], best_id[K];
            for (int k = 0; k < K; k++) { best_d[k] = 0xFFFFFFFFu; best_id[k] = 0; }
            for (uint32_t a = 0; a < 20736; a++) {
                uint32_t d = bfs_hnsw_dist(a, q);
                /* insert if better than worst */
                int w = 0;
                for (int k = 1; k < K; k++) if (best_d[k] > best_d[w]) w = k;
                if (d < best_d[w]) { best_d[w] = d; best_id[w] = a; }
            }
            g_sink += best_id[0];
        }
    }
    uint64_t t1 = __rdtscp(&aux);
    double cyc_flat = (double)(t1 - t0) / (1000.0 * 5);

    t0 = __rdtscp(&aux);
    for (int it = 0; it < 3; it++) {
        for (int i = 0; i < 200; i++) {
            uint32_t out[K];
            quadtree_topk(queries[i], out);
            g_sink += out[0];
        }
    }
    t1 = __rdtscp(&aux);
    double cyc_qt = (double)(t1 - t0) / (200.0 * 3);

    /* DHNSW */
    t0 = __rdtscp(&aux);
    uint64_t vis_sum = 0;
    for (int it = 0; it < 5; it++) {
        for (int i = 0; i < 1000; i++) {
            uint32_t out[K];
            bfs_hnsw_search(&g, queries[i], K, out);
            g_sink += out[0];
            vis_sum += hnsw_last_visited;
        }
    }
    t1 = __rdtscp(&aux);
    double cyc_hnsw = (double)(t1 - t0) / (1000.0 * 5);


    printf("Latency (cycles/query, top-%d):\n", K);
    printf("  Flat scan : %.1f\n", cyc_flat);
    printf("  Quadtree  : %.1f\n", cyc_qt);
    printf("  DHNSW     : %.1f\n", cyc_hnsw);
    printf("  DHNSW vs Flat speedup: %.2fx %s\n", cyc_flat / cyc_hnsw,
           (cyc_flat / cyc_hnsw >= 5.0) ? "PASS" : "FAIL");

    bfs_hnsw_free(&g);
    return 0;
}
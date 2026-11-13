/*
 * test_spatial_locality.c — b_fs spatial locality benchmark
 * Flat Grid (row-major) vs Hierarchical Tree (DFS quadtree) address mapping
 * over 20736 slots (144x144). Measures neighbor-query latency + index distance.
 * Integer-only, zero allocation (static buffers), lossless bijection proof.
 * BUILD: gcc -O2 -Wall -Wextra -I. -Icore -o build/test_spatial_locality tests/test_spatial_locality.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <x86intrin.h>
#include "b_fs_base.h"

#define G  144u          /* grid side */
#define N  20736u        /* G*G */
#define Q  1000u         /* random queries */
#define IT 50u           /* iterations per mode */

static int hier_order[N];
static uint32_t hier_pos[N];
static volatile int32_t g_sink = 0;

/* DFS quadtree subdivision fill (integer splits only). */
static uint32_t counter = 0;
static void fill_order(int x0, int y0, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (w == 1 && h == 1) {
        hier_order[counter] = (uint32_t)(y0 * (int)G + x0);
        hier_pos[hier_order[counter]] = counter;
        counter++;
        return;
    }
    int w1 = w / 2, w2 = w - w1;
    int h1 = h / 2, h2 = h - h1;
    fill_order(x0,       y0,       w1, h1);
    fill_order(x0 + w1, y0,       w2, h1);
    fill_order(x0,       y0 + h1, w1, h2);
    fill_order(x0 + w1, y0 + h1, w2, h2);
}

static uint32_t lcg(uint32_t *s) {
    *s = *s * 1664525u + 1013904223u;
    return *s;
}

int main(void) {
    printf("=== Spatial Locality Benchmark (b_fs) ===\n");
    printf("Space: %u (%ux%u), queries=%u, iters=%u\n\n", N, G, G, Q, IT);

    /* Build hierarchical mapping. */
    counter = 0;
    fill_order(0, 0, (int)G, (int)G);

    /* Bijection proof: Flat (identity) + Hier. */
    /* Flat: trivially bijective; verify seen coverage. */
    {
        static uint8_t seen[N];
        memset(seen, 0, N);
        int ok = 1;
        for (uint32_t i = 0; i < N; i++) {
            /* flat cell id == i */
            if (seen[i]) { ok = 0; break; }
            seen[i] = 1;
        }
        /* all seen */
        for (uint32_t i = 0; i < N; i++) if (!seen[i]) { ok = 0; break; }
        printf("Bijection Flat Grid: %s\n", ok ? "PASS" : "FAIL");
    }
    /* Hier: verify order is permutation + inverse roundtrip. */
    {
        static uint8_t seen[N];
        memset(seen, 0, N);
        int ok = 1;
        for (uint32_t k = 0; k < N; k++) {
            uint32_t cell = hier_order[k];
            if (cell >= N || seen[cell]) { ok = 0; break; }
            seen[cell] = 1;
            if (hier_pos[cell] != k) { ok = 0; break; }
        }
        for (uint32_t i = 0; i < N; i++) if (!seen[i]) { ok = 0; break; }
        /* roundtrip cell->pos->order */
        for (uint32_t cell = 0; cell < N && ok; cell++) {
            if (hier_order[hier_pos[cell]] != (int)cell) { ok = 0; break; }
        }
        printf("Bijection Hierarchical (DFS quadtree): %s (%u unique)\n\n",
               ok ? "PASS" : "FAIL", counter);
    }

    /* Data buffers (static, zero-alloc hot path). */
    static int32_t mem_flat[N];
    static int32_t mem_hier[N];
    uint32_t seed = 0xC0FFEEu;
    for (uint32_t i = 0; i < N; i++) {
        uint32_t v = lcg(&seed);
        mem_flat[i] = (int32_t)v;
        mem_hier[i] = (int32_t)(v ^ 0x5A5A5A5Au);
    }
    /* put same logical value at mapped phys for both (value at cell). */
    for (uint32_t cell = 0; cell < N; cell++) {
        uint32_t v = (uint32_t)cell * 2654435761u;
        mem_flat[cell] = (int32_t)v;
        mem_hier[hier_pos[cell]] = (int32_t)v;
    }

    /* Random interior centers. */
    static uint32_t centers[Q];
    seed = 0xBEEFu;
    for (uint32_t q = 0; q < Q; q++) {
        uint32_t r = 1 + (lcg(&seed) % (G - 2));
        uint32_t c = 1 + (lcg(&seed) % (G - 2));
        centers[q] = r * G + c;
    }

    /* Index distance metrics. */
    double flat_span = 0, flat_jump = 0;
    double hier_span = 0, hier_jump = 0;
    for (uint32_t q = 0; q < Q; q++) {
        uint32_t cid = centers[q];
        uint32_t r = cid / G, c = cid % G;
        /* neighbor cell ids */
        uint32_t n[4] = { (r-1)*G + c, (r+1)*G + c, r*G + (c-1), r*G + (c+1) };
        /* flat phys */
        uint32_t f[4] = { n[0], n[1], n[2], n[3] };
        uint32_t fmin = f[0], fmax = f[0];
        uint32_t fj = 0;
        for (int i = 1; i < 4; i++) {
            if (f[i] < fmin) fmin = f[i];
            if (f[i] > fmax) fmax = f[i];
            if (i > 0) {
                uint32_t d = f[i] > f[i-1] ? f[i]-f[i-1] : f[i-1]-f[i];
                fj += d;
            }
        }
        flat_span += (double)(fmax - fmin);
        flat_jump += (double)fj;

        /* hier phys */
        uint32_t h[4] = { hier_pos[n[0]], hier_pos[n[1]], hier_pos[n[2]], hier_pos[n[3]] };
        uint32_t hmin = h[0], hmax = h[0];
        uint32_t hj = 0;
        for (int i = 1; i < 4; i++) {
            if (h[i] < hmin) hmin = h[i];
            if (h[i] > hmax) hmax = h[i];
            if (i > 0) {
                uint32_t d = h[i] > h[i-1] ? h[i]-h[i-1] : h[i-1]-h[i];
                hj += d;
            }
        }
        hier_span += (double)(hmax - hmin);
        hier_jump += (double)hj;
    }
    printf("Index distance (lower=better locality), avg over %u queries:\n", Q);
    printf("  Flat Grid : avg span = %.1f slots, avg jump = %.1f slots\n", flat_span/Q, flat_jump/Q);
    printf("  Hier Tree : avg span = %.1f slots, avg jump = %.1f slots\n\n", hier_span/Q, hier_jump/Q);

    /* Warm-up. */
    for (uint32_t q = 0; q < Q; q++) {
        uint32_t cid = centers[q];
        uint32_t r = cid / G, c = cid % G;
        uint32_t n0 = (r-1)*G + c, n1 = (r+1)*G + c, n2 = r*G + (c-1), n3 = r*G + (c+1);
        int32_t acc = mem_flat[n0]+mem_flat[n1]+mem_flat[n2]+mem_flat[n3];
        g_sink = acc;
    }

    unsigned int aux = 0;

    /* Flat benchmark. */
    uint64_t t0 = __rdtscp(&aux);
    for (uint32_t it = 0; it < IT; it++) {
        for (uint32_t q = 0; q < Q; q++) {
            uint32_t cid = centers[q];
            uint32_t r = cid / G, c = cid % G;
            uint32_t p0 = (r-1)*G + c;
            uint32_t p1 = (r+1)*G + c;
            uint32_t p2 = r*G + (c-1);
            uint32_t p3 = r*G + (c+1);
            int32_t acc = mem_flat[p0] + mem_flat[p1] + mem_flat[p2] + mem_flat[p3];
            g_sink = acc;
        }
    }
    uint64_t t1 = __rdtscp(&aux);
    double cycles_flat = (double)(t1 - t0) / (double)(Q * IT);

    /* Hier benchmark. */
    t0 = __rdtscp(&aux);
    for (uint32_t it = 0; it < IT; it++) {
        for (uint32_t q = 0; q < Q; q++) {
            uint32_t cid = centers[q];
            uint32_t r = cid / G, c = cid % G;
            uint32_t n0 = (r-1)*G + c, n1 = (r+1)*G + c, n2 = r*G + (c-1), n3 = r*G + (c+1);
            uint32_t p0 = hier_pos[n0], p1 = hier_pos[n1], p2 = hier_pos[n2], p3 = hier_pos[n3];
            int32_t acc = mem_hier[p0] + mem_hier[p1] + mem_hier[p2] + mem_hier[p3];
            g_sink = acc;
        }
    }
    t1 = __rdtscp(&aux);
    double cycles_hier = (double)(t1 - t0) / (double)(Q * IT);

    printf("Latency (cycles/query, lower=better):\n");
    printf("  Flat Grid : %.2f cycles\n", cycles_flat);
    printf("  Hier Tree : %.2f cycles\n", cycles_hier);
    if (cycles_flat > 0)
        printf("  Ratio (Hier/Flat): %.3fx  (%s)\n",
               cycles_hier / cycles_flat,
               (cycles_hier < cycles_flat) ? "Hier faster" : "Flat faster");
    printf("\nNote: run under `perf stat` for hardware cache-miss counters.\n");
    printf("Sink (anti-DCE): %d\n", g_sink);
    return 0;
}
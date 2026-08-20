/*
 * test_centroid_walk_probe.c — EXPERIMENT: walk on triangle centroids
 * ═══════════════════════════════════════════════════════════════════
 *
 * User hypothesis (2026-08-21):
 *   "เดินบน centroid ของ triangle — centroid = จุดตัดของมุม 30° สองเส้น
 *    = dual transform เป็น dodecahedron (face center ของ icosa = vertex
 *    ของ dodeca)"
 *
 * Field: node_id = hi·81 + lo, hi ∈ [0,256), lo ∈ [0,81), 20736 = 256·81
 *   mixed-radix 3 axes:
 *     axis 0 = lo ± 1   (k-axis, stride 1,  ปรับ 3-ladder)
 *     axis 1 = lo ± 9   (j-axis, stride 9)
 *     axis 2 = hi ± 1   (i-axis, stride 81, เปลี่ยน 4-ladder)
 *
 * Centroid (face center) of a cell = th_cell_anchor(node, aperture, depth)
 *   = canonical address of the cell containing node (tri_hex_tess.h).
 *
 * The walk: cross edge d (stride s_d) then SNAP to the centroid of the
 * destination cell — "จอดลง snap เข้า 3 หน้า 3 แกนเสมอ".
 *
 * MEASURES (no expected values baked in — report what is true):
 *   M1 node ↔ (i,j,k) 3-axis decomposition is a bijection on 20736
 *   M2 straight-line walk along each axis: orbit size (coverage)
 *   M3 reversibility: walk +s then -s returns home (all nodes)
 *   M4 parity: does crossing an edge flip triangle up/down (node parity)?
 *   M5 centroid snap: anchor(n) is stable under depth, walk anchors coverage
 *   M6 "3 faces / 3 axes" — from any centroid, the 3 crossing edges go to
 *     3 DISTINCT centroids (the 3 adjacent faces)
 *
 * BUILD: gcc -O2 -Wall -o build/test_centroid_walk_probe tests/test_centroid_walk_probe.c
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../core/tri_hex_tess.h"

#define STRIDE0 1u
#define STRIDE1 9u
#define STRIDE2 81u
#define N_AXES  3u

static const uint32_t STRIDE[N_AXES] = {STRIDE0, STRIDE1, STRIDE2};

static void axis_decomp(uint32_t node, uint32_t out[N_AXES]) {
    out[0] = node % 9u;          /* k ∈ [0,9)   */
    out[1] = (node / 9u) % 9u;   /* j ∈ [0,9)   */
    out[2] = node / 81u;         /* i ∈ [0,256) */
}

static uint32_t axis_rebuild(const uint32_t v[N_AXES]) {
    return v[2] * 81u + v[1] * 9u + v[0];
}

static void measure_bijection(void) {
    printf("── M1 node ↔ (i,j,k) bijection\n");
    uint8_t *seen = calloc(GEO_FULL, 1);
    uint32_t ok = 1;
    for (uint32_t n = 0; n < GEO_FULL; n++) {
        uint32_t v[N_AXES]; axis_decomp(n, v);
        if (v[2] >= 256u || v[1] >= 9u || v[0] >= 9u) { ok = 0; break; }
        uint32_t back = axis_rebuild(v);
        if (back != n) { ok = 0; break; }
        if (seen[back]) { ok = 0; break; }
        seen[back] = 1;
    }
    uint32_t cnt = 0;
    for (uint32_t i = 0; i < GEO_FULL; i++) cnt += seen[i];
    printf("  decompose→rebuild roundtrip: %s, distinct=%u of %u\n",
           ok ? "YES" : "NO", cnt, GEO_FULL);
    free(seen);
}

static void measure_orbit(void) {
    printf("── M2 straight-line walk along each axis (orbit = coverage)\n");
    for (uint32_t d = 0; d < N_AXES; d++) {
        uint32_t stride = STRIDE[d];
        uint32_t g = stride, b = GEO_FULL;
        while (b) { uint32_t t = g % b; g = b; b = t; }
        uint32_t orbit = GEO_FULL / g;
        printf("  axis %u stride=%-4u orbit=%u (%.2f%%), keyframes=%u\n",
               d, stride, orbit, 100.0 * orbit / GEO_FULL,
               (GEO_FULL + orbit - 1) / orbit);
    }
}

static void measure_reversible(void) {
    printf("── M3 reversibility: +stride then −stride returns home\n");
    uint32_t ok = 1, bad = 0;
    for (uint32_t d = 0; d < N_AXES; d++) {
        for (uint32_t n = 0; n < GEO_FULL; n++) {
            uint32_t fwd = (n + STRIDE[d]) % GEO_FULL;
            uint32_t home = (fwd + GEO_FULL - STRIDE[d]) % GEO_FULL;
            if (home != n) { ok = 0; bad++; if (bad > 3) break; }
        }
    }
    printf("  all nodes × 3 axes: %s\n", ok ? "YES" : "NO");
}

static void measure_parity(void) {
    printf("── M4 parity: crossing edge d flips node parity?\n");
    for (uint32_t d = 0; d < N_AXES; d++) {
        uint32_t s = STRIDE[d];
        uint32_t flip = 0, stay = 0;
        for (uint32_t n = 0; n < GEO_FULL; n++) {
            uint32_t fwd = (n + s) % GEO_FULL;
            if ((fwd & 1u) != (n & 1u)) flip++; else stay++;
        }
        printf("  axis %u stride=%u: parity-flip %u (%.2f%%), stay %u\n",
               d, s, flip, 100.0 * flip / GEO_FULL, stay);
    }
}

/* centroid of cell = anchor of the 4-ladder cell at depth (canonical address) */
static uint32_t centroid_of(uint32_t node, uint32_t depth) {
    return th_cell_anchor(node, 4u, depth);
}

static void measure_anchor_stability(void) {
    printf("── M5 centroid snap (th_cell_anchor, aperture 4)\n");
    for (uint32_t depth = 0; depth <= 4; depth++) {
        uint32_t cells = th_cell_count(4u, depth);
        uint32_t size = th_cell_size(4u, depth);
        /* anchor is idempotent: anchor(anchor(n)) == anchor(n) */
        uint32_t idem = 1, within = 1;
        uint32_t n = 0;
        for (uint32_t k = 0; k < 1000; k++) {
            n = (n * 1664525u + 1013904223u) & 0x7FFFFFFFu;
            n %= GEO_FULL;
            uint32_t a = centroid_of(n, depth);
            uint32_t a2 = centroid_of(a, depth);
            if (a2 != a) idem = 0;
            uint32_t v[N_AXES]; axis_decomp(a, v);
            if (v[2] >= 256u) within = 0;
        }
        printf("  depth=%u cells=%u size=%u  anchor idempotent=%s\n",
               depth, cells, size, idem ? "YES" : "NO");
        (void)within;
    }
}

/* M6: from a centroid, crossing the 3 edges → 3 DISTINCT centroids? */
static void measure_3faces(void) {
    printf("── M6 from a centroid, 3 crossing edges → 3 distinct centroids\n");
    uint32_t ok = 1, samples = 0;
    for (uint32_t n = 0; n < GEO_FULL; n += 997u) {
        uint32_t c = centroid_of(n, 1u);
        uint32_t a[3];
        for (uint32_t d = 0; d < 3; d++)
            a[d] = centroid_of((c + STRIDE[d]) % GEO_FULL, 1u);
        if (a[0] == a[1] || a[0] == a[2] || a[1] == a[2]) ok = 0;
        samples++;
    }
    printf("  samples=%u all-distinct=%s\n", samples, ok ? "YES" : "NO");
}

int main(void) {
    printf("test_centroid_walk_probe — walk on triangle centroids\n");
    printf("  field: node = i·81 + j·9 + k (i<256, j<9, k<9) = %u\n", GEO_FULL);
    measure_bijection();
    measure_orbit();
    measure_reversible();
    measure_parity();
    measure_anchor_stability();
    measure_3faces();
    printf("── done (measurements, no pass/fail)\n");
    return 0;
}
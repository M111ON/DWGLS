/* geo_gidpith_weave.h — 4D weave on the gidpith vertex graph.
 *
 * The weave geo_jump builds by hand (Hilbert walls + Peano vertical) is
 * derived here from the actual graph: 384 vertices, 768 edges, degree 4
 * (from Klitzing gidpith coordinates; edge table in gidpith_edges.inc).
 * gidpith is vertex-transitive, so apex 0 stands for any vertex.
 *
 * wall-walk (Hilbert analog): greedy traversal confined to one BFS shell.
 * spine-walk (Peano analog): shortest path apex -> antipode, monotone in
 * shell order — orientation + placement direction, same role as Peano.
 *
 * Header-only, integer-only. Tables are static const.
 */
#ifndef GEO_GIDPITH_WEAVE_H
#define GEO_GIDPITH_WEAVE_H

#include <stdint.h>

#define HJ_VERTS  384u
#define HJ_EDGES  768u
#define HJ_DEGREE 4u

#include "gidpith_edges.inc"

/* neighbor table: HJ_NB[v*4+k]. Built once by hj_build(). */
static uint16_t HJ_NB[HJ_VERTS * HJ_DEGREE];
static int HJ_BUILT = 0;

static inline void hj_build(void) {
    if (HJ_BUILT) return;
    uint8_t cnt[HJ_VERTS] = {0};
    for (uint32_t e = 0; e < HJ_EDGES; e++) {
        uint16_t a = HJ_EDGE_A[e], b = HJ_EDGE_B[e];
        HJ_NB[a * HJ_DEGREE + cnt[a]++] = b;
        HJ_NB[b * HJ_DEGREE + cnt[b]++] = a;
    }
    HJ_BUILT = 1;
}

/* BFS shells from apex: shell[v] = graph distance. Returns shell count. */
static inline uint32_t hj_shells(uint16_t apex, uint8_t *shell) {
    hj_build();
    for (uint32_t i = 0; i < HJ_VERTS; i++) shell[i] = 0xFF;
    uint16_t queue[HJ_VERTS];
    uint32_t head = 0, tail = 0, maxshell = 0;
    shell[apex] = 0;
    queue[tail++] = apex;
    while (head < tail) {
        uint16_t v = queue[head++];
        for (uint32_t k = 0; k < HJ_DEGREE; k++) {
            uint16_t w = HJ_NB[v * HJ_DEGREE + k];
            if (shell[w] == 0xFF) {
                shell[w] = (uint8_t)(shell[v] + 1u);
                if (shell[w] > maxshell) maxshell = shell[w];
                queue[tail++] = w;
            }
        }
    }
    return maxshell + 1u;
}

/* spine: shortest path apex -> target. Writes verts into path[], returns length. */
static inline uint32_t hj_spine(uint16_t apex, uint16_t target, uint16_t *path) {
    hj_build();
    uint8_t shell[HJ_VERTS];
    int16_t parent[HJ_VERTS];
    for (uint32_t i = 0; i < HJ_VERTS; i++) { shell[i] = 0xFF; parent[i] = -1; }
    uint16_t queue[HJ_VERTS];
    uint32_t head = 0, tail = 0;
    shell[apex] = 0;
    queue[tail++] = apex;
    while (head < tail) {
        uint16_t v = queue[head++];
        if (v == target) break;
        for (uint32_t k = 0; k < HJ_DEGREE; k++) {
            uint16_t w = HJ_NB[v * HJ_DEGREE + k];
            if (shell[w] == 0xFF) {
                shell[w] = (uint8_t)(shell[v] + 1u);
                parent[w] = (int16_t)v;
                queue[tail++] = w;
            }
        }
    }
    if (shell[target] == 0xFF) return 0;
    uint32_t len = (uint32_t)shell[target] + 1u;
    uint16_t v = target;
    for (uint32_t i = len; i-- > 0;) {
        path[i] = v;
        v = (uint16_t)parent[v];
    }
    return len;
}

/* wall-walk: greedy no-repeat traversal confined to a shell band
 * {slow, slow+1}. A single shell provably contains no edges (the graph is
 * bipartite by BFS shell), so a shell pair is the minimal floor-like unit —
 * the 4D analog of Hilbert's 2D maze floor. Writes verts, returns count. */
static inline uint32_t hj_wall(uint16_t start, const uint8_t *shell,
                               uint8_t slow, uint16_t *walk) {
    hj_build();
    uint8_t seen[HJ_VERTS] = {0};
    uint16_t v = start;
    uint32_t n = 0;
    for (;;) {
        walk[n++] = v;
        seen[v] = 1;
        uint16_t next = 0xFFFF;
        for (uint32_t k = 0; k < HJ_DEGREE; k++) {
            uint16_t w = HJ_NB[v * HJ_DEGREE + k];
            if (!seen[w] && (shell[w] == slow || shell[w] == slow + 1u)) {
                next = w;
                break;
            }
        }
        if (next == 0xFFFF) return n;
        v = next;
    }
}

#endif /* GEO_GIDPITH_WEAVE_H */

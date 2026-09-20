/* ═══════════════════════════════════════════════════════════════════════════
 * geo_net_walk.h — fold-net engine: committed graph only, zero geometry
 * ═══════════════════════════════════════════════════════════════════════════
 * A net = faces + half-edge gluings. Solid/dashed = glued/open.
 * Walks, counts, Euler run on the COMMITTED (glued) graph — 3D positions
 * never enter. Unmapped (dashed/gray) simply isn't in the pairing table.
 *
 * Convention: face f has sides[f] edges (cyclic order). Corner c of f =
 * start-vertex of edge c. Glued pair (f,i)<->(g,j) runs opposite ways:
 * corner (f,i) unites with corner (g,(j+1)%sides[g]).
 * Open half-edge: peer_f < 0 (boundary/dashed — excluded from walks).
 *
 * Closed solid -> Euler V-E+F = 2. Open net (disk) -> 1.
 * Header-only, int-only, no malloc (caller scratch for union-find).
 * ═══════════════════════════════════════════════════════════════════════════ */
#ifndef GEO_NET_WALK_H
#define GEO_NET_WALK_H

#include <stdint.h>

/* reciprocity: every glued pair points back. Returns -1 ok, else bad half-edge. */
static inline int32_t nw_reciprocal(uint32_t F, uint32_t SMAX,
                                    const uint32_t *sides,
                                    const int32_t *peer_f, const int32_t *peer_e) {
    for (uint32_t f = 0; f < F; f++)
        for (uint32_t i = 0; i < sides[f]; i++) {
            int32_t g = peer_f[f * SMAX + i];
            if (g < 0) continue;
            int32_t j = peer_e[f * SMAX + i];
            if (g >= (int32_t)F || j < 0 || j >= (int32_t)sides[(uint32_t)g]) return (int32_t)(f * SMAX + i);
            if (peer_f[(uint32_t)g * SMAX + (uint32_t)j] != (int32_t)f ||
                peer_e[(uint32_t)g * SMAX + (uint32_t)j] != (int32_t)i)
                return (int32_t)(f * SMAX + i);
        }
    return -1;
}

/* BFS over glued edges from face 0. visited[f]=1. Returns count (connectedness). */
static inline uint32_t nw_walk(uint32_t F, uint32_t SMAX,
                               const uint32_t *sides,
                               const int32_t *peer_f,
                               uint8_t *visited) {
    for (uint32_t f = 0; f < F; f++) visited[f] = 0;
    uint32_t stack[512];
    uint32_t top = 0, count = 0;
    stack[top++] = 0;
    visited[0] = 1;
    while (top > 0) {
        uint32_t f = stack[--top];
        count++;
        for (uint32_t i = 0; i < sides[f]; i++) {
            int32_t g = peer_f[f * SMAX + i];
            if (g < 0 || visited[(uint32_t)g]) continue;
            if (top >= 512) continue;
            visited[(uint32_t)g] = 1;
            stack[top++] = (uint32_t)g;
        }
    }
    return count;
}

/* union-find root with path halving (inline helper) */
static inline uint32_t nw_find(int32_t *par, uint32_t x) {
    while (par[x] != (int32_t)x) { par[x] = par[par[x]]; x = (uint32_t)par[x]; }
    return x;
}

/* vertex count: union corners across gluings, count sets.
 * parent scratch: int32_t[F*SMAX]. Returns corner sets = V. */
static inline uint32_t nw_count_vertices(uint32_t F, uint32_t SMAX,
                                         const uint32_t *sides,
                                         const int32_t *peer_f, const int32_t *peer_e,
                                         int32_t *parent) {
    uint32_t N = 0;
    for (uint32_t f = 0; f < F; f++) N += sides[f];
    for (uint32_t x = 0; x < N; x++) parent[x] = (int32_t)x;
    /* corner id: prefix-sum offset per face (faces may differ in sides) */
    for (uint32_t f = 0; f < F; f++) {
        uint32_t off_f = 0;
        for (uint32_t k = 0; k < f; k++) off_f += sides[k];
        for (uint32_t i = 0; i < sides[f]; i++) {
            int32_t g = peer_f[f * SMAX + i];
            if (g < 0) continue;
            uint32_t j = (uint32_t)peer_e[f * SMAX + i];
            uint32_t off_g = 0;
            for (uint32_t k = 0; k < (uint32_t)g; k++) off_g += sides[k];
            uint32_t a = off_f + i;
            uint32_t b = off_g + (j + 1u) % sides[(uint32_t)g];
            uint32_t ra = nw_find(parent, a), rb = nw_find(parent, b);
            if (ra != rb) parent[ra] = (int32_t)rb;
        }
    }
    uint32_t sets = 0;
    for (uint32_t x = 0; x < N; x++)
        if (parent[x] == (int32_t)x) sets++;
    return sets;
}

/* edge count: E = total half-edges - glued pairs (each pair merges 2 into 1).
 * Returns packed: E in low 24 bits, open boundary halves in high 8? No —
 * keep simple: returns E; boundary counted separately below. */
static inline uint32_t nw_count_edges(uint32_t F, uint32_t SMAX,
                                      const uint32_t *sides, const int32_t *peer_f) {
    uint32_t halves = 0, pairs = 0;
    for (uint32_t f = 0; f < F; f++)
        for (uint32_t i = 0; i < sides[f]; i++) {
            halves++;
            if (peer_f[f * SMAX + i] >= 0) pairs++;
        }
    return halves - pairs / 2u;
}

/* fan cartridge builder: center b-gon + 3 a-gon petals on edges 0,1,2.
 * sides[4] = {b,a,a,a}; 3 petals regardless of b (they attach to the
 * seed sides, not the shaft sides). Fills peer tables with -1 then the
 * 6 glued halves. No config — gluing is fixed by the principle. */
static inline void nw_build_fan(uint32_t a, uint32_t b, uint32_t SMAX,
                                uint32_t *sides, int32_t *peer_f, int32_t *peer_e) {
    sides[0] = b; sides[1] = a; sides[2] = a; sides[3] = a;
    for (uint32_t f = 0; f < 4; f++)
        for (uint32_t i = 0; i < SMAX; i++) { peer_f[f * SMAX + i] = -1; peer_e[f * SMAX + i] = 0; }
    peer_f[0 * SMAX + 0] = 1; peer_e[0 * SMAX + 0] = 0;
    peer_f[0 * SMAX + 1] = 2; peer_e[0 * SMAX + 1] = 0;
    peer_f[0 * SMAX + 2] = 3; peer_e[0 * SMAX + 2] = 0;
    peer_f[1 * SMAX + 0] = 0; peer_e[1 * SMAX + 0] = 0;
    peer_f[2 * SMAX + 0] = 0; peer_e[2 * SMAX + 0] = 1;
    peer_f[3 * SMAX + 0] = 0; peer_e[3 * SMAX + 0] = 2;
}

/* open (dashed/boundary) half-edges — excluded from walks by construction */
static inline uint32_t nw_count_open(uint32_t F, uint32_t SMAX,
                                     const uint32_t *sides, const int32_t *peer_f) {
    uint32_t n = 0;
    for (uint32_t f = 0; f < F; f++)
        for (uint32_t i = 0; i < sides[f]; i++)
            if (peer_f[f * SMAX + i] < 0) n++;
    return n;
}

#endif /* GEO_NET_WALK_H */

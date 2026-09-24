/* mm_route.h — major/minor routing over multiverse nodes.
 *
 * MAJOR (taproot): chains exit(n)→entry(n+1). Node-id adjacency lives in
 * the caller's graph (anchor ids); this header builds the chain links and
 * verifies pin discipline. Major never touches non-pinned cells.
 *
 * MINOR (rootlets): cross through minor-usable cells (unpinned) of
 * edge-adjacent nodes, or bridge near-duplicate layers (|dlayer| ≤ 1) at
 * the same climate slide. Collision-freedom by construction: minors can
 * never occupy a pinned cell, majors never leave theirs.
 *
 * Header-only, std-only (deps: mv_node.h). No separate build.
 */
#ifndef MM_ROUTE_H
#define MM_ROUTE_H

#include <stdint.h>
#include "mv_node.h"

/* major link: extend chain with next node (same layer+slide travel together;
 * caller guarantees id-adjacency in its graph). 0=ok, -1=bad args. */
static inline int mm_major_step(const MVNode *cur, uint32_t next_id, uint8_t next_orient,
                                MVNode *out) {
    if (!cur || !out) return -1;
    return mv_init(out, next_id, cur->layer, cur->slide, next_orient);
}

/* minor-usable mask: bit c set iff cell c is free for minors. */
static inline uint16_t mm_minor_free(const MVNode *n) {
    if (!n) return 0;
    uint16_t m = 0x1FFu;
    m &= (uint16_t) ~(1u << n->entry);
    m &= (uint16_t) ~(1u << n->exit);
    return m;
}

/* minor crossing a(cell_a) → b(cell_b): both cells minor-usable,
 * edge-adjacent, same slide, layers equal or ±1 (near-duplicate bridge).
 * 1=valid, 0=refused. */
static inline int mm_minor_cross(const MVNode *a, uint8_t cell_a,
                                 const MVNode *b, uint8_t cell_b) {
    if (!a || !b || cell_a > 8 || cell_b > 8) return 0;
    if (mv_pinned(a, cell_a) != 0 || mv_pinned(b, cell_b) != 0) return 0;
    if (!mv_edge_adj(cell_a, cell_b)) return 0;
    if (a->slide != b->slide) return 0;
    int dl = (int)a->layer - (int)b->layer;
    if (dl < -1 || dl > 1) return 0;
    return 1;
}

/* minor path inside/between nodes: every step a valid minor cross
 * (same-node steps model cell_a→cell_b walk with a==b node). 1=valid. */
static inline int mm_minor_path(const MVNode *nodes, const uint8_t *cells, int n) {
    if (!nodes || !cells || n <= 0) return 0;
    for (int i = 0; i < n; i++)
        if (mv_pinned(&nodes[i], cells[i]) != 0) return 0;
    for (int i = 0; i + 1 < n; i++) {
        if (!mv_edge_adj(cells[i], cells[i + 1])) return 0;
        if (nodes[i].slide != nodes[i + 1].slide) return 0;
        int dl = (int)nodes[i].layer - (int)nodes[i + 1].layer;
        if (dl < -1 || dl > 1) return 0;
    }
    return 1;
}

/* chain pin audit: every node has distinct entry/exit (construction
 * invariant) — returns count audited, -1 on null. */
static inline int mm_chain_audit(const MVNode *chain, int n) {
    if (!chain || n <= 0) return -1;
    for (int i = 0; i < n; i++)
        if (chain[i].entry == chain[i].exit || chain[i].entry > 8 || chain[i].exit > 8)
            return -1;
    return n;
}

#endif /* MM_ROUTE_H */

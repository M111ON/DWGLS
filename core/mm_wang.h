/* mm_wang.h — Wang-tile gating for minor crossings.
 *
 * Every node exposes 4 edge colors (N/E/S/W), derived deterministically
 * from (node, layer, slide): color = (node*5 + layer*11 + (slide%144)*7
 * + edge*3) % 8. A boundary crossing is OPEN by default iff the two
 * facing colors match (classic Wang rule); dead/meaningless paths stay
 * shut. Shut gates open ON DEMAND via an explicit override table
 * (fixed cap, single-session; per-fs mutex if threaded).
 *
 * Depends: mv_node.h (cells), mm_route.h (cross predicate).
 * Header-only. No separate build.
 */
#ifndef MM_WANG_H
#define MM_WANG_H

#include <stdint.h>
#include "mv_node.h"
#include "mm_route.h"

#define MMW_COLORS 8u
#define MMW_EDGE_N 0u
#define MMW_EDGE_E 1u
#define MMW_EDGE_S 2u
#define MMW_EDGE_W 3u
#define MMW_GATES 256u

/* deterministic edge color (placement rule, documented above). */
static inline uint8_t mmw_color(uint32_t node, uint16_t layer, uint32_t slide, uint8_t edge) {
    if (edge > 3) return 0xFF;
    return (uint8_t)((node * 5u + (uint32_t)layer * 11u + (slide % 144u) * 7u + (uint32_t)edge * 3u) % MMW_COLORS);
}

/* edge direction of a co-located crossing ca→cb (N/E/S/W), -1 if not adjacent. */
static inline int mmw_cross_edge(uint8_t ca, uint8_t cb) {
    if (!mv_edge_adj(ca, cb)) return -1;
    int dx = (int)(cb % 3) - (int)(ca % 3);
    int dy = (int)(cb / 3) - (int)(ca / 3);
    if (dx == 1) return MMW_EDGE_E;
    if (dx == -1) return MMW_EDGE_W;
    if (dy == 1) return MMW_EDGE_S;
    return MMW_EDGE_N;
}

/* opposite edge for the far side of a crossing. */
static inline uint8_t mmw_opp(uint8_t e) { return (uint8_t)(e ^ 2u); } /* N↔S, E↔W */

/* default gate: open iff facing colors match. */
static inline int mmw_default_open(const MVNode *a, uint8_t edge_a,
                                   const MVNode *b, uint8_t edge_b) {
    if (!a || !b || edge_a > 3 || edge_b > 3) return 0;
    return mmw_color(a->node, a->layer, a->slide, edge_a) ==
           mmw_color(b->node, b->layer, b->slide, edge_b);
}

/* override table (ponytail: single-session; key = node+edge). */
typedef struct { uint32_t node; uint8_t edge; int8_t open; } MMGate;
static MMGate _mmw_gates[MMW_GATES];
static int _mmw_ngates = 0;

static inline void mmw_override(uint32_t node, uint8_t edge, int open) {
    if (edge > 3) return;
    for (int i = 0; i < _mmw_ngates; i++)
        if (_mmw_gates[i].node == node && _mmw_gates[i].edge == edge) {
            _mmw_gates[i].open = open ? 1 : 0;
            return;
        }
    if (_mmw_ngates < (int)MMW_GATES) {
        _mmw_gates[_mmw_ngates].node = node;
        _mmw_gates[_mmw_ngates].edge = edge;
        _mmw_gates[_mmw_ngates].open = open ? 1 : 0;
        _mmw_ngates++;
    }
}
static inline void mmw_clear(void) { _mmw_ngates = 0; }

/* effective gate: override wins, else color-match default. */
static inline int mmw_open(const MVNode *a, uint8_t edge_a,
                           const MVNode *b, uint8_t edge_b) {
    if (!a || !b) return 0;
    for (int i = 0; i < _mmw_ngates; i++) {
        if (_mmw_gates[i].node == a->node && _mmw_gates[i].edge == edge_a)
            return _mmw_gates[i].open;
        if (_mmw_gates[i].node == b->node && _mmw_gates[i].edge == edge_b)
            return _mmw_gates[i].open;
    }
    return mmw_default_open(a, edge_a, b, edge_b);
}

/* gated minor cross: base predicate + open gate on the crossing edge. */
static inline int mm_minor_cross_gated(const MVNode *a, uint8_t cell_a,
                                       const MVNode *b, uint8_t cell_b) {
    if (!mm_minor_cross(a, cell_a, b, cell_b)) return 0;
    int e = mmw_cross_edge(cell_a, cell_b);
    if (e < 0) return 0;
    return mmw_open(a, (uint8_t)e, b, mmw_opp((uint8_t)e));
}

#endif /* MM_WANG_H */

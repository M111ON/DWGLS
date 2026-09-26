/* geo_gidpith_gates.h — gidpith vertex-layer gate map for hyper_jump towers.
 *
 * Source: Klitzing gidpith.htm (omnitruncated tesseract, x3x3x4x):
 * 38 layer-rows across 24 layer-numbers; layer 1 carries all four cell
 * firsts (entry pole); opposites land per cell, NOT mirrored by layer:
 * girco->8a, toe->15, op->16a, hip->24 (exit pole).
 *
 * Tower binding (pole-to-pole lock): tower0 -> entry row, tower1 ->
 * middle rows, tower2 -> exit row.
 *
 * Header-only, integer-only, stdint only.
 */
#ifndef GEO_GIDPITH_GATES_H
#define GEO_GIDPITH_GATES_H

#include <stdint.h>

#define HJ_GATE_TOE    1u
#define HJ_GATE_HIP    2u
#define HJ_GATE_OP     4u
#define HJ_GATE_GIRCO  8u
#define HJ_GATE_ALL    15u

#define HJ_GATE_ROWS   38u

typedef struct {
    const char *layer;   /* "1".."24" with a/b/c sublayers */
    uint8_t first;       /* cell types appearing first here (bit flags) */
    uint8_t opposite;    /* cell types antipodal here (bit flags) */
} HjGate;

/* Row order = source order (entry pole -> exit pole). */
static const HjGate HJ_GATES[HJ_GATE_ROWS] = {
    {"1",  HJ_GATE_ALL, 0},             /* 0: entry — all four firsts */
    {"2",  0, 0},                       /* 1 */
    {"3a", 0, 0},                       /* 2 */
    {"3b", 0, 0},                       /* 3 */
    {"4",  0, 0},                       /* 4 */
    {"5",  0, 0},                       /* 5 */
    {"6a", 0, 0},                       /* 6 */
    {"6b", 0, 0},                       /* 7 */
    {"7a", 0, 0},                       /* 8 */
    {"7b", 0, 0},                       /* 9 */
    {"8a", 0, HJ_GATE_GIRCO},           /* 10: opposite girco */
    {"8b", 0, 0},                       /* 11 */
    {"8c", 0, 0},                       /* 12 */
    {"9a", 0, 0},                       /* 13 */
    {"9b", 0, 0},                       /* 14 */
    {"9c", 0, 0},                       /* 15 */
    {"10a", 0, 0},                      /* 16 */
    {"10b", 0, 0},                      /* 17 */
    {"11", 0, 0},                       /* 18 */
    {"12a", 0, 0},                      /* 19: transit region start */
    {"12b", 0, 0},                      /* 20 */
    {"13a", 0, 0},                      /* 21 */
    {"13b", 0, 0},                      /* 22: transit region end */
    {"14a", 0, 0},                      /* 23 */
    {"14b", 0, 0},                      /* 24 */
    {"15", 0, HJ_GATE_TOE},             /* 25: opposite toe */
    {"16a", 0, HJ_GATE_OP},             /* 26: opposite op */
    {"16b", 0, 0},                      /* 27 */
    {"17a", 0, 0},                      /* 28 */
    {"17b", 0, 0},                      /* 29 */
    {"18", 0, 0},                       /* 30 */
    {"19a", 0, 0},                      /* 31 */
    {"19b", 0, 0},                      /* 32 */
    {"20", 0, 0},                       /* 33 */
    {"21", 0, 0},                       /* 34 */
    {"22", 0, 0},                       /* 35 */
    {"23", 0, 0},                       /* 36 */
    {"24", 0, HJ_GATE_HIP},             /* 37: exit pole — opposite hip */
};

#define HJ_ROW_ENTRY   0u    /* layer 1 */
#define HJ_ROW_EXIT    37u   /* layer 24 */
#define HJ_ROW_MID_A   19u   /* layer 12a */
#define HJ_ROW_MID_B   22u   /* layer 13b */

/* tower -> gate row (pole-to-pole lock). */
static inline uint32_t hj_tower_gate(uint32_t tower) {
    if (tower == 0u) return HJ_ROW_ENTRY;
    if (tower == 1u) return HJ_ROW_MID_A;
    return HJ_ROW_EXIT;
}

/* find the row carrying the opposite of a cell type; 0xFF if none. */
static inline uint32_t hj_opposite_row(uint8_t cell) {
    for (uint32_t i = 0; i < HJ_GATE_ROWS; i++)
        if (HJ_GATES[i].opposite & cell) return i;
    return 0xFFu;
}

#endif /* GEO_GIDPITH_GATES_H */

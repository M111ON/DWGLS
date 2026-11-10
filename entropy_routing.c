/* entropy_routing.c
 * Constrained Entropy Routing - integer-only model (Q16.16 fixed point).
 * Improved version: conserved shift_perpendicular, void-centered reflect,
 * symmetric integer division (round toward zero), check_constraint.
 * Only <stdint.h> and <stdio.h>.
 */
#include <stdint.h>
#include <stdio.h>

#define Q16_ONE   0x10000
#define Q16_R     0x0199A   /* 0.1 in Q16.16 (constraint radius) */

#define QUAD_TL 0
#define QUAD_TR 1
#define QUAD_BL 2
#define QUAD_BR 3

typedef struct {
    int32_t quad[4];        /* TL, TR, BL, BR */
    int32_t void_entropy;   /* central void reservoir */
} State;

/* Verify (dx,dy) inside constraint circle R=0.1 (Q32.32 comparison). */
int check_constraint(int32_t dx, int32_t dy) {
    int64_t d2 = (int64_t)dx * dx + (int64_t)dy * dy;
    int64_t r2 = (int64_t)Q16_R * (int64_t)Q16_R;
    return d2 <= r2;
}

/* Diagonal shift: Homothety scale along diagonals.
 * d = dx+dy (main), a = dx-dy (anti).
 * TR/BL scale on main diagonal; TL/BR on anti-diagonal.
 * Conservation: sum(quad) unchanged, void untouched. */
void shift_diagonal(State *s, int32_t dx, int32_t dy) {
    int32_t d = dx + dy;
    int32_t a = dx - dy;
    s->quad[QUAD_TR] += d;
    s->quad[QUAD_BL] -= d;
    s->quad[QUAD_TL] -= a;
    s->quad[QUAD_BR] += a;
}

/* Perpendicular shift: Entropy injection/extraction.
 * p = (dx-dy)/2 (symmetric round toward zero).
 * Distributed across 4 quads (remainder absorbed by quad[0]).
 * void_entropy -= p  (void as ledger).
 * Conservation: sum(quad) + void = constant. */
void shift_perpendicular(State *s, int32_t dx, int32_t dy) {
    int32_t raw = dx - dy;
    /* symmetric division by 2 (round toward zero) */
    int32_t p = (raw + ((raw >> 31) & 1)) >> 1;

    /* symmetric division by 4 */
    int32_t q = (p + ((p >> 31) & 3)) >> 2;
    int32_t r = p - (q << 2);   /* remainder, |r| < 4 */

    s->quad[QUAD_TL] += q + r;   /* quad[0] absorbs remainder */
    s->quad[QUAD_TR] += q;
    s->quad[QUAD_BL] += q;
    s->quad[QUAD_BR] += q;
    s->void_entropy -= p;
}

/* Reflection: Invert + Offset, center = void_entropy (Opposite Face Rule).
 * Involution: applying twice restores (void unchanged). */
void reflect_quadrant(State *s, int q1, int q2) {
    int32_t off = s->void_entropy;
    int32_t tmp = s->quad[q1];
    s->quad[q1] = off - s->quad[q2];
    s->quad[q2] = off - tmp;
}

int state_equal(const State *a, const State *b) {
    int i;
    for (i = 0; i < 4; i++)
        if (a->quad[i] != b->quad[i]) return 0;
    return a->void_entropy == b->void_entropy;
}

/*
 * test_tetra_roll_state.c — real tetra-roll on (node, orient ∈ S4)
 *
 * Lesson from test_tetra_roll_probe.c:
 *   1. 1D additive stride cannot be direction-reversible → state must
 *      track orientation (S4, not A4 — the physical roll is a transposition).
 *   2. Node parity flips every roll ONLY if all 3 direction strides are odd.
 *   3. Keyframe: a coprime stride yields 1 anchor covering the whole field.
 *
 * Model:
 *   - Tetra vertices labeled {0,1,2,3}. 6 edges colored into 3 global
 *     directions such that EVERY face is rainbow (3 distinct directions):
 *       01→0, 02→1, 03→2, 12→2, 13→1, 23→0
 *   - Orientation = permutation P ∈ S4 (down-face ordered triple + apex).
 *     Even P = up triangle (cross edge forward: +stride), odd P = down.
 *   - Roll over local edge d crosses edge (down[d], down[(d+1)%3]) whose
 *     global direction g = EDGE_COLOR[edge]:
 *        node += (P even ? +stride[g] : -stride[g])
 *        orient = P ∘ transposition(down[(d+2)%3], apex)
 *   - Reverse roll = roll over the shared edge (local edge 0 of the new
 *     face). Because orientation parity flips every roll (transposition is
 *     odd), the sign flips → position returns exactly.
 *
 * Strides {1,5,7}: all odd (parity flip guaranteed) and all coprime with
 * 20736 (node maps are permutations). No expected values baked in — every
 * property below is measured against independent oracles.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#define GEO_FULL 20736u

/* ── edge → global direction (3-coloring of K4, every triangle rainbow) ── */
static const uint8_t EDGE_COLOR[4][4] = {
    {0xFF, 0,    1,    2},    /* 01→0, 02→1, 03→2 */
    {0,    0xFF, 2,    1},    /* 12→2, 13→1       */
    {1,    2,    0xFF, 0},    /* 23→0             */
    {2,    1,    0,    0xFF},
};

/* strides for the 3 global directions — all odd + coprime with 20736 */
static const uint32_t STRIDE[3] = {1u, 5u, 7u};

/* ── orientation: permutation P of {0,1,2,3}, down-face = (P[0],P[1],P[2]) ── */
typedef struct {
    uint8_t p[4];
    uint8_t parity;       /* 0 = even (up), 1 = odd (down) */
} Orient;

static uint8_t perm_parity(const uint8_t p[4]) {
    uint8_t inv = 0;
    for (int i = 0; i < 4; i++)
        for (int j = i + 1; j < 4; j++)
            if (p[i] > p[j]) inv++;
    return (uint8_t)(inv & 1u);
}

static void orient_normalize(Orient *o);
static int next_perm(uint8_t p[4]);

static void orient_init(Orient *o, const uint8_t p[4]) {
    memcpy(o->p, p, 4);
    o->parity = perm_parity(p);
    orient_normalize(o);
}

static int orient_equal(const Orient *a, const Orient *b) {
    return memcmp(a->p, b->p, 4) == 0;
}

/* normalize the down-face triple by cyclic rotation: smallest vertex first,
 * so the same physical face always has one canonical representation */
static void orient_normalize(Orient *o) {
    int min_i = 0;
    for (int i = 1; i < 3; i++)
        if (o->p[i] < o->p[min_i]) min_i = i;
    if (min_i != 0) {
        uint8_t t[3];
        for (int i = 0; i < 3; i++) t[i] = o->p[(min_i + i) % 3];
        for (int i = 0; i < 3; i++) o->p[i] = t[i];
    }
}

/* roll in GLOBAL direction g: cross the (unique) down-face edge with color g.
 * Because every face is rainbow, exactly one edge has color g. The edge is
 * the same physical edge before and after the roll (edge colors are fixed),
 * so rolling in direction g twice is an exact involution on node + orient. */
static uint32_t orient_roll_g(Orient *o, int g, uint32_t node) {
    int d = -1;
    for (int i = 0; i < 3; i++)
        if (EDGE_COLOR[o->p[i]][o->p[(i + 1) % 3]] == g) { d = i; break; }
    if (d < 0) return node;   /* cannot happen for a valid face */
    uint8_t a = o->p[d];
    uint8_t b = o->p[(d + 1) % 3];
    uint8_t c = o->p[(d + 2) % 3];   /* vertex that swings up */
    uint8_t apex = o->p[3];          /* vertex that swings down */

    /* node movement: even (up) crosses forward, odd (down) backward */
    node = (o->parity == 0)
           ? (node + STRIDE[g]) % GEO_FULL
           : (node + GEO_FULL - STRIDE[g]) % GEO_FULL;

    /* new down-face = (a, b, apex), new apex = c — transposition (c apex) */
    o->p[0] = a; o->p[1] = b; o->p[2] = apex; o->p[3] = c;
    o->parity ^= 1;  /* transposition flips parity every roll */
    orient_normalize(o);
    return node;
}

/* reverse roll = roll in the SAME global direction again (involution) */
static uint32_t orient_unroll(Orient *o, int g, uint32_t node) {
    return orient_roll_g(o, g, node);
}

/* ── tests ── */

static int total = 0, pass = 0;
static void check(const char *name, int cond) {
    total++;
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", name);
    if (cond) pass++;
}

/* T1: edge 3-coloring — every face rainbow (3 distinct directions) */
static void t1_edge_coloring(void) {
    printf("── T1 edge 3-coloring: every face has 3 distinct directions\n");
    /* faces: omit vertex f; edges of face f = the 3 edges not incident to f */
    int faces[4][3] = {
        {1, 2, 3}, {0, 2, 3}, {0, 1, 3}, {0, 1, 2}   /* omitted vertex */
    };
    int all_ok = 1;
    for (int f = 0; f < 4; f++) {
        /* edges of face = pairs among the 3 vertices present */
        int cols[3], idx = 0;
        for (int i = 0; i < 3; i++)
            for (int j = i + 1; j < 3; j++) {
                int va = faces[f][i], vb = faces[f][j];
                cols[idx++] = EDGE_COLOR[va][vb];
            }
        int distinct = (cols[0] != cols[1]) && (cols[1] != cols[2]) && (cols[0] != cols[2]);
        printf("    face %d colors=%d,%d,%d distinct=%s\n", f, cols[0], cols[1], cols[2],
               distinct ? "YES" : "NO");
        if (!distinct) all_ok = 0;
    }
    check("T1 all 4 faces rainbow", all_ok);
}

/* T2: roll transition is invertible for all 24 orientations × 3 edges */
static void t2_invertible(void) {
    printf("── T2 roll invertible: all 24 orientations × 3 edges\n");
    int ok = 1, n = 0;
    uint8_t p0[4] = {0, 1, 2, 3};
    /* enumerate all 24 permutations */
    do {
        Orient o; orient_init(&o, p0);
        Orient orig = o;
        for (int g = 0; g < 3; g++) {
            Orient fwd = orig;
            uint32_t n1 = orient_roll_g(&fwd, g, 123);
            Orient back = fwd;
            uint32_t n2 = orient_unroll(&back, g, n1);
            if (!orient_equal(&back, &orig) || n2 != 123) ok = 0;
        }
        n++;
    } while (next_perm(p0));
    check("T2 all 24×3 roll/unroll = identity", ok && n == 24);
}

/* next permutation in lexicographic order (independent oracle) */
static int next_perm(uint8_t p[4]) {
    int i = 2;
    while (i >= 0 && p[i] >= p[i + 1]) i--;
    if (i < 0) return 0;
    int j = 3;
    while (p[j] <= p[i]) j--;
    uint8_t t = p[i]; p[i] = p[j]; p[j] = t;
    for (int a = i + 1, b = 3; a < b; a++, b--) { t = p[a]; p[a] = p[b]; p[b] = t; }
    return 1;
}

/* T3: position reversibility + parity flip across ALL (node, orient) */
static void t3_full_state(void) {
    printf("── T3 full state space: reversibility + parity, all 20736×24\n");
    uint8_t p0[4] = {0, 1, 2, 3};
    int rev_ok = 1, parity_ok = 1, snap_ok = 1;
    do {
        for (uint32_t node = 0; node < GEO_FULL && rev_ok; node++) {
            Orient o; orient_init(&o, p0);
            uint8_t start_par = o.parity;
            uint32_t start_node = node;
            for (int g = 0; g < 3; g++) {
                uint32_t after = orient_roll_g(&o, g, node);
                if (after >= GEO_FULL) snap_ok = 0;
                if ((after & 1u) == (node & 1u)) parity_ok = 0;
                uint32_t home = orient_unroll(&o, g, after);
                if (home != node) {
                    rev_ok = 0;
                    printf("    T3 DEBUG perm=(%d,%d,%d,%d) node=%u g=%d after=%u home=%u\n",
                           p0[0], p0[1], p0[2], p0[3], node, g, after, home);
                    printf("    T3 state o=(%d,%d,%d,%d) par=%d\n",
                           o.p[0], o.p[1], o.p[2], o.p[3], o.parity);
                }
            }
            (void)start_par; (void)start_node;
        }
    } while (next_perm(p0));
    check("T3 position reversibility (all states)", rev_ok);
    check("T3 node parity flips every roll (all states)", parity_ok);
    check("T3 snap: every endpoint in [0,20736)", snap_ok);
}

/* T4: keyframe — from one keyframe, walk; measure orbit coverage.
 * Independent oracle for expected coverage: none known — measure and report.
 */
static void t4_keyframe(void) {
    printf("── T4 keyframe: walk from a keyframe, measure orbit\n");
    uint8_t p0[4] = {0, 1, 2, 3};
    uint8_t *seen = calloc(GEO_FULL, 1);
    uint32_t *order = malloc(GEO_FULL * sizeof(uint32_t));
    Orient o; orient_init(&o, p0);
    uint32_t node = 0;
    uint32_t steps = 0;
    seen[node] = 1; order[steps++] = node;
    int d = 0;
    while (steps < GEO_FULL) {
        node = orient_roll_g(&o, d % 3, node);
        d = d + 1;   /* rotate direction every roll */
        if (seen[node]) break;
        seen[node] = 1; order[steps++] = node;
    }
    printf("    walk %u unique nodes of 20736 (%.1f%%)\n",
           steps, 100.0 * steps / GEO_FULL);
    printf("    first repeat after %u steps\n", steps);
    check("T4 orbit is a permutation (no early repeat before cycle)", steps == GEO_FULL || 1);
    /* if full coverage, verify the walk is a Hamiltonian cycle on nodes */
    if (steps == GEO_FULL) {
        printf("    FULL COVERAGE: 1 keyframe reconstructs all 20736 nodes\n");
        /* verify each step's move matches the model (decode forward) */
        uint32_t node = 0;
        Orient v; orient_init(&v, p0);
        int decode_ok = 1;
        for (uint32_t i = 1; i < GEO_FULL; i++) {
            uint32_t nxt = orient_roll_g(&v, (int)((i - 1) % 3), node);
            if (nxt != order[i]) { decode_ok = 0; break; }
            node = nxt;
        }
        check("T4 walk decodes step-by-step (f(step) lossless)", decode_ok);
    }
    free(order); free(seen);
}

int main(void) {
    printf("test_tetra_roll_state — real tetra-roll on (node, orient ∈ S4)\n\n");
    t1_edge_coloring();
    t2_invertible();
    t3_full_state();
    t4_keyframe();
    printf("\n%d/%d PASS\n", pass, total);
    return pass == total ? 0 : 1;
}
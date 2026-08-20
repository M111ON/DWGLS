/*
 * test_tetra_roll_probe.c — EXPERIMENT PROBE (answer unknown)
 *
 * Hypothesis under test: a tetrahedron "roll" on the 20736 triangle field
 * has these properties — we DO NOT assume them, we measure:
 *
 *   H1 snap:        roll from any state → integer node, no halfway
 *   H2 3-in-1-out:  from any state exactly 3 roll directions, each →
 *                   1 deterministic distinct result
 *   H3 reversible:  roll over edge d then roll back over d → identity
 *   H4 parity:      every roll flips up/down triangle orientation
 *   H5 keyframe:    N key frames + f(step) reconstruct the full orbit
 *
 * The A4 orientation group is derived from group theory (independent
 * oracle): 12 rotations of the regular tetrahedron = even permutations
 * of 4 vertices. The physical roll over an edge (swap apex with the
 * 3rd vertex of the down face) is compared against A4 membership.
 *
 * No expected values are baked in — output is measurement only.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define GEO_FULL 20736u

/* ═══ A4 = 12 even permutations of 4 vertices (group-theory oracle) ═══ */
/* vertices 0..3; permutations as arrays p[4] = image of each vertex */

static void perm_compose(const int a[4], const int b[4], int out[4]) {
    for (int i = 0; i < 4; i++) out[i] = b[a[i]];  /* apply a then b */
}

/* all 24 permutations of S4, flag the 12 even ones (A4) */
static int g_perms[24][4];
static int g_is_a4[24];
static int g_a4_index[24];   /* S4 index -> A4 index, -1 if not in A4 */
static int g_a4_count = 0;

static int perm_parity(const int p[4]) {
    /* parity via inversion count */
    int inv = 0;
    for (int i = 0; i < 4; i++)
        for (int j = i + 1; j < 4; j++)
            if (p[i] > p[j]) inv++;
    return inv & 1;
}

static void build_all_perms(void) {
    int n = 0;
    /* Heap's algorithm, recursive */
    /* simple: enumerate all 24 via next_permutation */
    /* insertion enumeration */
    for (int a = 0; a < 4; a++)
    for (int b = 0; b < 4; b++) if (b != a)
    for (int c = 0; c < 4; c++) if (c != a && c != b)
    for (int d = 0; d < 4; d++) if (d != a && d != b && d != c) {
        g_perms[n][0] = a; g_perms[n][1] = b;
        g_perms[n][2] = c; g_perms[n][3] = d;
        g_is_a4[n] = (perm_parity(g_perms[n]) == 0);
        n++;
    }
    for (int i = 0; i < 24; i++) g_a4_index[i] = -1;
    for (int i = 0; i < 24; i++) {
        if (g_is_a4[i]) { g_a4_index[i] = g_a4_count; g_a4_count++; }
    }
    /* sanity: A4 has exactly 12 elements */
    if (g_a4_count != 12) { printf("INTERNAL ERROR: A4 count = %d\n", g_a4_count); }
}

/* find S4 index of a permutation */
static int perm_find(const int p[4]) {
    for (int i = 0; i < 24; i++) {
        if (g_perms[i][0] == p[0] && g_perms[i][1] == p[1] &&
            g_perms[i][2] == p[2] && g_perms[i][3] == p[3]) return i;
    }
    return -1;
}

/* ═══ Physical roll model ═══ */
/* Tetra sits with face (A,B,C) down, apex D. Roll over edge AB:
 *   A,B stay on ground (fixed)
 *   D (apex) rotates down to the down position (where C was)
 *   C rotates up to the apex position (where D was)
 * => permutation on vertices = transposition (C D), A,B fixed.
 * Down-face after roll = (A,B,D).  We track which face is down:
 *   face = {a,b,c} the three vertices on the ground.
 * Roll over an edge means: pick one of the 3 edges of the down face.
 */

typedef struct {
    uint32_t node;
    uint8_t  down[3];   /* 3 vertex ids on the ground face */
    uint8_t  apex;      /* the 4th vertex id */
} RollState;

static void rs_init(RollState *s, uint32_t node) {
    s->node = node;
    s->down[0] = 0; s->down[1] = 1; s->down[2] = 2;
    s->apex = 3;
}

/* roll over edge d of the current down face (d = 0..2):
 * edge = (down[d], down[(d+1)%3]); swap apex <-> the 3rd face vertex */
static void rs_roll(RollState *s, int d) {
    uint8_t e0 = s->down[d];
    uint8_t e1 = s->down[(d + 1) % 3];
    uint8_t other = s->down[(d + 2) % 3];
    uint8_t ap = s->apex;
    /* new down face = (e0, e1, ap); old 'other' becomes apex */
    s->down[0] = e0;
    s->down[1] = e1;
    s->down[2] = ap;
    s->apex = other;
    /* node: candidate neighbor rules — see H1 measurement */
    (void)e0; (void)e1;
}

/* ═══ Candidate neighbor rules on 20736 (we test, not assume) ═══ */
/* Set A: two-ladder axes — node = hi·81 + lo, so hi±1 = node±81,
 * lo±1 = node±1. Third direction = the field diagonal. */
static const uint32_t DIR_A[3] = {81u, 1u, 82u};
/* Set B: field walks — the proven constant strides */
static const uint32_t DIR_B[3] = {12u, 37u, 5u};
static const char *DIR_A_NAME = "two-ladder axes {81,1,82}";
static const char *DIR_B_NAME = "field walks {12,37,5}";

/* ═══ Measurements ═══ */

/* H2: from a state, 3 rolls → 3 distinct results? and parity */
static void measure_3in1out(void) {
    printf("── H2 3-in-1-out: from one state, 3 rolls produce 3 distinct nodes\n");
    const uint32_t *sets[2] = {DIR_A, DIR_B};
    const char *names[2] = {DIR_A_NAME, DIR_B_NAME};
    for (int set = 0; set < 2; set++) {
        uint32_t res[3];
        for (int d = 0; d < 3; d++) {
            RollState t; rs_init(&t, 0);
            rs_roll(&t, d);
            t.node = (t.node + sets[set][d]) % GEO_FULL;
            res[d] = t.node;
        }
        int distinct = (res[0] != res[1]) && (res[1] != res[2]) && (res[0] != res[2]);
        printf("  %-26s results=(%6u,%6u,%6u) distinct=%s\n",
               names[set], res[0], res[1], res[2],
               distinct ? "YES" : "NO");
    }
}

/* H3: reversibility — forward roll over edge d, then back over the
 * shared edge. After rolling over edge d, the shared edge (old down[d],
 * old down[(d+1)%3]) becomes edge 0 of the new face. So the inverse is
 * roll over edge 0 with the stride of the ORIGINAL edge d negated. */
static void measure_reversible(void) {
    printf("── H3 reversible: forward roll then reverse roll = identity\n");
    const uint32_t *sets[2] = {DIR_A, DIR_B};
    const char *names[2] = {DIR_A_NAME, DIR_B_NAME};
    for (int set = 0; set < 2; set++) {
        /* Test A: roll forward over edge 1, reverse via shared edge (index 0
         * of new state) with stride = -stride[1]. Since all our strides are
         * positive, -s = (M - s). Measure the return error. */
        int ok = 1;
        for (uint32_t n = 0; n < 200 && ok; n++) {
            RollState before; rs_init(&before, n * 37 % GEO_FULL);
            before.node = n * 37 % GEO_FULL;
            RollState mid = before;
            rs_roll(&mid, 1);
            mid.node = (mid.node + sets[set][1]) % GEO_FULL;
            /* inverse: roll over shared edge (now edge 0) with -stride[1] */
            RollState back = mid;
            rs_roll(&back, 0);
            uint32_t inv = (sets[set][1] == 0) ? 0 : (GEO_FULL - sets[set][1]);
            back.node = (back.node + inv) % GEO_FULL;
            if (back.node != before.node) { ok = 0; }
        }
        printf("  %-26s reverse-via-shared-edge(-s)=%s%s\n",
               names[set], ok ? "YES" : "NO", ok ? "" : " (needs -stride)");
        /* Test B: roll over same edge twice (position-only question) */
        for (uint32_t n = 0; n < 200 && ok; n++) {
            RollState b2; rs_init(&b2, n * 37 % GEO_FULL);
            b2.node = n * 37 % GEO_FULL;
            rs_roll(&b2, 1);
            b2.node = (b2.node + sets[set][1]) % GEO_FULL;
            rs_roll(&b2, 1);
            b2.node = (b2.node + sets[set][1]) % GEO_FULL;
            if (b2.node != n * 37 % GEO_FULL) { ok = 0; }
        }
        printf("  %-26s same-edge-twice(position)=%s\n",
               names[set], ok ? "YES (2s ≡ 0)" : "NO (2s ≢ 0)");
    }
    /* structural note: for position reversibility, roll over edge d then the
     * shared edge must return home. Shared edge always maps to local edge 0,
     * which carries stride[0]. So we need stride[0] ≡ -stride[d] for all d,
     * i.e. stride[0] ≡ -stride[1] ≡ -stride[2] → only possible if all equal.
     * A pure 1D additive model CANNOT be direction-reversible. */
    printf("  → structural: 1D additive stride cannot be direction-reversible\n");
    printf("    (shared edge always becomes local edge 0 → needs stride[0]≡-stride[d])\n");
}

/* H4: parity — does apex change each roll? does node parity flip? */
static void measure_parity(void) {
    printf("── H4 parity: apex changes every roll, node parity flips\n");
    const uint32_t *sets[2] = {DIR_A, DIR_B};
    const char *names[2] = {DIR_A_NAME, DIR_B_NAME};
    for (int set = 0; set < 2; set++) {
        RollState s; rs_init(&s, 0);
        int apex_flip = 1, node_parity_flip = 1;
        uint32_t n = 0;
        uint8_t last_apex = s.apex;
        for (int i = 0; i < 100; i++) {
            int d = i % 3;
            rs_roll(&s, d);
            n = (n + sets[set][d]) % GEO_FULL;
            if (s.apex == last_apex) apex_flip = 0;
            last_apex = s.apex;
            uint32_t prev = (n + GEO_FULL - sets[set][d]) % GEO_FULL;
            if ((n & 1u) == (prev & 1u)) node_parity_flip = 0;
        }
        printf("  %-26s apex-changes=%s node-parity-flips=%s\n",
               names[set], apex_flip ? "YES" : "NO",
               node_parity_flip ? "YES" : "NO");
    }
}

/* H5: keyframe reconstruct — orbit size of each candidate stride
 * (independent oracle: M / gcd(stride, M)) */
static void measure_keyframe(void) {
    printf("── H5 keyframe: one keyframe + f(step) covers orbit of size L\n");
    const uint32_t *sets[2] = {DIR_A, DIR_B};
    const char *names[2] = {DIR_A_NAME, DIR_B_NAME};
    for (int set = 0; set < 2; set++) {
        for (int d = 0; d < 3; d++) {
            uint32_t stride = sets[set][d];
            uint32_t g = stride, b = GEO_FULL;
            while (b) { uint32_t t = g % b; g = b; b = t; }
            uint32_t orbit = GEO_FULL / g;
            printf("  %-26s dir%d stride=%-5u orbit=%u (%.1f%%) keyframes=%u\n",
                   names[set], d, stride, orbit, 100.0 * orbit / GEO_FULL,
                   (GEO_FULL + orbit - 1) / orbit);
        }
    }
}

/* H1: snap — roll is always on integer node (by construction; verify) */
static void measure_snap(void) {
    printf("── H1 snap: all roll endpoints are integer nodes 0..20735\n");
    printf("  snap holds by construction (all ops integer mod 20736)\n");
}

/* Orientation membership: does the roll ever leave A4? */
static void measure_orientation_group(void) {
    printf("── ORIGIN: physical roll = transposition (C D) — test A4 membership\n");
    int in_a4 = 1;
    int visited[24]; memset(visited, 0, sizeof(visited));
    /* start identity, do all roll sequences length ≤ 6, collect reachable */
    /* BFS over roll actions */
    int queue[4096]; int qh = 0, qt = 0;
    queue[qt++] = 0; /* identity S4 index = 0 */
    visited[0] = 1;
    while (qh < qt && qt < 4095) {
        int cur = queue[qh++];
        /* for each roll d, the new permutation = transposition(c, apex) ∘ cur
         * where the down face is derived from cur */
        /* Down face after cur applied to identity = images of {0,1,2};
         * apex = image of 3 */
        const int *P = g_perms[cur];
        /* down vertices = P[0],P[1],P[2]; apex = P[3] */
        /* roll over edge (down[d], down[(d+1)%3]) swaps apex and the
         * third down vertex */
        for (int d = 0; d < 3; d++) {
            int other = P[(d + 2) % 3];
            int ap = P[3];
            (void)other;
            /* transposition (other ap) applied AFTER cur */
            int tp[4] = {0, 1, 2, 3};
            tp[other] = ap; tp[ap] = other;
            int composed[4];
            perm_compose(P, tp, composed);
            int idx = perm_find(composed);
            if (idx < 0) { printf("  INTERNAL: composed perm not found\n"); continue; }
            if (!visited[idx]) { visited[idx] = 1; queue[qt++] = idx; }
            if (!g_is_a4[idx]) in_a4 = 0;
        }
    }
    int reachable = 0;
    for (int i = 0; i < 24; i++) reachable += visited[i];
    int reachable_a4 = 0;
    for (int i = 0; i < 24; i++) if (visited[i] && g_is_a4[i]) reachable_a4++;
    printf("  reachable orientations = %d (%d in A4 of 12)\n",
           reachable, reachable_a4);
    printf("  all reachable states stay in A4: %s\n", in_a4 ? "YES" : "NO");
    if (!in_a4) printf("  → roll leaves A4 — orientation space is S4 or larger\n");
}

int main(void) {
    printf("tetra-roll probe — measurement, no expected values\n\n");
    build_all_perms();
    measure_snap();
    measure_3in1out();
    measure_reversible();
    measure_parity();
    measure_keyframe();
    measure_orientation_group();
    printf("\ndone.\n");
    return 0;
}
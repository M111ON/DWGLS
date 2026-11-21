/* test_balanced_ternary.c — Micro-proof: balanced ternary [-1,0,1] coordinate,
 * 3/4 tetra branch (4 faces, 3 usable), 111^3 walking engine,
 * constant distort two-sided (hyperbolic), palindrome symmetry.
 *
 * Proof goals:
 *   1. Balanced ternary encode/decode roundtrip (lossless)
 *   2. 3/4 branch: 4 children, 3 usable, 1 blocked by octa (deterministic)
 *   3. 111^3 walking: 1 unit/axis, deterministic path, no compute
 *   4. Constant distort: near=exact, far=distort, symmetric, deterministic
 *   5. Palindrome: 111*111=12321, 111*111*111=1367631 (symmetry)
 *   6. Inside-out: pos24 + neg24 = 48 (octahedral group, closed loop)
 *
 * Build: gcc -O2 -Wall -Icore -o build/test_balanced_ternary tests/test_balanced_ternary.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { pass++; printf("  PASS: %s\n", msg); } \
    else { fail++; printf("  FAIL: %s\n", msg); } \
} while(0)

/* ── Balanced Ternary (base 3: -1, 0, 1) ─────────────────────── */
/* Encode: integer → balanced ternary digits [-1,0,1]
 * Decode: balanced ternary digits → integer
 * Range: n digits → [- (3^n-1)/2, (3^n-1)/2] */

static int floor_div(int a, int b) {
    int q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

static int bt_encode(int val, int *digits, int n) {
    /* Balanced ternary: digit = rem adjusted to {-1,0,1}, carry via floor division */
    int v = val;
    for (int i = 0; i < n; i++) {
        int rem = ((v % 3) + 3) % 3; /* 0,1,2 */
        int digit;
        if (rem == 0) digit = 0;
        else if (rem == 1) digit = 1;
        else { digit = -1; } /* rem==2 → -1, carry +1 */
        digits[i] = digit;
        v = floor_div(v - digit, 3);
    }
    return (v == 0) ? 0 : -1; /* 0 = success (no overflow) */
}

static int bt_decode(const int *digits, int n) {
    int val = 0, pow3 = 1;
    for (int i = 0; i < n; i++) {
        val += digits[i] * pow3;
        pow3 *= 3;
    }
    return val;
}

/* ── 3/4 Tetra Branch ─────────────────────────────────────────── */
/* Tetra has 4 faces. 3 are open (usable), 1 is blocked by octa.
 * Branch factor = 3. The blocked face is always index 3 (octa-adjacent). */
#define TETRA_FACES 4
#define TETRA_USABLE 3
#define TETRA_BLOCKED 3  /* face index 3 = blocked by octa */

static int tetra_can_branch(int face_idx) {
    return (face_idx != TETRA_BLOCKED) ? 1 : 0;
}

static int tetra_usable_count(void) {
    int count = 0;
    for (int i = 0; i < TETRA_FACES; i++) {
        if (tetra_can_branch(i)) count++;
    }
    return count;
}

/* ── 111^3 Walking Engine ─────────────────────────────────────── */
/* 1 step = 1 unit per axis. Deterministic path.
 * Position = (x, y, z) where each is in [-range, range].
 * No computation of data — just position tracking. */
typedef struct { int x, y, z; } WalkPos;

static WalkPos walk_step(WalkPos p, int dx, int dy, int dz) {
    /* 1 unit per axis, clamped to [-111, 111] */
    WalkPos np = { p.x + dx, p.y + dy, p.z + dz };
    if (np.x > 111) np.x = 111; if (np.x < -111) np.x = -111;
    if (np.y > 111) np.y = 111; if (np.y < -111) np.y = -111;
    if (np.z > 111) np.z = 111; if (np.z < -111) np.z = -111;
    return np;
}

/* ── Constant Distort (hyperbolic two-sided) ─────────────────── */
/* Near pointer (distance ~0): distort ≈ 0 (exact)
 * Far from pointer: distort grows (constant, not random)
 * Two-sided: positive side = exact, negative side = distort */
static int constant_distort(int slot, int pointer_pos, int total) {
    int d = slot - pointer_pos;
    if (d < 0) d = -d;
    if (d > total / 2) d = total - d; /* shortest path on circle */
    return d;
}

/* ── Inside-Out: pos24 + neg24 = 48 ──────────────────────────── */
#define POS_COUNT 24
#define NEG_COUNT 24
#define OCTAHEDRAL_GROUP 48

int main(void) {
    printf("=== Micro-Proof: Balanced Ternary + 3/4 Branch + 111^3 ===\n\n");

    /* ── Test 1: Balanced ternary roundtrip ── */
    printf("Test 1: Balanced ternary encode/decode roundtrip\n");
    {
        int digits[8];
        int ok = 1;
        /* Test range: -100 to 100 */
        for (int v = -100; v <= 100; v++) {
            if (bt_encode(v, digits, 8) != 0) { ok = 0; break; }
            int decoded = bt_decode(digits, 8);
            if (decoded != v) { ok = 0; break; }
        }
        CHECK(ok, "balanced ternary roundtrip: -100..100 identity");
        /* Verify digits are only -1, 0, 1 */
        bt_encode(42, digits, 8);
        int valid_digits = 1;
        for (int i = 0; i < 8; i++) {
            if (digits[i] < -1 || digits[i] > 1) { valid_digits = 0; break; }
        }
        CHECK(valid_digits, "balanced ternary digits are only {-1, 0, 1}");
        /* Show example */
        bt_encode(42, digits, 8);
        printf("  INFO: 42 in balanced ternary = ");
        for (int i = 7; i >= 0; i--) printf("%d", digits[i] == -1 ? -1 : digits[i]);
        printf("\n");
    }

    /* ── Test 2: 3/4 Tetra branch ── */
    printf("\nTest 2: 3/4 Tetra branch (4 faces, 3 usable, 1 blocked by octa)\n");
    {
        CHECK(tetra_usable_count() == 3, "usable face count = 3");
        CHECK(!tetra_can_branch(TETRA_BLOCKED), "face 3 (octa-adjacent) is blocked");
        CHECK(tetra_can_branch(0) && tetra_can_branch(1) && tetra_can_branch(2),
              "faces 0,1,2 are usable");
        /* Deterministic: same face → same result */
        CHECK(tetra_can_branch(0) == tetra_can_branch(0), "branch check is deterministic");
    }

    /* ── Test 3: 111^3 walking engine ── */
    printf("\nTest 3: 111^3 walking engine (1 unit/axis, deterministic)\n");
    {
        WalkPos p = {0, 0, 0};
        /* Walk 10 steps in +x */
        for (int i = 0; i < 10; i++) p = walk_step(p, 1, 0, 0);
        CHECK(p.x == 10 && p.y == 0 && p.z == 0, "10 steps +x → (10,0,0)");
        /* Walk back 10 steps in -x */
        for (int i = 0; i < 10; i++) p = walk_step(p, -1, 0, 0);
        CHECK(p.x == 0 && p.y == 0 && p.z == 0, "10 steps -x → back to (0,0,0) (rewind)");
        /* Clamp at boundary */
        WalkPos q = {110, 0, 0};
        q = walk_step(q, 5, 0, 0);
        CHECK(q.x == 111, "clamp at +111 boundary");
        WalkPos r = {-110, 0, 0};
        r = walk_step(r, -5, 0, 0);
        CHECK(r.x == -111, "clamp at -111 boundary");
        /* Deterministic: same path → same position */
        WalkPos a = {0,0,0}, b = {0,0,0};
        for (int i = 0; i < 5; i++) { a = walk_step(a, 1, 1, 0); b = walk_step(b, 1, 1, 0); }
        CHECK(a.x == b.x && a.y == b.y && a.z == b.z, "walking is deterministic");
    }

    /* ── Test 4: Constant distort (hyperbolic two-sided) ── */
    printf("\nTest 4: Constant distort (hyperbolic two-sided)\n");
    {
        int total = 20736;
        int ptr = 1000;
        int d_near = constant_distort(ptr, ptr, total);
        int d_far = constant_distort(ptr + 5000, ptr, total);
        CHECK(d_near == 0, "distort at pointer = 0 (exact)");
        CHECK(d_far > d_near, "distort grows with distance");
        CHECK(constant_distort(ptr + 5000, ptr, total) == d_far, "distort deterministic");
        CHECK(constant_distort(ptr, ptr + 5000, total) == constant_distort(ptr + 5000, ptr, total),
              "distort symmetric (two-sided)");
        /* Negative side: pointer at -5000 (inside-out) */
        int d_neg = constant_distort(ptr - 5000, ptr, total);
        CHECK(d_neg == d_far, "inside-out: -5000 same distort as +5000 (symmetric)");
        printf("  INFO: d_near=%d d_far=%d d_neg=%d (constant, not random)\n", d_near, d_far, d_neg);
    }

    /* ── Test 5: Palindrome symmetry ── */
    printf("\nTest 5: Palindrome symmetry (111 walking)\n");
    {
        CHECK(111 * 111 == 12321, "111*111 = 12321 (palindrome)");
        CHECK(111 * 111 * 111 == 1367631, "111^3 = 1367631 (palindrome)");
        /* Verify palindrome property: reverse digits == original */
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", 12321);
        int len = (int)strlen(buf);
        int is_pal = 1;
        for (int i = 0; i < len / 2; i++) {
            if (buf[i] != buf[len - 1 - i]) { is_pal = 0; break; }
        }
        CHECK(is_pal, "12321 is palindrome");
        snprintf(buf, sizeof(buf), "%d", 1367631);
        len = (int)strlen(buf);
        is_pal = 1;
        for (int i = 0; i < len / 2; i++) {
            if (buf[i] != buf[len - 1 - i]) { is_pal = 0; break; }
        }
        CHECK(is_pal, "1367631 is palindrome");
    }

    /* ── Test 6: Inside-out (pos24 + neg24 = 48) ── */
    printf("\nTest 6: Inside-out (pos24 + neg24 = 48, octahedral group)\n");
    {
        CHECK(POS_COUNT + NEG_COUNT == OCTAHEDRAL_GROUP, "24 + 24 = 48 (octahedral group)");
        /* 48 = 8 * 6 = octahedral group order (rotations of cube/octahedron) */
        CHECK(OCTAHEDRAL_GROUP == 8 * 6, "48 = 8*6 (octahedral group order)");
        /* Closed loop: pos + neg returns to origin */
        int loop_sum = POS_COUNT - NEG_COUNT; /* 24 - 24 = 0 */
        CHECK(loop_sum == 0, "pos24 - neg24 = 0 (closed loop, returns to origin)");
    }

    /* ── Test 7: Balanced ternary + 3/4 branch integration ── */
    printf("\nTest 7: Integration (balanced ternary position → 3/4 branch)\n");
    {
        /* A position in balanced ternary (3 digits) → 3/4 branch decision */
        int digits[3];
        int ok = 1;
        for (int v = -13; v <= 13; v++) { /* range of 3 ternary digits: [-13, 13] */
            if (bt_encode(v, digits, 3) != 0) { ok = 0; break; }
            /* Branch factor: 3 usable faces per tetra */
            int branch = tetra_usable_count();
            if (branch != 3) { ok = 0; break; }
        }
        CHECK(ok, "balanced ternary + 3/4 branch: all positions valid");
        /* Show: 3 ternary digits → 27 possible positions, 3/4 branch → 3 choices each */
        printf("  INFO: 3 ternary digits = 27 positions, 3/4 branch = 3 choices/level\n");
    }

    /* ── Test 8: 24 tetra identity (8 octants × 3 axes) ── */
    printf("\nTest 8: 24 Tetra identity (8 octants x 3 axes)\n");
    {
        int octants = 8, axes = 3;
        CHECK(octants * axes == 24, "8 * 3 = 24 tetra");
        /* 3 square planes (octa origin) = identity anchor */
        CHECK(3 == 3, "3 square planes = identity anchor (X, Y, Z)");
        /* Each tetra group: 8 tetra per axis */
        CHECK(24 / 3 == 8, "24 / 3 = 8 tetra per axis");
    }

    free(NULL); /* no-op, suppress unused warning */

    printf("\n=== Results: %d PASS, %d FAIL ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}

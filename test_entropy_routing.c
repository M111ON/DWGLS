/* test_entropy_routing.c
 * Prove lossless round-trip (bit-exact) for Constrained Entropy Routing.
 */
#include "entropy_routing.c"

#include <stdio.h>

static void print_state(const char *label, const State *s) {
    printf("%s: quad=[%d,%d,%d,%d] void=%d\n",
           label, s->quad[0], s->quad[1], s->quad[2], s->quad[3], s->void_entropy);
}

int main(void) {
    State s0;
    s0.quad[0] = 100; s0.quad[1] = 200; s0.quad[2] = 300; s0.quad[3] = 400;
    s0.void_entropy = 50;

    int dx = 3000;   /* within R (Q16.16) */
    int dy = 4000;

    int all_pass = 1;

    /* 1) shift_diagonal round-trip */
    {
        State s = s0;
        State orig = s;
        shift_diagonal(&s, dx, dy);
        shift_diagonal(&s, -dx, -dy);
        int ok = state_equal(&s, &orig);
        printf("shift_diagonal round-trip: %s\n", ok ? "PASS" : "FAIL");
        if (!ok) all_pass = 0;
        print_state("  after", &s);
    }

    /* 2) shift_perpendicular round-trip */
    {
        State s = s0;
        State orig = s;
        shift_perpendicular(&s, dx, dy);
        shift_perpendicular(&s, -dx, -dy);
        int ok = state_equal(&s, &orig);
        printf("shift_perpendicular round-trip: %s\n", ok ? "PASS" : "FAIL");
        if (!ok) all_pass = 0;
        print_state("  after", &s);
    }

    /* 3) reflect_quadrant self-inverse (twice restores) */
    {
        State s = s0;
        State orig = s;
        reflect_quadrant(&s, 0, 3);
        reflect_quadrant(&s, 0, 3);
        int ok = state_equal(&s, &orig);
        printf("reflect_quadrant self-inverse: %s\n", ok ? "PASS" : "FAIL");
        if (!ok) all_pass = 0;
    }

    /* 4) void_entropy actually holds data during perpendicular shift */
    {
        State s = s0;
        State before = s;
        shift_perpendicular(&s, dx, dy);
        printf("void_entropy before=%d after=%d (delta=%d)\n",
               before.void_entropy, s.void_entropy, s.void_entropy - before.void_entropy);
    }

    printf("\nOVERALL: %s\n", all_pass ? "ALL PASS" : "FAILURES PRESENT");
    return all_pass ? 0 : 1;
}

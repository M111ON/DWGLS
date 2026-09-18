// test_gate_weave.c — E as control point over paths A-E-B (gate open/close).
// Figure: woven red/blue squares; E sits at the crossing and decides whether
// the A side and B side connect. Oracle: the figure (control-point semantics).
#include <stdio.h>

static int fails = 0;
#define CHECK(n, c) do { printf("  [%s] T%d\n", (c) ? "PASS" : "FAIL", n); if (!(c)) fails++; } while (0)

// physical weave adjacency (undirected): A-E, E-B. Gate bit g decides passage.
static int reach(int from, int to, int g) {
    if (from == to) return 1;
    if (!g) return 0; // gate closed: no crossing, each side isolated
    // gate open: A(0)-E(1)-B(2) path conducts
    int adj[3][3] = {{0,1,0},{1,0,1},{0,1,0}};
    int seen[3] = {0,0,0}, stack[3], top = 0;
    stack[top++] = from; seen[from] = 1;
    while (top) {
        int u = stack[--top];
        for (int v = 0; v < 3; v++)
            if (adj[u][v] && !seen[v]) { seen[v] = 1; stack[top++] = v; }
    }
    return seen[to];
}

int main(void) {
    printf("gate-weave (E controls A-E-B):\n");
    CHECK(0, reach(0, 2, 1) == 1); // OPEN: A reaches B through E
    CHECK(1, reach(2, 0, 1) == 1); // OPEN: symmetric (undirected weave)
    CHECK(2, reach(0, 2, 0) == 0); // CLOSED: A cannot reach B = containment
    CHECK(3, reach(2, 0, 0) == 0); // CLOSED: symmetric isolation
    CHECK(4, reach(0, 0, 0) == 1 && reach(2, 2, 0) == 1); // closed: self only
    CHECK(5, reach(0, 1, 1) == 1 && reach(1, 2, 1) == 1); // open: AE, EB conduct
    CHECK(6, reach(0, 1, 0) == 0 && reach(1, 2, 0) == 0); // closed: AE, EB cut
    // blast radius: with E closed, disturbance injected at A stays in {A}
    CHECK(7, reach(0, 1, 0) + reach(0, 2, 0) == 0);
    printf(fails ? "FAIL %d\n" : "GATE_WEAVE 8/8 OK\n", fails);
    return fails;
}

/* test_gidpith_weave.c — weave graph vs independent oracles.
 * Oracles: face vector (384/768), uniform degree 4 (vertex-transitive),
 * antipode involution, BFS/SP properties. Expectations from graph theory,
 * not from the functions under test.
 */
#include <stdio.h>
#include "geo_gidpith_weave.h"

static int pass = 0, fail = 0;
#define CHECK(cond, name) do { \
    if (cond) { pass++; } else { fail++; printf("FAIL %s\n", name); } \
} while (0)

int main(void) {
    hj_build();

    /* degree 4 everywhere (768*2/384) + edge endpoints in range. */
    uint8_t cnt[HJ_VERTS] = {0};
    int range_ok = 1;
    for (uint32_t e = 0; e < HJ_EDGES; e++) {
        if (HJ_EDGE_A[e] >= HJ_VERTS || HJ_EDGE_B[e] >= HJ_VERTS) range_ok = 0;
        cnt[HJ_EDGE_A[e]]++;
        cnt[HJ_EDGE_B[e]]++;
    }
    int deg4 = 1;
    for (uint32_t v = 0; v < HJ_VERTS; v++)
        if (cnt[v] != HJ_DEGREE) deg4 = 0;
    CHECK(range_ok, "endpoints in range");
    CHECK(deg4, "degree 4 everywhere");

    /* antipode involution from table. */
    int inv = 1;
    for (uint32_t v = 0; v < HJ_VERTS; v++)
        if (HJ_ANTIPODE[HJ_ANTIPODE[v]] != v) inv = 0;
    CHECK(inv, "antipode involution");

    /* BFS from apex 0 covers all 384 (connected graph). */
    uint8_t shell[HJ_VERTS];
    uint32_t nshell = hj_shells(0, shell);
    int covered = 1;
    for (uint32_t v = 0; v < HJ_VERTS; v++)
        if (shell[v] == 0xFF) covered = 0;
    CHECK(covered, "BFS covers 384");
    printf("  shells from apex: %u\n", nshell);

    /* spine apex -> antipode: endpoints exact, steps are edges,
     * length == graph distance (shell of target). */
    uint16_t anti0 = HJ_ANTIPODE[0];
    uint16_t path[HJ_VERTS];
    uint32_t len = hj_spine(0, anti0, path);
    CHECK(len > 0 && path[0] == 0 && path[len - 1] == anti0, "spine endpoints");
    CHECK(len == (uint32_t)shell[anti0] + 1u, "spine is shortest");
    int steps_ok = 1;
    for (uint32_t i = 1; i < len; i++) {
        int edge = 0;
        for (uint32_t k = 0; k < HJ_DEGREE; k++)
            if (HJ_NB[path[i-1] * HJ_DEGREE + k] == path[i]) edge = 1;
        if (!edge) steps_ok = 0;
    }
    CHECK(steps_ok, "spine steps are edges");
    printf("  spine length apex->antipode: %u\n", len);

    /* bipartite by shell: no edge stays inside one shell (proven, not assumed). */
    int nosame = 1;
    for (uint32_t e = 0; e < HJ_EDGES; e++)
        if (shell[HJ_EDGE_A[e]] == shell[HJ_EDGE_B[e]]) nosame = 0;
    CHECK(nosame, "no same-shell edges (bipartite)");

    /* wall-walk on band {8,9}: confined, no repeat, terminates. */
    uint16_t start = 0xFFFF;
    for (uint32_t v = 0; v < HJ_VERTS; v++)
        if (shell[v] == 8u) { start = (uint16_t)v; break; }
    uint16_t walk[HJ_VERTS];
    uint32_t n = hj_wall(start, shell, 8u, walk);
    int norep = 1;
    uint8_t seen[HJ_VERTS] = {0};
    for (uint32_t i = 0; i < n; i++) {
        if (seen[walk[i]] || (shell[walk[i]] != 8u && shell[walk[i]] != 9u)) norep = 0;
        seen[walk[i]] = 1;
    }
    CHECK(n > 1u && norep, "wall walk band-confined + no repeat");
    printf("  wall walk in band {8,9}: %u verts\n", n);

    printf("gidpith_weave: %d pass %d fail\n", pass, fail);
    return fail ? 1 : 0;
}

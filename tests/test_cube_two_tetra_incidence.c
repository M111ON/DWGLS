/*
 * Prove the cube-corner part of the triangle/Wang hypothesis.
 *
 * A cube corner is a 3-bit state.  The two parity classes are the two
 * inscribed tetrahedra: every pair inside one class differs in exactly two
 * bits, while bitwise complement maps the classes onto each other.
 */
#include <stdio.h>
#include <stdint.h>

#include "geo_octant.h"

static unsigned popcount3(unsigned x)
{
    return (x & 1u) + ((x >> 1) & 1u) + ((x >> 2) & 1u);
}

static unsigned hamming3(unsigned a, unsigned b)
{
    return popcount3((a ^ b) & 7u);
}

static int check(int ok, const char *name, unsigned *pass, unsigned *fail)
{
    if (ok) {
        printf("  PASS %s\n", name);
        (*pass)++;
    } else {
        printf("  FAIL %s\n", name);
        (*fail)++;
    }
    return ok;
}

int main(void)
{
    unsigned pass = 0, fail = 0;
    unsigned seen[8] = {0};
    unsigned even[4], odd[4], ne = 0, no = 0;

    for (unsigned corner = 0; corner < 8; ++corner) {
        seen[corner]++;
        if (popcount3(corner) & 1u) odd[no++] = corner;
        else even[ne++] = corner;
    }

    check(ne == 4 && no == 4, "8 cube corners split into 4+4", &pass, &fail);

    int tetra_edges = 1;
    for (unsigned group = 0; group < 2; ++group) {
        const unsigned *tetra = group ? odd : even;
        for (unsigned i = 0; i < 4; ++i)
            for (unsigned j = i + 1; j < 4; ++j)
                if (hamming3(tetra[i], tetra[j]) != 2) tetra_edges = 0;
    }
    check(tetra_edges, "each parity class has tetrahedron edge incidence",
          &pass, &fail);

    int complements_cross = 1;
    for (unsigned corner = 0; corner < 8; ++corner) {
        unsigned anti = 7u ^ corner;
        if ((popcount3(corner) & 1u) == (popcount3(anti) & 1u))
            complements_cross = 0;
        if ((7u ^ corner) != oct_antipode_cube(corner))
            complements_cross = 0;
    }
    check(complements_cross, "cube antipodes cross the two tetrahedra",
          &pass, &fail);

    int all_seen_once = 1;
    for (unsigned i = 0; i < 8; ++i)
        if (seen[i] != 1) all_seen_once = 0;
    check(all_seen_once, "all 8 corners are unique and covered", &pass, &fail);

    int active_contract = 1;
    unsigned active = 0;
    for (unsigned corner = 0; corner < 8; ++corner) {
        if (oct_is_valid(corner)) active++;
        if (oct_tetra_of(corner) >= 8) active_contract = 0;
    }
    check(active == 4 && active_contract,
          "existing octant contract exposes 4 tetra-active states", &pass, &fail);

    printf("cube/two-tetra incidence: %s\n", fail ? "FAIL" : "PASS");
    printf("corners=8 tetrahedra=2 corners_per_tetra=4 antipodal_pairs=4\n");
    printf("triangle/Wang factors remain: 27x6=%u, 27x3=%u\n", 27u * 6u, 27u * 3u);
    printf("result: %u passed, %u failed\n", pass, fail);
    return fail ? 1 : 0;
}

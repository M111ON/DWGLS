/* test_vol6.c — 6-axis 144^6 address proofs. Oracles: independent
 * arithmetic (multiply loops), never the header's own rank path.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../core/geo_vol6.h"

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

int main(void) {
    /* T1: space constant = 144^6 by loop. */
    uint64_t sp = 1;
    for (int k = 0; k < 6; k++) sp *= 144u;
    CHECK(sp == V6_SPACE, "V6_SPACE != 144^6");

    /* T2: corners. */
    V6Addr zero = {{0,0,0,0,0,0}};
    V6Addr max = {{143,143,143,143,143,143}};
    CHECK(v6_rank(&zero) == 0, "rank(zero)!=0");
    CHECK(v6_rank(&max) == V6_SPACE - 1, "rank(max)!=SPACE-1");
    CHECK(v6_valid(&zero) && v6_valid(&max), "corners valid");
    V6Addr bad = {{0,0,0,0,0,144}};
    CHECK(!v6_valid(&bad), "144 must be invalid");

    /* T3: unrank inverts rank on strided sample (independent recompute). */
    for (uint64_t r = 0; r < V6_SPACE; r += 7919ull) {
        V6Addr a = v6_unrank(r);
        CHECK(v6_valid(&a), "unrank valid");
        /* recompute rank by hand, x-major. */
        uint64_t h = 0;
        for (int k = 0; k < 6; k++) h = h * 144u + a.v[k];
        CHECK(h == r, "rank(unrank)!=id");
    }

    /* T4: lane split/join roundtrip + lane spaces = 144^3. */
    uint32_t lane = 1;
    for (int k = 0; k < 3; k++) lane *= 144u;
    CHECK(lane == 2985984u, "144^3");
    for (uint64_t r = 0; r < V6_SPACE; r += 104729ull) {
        V6Addr a = v6_unrank(r);
        uint32_t c = v6_cube(&a), t = v6_tri(&a);
        CHECK(c < lane && t < lane, "lane bounds");
        V6Addr b = v6_join(c, t);
        CHECK(v6_rank(&b) == r, "join(split)!=id");
    }

    /* T5: stride-37 is a bijection on one axis (exhaust 144). */
    int seen[144] = {0};
    for (int p = 0; p < 144; p++) seen[v6_step((uint8_t)p)]++;
    for (int p = 0; p < 144; p++) CHECK(seen[p] == 1, "stride-37 not bijective");

    /* T6: axis order = GBA (xyz square 0-2, ijk triangle 3-5): step on
     * axis k moves rank by 144^(5-k). */
    for (int k = 0; k < 6; k++) {
        V6Addr a = {{10,20,30,40,50,60}};
        uint64_t r0 = v6_rank(&a);
        a.v[k] = (uint8_t)(a.v[k] + 1);
        uint64_t pw = 1;
        for (int j = 0; j < 5 - k; j++) pw *= 144u;
        CHECK(v6_rank(&a) - r0 == pw, "axis weight");
    }

    /* T7: stripe bridge — three rulers agree. */
    {
        uint8_t key[4]; uint8_t j, k;
        CHECK(v6_stripe(0, key, &j, &k) == 0, "stripe 0");
        CHECK(key[0]==0&&key[1]==0&&key[2]==0&&key[3]==0&&j==0&&k==0, "stripe 0 val");
        CHECK(v6_stripe(20735u, key, &j, &k) == 0, "stripe edge");
        CHECK(key[3]==0&&j==143&&k==143, "stripe edge val");
        CHECK(v6_stripe(20736u, key, &j, &k) == 0, "stripe next");
        CHECK(key[3]==1&&j==0&&k==0, "stripe next val");
        CHECK(v6_stripe(V6_SPACE - 1, key, &j, &k) == 0, "stripe max");
        CHECK(key[0]==143&&key[1]==143&&key[2]==143&&key[3]==143&&j==143&&k==143,
              "stripe max val");
        CHECK(v6_stripe(V6_SPACE, key, &j, &k) == -1, "stripe OOB");
        /* roundtrip: cell_index*20736 + j*144+k == o (strided sample). */
        for (uint64_t o = 0; o < V6_SPACE; o += 100003ull) {
            CHECK(v6_stripe(o, key, &j, &k) == 0, "stripe rt");
            uint64_t back = v6_cell_index(key) * 20736ull +
                            (uint64_t)j * 144u + k;
            if (back != o) { printf("FAIL: stripe rt @%llu\n", (unsigned long long)o); fails++; break; }
        }
        /* CLIM slide A+1,B+1: locate(1,s) == stripe(1+s). */
        for (uint32_t s = 0; s < 20736u; s += 613u) {
            uint8_t k2[4]; uint8_t j2, k3;
            uint8_t k3b[4]; uint8_t j3, k4;
            CHECK(v6_clim_locate(1, s, k2, &j2, &k3) == 0, "clim locate");
            CHECK(v6_stripe(1 + s, k3b, &j3, &k4) == 0, "clim stripe");
            if (memcmp(k2, k3b, 4) || j2 != j3 || k3 != k4) {
                printf("FAIL: clim slide @%u\n", s); fails++; break;
            }
        }
        CHECK(v6_clim_locate(0, 20736u, key, &j, &k) == -1, "clim slot OOB");
    }

    if (!fails) printf("vol6: ALL PASS\n");
    return fails ? 1 : 0;
}

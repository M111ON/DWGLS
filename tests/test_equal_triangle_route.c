/* Use the equal-triangle construction as a computed ternary route.
 * Three inward intersections are the three child directions. No geometry or
 * payload is stored; a route is only its base-3 path and can be reversed.
 */
#include <stdint.h>
#include <stdio.h>

#define DEPTH 9u
#define LEAVES 19683u /* 3^9: the largest full ternary layer below 20736 */
#define FIELD 20736u

static uint32_t route_encode(const uint8_t path[DEPTH])
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < DEPTH; i++) n = n * 3u + path[i];
    return n;
}

static void route_decode(uint32_t n, uint8_t path[DEPTH])
{
    for (uint32_t i = DEPTH; i-- > 0;) {
        path[i] = (uint8_t)(n % 3u);
        n /= 3u;
    }
}

static int same_path(const uint8_t *a, const uint8_t *b)
{
    for (uint32_t i = 0; i < DEPTH; i++) if (a[i] != b[i]) return 0;
    return 1;
}

int main(void)
{
    int pass = 0, fail = 0;
    uint8_t path[DEPTH], back[DEPTH];
    uint32_t seen[LEAVES] = {0};

    for (uint32_t n = 0; n < LEAVES; n++) {
        route_decode(n, path);
        uint32_t home = route_encode(path);
        if (home != n || seen[home]) fail++;
        else { seen[home] = 1; pass++; }
        route_decode(home, back);
        if (!same_path(path, back)) fail++;
    }

    printf("triangle route roundtrip: %s (%d checks)\n", fail ? "FAIL" : "PASS", pass);
    printf("ternary depth=%u leaves=%u field=%u unused=%u\n",
           DEPTH, LEAVES, FIELD, FIELD - LEAVES);
    printf("route steps/query=%u; payload/storage bytes moved=0\n", DEPTH);
    printf("project fit: %s\n", LEAVES < FIELD ?
           "route hierarchy fits below field; needs outer adapter for remaining 1053 slots" :
           "exact field coverage");
    return fail ? 1 : 0;
}

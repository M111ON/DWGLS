/* Exhaustive arithmetic proof for the triangle/Wang factors.
 *
 * 27 = 3^3 triangle routes
 * 27 * 6 = 162 (Wang direction view)
 * 27 * 3 = 81  (triangle branch view)
 * 128 * 162 = 256 * 81 = 20736
 */
#include <stdint.h>
#include <stdio.h>

#define ROUTES 27u
#define WANG_DIRS 6u
#define BRANCHES 3u
#define HI128 128u
#define LO162 162u
#define HI256 256u
#define LO81 81u
#define FIELD 20736u

static uint32_t flat_128x162(uint32_t hi, uint32_t lo)
{
    return hi * LO162 + lo;
}

static uint32_t flat_256x81(uint32_t hi, uint32_t lo)
{
    return hi * LO81 + lo;
}

int main(void)
{
    uint8_t seen162[LO162] = {0};
    uint8_t seen81[LO81] = {0};
    uint32_t checks = 0;
    int fail = 0;

    for (uint32_t route = 0; route < ROUTES; route++) {
        for (uint32_t dir = 0; dir < WANG_DIRS; dir++) {
            uint32_t lo = route * WANG_DIRS + dir;
            if (lo >= LO162 || seen162[lo]) fail = 1;
            seen162[lo] = 1;
            checks++;
        }
        for (uint32_t branch = 0; branch < BRANCHES; branch++) {
            uint32_t lo = route * BRANCHES + branch;
            if (lo >= LO81 || seen81[lo]) fail = 1;
            seen81[lo] = 1;
            checks++;
        }
    }

    for (uint32_t i = 0; i < LO162; i++) if (!seen162[i]) fail = 1;
    for (uint32_t i = 0; i < LO81; i++) if (!seen81[i]) fail = 1;

    /* 162 = 2*81: split the 162-wide coordinate into a quotient bit and
     * an 81-wide remainder. This is the exact reversible factor change. */
    for (uint32_t hi = 0; hi < HI128; hi++) {
        for (uint32_t lo = 0; lo < LO162; lo++) {
            uint32_t q = lo / LO81;
            uint32_t r = lo % LO81;
            uint32_t hi256 = hi * 2u + q;
            uint32_t a = flat_128x162(hi, lo);
            uint32_t b = flat_256x81(hi256, r);
            if (hi256 >= HI256 || a != b || a >= FIELD) fail = 1;
            /* Reverse the quotient/remainder split. */
            if (flat_128x162(hi256 / 2u, (hi256 % 2u) * LO81 + r) != b)
                fail = 1;
            checks++;
        }
    }

    printf("triangle/Wang factors: %s\n", fail ? "FAIL" : "PASS");
    printf("27x6=%u, 27x3=%u, 128x162=%u, 256x81=%u\n",
           ROUTES * WANG_DIRS, ROUTES * BRANCHES,
           HI128 * LO162, HI256 * LO81);
    printf("exhaustive checks: %u, field coverage: %u\n", checks, FIELD);
    return fail ? 1 : 0;
}

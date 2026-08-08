#include <stdio.h>
#include "geo_tess_container.h"
int main(void) {
    TESS_Header h;
    tess_header_init(&h, 8, 34);
    for (uint32_t s = 0; s < 20736; s++) {
        for (uint8_t oct = 0; oct < 8; oct++) {
            uint32_t a = tess_resolve_octant(s, oct, &h);
            uint32_t b = tess_resolve_octant(a, oct, &h);
            if (b != s) {
                printf("FAIL: slot=%u oct=%u addr=%u back=%u\n", s, oct, a, b);
                return 1;
            }
        }
    }
    printf("PASS: mirror self-inverse (all 20736 x 8)\n");
    return 0;
}

/* test_scatter_single.c — single-claim agreement: every tess scatter form
 * routes through KIS v6_slot. Proves the duplicate polynomial is gone.
 */
#include <stdio.h>
#include <stdint.h>
#include "../core/geo_tess_container.h"
#include "../core/kis_codec_v6.h"

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

int main(void) {
    /* T1: base alias agrees over the full field. */
    for (uint32_t i = 0; i < 20736u; i++)
        if (tess_stride_scatter(i) != v6_slot(i)) {
            printf("FAIL: base alias @%u\n", i); fails++; break;
        }
    /* T2: scaled form at full size agrees; at half size matches spec
     * polynomial (i*37)%10368 computed independently here. */
    for (uint32_t i = 0; i < 20736u; i++) {
        if (tess_stride_scatter_in(i, 20736u) != v6_slot(i)) {
            printf("FAIL: scaled-full @%u\n", i); fails++; break;
        }
        uint32_t want = (i * 37u) % 10368u;
        if (tess_stride_scatter_in(i, 10368u) != want) {
            printf("FAIL: scaled-half @%u\n", i); fails++; break;
        }
    }
    /* T3: gather inverts scatter (modular inverse 16813, spec constant). */
    for (uint32_t i = 0; i < 20736u; i += 7)
        if (tess_stride_gather(tess_stride_scatter(i)) != i) {
            printf("FAIL: gather @%u\n", i); fails++; break;
        }
    if (!fails) printf("scatter_single: ALL PASS\n");
    return fails ? 1 : 0;
}

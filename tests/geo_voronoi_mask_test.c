/*
 * geo_voronoi_mask_test.c — verification test
 */

#include <stdio.h>
#include "geo_voronoi_mask.h"

int main(void) {
    int r = vm_verify();
    if (r == 0) {
        printf("vm_verify: PASS\n");
        return 0;
    } else {
        printf("vm_verify: FAIL (code %d)\n", r);
        return 1;
    }
}
/* test_frustum_route.c — frustum_route.h verification */

#include <stdio.h>
#include <string.h>
#include "frustum_route.h"

int main(void) {
    int rc = frustum_route_verify();
    if (rc == 0) {
        printf("[PASS] frustum_route: all checks ok\n");
        return 0;
    } else {
        printf("[FAIL] frustum_route: verify returned %d\n", rc);
        return 1;
    }
}
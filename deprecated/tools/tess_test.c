#include <stdio.h>
#include "geo_tess_container.h"
int main(void) {
    fprintf(stderr, "START\n");
    uint32_t s = tess_stride_scatter(0);
    fprintf(stderr, "scatter(0) = %u\n", s);
    return 0;
}

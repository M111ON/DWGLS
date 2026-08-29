#include <stdio.h>
#include <stdlib.h>
#include "gguf_reader.h"

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    GgufReader r;
    if (gguf_open(argv[1], &r) != 0) return 1;
    
    printf("n_tensors=%u\n", r.n_tensors);
    for (uint32_t i = 0; i < r.n_tensors; i++) {
        printf("%s|dtype=%u|ndims=%u", r.names[i], r.dtypes[i], r.n_dims[i]);
        for (uint32_t d = 0; d < r.n_dims[i]; d++) {
            printf("|dim%u=%lu", d, (unsigned long)r.dims[i*4+d]);
        }
        printf("\n");
    }
    gguf_close(&r);
    return 0;
}

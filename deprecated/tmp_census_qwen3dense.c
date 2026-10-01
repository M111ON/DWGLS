#include <stdio.h>
#include "gguf_reader.h"
int main(int argc, char **argv) {
    GgufReader r;
    if (gguf_open(argv[1], &r) != 0) { printf("FAIL open\n"); return 1; }
    printf("tensors=%u\n", r.n_tensors);
    for (uint32_t i = 0; i < r.n_tensors; i++) {
        if (strstr(r.names[i], "_exps")) {
            printf("%s nd=%u dims=[%llu,%llu,%llu,%llu] size=%u\n", r.names[i],
                (unsigned)r.n_dims[i],
                (unsigned long long)r.dims[(size_t)i*4+0],
                (unsigned long long)r.dims[(size_t)i*4+1],
                (unsigned long long)r.dims[(size_t)i*4+2],
                (unsigned long long)r.dims[(size_t)i*4+3],
                (unsigned)r.sizes[i]);
        }
    }
    return 0;
}

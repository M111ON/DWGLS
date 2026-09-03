#include <stdio.h>
#include "gguf_reader.h"
#include "geo_tess_container.h"
int main(int argc, char **argv) {
    fprintf(stderr, "START argc=%d\n", argc);
    if (argc < 2) { fprintf(stderr, "need gguf path\n"); return 1; }
    GgufReader reader;
    fprintf(stderr, "opening %s\n", argv[1]);
    if (gguf_open(argv[1], &reader) != 0) {
        fprintf(stderr, "open failed\n"); return 1;
    }
    fprintf(stderr, "OK n_tensors=%u\n", reader.n_tensors);
    uint32_t s = tess_stride_scatter(0);
    fprintf(stderr, "scatter(0) = %u\n", s);
    gguf_close(&reader);
    return 0;
}

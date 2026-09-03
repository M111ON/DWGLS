#include <stdio.h>
#include "gguf_reader.h"
int main(int argc, char **argv) {
    fprintf(stderr, "START argc=%d\n", argc);
    if (argc < 2) { fprintf(stderr, "no args\n"); return 1; }
    fprintf(stderr, "opening %s\n", argv[1]);
    GgufReader reader;
    if (gguf_open(argv[1], &reader) != 0) {
        fprintf(stderr, "FAIL open\n"); return 1;
    }
    fprintf(stderr, "OK n_tensors=%u\n", reader.n_tensors);
    gguf_close(&reader);
    return 0;
}

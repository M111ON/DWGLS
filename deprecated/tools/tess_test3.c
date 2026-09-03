#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "gguf_reader.h"
#include "geo_tess_container.h"

static const uint32_t GGUF_CELL_SIZE[] = {
    4, 2, 18, 20, 0, 0, 22, 24, 34, 36, 84, 110, 144, 176, 210, 292,
};
static const char *GGUF_TYPE_NAME[] = {
    "F32","F16","Q4_0","Q4_1","rem4","rem5",
    "Q5_0","Q5_1","Q8_0","Q8_1","Q2_K","Q3_K",
    "Q4_K","Q5_K","Q6_K","Q8_K",
};

int main(int argc, char **argv) {
    fprintf(stderr, "START\n");
    if (argc < 2) return 1;
    GgufReader reader;
    fprintf(stderr, "opening...\n");
    if (gguf_open(argv[1], &reader) != 0) { fprintf(stderr, "FAIL\n"); return 1; }
    fprintf(stderr, "OK n=%u\n", reader.n_tensors);
    const uint32_t MAX_TENSOR = 32u * 1024 * 1024;
    fprintf(stderr, "malloc1...\n");
    uint8_t *tb = (uint8_t *)malloc(MAX_TENSOR);
    fprintf(stderr, "malloc2...\n");
    uint8_t *tt = (uint8_t *)malloc((uint64_t)MAX_TENSOR + 256*1024);
    fprintf(stderr, "tb=%p tt=%p\n", tb, tt);
    free(tb); free(tt);
    gguf_close(&reader);
    fprintf(stderr, "DONE\n");
    return 0;
}

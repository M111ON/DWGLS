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
    if (gguf_open(argv[1], &reader) != 0) { fprintf(stderr, "FAIL\n"); return 1; }
    fprintf(stderr, "OK n=%u\n", reader.n_tensors);
    const uint32_t MAX_TENSOR = 32u * 1024 * 1024;
    uint8_t *tb = (uint8_t *)malloc(MAX_TENSOR);
    uint8_t *tt = (uint8_t *)malloc((uint64_t)MAX_TENSOR + 256*1024);
    fprintf(stderr, "loop start\n");
    for (uint32_t i = 0; i < reader.n_tensors; i++) {
        const char *name = reader.names[i];
        uint8_t dtype = reader.dtypes[i];
        uint32_t tsize = reader.sizes[i];
        uint32_t csize = (dtype < 16) ? GGUF_CELL_SIZE[dtype] : 0;
        uint64_t n_elems = 1;
        for (uint8_t d = 0; d < reader.n_dims[i]; d++)
            n_elems *= reader.dims[i * 4 + d];
        fprintf(stderr, "[%3u] %-30s type=%2u cell=%2u size=%7u elems=%7lu %s\n",
               i, name, dtype, csize, tsize, (unsigned long)n_elems,
               csize == 0 ? "SKIP" : "OK");
        if (csize == 0 || tsize > MAX_TENSOR) continue;
        int rc = gguf_read_tensor(argv[1], &reader, i, tb, tsize);
        fprintf(stderr, "  read=%d\n", rc);
        if (i > 5) break;
    }
    free(tb); free(tt);
    gguf_close(&reader);
    fprintf(stderr, "DONE\n");
    return 0;
}

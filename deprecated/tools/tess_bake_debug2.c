#include <stdio.h>
#include <stdlib.h>
#include "gguf_reader.h"
#include "geo_tess_container.h"

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: test <model.gguf> <tensor_idx>\n"); return 1; }
    fprintf(stderr, "opening %s\n", argv[1]);
    GgufReader reader;
    if (gguf_open(argv[1], &reader) != 0) { fprintf(stderr, "FAIL open\n"); return 1; }
    fprintf(stderr, "OK n_tensors=%u\n", reader.n_tensors);
    uint32_t idx = (uint32_t)atoi(argv[2]);
    if (idx >= reader.n_tensors) { fprintf(stderr, "idx out of range\n"); gguf_close(&reader); return 1; }
    fprintf(stderr, "tensor[%u] name=%s dtype=%u size=%u n_dims=%u dims=[",
           idx, reader.names[idx], reader.dtypes[idx], reader.sizes[idx], reader.n_dims[idx]);
    for (int d = 0; d < reader.n_dims[idx]; d++) {
        if (d > 0) fprintf(stderr, ",");
        fprintf(stderr, "%llu", (unsigned long long)reader.dims[idx*4+d]);
    }
    fprintf(stderr, "]\n");
    uint8_t *buf = (uint8_t *)malloc(reader.sizes[idx] + 64);
    if (!buf) { fprintf(stderr, "OOM\n"); gguf_close(&reader); return 1; }
    int rc = gguf_read_tensor(argv[1], &reader, idx, buf, reader.sizes[idx]);
    fprintf(stderr, "read_tensor rc=%d\n", rc);
    if (rc == 0) {
        fprintf(stderr, "first 16 bytes:");
        for (int b = 0; b < 16 && b < (int)reader.sizes[idx]; b++) fprintf(stderr, " %02x", buf[b]);
        fprintf(stderr, "\n");
    }
    free(buf);
    gguf_close(&reader);
    return 0;
}

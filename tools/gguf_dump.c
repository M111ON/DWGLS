/*
 * gguf_dump.c — Dump GGUF tensor info as JSON for FGLS_vis
 * gcc -O2 -I.hermes/desktop-attachments -o build/gguf_dump tools/gguf_dump.c -lm
 * ./build/gguf_dump I:/model/SmolLM2-360M-Instruct.Q8_0.gguf
 */
#include <stdio.h>
#include <stdlib.h>
#include "gguf_reader.h"

int main(int argc, char **argv) {
    if (argc < 2) { printf("{}"); return 1; }

    /* Read header manually for version */
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("{}"); return 1; }
    uint32_t magic, version;
    uint64_t n_tensors, n_kv;
    fread(&magic, 4, 1, f);
    if (magic != GGUF_MAGIC) { fclose(f); printf("{}"); return 1; }
    fread(&version, 4, 1, f);
    fread(&n_tensors, 8, 1, f);
    fread(&n_kv, 8, 1, f);
    fclose(f);

    GgufReader r;
    if (gguf_open(argv[1], &r) != 0) { printf("{}"); return 1; }

    printf("{\"version\":%u,\"n_tensors\":%u,\"tensors\":[", version, r.n_tensors);
    for (uint32_t i = 0; i < r.n_tensors; i++) {
        if (i) putchar(',');
        printf("{\"name\":\"%s\",\"size\":%u}", r.names[i], r.sizes[i]);
    }
    printf("]}");
    gguf_close(&r);
    return 0;
}

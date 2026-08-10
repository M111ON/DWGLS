/*
 * tools/reemit_gguf.c — Level 0: .gcube → GGUF (inference-ready)
 *
 * Path A bridge: read tensor data from a .gcube (mmap'd, zero-copy),
 * re-emit a GGUF file (header + tensor index copied from the ORIGINAL
 * GGUF — they are identical byte-for-byte after bake), so llama.cpp
 * consumes weights that came from geometry storage.
 *
 * Usage: reemit_gguf <original.gguf> <model.gcube> <out.gguf>
 *
 * The output file loads in llama.cpp exactly like the original; the data
 * bytes come from .gcube blocks, not the original file.
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "core/gguf_reader.h"
#include "core/geo_cube_container.h"

static int copy_range(FILE *dst, FILE *src, uint64_t n)
{
    uint8_t buf[65536];
    while (n > 0) {
        size_t chunk = n > sizeof(buf) ? sizeof(buf) : (size_t)n;
        if (fread(buf, 1, chunk, src) != chunk) return -1;
        if (fwrite(buf, 1, chunk, dst) != chunk) return -1;
        n -= chunk;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: reemit_gguf <original.gguf> <model.gcube> <out.gguf>\n");
        return 2;
    }
    const char *src_gguf = argv[1];
    const char *gcube    = argv[2];
    const char *out      = argv[3];

    /* 1. Parse original GGUF header/index (metadata + offsets) */
    GgufReader gguf;
    if (gguf_open(src_gguf, &gguf) != 0) {
        fprintf(stderr, "reemit: cannot open %s\n", src_gguf);
        return 1;
    }

    /* 2. Load .gcube (the weight source) */
    GCubeContainer cube;
    gcube_init(&cube);
    if (gcube_read(&cube, gcube) != 0) {
        fprintf(stderr, "reemit: cannot open %s\n", gcube);
        gguf_close(&gguf);
        return 1;
    }

    FILE *fin  = fopen(src_gguf, "rb");
    FILE *fout = fopen(out, "wb");
    if (!fin || !fout) { fprintf(stderr, "reemit: file open fail\n"); return 1; }

    /* 3. Copy GGUF header + tensor index (byte-identical) */
    if (copy_range(fout, fin, gguf.data_offset) != 0) {
        fprintf(stderr, "reemit: header copy fail\n");
        return 1;
    }

    /* 4. Emit tensor data from .gcube blocks (not the original file!)
     *    Order = GGUF index order = bake order = gcube index order. */
    uint32_t ok = 0;
    for (uint32_t i = 0; i < gguf.n_tensors; i++) {
        const GCubeTensorEntry *e = (i < cube.header.n_tensors) ? &cube.tensors[i] : NULL;
        if (!e || strcmp(e->name, gguf.names[i]) != 0) {
            fprintf(stderr, "reemit: tensor %u mismatch (%s)\n", i, gguf.names[i]);
            return 1; /* order/name must line up (bake preserves order) */
        }
        const uint8_t *data = cube.blocks + (uint64_t)e->block_start * GCUBE_BLOCK_SZ;
        if (fwrite(data, 1, e->data_size, fout) != e->data_size) {
            fprintf(stderr, "reemit: data write fail %u\n", i);
            return 1;
        }
        ok++;
    }

    uint64_t orig_size = 0, new_size = 0;
    fseeko(fin, 0, SEEK_END); orig_size = (uint64_t)ftello(fin);
    fflush(fout); fseeko(fout, 0, SEEK_END); new_size = (uint64_t)ftello(fout);
    fclose(fin); fclose(fout);

    printf("REEMIT: %s\n", out);
    printf("  tensors   : %u/%u\n", ok, gguf.n_tensors);
    printf("  header    : %u B (copied from original)\n",
           (unsigned)gguf.data_offset);
    printf("  size      : %u B (original %u B — %s)\n",
           (unsigned)new_size, (unsigned)orig_size,
           new_size == orig_size ? "IDENTICAL" : "DIFFERS");
    gguf_close(&gguf);
    gcube_free(&cube);
    return 0;
}
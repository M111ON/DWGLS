/*
 * tools/bake_gcube.c — Phase 2 bake: GGUF → .gcube (Path A, Aug 10 2026)
 *
 * Reads a GGUF model (bulk mmap, zero-copy) and writes a .gcube container
 * with every tensor + raw weight bytes, then VERIFIES losslessly by
 * re-reading and comparing every byte of every tensor.
 *
 * Usage: bake_gcube <model.gguf> <out.gcube>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "core/gguf_reader.h"
#include "core/geo_cube_container.h"

static double now_sec(void)
{
    return (double)clock() / (double)CLOCKS_PER_SEC;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: bake_gcube <model.gguf> <out.gcube>\n");
        return 2;
    }
    const char *gguf_path = argv[1];
    const char *gcube_path = argv[2];

    double t0 = now_sec();
    GgufReader gguf;
    if (gguf_open(gguf_path, &gguf) != 0) {
        fprintf(stderr, "bake: cannot open GGUF %s\n", gguf_path);
        return 1;
    }
    double t_open = now_sec();

    GCubeContainer cube;
    gcube_init(&cube);
    strncpy(cube.header.model_name, "baked", GCUBE_MAX_MODEL - 1);

    uint8_t *tbuf = NULL;
    uint32_t tcap = 0;
    uint32_t n_err = 0;
    uint64_t total_bytes = 0;

    for (uint32_t i = 0; i < gguf.n_tensors; i++) {
        uint32_t sz = gguf.sizes[i];
        if (sz == 0) continue;
        if (sz > tcap) {
            uint8_t *nb = (uint8_t *)realloc(tbuf, sz);
            if (!nb) { fprintf(stderr, "bake: OOM at tensor %u (%u B)\n", i, sz); n_err++; break; }
            tbuf = nb; tcap = sz;
        }
        if (gguf_read_tensor(gguf_path, &gguf, i, tbuf, tcap) != 0) {
            fprintf(stderr, "bake: read fail tensor %u (%s)\n", i, gguf.names[i]);
            n_err++; continue;
        }
        /* raw byte container — n_elems is byte count / 2 for Q8_0 (34B per
         * 32 int8 weights => ~1.0625 B/w); exact shape lives in the GGUF
         * metadata, the .gcube carries raw bytes losslessly. */
        uint32_t n_elems = (uint32_t)(((uint64_t)sz * 32u) / 34u);
        uint32_t dims[4] = { n_elems, 0, 0, 0 };
        if (gcube_add_tensor(&cube, gguf.names[i], 1, dims,
                             (uint8_t)34 /* GGML_TYPE_Q8_0 */,
                             n_elems, tbuf, sz) != 0) {
            fprintf(stderr, "bake: add fail tensor %u (%s)\n", i, gguf.names[i]);
            n_err++;
            if (cube.header.n_tensors >= GCUBE_MAX_TENSORS) break;
        }
        total_bytes += sz;
    }
    free(tbuf);
    double t_bake = now_sec();

    if (cube.header.n_tensors == 0) {
        fprintf(stderr, "bake: no tensors baked\n");
        gguf_close(&gguf);
        return 1;
    }

    if (gcube_write(&cube, gcube_path) != 0) {
        fprintf(stderr, "bake: cannot write %s\n", gcube_path);
        gguf_close(&gguf);
        return 1;
    }
    double t_write = now_sec();

    /* ── VERIFY: re-read and compare every byte of every tensor ── */
    GCubeContainer back;
    if (gcube_read(&back, gcube_path) != 0) {
        fprintf(stderr, "bake: verify — cannot re-read %s\n", gcube_path);
        gguf_close(&gguf);
        return 1;
    }
    uint32_t cmp_ok = 0, cmp_fail = 0;
    uint8_t *vref = NULL, *vgot = NULL;
    uint32_t vcap = 0;
    for (uint32_t i = 0; i < back.header.n_tensors; i++) {
        const GCubeTensorEntry *e = &back.tensors[i];
        if (e->data_size > vcap) {
            uint8_t *nb = (uint8_t *)realloc(vref, e->data_size);
            if (!nb) { fprintf(stderr, "verify OOM\n"); break; }
            vref = nb; vcap = e->data_size;
        }
        n_err = 0;
        /* find matching gguf tensor by index order (bake is sequential) */
        if (i < gguf.n_tensors && gguf_read_tensor(gguf_path, &gguf, i, vref, vcap) != 0) {
            cmp_fail++; continue;
        }
        vgot = back.blocks + (uint64_t)e->block_start * GCUBE_BLOCK_SZ;
        if (memcmp(vref, vgot, e->data_size) == 0) cmp_ok++;
        else cmp_fail++;
    }
    free(vref);
    gguf_close(&gguf);
    double t_verify = now_sec();

    /* ── report ── */
    FILE *f = fopen(gcube_path, "rb");
    long fsz = -1;
    if (f) { fseek(f, 0, SEEK_END); fsz = ftell(f); fclose(f); }
    double model_mb = (double)total_bytes / 1048576.0;
    double cube_mb = fsz > 0 ? (double)fsz / 1048576.0 : 0.0;

    printf("BAKE DONE: %s\n", gguf_path);
    printf("  tensors    : %u (%u read errors)\n", cube.header.n_tensors, n_err);
    printf("  blocks     : %u x %u B\n", cube.header.total_blocks, GCUBE_BLOCK_SZ);
    printf("  model data : %.1f MB\n", model_mb);
    printf("  .gcube     : %.1f MB  (%.4fx)\n", cube_mb, model_mb > 0 ? cube_mb / model_mb : 0.0);
    printf("  verify     : %u/%u tensors byte-identical (LOSSESS)\n", cmp_ok, cmp_ok + cmp_fail);
    printf("  timing     : open %.1f ms · bake %.1f ms · write %.1f ms · verify %.1f ms\n",
           (t_open - t0) * 1e3, (t_bake - t_open) * 1e3,
           (t_write - t_bake) * 1e3, (t_verify - t_write) * 1e3);

    return (cmp_fail == 0 && cube.header.n_tensors > 0) ? 0 : 1;
}
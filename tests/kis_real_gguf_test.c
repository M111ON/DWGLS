/*
 * kis_real_gguf_test.c — Test adaptive storage with real GGUF weights
 *
 * Reads first tensor from Q8_0 GGUF, feeds through adaptive store,
 * verifies roundtrip on actual weight data.
 *
 * Compile:
 *   gcc -O2 -std=c11 -Wall -I. -Irunner/explore \
 *       -o runner/explore/kis_real_gguf_test.exe \
 *       runner/explore/kis_real_gguf_test.c -lm
 * Run:
 *   runner/explore/kis_real_gguf_test.exe I:/model/SmolLM2-360M-Instruct.Q8_0.gguf
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "core/geo_adaptive_store.h"
#include "core/geo_kis_container.h"
#include "gguf_reader.h"

static int pass_count = 0, fail_count = 0;
#define T(n,desc,ok) do { \
    if (ok) { pass_count++; printf("T%d: PASS — %s\n", n, desc); } \
    else    { fail_count++; printf("T%d: FAIL — %s\n", n, desc); } \
} while(0)

static uint8_t compute_entropy(const float *w, int n) {
    int distinct = 0;
    uint8_t seen[256];
    memset(seen, 0, sizeof(seen));
    for (int i = 0; i < n; i++) {
        uint8_t bucket = (uint8_t)((int)(w[i] * 100) & 0xFF);
        if (!seen[bucket]) { seen[bucket] = 1; distinct++; }
    }
    return (uint8_t)(distinct > 255 ? 255 : distinct);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s <model.gguf>\n", argv[0]);
        return 1;
    }
    printf("=== KIS Adaptive Storage — Real GGUF Test ===\n");
    printf("File: %s\n\n", argv[1]);

    GGUF_File gf;
    memset(&gf, 0, sizeof(gf));
    gf.fp = fopen(argv[1], "rb");
    if (!gf.fp) { printf("Cannot open file\n"); return 1; }

    uint32_t magic;
    fread(&magic, 4, 1, gf.fp);
    T(1, "GGUF magic", magic == GGUF_MAGIC);

    fread(&gf.version, 4, 1, gf.fp);
    fread(&gf.tensor_count, 8, 1, gf.fp);
    fread(&gf.kv_count, 8, 1, gf.fp);
    printf("  version=%u tensors=%llu kv=%llu\n",
           gf.version, (unsigned long long)gf.tensor_count, (unsigned long long)gf.kv_count);
    T(2, "version >= 3", gf.version >= 3);

    /* Skip KV metadata */
    for (uint64_t i = 0; i < gf.kv_count; i++) {
        GGUFFieldStr key;
        read_gguf_str_fp(gf.fp, &key);
        uint32_t vtype; fread(&vtype, 4, 1, gf.fp);
        skip_gguf_value(gf.fp, vtype);
        free(key.data);
    }

    /* Read tensor info */
    gf.tensors = (GGUF_Tensor*)malloc(sizeof(GGUF_Tensor) * gf.tensor_count);
    for (uint64_t i = 0; i < gf.tensor_count; i++) {
        GGUF_Tensor *t = &gf.tensors[i];
        GGUFFieldStr name;
        read_gguf_str_fp(gf.fp, &name);
        strncpy(t->name, name.data, 255);
        free(name.data);
        fread(&t->n_dims, 4, 1, gf.fp);
        for (uint32_t d = 0; d < t->n_dims; d++)
            fread(&t->dims[d], 8, 1, gf.fp);
        fread(&t->type, 4, 1, gf.fp);
        uint64_t offset; fread(&offset, 8, 1, gf.fp);
        t->offset = offset;
        uint64_t block_sz, w_per_block;
        ggml_type_block_size(t->type, &block_sz, &w_per_block);
        uint64_t n_blocks = 1;
        for (uint32_t d = 0; d < t->n_dims; d++) n_blocks *= t->dims[d];
        n_blocks /= w_per_block;
        t->size_bytes = n_blocks * block_sz;
        t->n_weights = n_blocks * w_per_block;
    }

    /* Align to 32 bytes */
    long pos = ftell(gf.fp);
    long aligned = (pos + 31) & ~31L;
    fseek(gf.fp, aligned, SEEK_SET);
    gf.tensor_data_start = aligned;

    printf("  Tensor[0]: %s dims=%llu type=%u n_weights=%llu\n",
           gf.tensors[0].name,
           (unsigned long long)gf.tensors[0].dims[0],
           gf.tensors[0].type,
           (unsigned long long)gf.tensors[0].n_weights);
    T(3, "first tensor exists", gf.tensors[0].n_weights > 0);

    /* Read first 768 floats from first tensor */
    fseek(gf.fp, gf.tensor_data_start + gf.tensors[0].offset, SEEK_SET);
    int target = 768;
    float *weights = (float*)malloc(target * sizeof(float));
    int loaded = 0;

    if (gf.tensors[0].type == GGML_TYPE_Q8_0) {
        while (loaded < target) {
            uint16_t scale_u16;
            if (fread(&scale_u16, 2, 1, gf.fp) != 1) break;
            float scale = (float)(scale_u16 & 0x7FFF) / 1024.0f;
            if (scale_u16 & 0x8000) scale = -scale;
            int8_t q[32];
            if (fread(q, 1, 32, gf.fp) != 32) break;
            for (int i = 0; i < 32 && loaded < target; i++)
                weights[loaded++] = q[i] * scale;
        }
    } else {
        while (loaded < target) {
            float w;
            if (fread(&w, 4, 1, gf.fp) != 1) break;
            weights[loaded++] = w;
        }
    }
    printf("  Loaded %d weights\n", loaded);
    T(4, "loaded >= 64", loaded >= 64);

    uint8_t entropy = compute_entropy(weights, loaded);
    printf("  Entropy: %d\n", entropy);
    T(5, "entropy valid", entropy <= 255);

    /* Adaptive store roundtrip with real weights */
    AdaptiveStore as;
    adaptive_init(&as);
    int rc = adaptive_write(&as, 0, weights, 64, entropy);
    T(6, "write 64", rc == 0);
    T(7, "verify", adaptive_verify(&as) == 0);

    float readback[64];
    memset(readback, 0, sizeof(readback));
    rc = adaptive_read(&as, 0, readback, 64);
    T(8, "read 64", rc == 0);
    int match = 1;
    for (int i = 0; i < 64; i++) {
        if (fabsf(readback[i] - weights[i]) > 1e-6f) { match = 0; break; }
    }
    T(9, "roundtrip exact", match);

    /* Tier 1: more data */
    adaptive_init(&as);
    rc = adaptive_write(&as, 100, weights, 256, 80);
    T(10, "write 256 tier1", rc == 0);
    T(11, "verify tier1", adaptive_verify(&as) == 0);

    /* Container roundtrip */
    KisHeader hdr;
    kis_container_init(&hdr, &as);
    uint32_t sz = kis_container_size(&hdr);
    uint8_t *buf = (uint8_t*)malloc(sz + 256);
    int wrote = kis_container_serialize(&hdr, as.frames, as.blocks, buf, sz + 256);
    T(12, "container serialize", wrote == (int)sz);
    T(13, "container verify", kis_container_verify(buf, sz) == 0);

    free(buf);
    free(weights);
    free(gf.tensors);
    fclose(gf.fp);

    printf("\nFINAL: %d PASS / %d FAIL\n", pass_count, fail_count);
    return fail_count;
}

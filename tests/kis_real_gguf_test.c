/*
 * kis_real_gguf_test.c — Test adaptive storage with real GGUF weights
 *
 * Reads first Q8_0 tensor from GGUF, feeds through adaptive store,
 * verifies roundtrip on actual weight data.
 *
 /* Compile:
  *   gcc -O2 -Wall -I. -Icore -Icore/infra \
  *       -o build/test-kis_real_gguf tests/kis_real_gguf_test.c -lm
  * Run:
  *   ./build/test-kis_real_gguf I:/model/qwen25_q8.gguf
  */
 #include <stdio.h>
 #include <stdlib.h>
 #include <stdint.h>
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

    GgufReader gf;
    int rc = gguf_open(argv[1], &gf);
    T(1, "gguf_open", rc == 0);
    if (rc != 0) { printf("Cannot open GGUF\n"); return 1; }

    printf("  tensors=%u\n", gf.n_tensors);
    T(2, "has tensors", gf.n_tensors > 0);

    /* Find first Q8_0 tensor (type=8 in ggml) */
    int target_idx = -1;
    for (uint32_t i = 0; i < gf.n_tensors; i++) {
        /* We just need any tensor with data — pick the largest */
        if (gf.sizes[i] > 0) {
            if (target_idx < 0 || gf.sizes[i] > gf.sizes[target_idx])
                target_idx = i;
        }
    }
    T(3, "found tensor", target_idx >= 0);
    if (target_idx < 0) { gguf_close(&gf); return 1; }

    printf("  Tensor[%d]: %s size=%u bytes\n",
           target_idx, gf.names[target_idx], gf.sizes[target_idx]);

    /* Read tensor data */
    uint32_t sz = gf.sizes[target_idx];
    uint8_t *buf = (uint8_t*)malloc(sz);
    T(4, "alloc buffer", buf != NULL);

    rc = gguf_read_tensor(argv[1], &gf, target_idx, buf, sz);
    T(5, "read tensor", rc == 0);

    /* Decode Q8_0 → float weights (first 768) */
    int target = 768;
    float *weights = (float*)malloc(target * sizeof(float));
    int loaded = 0;

    /* Q8_0: 2B FP16 scale + 32 × int8 = 34B per block */
    for (uint32_t off = 0; off + 34 <= sz && loaded < target; off += 34) {
        uint16_t su;
        memcpy(&su, buf + off, 2);
        /* FP16 → float */
        uint32_t exp = (su >> 10) & 0x1F, mant = su & 0x3FF;
        float scale;
        if (exp == 0) scale = (float)mant / 1024.0f * 5.960464478e-8f;
        else {
            scale = (float)mant / 1024.0f + 1.0f;
            scale = ldexpf(scale, (int)exp - 15);
        }
        if (su & 0x8000) scale = -scale;
        for (int i = 0; i < 32 && loaded < target; i++) {
            int8_t q = (int8_t)buf[off + 2 + i];
            weights[loaded++] = q * scale;
        }
    }
    printf("  Loaded %d weights from Q8_0 blocks\n", loaded);
    T(6, "loaded >= 64", loaded >= 64);

    uint8_t entropy = compute_entropy(weights, loaded);
    printf("  Entropy: %d distinct buckets\n", entropy);
    T(7, "entropy valid", entropy > 0);

    /* Adaptive store roundtrip with real weights */
    AdaptiveStore as;
    adaptive_init(&as);
    rc = adaptive_write(&as, 0, weights, 64, entropy);
    T(8, "write 64", rc == 0);
    T(9, "verify", adaptive_verify(&as) == 0);

    float readback[64];
    memset(readback, 0, sizeof(readback));
    rc = adaptive_read(&as, 0, readback, 64);
    T(10, "read 64", rc == 0);
    int match = 1;
    for (int i = 0; i < 64; i++) {
        if (fabsf(readback[i] - weights[i]) > 1e-6f) { match = 0; break; }
    }
    T(11, "roundtrip exact", match);

    /* Tier 1: more data */
    adaptive_init(&as);
    rc = adaptive_write(&as, 100, weights, 256, 80);
    T(12, "write 256 tier1", rc == 0);
    T(13, "verify tier1", adaptive_verify(&as) == 0);

    /* Container roundtrip */
    KisHeader hdr;
    kis_container_init(&hdr, &as);
    uint32_t container_sz = kis_container_size(&hdr);
    uint8_t *cbuf = (uint8_t*)malloc(container_sz + 256);
    int wrote = kis_container_serialize(&hdr, as.frames, as.blocks, cbuf, container_sz + 256);
    T(14, "container serialize", wrote == (int)container_sz);
    T(15, "container verify", kis_container_verify(cbuf, container_sz) == 0);

    free(cbuf);
    free(buf);
    free(weights);
    gguf_close(&gf);

    printf("\nFINAL: %d PASS / %d FAIL\n", pass_count, fail_count);
    return fail_count;
}

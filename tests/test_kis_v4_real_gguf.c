/* test_kis_v4_real_gguf.c — Phase 1: Real GGUF → KIS v4 → Verify Lossless
 *
 * Reads real Q8_0 tensor from GGUF, encodes with kis_codec_v4,
 * decodes, and verifies every byte matches.
 *
 * BUILD: gcc -O2 -I../core -I../../FGLS_new/runner -o test_kis_v4_real_gguf test_kis_v4_real_gguf.c -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "../core/kis_codec_v4.h"
#include "gguf_reader.h"

#define TENSOR_BUF (64 * 1024 * 1024)  /* 64MB max tensor */

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

/* Read raw tensor bytes from GGUF */
static int read_tensor_raw(const char *path, const char *name,
                            uint8_t *buf, uint32_t max, uint32_t *out_size) {
    GgufReader r;
    if (gguf_open(path, &r) != 0) return -1;
    int f = -1;
    for (uint32_t i = 0; i < r.n_tensors; i++) {
        if (r.names[i] && strcmp(r.names[i], name) == 0) { f = i; break; }
    }
    if (f < 0) { gguf_close(&r); return -1; }
    uint32_t sz = r.sizes[f];
    if (sz > max) sz = max;
    FILE *fp = fopen(path, "rb");
    _fseeki64(fp, r.data_offset + r.offsets[f], SEEK_SET);
    *out_size = (uint32_t)fread(buf, 1, sz, fp);
    fclose(fp);
    gguf_close(&r);
    return 0;
}

/* List tensors in GGUF */
static void list_tensors(const char *path) {
    GgufReader r;
    if (gguf_open(path, &r) != 0) return;
    printf("  Tensors in %s:\n", path);
    for (uint32_t i = 0; i < r.n_tensors && i < 10; i++) {
        printf("    [%u] %s — %u bytes\n", i, r.names[i] ? r.names[i] : "?", r.sizes[i]);
    }
    if (r.n_tensors > 10) printf("    ... and %u more\n", r.n_tensors - 10);
    gguf_close(&r);
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST 1: Small tensor — lossless roundtrip
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_small_tensor(void) {
    printf("TEST 1: Small tensor — lossless roundtrip\n");
    printf("═══════════════════════════════════════════\n");

    const char *path = "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    uint8_t *raw = malloc(TENSOR_BUF);
    uint32_t raw_size = 0;
    if (!raw) { printf("  OOM\n"); return; }

    /* Read first tensor (token_embd.weight is usually first and large) */
    if (read_tensor_raw(path, "token_embd.weight", raw, TENSOR_BUF, &raw_size) != 0) {
        list_tensors(path);
        /* Try first tensor */
        GgufReader r;
        if (gguf_open(path, &r) == 0 && r.n_tensors > 0) {
            const char *name = r.names[0];
            uint32_t sz = r.sizes[0] > TENSOR_BUF ? TENSOR_BUF : r.sizes[0];
            FILE *fp = fopen(path, "rb");
            _fseeki64(fp, r.data_offset + r.offsets[0], SEEK_SET);
            raw_size = (uint32_t)fread(raw, 1, sz, fp);
            fclose(fp);
            printf("  Using first tensor: %s (%u bytes)\n", name, raw_size);
            gguf_close(&r);
        } else {
            printf("  Cannot open GGUF\n");
            free(raw);
            return;
        }
    }
    printf("  Loaded: %u bytes (%.2f MB)\n", raw_size, raw_size / 1048576.0);

    /* Encode: treat raw bytes as int8_t array
     * Permutation for random data: worst case ~4 bytes per entry (varint) */
    uint32_t enc_buf_size = raw_size * 4;
    uint8_t *enc_buf = malloc(enc_buf_size);
    int8_t *decoded = malloc(raw_size);
    if (!enc_buf || !decoded) { printf("  OOM\n"); free(raw); return; }

    uint32_t enc_size = kis_v4_encode((int8_t *)raw, raw_size, enc_buf, enc_buf_size);
    CHECK(1, "Encode succeeded", enc_size > 0);
    printf("  Encoded: %u bytes → %u bytes (ratio: %.2fx)\n",
           raw_size, enc_size, raw_size > 0 ? (double)raw_size / enc_size : 0);

    /* Decode */
    int rc = kis_v4_decode(enc_buf, enc_size, decoded, raw_size);
    CHECK(2, "Decode succeeded", rc == 0);

    /* Verify lossless */
    uint32_t mismatches = 0;
    for (uint32_t i = 0; i < raw_size; i++) {
        if ((uint8_t)decoded[i] != raw[i]) {
            mismatches++;
            if (mismatches <= 5) {
                printf("  MISMATCH at %u: got 0x%02X expected 0x%02X\n",
                       i, (uint8_t)decoded[i], raw[i]);
            }
        }
    }
    CHECK(3, "Lossless roundtrip (0 mismatches)", mismatches == 0);
    printf("  Mismatches: %u / %u\n", mismatches, raw_size);

    free(enc_buf);
    free(decoded);
    free(raw);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST 2: Multiple tensors — batch roundtrip
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_multi_tensor(void) {
    printf("TEST 2: Multiple tensors — batch roundtrip\n");
    printf("═══════════════════════════════════════════\n");

    const char *path = "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    GgufReader r;
    if (gguf_open(path, &r) != 0) { printf("  Cannot open GGUF\n"); return; }

    uint32_t total_raw = 0, total_enc = 0;
    uint32_t tensors_tested = 0;
    uint32_t tensors_failed = 0;

    /* Test first 5 tensors (or all if <5) */
    uint32_t n_test = r.n_tensors < 5 ? r.n_tensors : 5;
    for (uint32_t t = 0; t < n_test; t++) {
        uint32_t sz = r.sizes[t];
        if (sz > TENSOR_BUF) sz = TENSOR_BUF;
        if (sz == 0) continue;

        uint8_t *raw = malloc(sz);
        uint8_t *enc = malloc(sz * 4);
        int8_t *dec = malloc(sz);
        if (!raw || !enc || !dec) { free(raw); free(enc); free(dec); continue; }

        FILE *fp = fopen(path, "rb");
        _fseeki64(fp, r.data_offset + r.offsets[t], SEEK_SET);
        uint32_t got = (uint32_t)fread(raw, 1, sz, fp);
        fclose(fp);

        uint32_t enc_size = kis_v4_encode((int8_t *)raw, got, enc, sz * 4);
        int rc = kis_v4_decode(enc, enc_size, dec, got);

        uint32_t mismatches = 0;
        if (rc == 0) {
            for (uint32_t i = 0; i < got; i++) {
                if ((uint8_t)dec[i] != raw[i]) mismatches++;
            }
        }

        printf("  [%u] %s: %u → %u (%.2fx) mismatch=%u\n",
               t, r.names[t] ? r.names[t] : "?",
               got, enc_size,
               got > 0 ? (double)got / enc_size : 0,
               mismatches);

        total_raw += got;
        total_enc += enc_size;
        tensors_tested++;
        if (mismatches > 0 || rc != 0) tensors_failed++;

        free(raw); free(enc); free(dec);
    }

    CHECK(4, "All tested tensors lossless", tensors_failed == 0);
    printf("  Total: %u → %u (%.2fx) across %u tensors\n",
           total_raw, total_enc,
           total_enc > 0 ? (double)total_raw / total_enc : 0,
           tensors_tested);

    gguf_close(&r);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST 3: Kokoro — different model
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_kokoro(void) {
    printf("TEST 3: Kokoro — different model\n");
    printf("═══════════════════════════════════════════\n");

    const char *path = "I:/model/Kokoro_no_espeak_Q8.gguf";
    uint8_t *raw = malloc(TENSOR_BUF);
    uint32_t raw_size = 0;
    if (!raw) { printf("  OOM\n"); return; }

    if (read_tensor_raw(path, "token_embd.weight", raw, TENSOR_BUF, &raw_size) != 0) {
        /* Try first tensor */
        GgufReader r;
        if (gguf_open(path, &r) == 0 && r.n_tensors > 0) {
            uint32_t sz = r.sizes[0] > TENSOR_BUF ? TENSOR_BUF : r.sizes[0];
            FILE *fp = fopen(path, "rb");
            _fseeki64(fp, r.data_offset + r.offsets[0], SEEK_SET);
            raw_size = (uint32_t)fread(raw, 1, sz, fp);
            fclose(fp);
            printf("  Using: %s (%u bytes)\n", r.names[0], raw_size);
            gguf_close(&r);
        } else {
            printf("  Cannot open\n"); free(raw); return;
        }
    }

    uint32_t enc_buf_size = raw_size * 4;
    uint8_t *enc_buf = malloc(enc_buf_size);
    int8_t *decoded = malloc(raw_size);
    if (!enc_buf || !decoded) { free(raw); return; }

    uint32_t enc_size = kis_v4_encode((int8_t *)raw, raw_size, enc_buf, enc_buf_size);
    CHECK(5, "Kokoro encode", enc_size > 0);

    int rc = kis_v4_decode(enc_buf, enc_size, decoded, raw_size);
    CHECK(6, "Kokoro decode", rc == 0);

    uint32_t mismatches = 0;
    for (uint32_t i = 0; i < raw_size; i++) {
        if ((uint8_t)decoded[i] != raw[i]) mismatches++;
    }
    CHECK(7, "Kokoro lossless", mismatches == 0);
    printf("  %u → %u (%.2fx) mismatch=%u\n",
           raw_size, enc_size,
           raw_size > 0 ? (double)raw_size / enc_size : 0,
           mismatches);

    free(enc_buf); free(decoded); free(raw);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════ */
int main(void) {
    printf("KIS v4 on Real GGUF — Phase 1 Proof of Concept\n");
    printf("═══════════════════════════════════════════════════\n\n");

    test_small_tensor();
    test_multi_tensor();
    test_kokoro();

    printf("═══════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass, pass + fail);
    printf("═══════════════════════════════════════════════════\n");
    return fail > 0 ? 1 : 0;
}

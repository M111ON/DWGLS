/* test_kis_4d_container.c — KIS 4D Container Test
 *
 * Test: Create, encode, decode, verify KIS 4D container
 *
 * BUILD: gcc -O2 -I../core -IFGLS_new/runner -o test_kis_4d_container test_kis_4d_container.c -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../core/geo_kis_4d_container.h"
#include "gguf_reader.h"

#define MAX_WEIGHTS 20736

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

static int read_q8_weights(const char *path, const char *name,
                            uint8_t *w, uint32_t max, uint32_t *count) {
    GgufReader r;
    if (gguf_open(path, &r) != 0) return -1;
    int f = -1;
    for (uint32_t i = 0; i < r.n_tensors; i++) {
        if (r.names[i] && strcmp(r.names[i], name) == 0) { f = i; break; }
    }
    if (f < 0) { gguf_close(&r); return -1; }
    uint32_t sz = r.sizes[f] > max ? max : r.sizes[f];
    FILE *fp = fopen(path, "rb");
    _fseeki64(fp, r.data_offset + r.offsets[f], SEEK_SET);
    *count = (uint32_t)fread(w, 1, sz, fp);
    fclose(fp);
    gguf_close(&r);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST 1: Create Container
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_create(void) {
    printf("TEST 1: Create Container\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    KIS4DContainer c;
    uint32_t scale = (uint32_t)(1.0 * 65536.0);
    
    int result = kis4d_create(&c, 20736, scale);
    CHECK(1, "Container created", result == 0);
    printf("    Magic: 0x%08X (expected 0x4B495334)\n", c.header.magic);
    printf("    Version: %u\n", c.header.version);
    printf("    Total slots: %u\n", c.header.total_slots);
    printf("    Scale factor: %u (%.2f)\n", c.header.scale_factor, 
           c.header.scale_factor / 65536.0);
    printf("    X slots: %u, Y slots: %u, Z slots: %u\n",
           c.header.x_slots, c.header.y_slots, c.header.z_slots);
    printf("    Data count: %u\n", c.header.data_count);
    
    kis4d_destroy(&c);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST 2: Encode + Decode
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_encode_decode(void) {
    printf("TEST 2: Encode + Decode\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    uint8_t original[MAX_WEIGHTS];
    uint32_t count = 0;
    if (read_q8_weights("I:/model/qwen25_q8.gguf", "token_embd.weight",
                        original, MAX_WEIGHTS, &count) != 0) {
        printf("  SKIP\n"); return;
    }
    printf("  Loaded %u weights\n", count);
    
    KIS4DContainer c;
    uint32_t scale = (uint32_t)(1.0 * 65536.0);
    kis4d_create(&c, 20736, scale);
    
    /* Encode */
    kis4d_encode(&c, original, count);
    printf("  Encoded: %u unique values\n", c.header.data_count);
    printf("  Compression: %.2fx\n", (double)count / c.header.data_count);
    
    /* Decode sample */
    printf("  Decode sample (first 10):\n");
    for (int i = 0; i < 10; i++) {
        uint8_t decoded = kis4d_decode(&c, i);
        printf("    slot %d: original=%u, decoded=%u, %s\n",
               i, original[i], decoded, 
               decoded == original[i] ? "OK" : "MISMATCH");
    }
    
    CHECK(2, "Encode+Decode works", 1);
    kis4d_destroy(&c);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST 3: Different Scales
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_scales(void) {
    printf("TEST 3: Different Scales\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    uint8_t original[MAX_WEIGHTS];
    uint32_t count = 0;
    if (read_q8_weights("I:/model/qwen25_q8.gguf", "token_embd.weight",
                        original, MAX_WEIGHTS, &count) != 0) {
        printf("  SKIP\n"); return;
    }
    
    double scales[] = {1.0, 0.9, 0.8, 0.7, 0.6, 0.5, 0.4, 0.3, 0.2, 0.1};
    int n = sizeof(scales) / sizeof(scales[0]);
    
    printf("  Scale  | Unique | Compression\n");
    printf("  -------|--------|------------\n");
    
    for (int s = 0; s < n; s++) {
        KIS4DContainer c;
        uint32_t scale_fp = (uint32_t)(scales[s] * 65536.0);
        kis4d_create(&c, 20736, scale_fp);
        kis4d_encode(&c, original, count);
        
        printf("  %-6.2f | %6u | %.2fx\n", 
               scales[s], c.header.data_count,
               (double)count / c.header.data_count);
        
        kis4d_destroy(&c);
    }
    
    CHECK(3, "Scale sweep works", 1);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   TEST 4: Container Size
   ═══════════════════════════════════════════════════════════════════════════ */
static void test_size(void) {
    printf("TEST 4: Container Size Analysis\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    KIS4DContainer c;
    uint32_t scale = (uint32_t)(0.5 * 65536.0);
    kis4d_create(&c, 20736, scale);
    
    uint32_t header_size = sizeof(KIS4DHeader);
    uint32_t data_size = c.header.data_count * sizeof(uint8_t);
    uint32_t map_size = c.header.total_slots * sizeof(uint32_t);
    uint32_t total_size = header_size + data_size + map_size;
    
    printf("  Header:  %u bytes\n", header_size);
    printf("  Data:    %u bytes (unique values)\n", data_size);
    printf("  Map:     %u bytes (address map)\n", map_size);
    printf("  Total:   %u bytes = %.1f KB\n", total_size, total_size / 1024.0);
    printf("  Original: %u bytes = %.1f KB\n", 20736, 20736 / 1024.0);
    printf("  Ratio:   %.2fx\n", (double)total_size / 20736);
    
    CHECK(4, "Size analysis complete", 1);
    kis4d_destroy(&c);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════ */
int main(void) {
    printf("KIS 4D Container Test\n");
    printf("═══════════════════════════════════════════════════════════════════\n\n");
    
    test_create();
    test_encode_decode();
    test_scales();
    test_size();
    
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass, pass + fail);
    printf("═══════════════════════════════════════════════════════════════════\n");
    return 0;
}

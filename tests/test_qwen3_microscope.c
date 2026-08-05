/* test_qwen3_microscope.c — φ-microscope on REAL Qwen3-4B GGUF
 * Uses gguf_open() from FGLS_new/beam_addressing/gguf_reader.h
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../core/geo_phi_microscope.h"
#include "gguf_reader.h"

/* Dequantize Q4_K_M: block = 144 bytes per 256 weights
 * Layout: float16 d(2B) + float16 dmin(2B) + 12B scales + 128B nibbles
 * Each nibble = 4-bit weight. Quantized: w = d * (nibble - 8) + dmin */
static float half_to_float(uint16_t h) {
    uint32_t sign = (h >> 15) & 1;
    uint32_t exp  = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FF;
    uint32_t f32;
    if (exp == 0)       f32 = (sign << 31) | (mant << 13);
    else if (exp == 31)  f32 = (sign << 31) | 0x7F800000 | (mant << 13);
    else                 f32 = (sign << 31) | ((exp + 127 - 15) << 23) | (mant << 13);
    float r; memcpy(&r, &f32, 4); return r;
}

static uint64_t dequant_q4km(const uint8_t *raw, uint64_t raw_len,
                              float *out, uint64_t max_out) {
    uint64_t idx = 0;
    for (uint64_t b = 0; b + 144 <= raw_len && idx + 256 <= max_out; b += 144) {
        uint16_t d_u16   = (uint16_t)(raw[b] | (raw[b+1] << 8));
        uint16_t dm_u16  = (uint16_t)(raw[b+2] | (raw[b+3] << 8));
        float d  = half_to_float(d_u16);
        float dm = half_to_float(dm_u16);
        if (d == 0.0f) d = 1.0f;
        for (uint32_t i = 0; i < 256 && idx < max_out; i++, idx++) {
            uint8_t byte_val = raw[b + 16 + i / 2];
            uint8_t nib = (i & 1) ? (byte_val >> 4) : (byte_val & 0x0F);
            out[idx] = d * ((float)nib - 8.0f) + dm;
        }
    }
    return idx;
}

/* Also handle Q4_0 (type 2): block = 18 bytes per 32 weights */
static uint64_t dequant_q40(const uint8_t *raw, uint64_t raw_len,
                             float *out, uint64_t max_out) {
    uint64_t idx = 0;
    for (uint64_t b = 0; b + 18 <= raw_len && idx + 32 <= max_out; b += 18) {
        uint16_t d_u16 = (uint16_t)(raw[b] | (raw[b+1] << 8));
        float d = half_to_float(d_u16);
        for (uint32_t i = 0; i < 32 && idx < max_out; i++, idx++) {
            uint8_t nib = (i & 1) ? (raw[b + 2 + i/2] >> 4) : (raw[b + 2 + i/2] & 0x0F);
            out[idx] = d * ((float)nib - 8.0f);
        }
    }
    return idx;
}

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1]
        : "F:/model/zimage/Qwen3-4B-Instruct-2507-Q4_K_M.gguf";

    printf("=== phi-Microscope on Qwen3-4B GGUF ===\n");
    printf("File: %s\n\n", path);

    GGUF_File *gf = gguf_open(path);
    if (!gf) { printf("Cannot open GGUF\n"); return 1; }

    printf("Tensors: %llu\n\n", (unsigned long long)gf->tensor_count);

    /* Print first 10 tensors */
    printf("  %-45s %12s %6s\n", "Name", "Weights", "Type");
    printf("  %-45s %12s %6s\n", "----", "-------", "----");
    for (uint64_t i = 0; i < gf->tensor_count && i < 10; i++) {
        printf("  %-45s %12llu %6u\n", gf->tensors[i].name,
               (unsigned long long)gf->tensors[i].n_weights, gf->tensors[i].type);
    }
    printf("\n");

    /* Find largest quantized tensor */
    int best = -1;
    uint64_t best_n = 0;
    for (uint64_t i = 0; i < gf->tensor_count; i++) {
        if (gf->tensors[i].n_weights > best_n &&
            (gf->tensors[i].type == 2 || gf->tensors[i].type == 3)) {
            best_n = gf->tensors[i].n_weights;
            best = (int)i;
        }
    }
    if (best < 0) {
        /* fallback: any tensor */
        for (uint64_t i = 0; i < gf->tensor_count; i++) {
            if (gf->tensors[i].n_weights > best_n) {
                best_n = gf->tensors[i].n_weights;
                best = (int)i;
            }
        }
    }

    GGUF_Tensor *t = &gf->tensors[best];
    printf("Analyzing: %s (%llu weights, type=%u)\n",
           t->name, (unsigned long long)t->n_weights, t->type);

    /* Read raw bytes */
    uint64_t raw_size = t->size_bytes;
    if (raw_size > 16 * 1024 * 1024) raw_size = 16 * 1024 * 1024;
    uint8_t *raw = (uint8_t*)malloc(raw_size);
    if (!raw) { printf("OOM\n"); gguf_close(gf); return 1; }

    fseek(gf->fp, gf->tensor_data_start + t->offset, SEEK_SET);
    size_t rd = fread(raw, 1, (size_t)raw_size, gf->fp);
    printf("Read: %zu bytes from offset %llu\n", rd, (unsigned long long)(gf->tensor_data_start + t->offset));
    gguf_close(gf);

    /* Dequantize */
    uint64_t max_w = 8192;
    float *w = (float*)malloc(max_w * sizeof(float));
    uint64_t n = 0;

    if (t->type == 3) {
        printf("Dequantizing Q4_K_M (144B blocks, 256 weights)...\n");
        n = dequant_q4km(raw, rd, w, max_w);
    } else if (t->type == 2) {
        printf("Dequantizing Q4_0 (18B blocks, 32 weights)...\n");
        n = dequant_q40(raw, rd, w, max_w);
    } else {
        printf("Using raw bytes (type %u)...\n", t->type);
        n = (rd < max_w) ? rd : max_w;
        for (uint64_t i = 0; i < n; i++) w[i] = (float)raw[i];
    }

    printf("Dequantized: %llu weights\n\n", (unsigned long long)n);

    if (n > 0) {
        phi_microscope(w, n, 0, 8);
    }

    free(raw);
    free(w);

    printf("\n=== DONE ===\n");
    return 0;
}

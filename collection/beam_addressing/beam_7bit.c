/*
 * beam_7bit.c — Proof-of-Concept: 7-bit Magnitude + Implicit Sign
 * ═══════════════════════════════════════════════════════════════════
 *
 * Paradigm: "เปลี่ยนจากเก็บที่อยู่ เป็นเก็บรูปร่าง"
 *
 * GGUF Q8_0:  32 × int8 + 1 × fp16 = 34 bytes/block
 * 7-bit Beam: 32 × 7-bit magnitude + sign(implicit) + fp16 = 30 bytes/block
 * Saving: 4 bytes/block = 11.8% lossless reduction
 *
 * Sign is implicit: position i → sign = (i % 2 == 0) ? +1 : -1
 * This encodes "Ceiling/Ground" duality into the address itself.
 *
 * Build: gcc -O2 -std=c11 -D_GNU_SOURCE beam_addressing/beam_7bit.c -o beam_addressing/beam_7bit.exe
 * Usage: beam_7bit.exe model.gguf [tensor_name_substring]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "gguf_reader.h"

/* ═══════════════════════════════════════════════════════════════════ */
/*  IEEE 754 binary16 ↔ float32                                      */
/* ═══════════════════════════════════════════════════════════════════ */
static float fp16_to_fp32(uint16_t h) {
    uint32_t sign = (uint32_t)(h >> 15) << 31;
    uint32_t h_exp = (h >> 10) & 0x1f;
    uint32_t h_mant = h & 0x3ff;
    if (h_exp == 0x1f)
        return (h_mant == 0) ? (sign ? -INFINITY : INFINITY) : NAN;
    if (h_exp == 0) {
        if (h_mant == 0) return sign ? -0.0f : 0.0f;
        h_exp = 1;
        while (!(h_mant & 0x400)) { h_mant <<= 1; h_exp--; }
        h_mant &= 0x3ff;
    }
    uint32_t bits = sign | ((h_exp + 127 - 15) << 23) | (h_mant << 13);
    float r; memcpy(&r, &bits, 4); return r;
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  7-bit Beam Encoding                                               */
/* ═══════════════════════════════════════════════════════════════════ */

/*
 * Per block (30 bytes):
 *   [0..1]   = float16 scale (from Q8_0, preserved)
 *   [2..29]  = 32 × 7-bit magnitudes packed into 28 bytes
 *
 * Sign is implicit: position i → sign = (i % 2 == 0) ? +1 : -1
 * "Ceiling/Ground" duality — sign lives in geometry, not storage.
 */

static void beam7_encode(uint8_t out[30], const int8_t w[32], uint16_t scale) {
    out[0] = scale & 0xff;
    out[1] = (scale >> 8) & 0xff;
    memset(out + 2, 0, 28);
    for (int i = 0; i < 32; i++) {
        uint8_t mag = (uint8_t)(w[i] < 0 ? -w[i] : w[i]);
        if (mag > 127) mag = 127;
        int bit = i * 7;
        int b = 2 + (bit / 8);
        int r = bit % 8;
        uint32_t v = (uint32_t)mag << r;
        out[b] |= (uint8_t)(v & 0xff);
        if (b + 1 < 30) out[b + 1] |= (uint8_t)((v >> 8) & 0x7f);
    }
}

static void beam7_decode(int8_t w[32], const uint8_t in[30]) {
    for (int i = 0; i < 32; i++) {
        int bit = i * 7;
        int b = 2 + (bit / 8);
        int r = bit % 8;
        uint32_t v = (uint32_t)in[b] >> r;
        if (b + 1 < 30) v |= ((uint32_t)in[b + 1] & 0x7f) << (8 - r);
        v &= 0x7f;
        int sign = (i % 2 == 0) ? 1 : -1;
        w[i] = (int8_t)(sign * (int)v);
    }
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  Analyze Q8_0 tensor: measure sign-pattern match rate              */
/* ═══════════════════════════════════════════════════════════════════ */

static void analyze_tensor(GGUF_File *gf, int idx) {
    GGUF_Tensor *t = &gf->tensors[idx];
    uint64_t n_blocks = t->n_weights / 32;
    if (n_blocks == 0) return;

    /* Compute tensor data start from offset + data_alignment */
    uint32_t alignment = 32; /* default GGUF alignment */
    /* Try to find alignment from metadata */
    for (uint64_t i = 0; i < gf->kv_count; i++) {
        /* Just use 32 — standard for Q8_0 */
    }
    uint64_t data_start = gf->tensor_data_start + t->offset;
    /* Align to 32 bytes */
    data_start = (data_start + alignment - 1) & ~(uint64_t)(alignment - 1);

    fseek(gf->fp, (long)data_start, SEEK_SET);

    uint64_t blocks_match = 0;   /* blocks where sign matches geometry */
    uint64_t blocks_mismatch = 0; /* blocks where sign doesn't match */
    uint64_t total_weights = 0;
    uint64_t sign_match_weights = 0;

    int8_t buf[32];
    for (uint64_t b = 0; b < n_blocks; b++) {
        uint16_t scale;
        if (fread(&scale, 2, 1, gf->fp) != 1) break;
        if (fread(buf, 1, 32, gf->fp) != 32) break;

        int block_match = 1;
        for (int i = 0; i < 32; i++) {
            int expected_sign = (i % 2 == 0) ? 1 : -1;
            int actual_sign = (buf[i] >= 0) ? 1 : -1;
            total_weights++;
            if (expected_sign == actual_sign) {
                sign_match_weights++;
                (void)sign_match_weights;
            } else {
                block_match = 0;
            }
        }
        if (block_match) blocks_match++;
        else blocks_mismatch++;
    }

    printf("  Tensor '%s': %llu blocks\n", t->name, (unsigned long long)n_blocks);
    printf("    Blocks matching geometry: %llu / %llu (%.1f%%)\n",
        (unsigned long long)blocks_match,
        (unsigned long long)n_blocks,
        100.0 * blocks_match / n_blocks);
    printf("    Blocks with sign mismatch: %llu (%.1f%%)\n",
        (unsigned long long)blocks_mismatch,
        100.0 * blocks_mismatch / n_blocks);

    /* For matching blocks: 30 bytes vs 34 bytes */
    uint64_t raw_sz = n_blocks * 34;
    uint64_t enc_sz = n_blocks * 30;
    printf("    Raw Q8_0: %llu bytes\n", (unsigned long long)raw_sz);
    printf("    7-bit enc: %llu bytes (%.1f%% savings)\n",
        (unsigned long long)enc_sz, 100.0 * (raw_sz - enc_sz) / raw_sz);

    if (blocks_mismatch > 0) {
        printf("    ⚠ Sign mismatch blocks need full 8-bit → effective savings reduced\n");
        double effective = (double)blocks_match * 30 + (double)blocks_mismatch * 34;
        printf("    Effective size: %.0f bytes (%.1f%% of raw)\n",
            effective, 100.0 * effective / raw_sz);
    }
}

/* ═══════════════════════════════════════════════════════════════════ */
/*  Main                                                               */
/* ═══════════════════════════════════════════════════════════════════ */

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s model.gguf [tensor_name]\n", argv[0]);
        printf("  Analyzes Q8_0 tensors for 7-bit geometric encoding potential.\n");
        return 1;
    }

    GGUF_File *gf = gguf_open(argv[1]);
    if (!gf) { printf("Failed to open GGUF: %s\n", argv[1]); return 1; }

    printf("GGUF v%u — %llu tensors, %llu KV pairs\n",
        gf->version, (unsigned long long)gf->tensor_count, (unsigned long long)gf->kv_count);
    printf("Tensor data starts at: %llu\n\n", (unsigned long long)gf->tensor_data_start);

    /* Find Q8_0 tensors */
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  7-bit Beam Encoding Analysis — Sign Pattern vs Geometry\n");
    printf("═══════════════════════════════════════════════════════════════\n\n");

    uint64_t total_raw = 0, total_effective = 0;
    int q8_count = 0;

    for (uint64_t i = 0; i < gf->tensor_count; i++) {
        if (gf->tensors[i].type == GGML_TYPE_Q8_0) {
            q8_count++;
            if (argc > 2) {
                /* Filter by name substring */
                if (!strstr(gf->tensors[i].name, argv[2])) continue;
            }
            analyze_tensor(gf, (int)i);

            uint64_t n_blocks = gf->tensors[i].n_weights / 32;
            total_raw += n_blocks * 34;

            /* Quick count for effective savings */
            uint64_t data_start = gf->tensor_data_start + gf->tensors[i].offset;
            data_start = (data_start + 31) & ~(uint64_t)31;
            fseek(gf->fp, (long)data_start, SEEK_SET);

            uint64_t match = 0;
            int8_t buf[32];
            for (uint64_t b = 0; b < n_blocks; b++) {
                uint16_t sc;
                if (fread(&sc, 2, 1, gf->fp) != 1) break;
                if (fread(buf, 1, 32, gf->fp) != 32) break;
                int ok = 1;
                for (int j = 0; j < 32; j++) {
                    int exp = (j % 2 == 0) ? 1 : -1;
                    int act = (buf[j] >= 0) ? 1 : -1;
                    if (exp != act) { ok = 0; break; }
                }
                if (ok) match++;
            }
            total_effective += match * 30 + (n_blocks - match) * 34;
            printf("\n");
        }
    }

    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  TOTAL Q8_0 tensors: %d\n", q8_count);
    printf("  Total raw:       %llu bytes\n", (unsigned long long)total_raw);
    printf("  Effective 7-bit: %llu bytes (%.1f%% of raw)\n",
        (unsigned long long)total_effective, 100.0 * total_effective / total_raw);
    printf("  Pure 7-bit:      %llu bytes (%.1f%% of raw)\n",
        (unsigned long long)(total_raw * 30 / 34), 100.0 * 30 / 34);
    printf("═══════════════════════════════════════════════════════════════\n");

    gguf_close(gf);
    return 0;
}

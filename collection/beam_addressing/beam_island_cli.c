/**
 * beam_island_cli.c
 * 
 * CLI tool: บีบ Q8_0 model weights ด้วย Island + Bitmap codec
 * 
 * Usage: ./beam_island_cli.exe [model.gguf]
 * 
 * 1. อ่าน GGUF tensor weights (skip header)
 * 2. แบ่งเป็น blocks ละ 32 weights
 * 3. บีบด้วย bitmap (skip zeros)
 * 4. แสดง compression ratio + roundtrip verify
 *
 * Compile: gcc -O2 -Wall -o beam_island_cli.exe beam_island_cli.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

/* ── Constants ───────────────────────────────────────────── */
#define BLOCK_SZ            32u
#define GEO_FIBO_CLOCK      1440u
#define GEO_PENTAGON_SZ     720u
#define CODEC_STRIDE        37u

/* ── Bitmap Codec ────────────────────────────────────────── */
typedef struct {
    uint32_t total_blocks;
    uint32_t total_weights;
    uint32_t total_zeros;
    uint32_t total_nonzero;
    uint32_t raw_bytes;        /* uncompressed: count + weights */
    uint32_t bitmap_bytes;     /* compressed: count + bitmap + nonzero */
    uint32_t simple_bytes;     /* simple: count + all weights */
    uint32_t verify_pass;
    uint32_t verify_fail;
} CodecStats;

static uint32_t encode_block_bitmap(const int8_t *w, uint32_t n, uint8_t *out) {
    if (n > BLOCK_SZ) n = BLOCK_SZ;
    
    /* Bitmap: bit i = 1 if w[i] != 0 */
    uint32_t bitmap = 0;
    uint32_t nz_count = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (w[i] != 0) {
            bitmap |= (1u << i);
            nz_count++;
        }
    }
    
    /* Header: count(1) + bitmap(4) = 5 bytes overhead */
    out[0] = (uint8_t)n;
    out[1] = (uint8_t)(bitmap & 0xFF);
    out[2] = (uint8_t)((bitmap >> 8) & 0xFF);
    out[3] = (uint8_t)((bitmap >> 16) & 0xFF);
    out[4] = (uint8_t)((bitmap >> 24) & 0xFF);
    
    /* Non-zero weights only */
    uint32_t idx = 5;
    for (uint32_t i = 0; i < n; i++) {
        if (w[i] != 0) out[idx++] = (uint8_t)(w[i] + 128);
    }
    return idx;
}

static uint32_t decode_block_bitmap(const uint8_t *in, uint32_t in_sz, int8_t *w, uint32_t max) {
    if (in_sz < 5) return 0;
    uint32_t n = in[0] < max ? in[0] : max;
    uint32_t bitmap = (uint32_t)in[1] | ((uint32_t)in[2] << 8) | 
                      ((uint32_t)in[3] << 16) | ((uint32_t)in[4] << 24);
    
    uint32_t w_idx = 5;
    uint32_t out_idx = 0;
    for (uint32_t i = 0; i < 32 && out_idx < n; i++) {
        if (bitmap & (1u << i)) {
            w[out_idx++] = (w_idx < in_sz) ? (int8_t)(in[w_idx] - 128) : 0;
            w_idx++;
        } else {
            w[out_idx++] = 0;
        }
    }
    return out_idx;
}

/* ── Island mapping ──────────────────────────────────────── */
static uint32_t weight_to_tile(int8_t w) {
    uint32_t abs_w = (uint32_t)(w >= 0 ? w : -w);
    uint32_t slot = (abs_w * CODEC_STRIDE) % GEO_PENTAGON_SZ;
    return (w >= 0) ? slot : (GEO_PENTAGON_SZ + slot);
}

/* ── Stats ───────────────────────────────────────────────── */
static void stats_init(CodecStats *s) {
    memset(s, 0, sizeof(CodecStats));
}

static void stats_print(const CodecStats *s) {
    printf("\n╔════════════════════════════════════════════════════╗\n");
    printf("║  Compression Results                              ║\n");
    printf("╚════════════════════════════════════════════════════╝\n\n");
    
    printf("  Blocks:      %u\n", s->total_blocks);
    printf("  Weights:     %u\n", s->total_weights);
    printf("  Zeros:       %u (%.1f%%)\n", s->total_zeros, 100.0*s->total_zeros/s->total_weights);
    printf("  Non-zero:    %u (%.1f%%)\n", s->total_nonzero, 100.0*s->total_nonzero/s->total_weights);
    printf("\n");
    printf("  Raw (Q8_0):      %u bytes\n", s->raw_bytes);
    printf("  Simple (count+W): %u bytes (%.3fx)\n", s->simple_bytes, (double)s->simple_bytes/s->raw_bytes);
    printf("  Bitmap (skip 0):  %u bytes (%.3fx)\n", s->bitmap_bytes, (double)s->bitmap_bytes/s->raw_bytes);
    printf("\n");
    printf("  Bitmap overhead:  5 bytes/block\n");
    printf("  Bitmap savings:   %.1f%% vs raw\n", 100.0*(1.0-(double)s->bitmap_bytes/s->raw_bytes));
    printf("\n");
    printf("  Verify: %u PASS, %u FAIL\n", s->verify_pass, s->verify_fail);
    printf("  Status: %s\n", s->verify_fail == 0 ? "✅ LOSSLESS" : "❌ LOSSY");
}

/* ── Main ────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    const char *model_path = (argc > 1) ? argv[1] : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    
    printf("╔════════════════════════════════════════════════════╗\n");
    printf("║  Island + Bitmap CLI — Q8_0 Compression           ║\n");
    printf("║  1440 = 2 × 720 = 2 pentagons                    ║\n");
    printf("║  Bitmap: skip zero weights → LOSSLESS             ║\n");
    printf("╚════════════════════════════════════════════════════╝\n\n");
    
    printf("Model: %s\n", model_path);
    
    FILE *f = fopen(model_path, "rb");
    if (!f) {
        printf("ERROR: Cannot open model file\n");
        return 1;
    }
    
    /* Read GGUF header (simplified) */
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    /* Read magic + version */
    uint32_t magic, version;
    fread(&magic, 4, 1, f);
    fread(&version, 4, 1, f);
    printf("GGUF magic: 0x%08X version: %u\n", magic, version);
    
    /* Skip rest of header — find tensor data start */
    /* For simplicity, skip first 1024 bytes (typical header size) */
    uint32_t header_skip = 1024;
    fseek(f, header_skip, SEEK_SET);
    
    long data_start = header_skip;
    long data_size = file_size - data_start;
    
    printf("Data offset: %u bytes\n", header_skip);
    printf("Data size:   %ld bytes\n", data_size);
    printf("Weights:     %ld (Q8_0 = 1 byte/weight)\n", data_size);
    printf("\n");
    
    /* Read weight data in chunks */
    uint32_t chunk_sz = 1024 * 1024;  /* 1MB chunks */
    uint8_t *raw_buf = malloc(chunk_sz);
    uint8_t *dec_buf = malloc(chunk_sz + 64);  /* extra for decode output */
    
    if (!raw_buf || !dec_buf) {
        printf("ERROR: malloc failed\n");
        fclose(f);
        return 1;
    }
    
    CodecStats stats;
    stats_init(&stats);
    
    /* Island distribution counters */
    uint32_t island_pos[720] = {0};
    uint32_t island_neg[720] = {0};
    uint32_t sign_pos=0, sign_neg=0, sign_zero=0;
    
    printf("Processing...\n");
    
    long total_read = 0;
    while (total_read < data_size) {
        uint32_t to_read = chunk_sz;
        if (total_read + to_read > data_size) to_read = (uint32_t)(data_size - total_read);
        
        size_t rd = fread(raw_buf, 1, to_read, f);
        if (rd == 0) break;
        total_read += rd;
        
        /* Process in blocks of 32 */
        uint32_t offset = 0;
        while (offset + BLOCK_SZ <= rd) {
            int8_t *weights = (int8_t *)(raw_buf + offset);
            
            /* Count zeros/nonzeros */
            uint32_t nz = 0;
            for (uint32_t i = 0; i < BLOCK_SZ; i++) {
                if (weights[i] == 0) stats.total_zeros++;
                else {
                    stats.total_nonzero++;
                    uint32_t tile = weight_to_tile(weights[i]);
                    if (tile < 720) { island_pos[tile]++; sign_pos++; }
                    else { island_neg[tile-720]++; sign_neg++; }
                }
            }
            
            /* Encode with bitmap */
            uint8_t encoded[64];
            uint32_t enc_sz = encode_block_bitmap(weights, BLOCK_SZ, encoded);
            
            /* Decode and verify */
            int8_t decoded[BLOCK_SZ];
            uint32_t dec_sz = decode_block_bitmap(encoded, enc_sz, decoded, BLOCK_SZ);
            
            int ok = (dec_sz == BLOCK_SZ);
            if (ok) {
                for (uint32_t i = 0; i < BLOCK_SZ; i++) {
                    if (decoded[i] != weights[i]) { ok = 0; break; }
                }
            }
            if (ok) stats.verify_pass++; else stats.verify_fail++;
            
            /* Update stats */
            stats.total_blocks++;
            stats.total_weights += BLOCK_SZ;
            stats.raw_bytes += BLOCK_SZ;       /* Q8_0: 1 byte/weight */
            stats.simple_bytes += 1 + BLOCK_SZ; /* count + weights */
            stats.bitmap_bytes += enc_sz;        /* actual encoded size */
            
            offset += BLOCK_SZ;
        }
        
        /* Progress */
        double pct = 100.0 * total_read / data_size;
        printf("\r  Progress: %.1f%% (%ld/%ld KB) blocks=%u", 
               pct, total_read/1024, data_size/1024, stats.total_blocks);
        fflush(stdout);
    }
    
    printf("\n");
    fclose(f);
    
    /* Print results */
    stats_print(&stats);
    
    /* Island analysis */
    printf("\n╔════════════════════════════════════════════════════╗\n");
    printf("║  Island Distribution                              ║\n");
    printf("╚════════════════════════════════════════════════════╝\n\n");
    
    uint32_t total_nonzero = sign_pos + sign_neg;
    printf("  Sign: pos=%u (%.1f%%) neg=%u (%.1f%%)\n", 
           sign_pos, 100.0*sign_pos/total_nonzero,
           sign_neg, 100.0*sign_neg/total_nonzero);
    
    /* Slot utilization */
    uint32_t used_pos=0, used_neg=0;
    for (uint32_t i=0; i<720; i++) {
        if (island_pos[i]>0) used_pos++;
        if (island_neg[i]>0) used_neg++;
    }
    printf("  Positive island: %u/720 slots (%.1f%%)\n", used_pos, 100.0*used_pos/720);
    printf("  Negative island: %u/720 slots (%.1f%%)\n", used_neg, 100.0*used_neg/720);
    printf("  Total tiles:     %u/1440 (%.1f%%)\n", used_pos+used_neg, 100.0*(used_pos+used_neg)/1440);
    
    /* Pentagon grouping */
    printf("\n  Pentagon distribution:\n");
    for (uint32_t p=0; p<5; p++) {
        uint32_t pc=0, nc=0;
        for (uint32_t t=p*3; t<(p+1)*3 && t<12; t++) {
            for (uint32_t s=t*60; s<(t+1)*60 && s<720; s++) {
                pc += island_pos[s];
                nc += island_neg[s];
            }
        }
        printf("    Pentagon %u: pos=%7u neg=%7u total=%7u\n", p, pc, nc, pc+nc);
    }
    
    free(raw_buf);
    free(dec_buf);
    return stats.verify_fail > 0 ? 1 : 0;
}

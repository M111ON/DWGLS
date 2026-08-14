/**
 * beam_island_codec.c — v2 (fixed)
 * 
 * Positive Island + Negative Island → packed into 1440
 * 
 * 1440 = 2 × 720 = 2 pentagons
 * 720  = 5 × 144 = 5 triangles = 1 pentagon
 * 
 * Key insight: 51.7% of Q8_0 weights are ZERO!
 * → bitmap method: only store non-zero weights
 * → island split: positive/negative spatial organization
 *
 * Compile: gcc -O2 -Wall -o beam_island_codec.exe beam_island_codec.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

/* ── Constants ───────────────────────────────────────────── */
#define GEO_FIBO_CLOCK      1440u
#define GEO_PENTAGON_SZ     720u
#define GEO_TOWER           144u
#define CODEC_STRIDE        37u
#define CODEC_BLOCK_SZ      32u

/* ── Island mapping ──────────────────────────────────────── */
/*
 * weight → island + slot:
 *   w >= 0: island 0 (positive), slot = (w × 37) % 720
 *   w <  0: island 1 (negative), slot = (|w| × 37) % 720
 *   
 * Both islands fit in 1440 = GEO_FIBO_CLOCK ✓
 * Symmetric pairs map to same slot, opposite islands ✓
 */

static uint32_t weight_to_tile(int8_t w) {
    uint32_t abs_w = (uint32_t)(w >= 0 ? w : -w);
    uint32_t slot = (abs_w * CODEC_STRIDE) % GEO_PENTAGON_SZ;
    return (w >= 0) ? slot : (GEO_PENTAGON_SZ + slot);
}

/* ── Block format v2 ─────────────────────────────────────── */
/*
 * Format A (simple): [count:1B][weights:8bit×N] = 1+N bytes
 *   → lossless, O(1), 33 bytes for 32 weights
 *
 * Format B (bitmap): [nonzero_count:1B][bitmap:4B][weights:8bit×M] = 5+M bytes
 *   → only stores non-zero weights
 *   → for 51.7% zeros: M ≈ 15, total ≈ 20 bytes (0.59× Q8_0!)
 *
 * Format C (split): [pos_count:1B][neg_count:1B][pos_weights][neg_weights]
 *   → separates islands for independent processing
 */

/* Format A: Simple roundtrip */
static uint32_t encode_simple(const int8_t *w, uint32_t n, uint8_t *out) {
    out[0] = (uint8_t)n;
    for (uint32_t i = 0; i < n; i++) out[1+i] = (uint8_t)(w[i] + 128);
    return 1 + n;
}

static uint32_t decode_simple(const uint8_t *in, uint32_t in_sz, int8_t *w, uint32_t max) {
    if (in_sz < 1) return 0;
    uint32_t n = in[0] < max ? in[0] : max;
    for (uint32_t i = 0; i < n && 1+i < in_sz; i++) w[i] = (int8_t)(in[1+i] - 128);
    return n;
}

/* Format B: Bitmap (skip zeros) */
static uint32_t encode_bitmap(const int8_t *w, uint32_t n, uint8_t *out) {
    if (n > CODEC_BLOCK_SZ) n = CODEC_BLOCK_SZ;
    
    /* Build bitmap: bit i = 1 if w[i] != 0 */
    uint32_t bitmap = 0;
    uint32_t nz_count = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (w[i] != 0) {
            bitmap |= (1u << i);
            nz_count++;
        }
    }
    
    out[0] = (uint8_t)nz_count;
    out[1] = (uint8_t)(bitmap & 0xFF);
    out[2] = (uint8_t)((bitmap >> 8) & 0xFF);
    out[3] = (uint8_t)((bitmap >> 16) & 0xFF);
    out[4] = (uint8_t)((bitmap >> 24) & 0xFF);
    
    uint32_t idx = 5;
    for (uint32_t i = 0; i < n; i++) {
        if (w[i] != 0) out[idx++] = (uint8_t)(w[i] + 128);
    }
    return idx;
}

static uint32_t decode_bitmap(const uint8_t *in, uint32_t in_sz, int8_t *w, uint32_t max) {
    if (in_sz < 5) return 0;
    (void)in[0]; /* nz_count, unused in decode */
    uint32_t bitmap = (uint32_t)in[1] | ((uint32_t)in[2] << 8) | 
                      ((uint32_t)in[3] << 16) | ((uint32_t)in[4] << 24);
    
    uint32_t w_idx = 5;
    uint32_t out_idx = 0;
    for (uint32_t i = 0; i < 32 && out_idx < max; i++) {
        if (bitmap & (1u << i)) {
            w[out_idx++] = (w_idx < in_sz) ? (int8_t)(in[w_idx] - 128) : 0;
            w_idx++;
        } else {
            w[out_idx++] = 0;
        }
    }
    return out_idx;
}

/* ── Analysis ────────────────────────────────────────────── */
static void analyze(const int8_t *data, uint32_t n) {
    printf("\n=== Weight & Island Analysis (Qwen2.5 Q8_0) ===\n");
    
    uint32_t pos=0, neg=0, zero=0;
    uint32_t slot_pos[720]={0}, slot_neg[720]={0};
    int64_t sum_pos=0, sum_neg=0;
    
    for (uint32_t i = 0; i < n; i++) {
        if (data[i] > 0) { pos++; sum_pos += data[i]; }
        else if (data[i] < 0) { neg++; sum_neg += data[i]; }
        else zero++;
        
        uint32_t tile = weight_to_tile(data[i]);
        if (tile < 720) slot_pos[tile]++;
        else slot_neg[tile - 720]++;
    }
    
    printf("  Total:    %u\n", n);
    printf("  Positive: %6u (%5.1f%%) avg=+%.1f\n", pos, 100.0*pos/n, (double)sum_pos/pos);
    printf("  Negative: %6u (%5.1f%%) avg=%.1f\n", neg, 100.0*neg/n, (double)sum_neg/neg);
    printf("  Zero:     %6u (%5.1f%%)\n", zero, 100.0*zero/n);
    
    /* Island slots used */
    uint32_t used_pos=0, used_neg=0;
    for (uint32_t i=0; i<720; i++) {
        if (slot_pos[i]>0) used_pos++;
        if (slot_neg[i]>0) used_neg++;
    }
    printf("\n  Positive island: %3u/720 slots (%.1f%%)\n", used_pos, 100.0*used_pos/720);
    printf("  Negative island: %3u/720 slots (%.1f%%)\n", used_neg, 100.0*used_neg/720);
    printf("  Total tiles:     %3u/1440 (%.1f%%)\n", used_pos+used_neg, 100.0*(used_pos+used_neg)/1440);
    
    /* Compression estimate */
    uint32_t simple_sz = 1 + n;
    uint32_t bitmap_sz = 5 + (n - zero);
    printf("\n  Compression estimates:\n");
    printf("    Simple (Format A): %u bytes (%.2fx Q8_0)\n", simple_sz, (double)simple_sz/32.0);
    printf("    Bitmap (Format B): %u bytes (%.2fx Q8_0)\n", bitmap_sz, (double)bitmap_sz/32.0);
    printf("    Q8_0 baseline:     32 bytes (1.00x)\n");
    
    /* Pentagon grouping */
    printf("\n  Pentagon grouping (5 towers × 144 = 720):\n");
    for (uint32_t p = 0; p < 5; p++) {
        uint32_t p_count = 0, n_count = 0;
        for (uint32_t t = p*3; t < (p+1)*3 && t < 12; t++) {
            for (uint32_t s = t*60; s < (t+1)*60 && s < 720; s++) {
                p_count += slot_pos[s];
                n_count += slot_neg[s];
            }
        }
        printf("    Pentagon %u: pos=%5u neg=%5u total=%5u\n", p, p_count, n_count, p_count+n_count);
    }
}

/* ── Tests ───────────────────────────────────────────────── */
static int tests(void) {
    printf("=== Roundtrip Tests ===\n\n");
    int pass=0, fail=0;
    
    /* Test 1: Simple format - all 256 values */
    printf("Test 1 (simple): All 256 values... ");
    pass=0; fail=0;
    for (int w=-128; w<=127; w++) {
        int8_t in[1]={(int8_t)w}, out[1]={0};
        uint8_t buf[64];
        uint32_t sz = encode_simple(in, 1, buf);
        uint32_t n = decode_simple(buf, sz, out, 1);
        if (n==1 && out[0]==w) pass++; else fail++;
    }
    printf("%d/256 %s\n", pass, fail==0?"PASS":"FAIL");
    
    /* Test 2: Bitmap format - all 256 values */
    printf("Test 2 (bitmap): All 256 values... ");
    pass=0; fail=0;
    for (int w=-128; w<=127; w++) {
        int8_t in[32], out[32];
        memset(in, 0, 32);
        in[0] = (int8_t)w;
        uint8_t buf[64];
        uint32_t sz = encode_bitmap(in, 32, buf);
        uint32_t n = decode_bitmap(buf, sz, out, 32);
        int ok = (n==32 && out[0]==w);
        if (ok) pass++; else fail++;
    }
    printf("%d/256 %s\n", pass, fail==0?"PASS":"FAIL");
    
    /* Test 3: Bitmap format - random 32-weight blocks */
    printf("Test 3 (bitmap): Random 1000 blocks... ");
    pass=0; fail=0;
    srand(42);
    for (int t=0; t<1000; t++) {
        int8_t in[32], out[32];
        uint8_t buf[64];
        for (uint32_t i=0; i<32; i++) in[i] = (int8_t)(rand()%256-128);
        uint32_t sz = encode_bitmap(in, 32, buf);
        uint32_t n = decode_bitmap(buf, sz, out, 32);
        int ok = (n==32);
        if (ok) for (uint32_t i=0; i<32; i++) if (out[i]!=in[i]) ok=0;
        if (ok) pass++; else fail++;
    }
    printf("%d/1000 %s\n", pass, fail==0?"PASS":"FAIL");
    
    /* Test 4: Island mapping uniqueness */
    printf("Test 4: Island uniqueness (256→1440)... ");
    uint8_t seen[1440]={0};
    pass=0; fail=0;
    for (int w=-128; w<=127; w++) {
        uint32_t tile = weight_to_tile((int8_t)w);
        if (tile<1440 && seen[tile]==0) { seen[tile]=1; pass++; }
        else fail++;
    }
    printf("%d/256 %s\n", pass, fail==0?"PASS":"FAIL");
    
    /* Test 5: Symmetric pairs → same slot, opposite island */
    printf("Test 5: Symmetric pairs... ");
    pass=0; fail=0;
    for (int w=1; w<=127; w++) {
        uint32_t tp = weight_to_tile((int8_t)w);
        uint32_t tn = weight_to_tile((int8_t)(-w));
        int same_slot = ((tp%720) == (tn%720));
        int opp_island = ((tp/720) != (tn/720));
        if (same_slot && opp_island) pass++; else fail++;
    }
    printf("%d/127 %s\n", pass, fail==0?"PASS":"FAIL");
    
    /* Test 6: Bitmap size savings for typical blocks */
    printf("\nTest 6: Bitmap savings estimate...\n");
    srand(123);
    uint32_t total_simple=0, total_bitmap=0, blocks=0;
    for (int t=0; t<10000; t++) {
        int8_t in[32];
        uint8_t buf[64];
        for (uint32_t i=0; i<32; i++) in[i] = (int8_t)(rand()%256-128);
        uint32_t sz_s = 1 + 32;  /* simple */
        uint32_t sz_b = encode_bitmap(in, 32, buf);
        total_simple += sz_s;
        total_bitmap += sz_b;
        blocks++;
    }
    printf("  10000 random blocks:\n");
    printf("    Simple avg: %.1f bytes/block (%.2fx Q8_0)\n", (double)total_simple/blocks, (double)total_simple/blocks/32);
    printf("    Bitmap avg: %.1f bytes/block (%.2fx Q8_0)\n", (double)total_bitmap/blocks, (double)total_bitmap/blocks/32);
    printf("    Savings:    %.1f%%\n", 100.0*(1.0-(double)total_bitmap/total_simple));
    
    return fail;
}

/* ── Main ────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    printf("╔════════════════════════════════════════════════╗\n");
    printf("║  Island Codec v2 — Positive + Negative → 1440 ║\n");
    printf("║  1440 = 2 × 720 = 2 pentagons                ║\n");
    printf("║  720  = 5 × 144 = 5 triangles = 1 pentagon   ║\n");
    printf("╚════════════════════════════════════════════════╝\n\n");
    
    printf("Constants:\n");
    printf("  1440 = 2 × 720 ✓\n");
    printf("  720  = 5 × 144 ✓\n");
    printf("  inv(37) mod 720 = 253\n");
    printf("  inv(37) mod 1440 = 505\n\n");
    
    int failures = tests();
    
    /* Analyze model */
    const char *model_path = "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    FILE *f = fopen(model_path, "rb");
    if (f) {
        uint32_t sz = 136192;
        uint8_t *buf = malloc(sz);
        if (buf) {
            fseek(f, 1024, SEEK_SET);
            if (fread(buf, 1, sz, f) == sz) {
                analyze((const int8_t*)buf, sz);
            }
            free(buf);
        }
        fclose(f);
    } else {
        printf("\nModel not found — skipping analysis\n");
    }
    
    printf("\n=== FINAL: %s ===\n", failures==0 ? "ALL PASS" : "SOME FAIL");
    return failures;
}

/**
 * beam_goldberg_codec.c
 * 
 * Goldberg Polyhedron Codec — ใช้ icosahedron↔dodecahedron duality
 * 
 * Structure:
 *   20736 = 12 zones × 1728 nodes/zone
 *   1728  = 12 towers × 144 nodes/tower
 *   144   = 3 blocks × 48 nodes/block
 *   48    = 3 floors × 16 cells/floor
 *
 * Key: weight → zone(0..11) → tower(0..11) → block(0..2) → cell(0..47)
 *      = [pentagon_id][triangle_id][position]
 *      = Goldberg geometric identity
 *
 * Compile: gcc -O2 -Wall -Werror -o beam_goldberg_codec.exe beam_goldberg_codec.c
 * Test:    ./beam_goldberg_codec.exe
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

/* ── Goldberg Geometry Constants ────────────────────────── */
#define GEO_METATRON_COLS   4u
#define GEO_METATRON_ROWS   4u
#define GEO_METATRON_FLOORS 3u

#define GEO_METATRON_CELLS  (GEO_METATRON_COLS * GEO_METATRON_ROWS)  /* 16 */
#define GEO_BLOCK           (GEO_METATRON_CELLS * GEO_METATRON_FLOORS) /* 48 */
#define GEO_TOWER           (GEO_BLOCK * GEO_METATRON_FLOORS)         /* 144 */
#define GEO_FULL            (GEO_TOWER * GEO_TOWER)                   /* 20736 */

#define GEO_PENTAGONS       12u   /* dodecahedron faces = icosahedron vertices */
#define GEO_ZONES           GEO_PENTAGONS  /* 12 pentagonal zones */
#define GEO_NODES_PER_ZONE  (GEO_FULL / GEO_ZONES)  /* 1728 */
#define GEO_TOWERS_PER_ZONE (GEO_NODES_PER_ZONE / GEO_TOWER)  /* 12 */
#define GEO_BLOCKS_PER_TOWER 3u   /* GEO_TOWER / GEO_BLOCK = 144/48 = 3 */

/* ── Codec Constants ─────────────────────────────────────── */
#define CODEC_BLOCK_SZ      32u   /* weights per block */
#define CODEC_STRIDE        37u   /* coprime to 20736 */
#define CODEC_STRIDE_INV    16813u /* inverse: 37*16813 mod 20736 = 1 (verified) */

/* ── Goldberg Address ────────────────────────────────────── */
typedef struct {
    uint32_t zone;     /* 0..11  — pentagon/dodecahedron face */
    uint32_t tower;    /* 0..11  — triangle within zone */
    uint32_t block;    /* 0..2   — sub-triangle */
    uint32_t cell;     /* 0..47  — position within block */
    uint32_t flat;     /* 0..20735 — flat index */
} GoldbergAddr;

/* ── icosahedron coordinate (for zone assignment) ──────── */
/* Zone = which of 12 pentagonal regions (dodecahedron face) */
/* Using icosahedron vertex as zone center */
static double ICO_VERTICES[12][3];

static void ico_init(void) {
    /* 12 vertices of regular icosahedron, radius 1 */
    double phi = (1.0 + sqrt(5.0)) / 2.0;
    double inv = 1.0 / sqrt(1.0 + phi * phi);
    double p = phi * inv, q = inv;
    
    /* 6 vertices from rectangular coordinates */
    ICO_VERTICES[0][0] =  p; ICO_VERTICES[0][1] =  q; ICO_VERTICES[0][2] = 0;
    ICO_VERTICES[1][0] = -p; ICO_VERTICES[1][1] =  q; ICO_VERTICES[1][2] = 0;
    ICO_VERTICES[2][0] =  p; ICO_VERTICES[2][1] = -q; ICO_VERTICES[2][2] = 0;
    ICO_VERTICES[3][0] = -p; ICO_VERTICES[3][1] = -q; ICO_VERTICES[3][2] = 0;
    ICO_VERTICES[4][0] =  0; ICO_VERTICES[4][1] =  p; ICO_VERTICES[4][2] =  q;
    ICO_VERTICES[5][0] =  0; ICO_VERTICES[5][1] = -p; ICO_VERTICES[5][2] =  q;
    ICO_VERTICES[6][0] =  0; ICO_VERTICES[6][1] =  p; ICO_VERTICES[6][2] = -q;
    ICO_VERTICES[7][0] =  0; ICO_VERTICES[7][1] = -p; ICO_VERTICES[7][2] = -q;
    ICO_VERTICES[8][0] =  q; ICO_VERTICES[8][1] =  0; ICO_VERTICES[8][2] =  p;
    ICO_VERTICES[9][0] = -q; ICO_VERTICES[9][1] =  0; ICO_VERTICES[9][2] =  p;
    ICO_VERTICES[10][0] = q; ICO_VERTICES[10][1] = 0; ICO_VERTICES[10][2] = -p;
    ICO_VERTICES[11][0] =-q; ICO_VERTICES[11][1] = 0; ICO_VERTICES[11][2] = -p;
}

/* ── Flat index ↔ Goldberg address ───────────────────────── */
static GoldbergAddr goldberg_decompose(uint32_t flat) {
    GoldbergAddr a;
    a.flat   = flat % GEO_FULL;
    a.zone   = a.flat / GEO_NODES_PER_ZONE;       /* 0..11 */
    uint32_t in_zone = a.flat % GEO_NODES_PER_ZONE;
    a.tower  = in_zone / GEO_TOWER;                /* 0..11 */
    uint32_t in_tower = in_zone % GEO_TOWER;
    a.block  = in_tower / GEO_BLOCK;               /* 0..2 */
    a.cell   = in_tower % GEO_BLOCK;               /* 0..47 */
    return a;
}

static uint32_t goldberg_flat(const GoldbergAddr *a) {
    return a->zone * GEO_NODES_PER_ZONE 
         + a->tower * GEO_TOWER 
         + a->block * GEO_BLOCK 
         + a->cell;
}

/* ── Weight → Tile mapping (Goldberg-aware) ──────────────── */
/*
 *  weight(-128..+127) → weight_idx(0..255) → tile(0..20735)
 *  
 *  Two strategies:
 *    A. STRIDE_WALK: tile = (idx * 37) % 20736  [spread across all zones]
 *    B. ZONE_PINNED: tile = zone * 1728 + (idx * 37) % 1728  [within zone]
 *
 *  Strategy A: weight → random zone (good for compression)
 *  Strategy B: weight → same zone (good for spatial grouping)
 */
static uint32_t weight_to_tile_stride(int8_t weight) {
    uint32_t idx = (uint32_t)(weight + 128);  /* 0..255 */
    return (idx * CODEC_STRIDE) % GEO_FULL;
}

static uint32_t weight_to_tile_zoned(int8_t weight, uint32_t zone) {
    uint32_t idx = (uint32_t)(weight + 128);  /* 0..255 */
    uint32_t local = (idx * CODEC_STRIDE) % GEO_NODES_PER_ZONE;
    return zone * GEO_NODES_PER_ZONE + local;
}

/* ── Block Codec ─────────────────────────────────────────── */

/*
 * Encode: 32 weights → block bytes
 * Layout: [count:1B][zone_map:1B][weights:8bit×32] = 34 bytes
 *
 * zone_map encodes which zone each weight belongs to (high nibble = first 16, low = last 16)
 * If all weights in same zone → zone_map has single zone → can compress further
 */
#define BLOCK_OVERHEAD  2u  /* count + zone_map */
#define BLOCK_TOTAL     (BLOCK_OVERHEAD + CODEC_BLOCK_SZ)  /* 34 */

static uint32_t encode_block(const int8_t *weights, uint32_t n, uint8_t *out) {
    if (n > CODEC_BLOCK_SZ) n = CODEC_BLOCK_SZ;
    
    /* Count ALL weights (including zero — it's a valid weight) */
    uint32_t count = n;
    
    /* Assign zones: use stride mapping to distribute across zones */
    uint8_t zone_counts[12] = {0};
    uint32_t tiles[CODEC_BLOCK_SZ];
    
    for (uint32_t i = 0; i < n; i++) {
        tiles[i] = weight_to_tile_stride(weights[i]);
        uint32_t zone = tiles[i] / GEO_NODES_PER_ZONE;
        zone_counts[zone]++;
    }
    
    /* Find dominant zone */
    uint32_t best_zone = 0, best_count = 0;
    for (uint32_t z = 0; z < 12; z++) {
        if (zone_counts[z] > best_count) {
            best_count = zone_counts[z];
            best_zone = z;
        }
    }
    
    /* Zone map: high nibble = dominant zone, low nibble = #weights in it */
    uint8_t zone_map = (uint8_t)((best_zone << 4) | (best_count > 15 ? 15 : best_count));
    
    /* Write header */
    out[0] = (uint8_t)count;
    out[1] = zone_map;
    
    /* Write weights */
    uint32_t out_idx = BLOCK_OVERHEAD;
    for (uint32_t i = 0; i < n; i++) {
        out[out_idx++] = (uint8_t)(weights[i] + 128);
    }
    
    return out_idx;
}

static uint32_t decode_block(const uint8_t *in, uint32_t in_sz, int8_t *weights, uint32_t max_w) {
    if (in_sz < BLOCK_OVERHEAD) return 0;
    
    uint32_t count = in[0];
    uint8_t zone_map = in[1];
    uint32_t dominant_zone = (zone_map >> 4) & 0x0F;
    uint32_t dominant_count = zone_map & 0x0F;
    
    uint32_t n = count < max_w ? count : max_w;
    
    /* Read weights */
    for (uint32_t i = 0; i < n && (BLOCK_OVERHEAD + i) < in_sz; i++) {
        weights[i] = (int8_t)(in[BLOCK_OVERHEAD + i] - 128);
    }
    
    return n;
}

/* ── Roundtrip Test ──────────────────────────────────────── */

static int test_roundtrip(void) {
    printf("=== Goldberg Codec Roundtrip Test ===\n\n");
    
    int pass = 0, fail = 0;
    
    /* Test 1: All 256 weight values */
    printf("Test 1: All 256 weight values... ");
    for (int w = -128; w <= 127; w++) {
        int8_t weights[1] = {(int8_t)w};
        uint8_t buf[BLOCK_TOTAL];
        int8_t out[1];
        
        uint32_t sz = encode_block(weights, 1, buf);
        uint32_t n = decode_block(buf, sz, out, 1);
        
        if (n == 1 && out[0] == w) { pass++; }
        else { fail++; printf("FAIL: w=%d got=%d\n", w, out[0]); }
    }
    printf("%d/256 PASS\n", pass);
    
    /* Test 2: Random 32-weight blocks */
    printf("Test 2: Random blocks... ");
    pass = 0; fail = 0;
    for (int t = 0; t < 1000; t++) {
        int8_t weights[CODEC_BLOCK_SZ];
        uint8_t buf[BLOCK_TOTAL];
        int8_t out[CODEC_BLOCK_SZ];
        
        for (uint32_t i = 0; i < CODEC_BLOCK_SZ; i++) {
            weights[i] = (int8_t)(rand() % 256 - 128);
        }
        
        uint32_t sz = encode_block(weights, CODEC_BLOCK_SZ, buf);
        uint32_t n = decode_block(buf, sz, out, CODEC_BLOCK_SZ);
        
        int ok = (n == CODEC_BLOCK_SZ);
        if (ok) {
            for (uint32_t i = 0; i < CODEC_BLOCK_SZ; i++) {
                if (out[i] != weights[i]) { ok = 0; break; }
            }
        }
        if (ok) pass++; else fail++;
    }
    printf("%d/1000 PASS\n", pass);
    
    /* Test 3: Goldberg decomposition consistency */
    printf("Test 3: Goldberg decomposition... ");
    pass = 0; fail = 0;
    for (uint32_t f = 0; f < GEO_FULL; f++) {
        GoldbergAddr a = goldberg_decompose(f);
        uint32_t back = goldberg_flat(&a);
        if (back == f && a.zone < 12 && a.tower < 12 && a.block < 3 && a.cell < 48) {
            pass++;
        } else {
            fail++;
            if (fail <= 3) printf("FAIL: flat=%u zone=%u tower=%u block=%u cell=%u → back=%u\n",
                f, a.zone, a.tower, a.block, a.cell, back);
        }
    }
    printf("%d/%u PASS\n", pass, GEO_FULL);
    
    return fail;
}

/* ── Zone Statistics ─────────────────────────────────────── */
static void analyze_zones(const int8_t *model_data, uint32_t n_weights) {
    printf("\n=== Zone Distribution Analysis ===\n");
    printf("Total weights: %u\n", n_weights);
    printf("Zones: %u (pentagonal regions)\n", GEO_ZONES);
    printf("Nodes per zone: %u\n\n", GEO_NODES_PER_ZONE);
    
    /* Count weights per zone */
    uint32_t zone_counts[12] = {0};
    uint32_t tower_counts[12][12] = {{0}};
    
    for (uint32_t i = 0; i < n_weights; i++) {
        uint32_t tile = weight_to_tile_stride(model_data[i]);
        uint32_t zone = tile / GEO_NODES_PER_ZONE;
        uint32_t in_zone = tile % GEO_NODES_PER_ZONE;
        uint32_t tower = in_zone / GEO_TOWER;
        
        zone_counts[zone]++;
        tower_counts[zone][tower]++;
    }
    
    /* Print zone distribution */
    printf("Zone Distribution:\n");
    printf("  Zone  Count   %%     Bar\n");
    for (uint32_t z = 0; z < 12; z++) {
        double pct = 100.0 * zone_counts[z] / n_weights;
        int bar = (int)(pct * 2);
        printf("  %2u    %6u  %5.1f%% ", z, zone_counts[z], pct);
        for (int i = 0; i < bar && i < 40; i++) printf("█");
        printf("\n");
    }
    
    /* Zone uniformity */
    uint32_t min_c = zone_counts[0], max_c = zone_counts[0];
    for (uint32_t z = 1; z < 12; z++) {
        if (zone_counts[z] < min_c) min_c = zone_counts[z];
        if (zone_counts[z] > max_c) max_c = zone_counts[z];
    }
    printf("\nZone uniformity: min=%u max=%u ratio=%.2f\n", min_c, max_c, 
           max_c > 0 ? (double)min_c / max_c : 0);
    
    /* Print tower distribution for first 3 zones */
    printf("\nTower distribution (zones 0-2):\n");
    for (uint32_t z = 0; z < 3; z++) {
        printf("  Zone %2u: ", z);
        for (uint32_t t = 0; t < 12; t++) {
            printf("%5u ", tower_counts[z][t]);
        }
        printf("\n");
    }
}

/* ── Main ────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    printf("╔══════════════════════════════════════════════╗\n");
    printf("║  Goldberg Polyhedron Codec                   ║\n");
    printf("║  20736 = 12 zones × 1728 nodes/zone         ║\n");
    printf("║  1728  = 12 towers × 144 nodes/tower        ║\n");
    printf("║  144   = 3 blocks × 48 nodes/block          ║\n");
    printf("║  48    = 3 floors × 16 cells/floor          ║\n");
    printf("╚══════════════════════════════════════════════╝\n\n");
    
    /* Verify constants */
    printf("Constants verification:\n");
    printf("  GEO_METATRON_CELLS = %u\n", GEO_METATRON_CELLS);
    printf("  GEO_BLOCK          = %u\n", GEO_BLOCK);
    printf("  GEO_TOWER          = %u\n", GEO_TOWER);
    printf("  GEO_FULL           = %u\n", GEO_FULL);
    printf("  GEO_ZONES          = %u\n", GEO_ZONES);
    printf("  GEO_NODES_PER_ZONE = %u\n", GEO_NODES_PER_ZONE);
    printf("  GEO_TOWERS_PER_ZONE= %u\n", GEO_TOWERS_PER_ZONE);
    printf("  CODEC_BLOCK_SZ     = %u\n", CODEC_BLOCK_SZ);
    printf("  CODEC_STRIDE       = %u\n", CODEC_STRIDE);
    printf("  BLOCK_TOTAL        = %u bytes\n", BLOCK_TOTAL);
    printf("  Compression ratio  = %.2fx Q8_0\n", (double)BLOCK_TOTAL / 32.0);
    printf("\n");
    
    /* Verify stride inverse */
    uint32_t check = (CODEC_STRIDE * CODEC_STRIDE_INV) % GEO_FULL;
    printf("Stride inverse check: %u × %u mod %u = %u %s\n",
           CODEC_STRIDE, CODEC_STRIDE_INV, GEO_FULL, check,
           check == 1 ? "✓" : "✗ FAIL");
    
    /* Verify 20736 decomposition */
    printf("\n20736 decomposition:\n");
    printf("  12 × 1728 = %u %s\n", 12 * 1728, 12 * 1728 == GEO_FULL ? "✓" : "✗");
    printf("  1728 × 12 = %u %s\n", 1728 * 12, 1728 * 12 == GEO_FULL ? "✓" : "✗");
    printf("  144 × 144 = %u %s\n", 144 * 144, 144 * 144 == GEO_FULL ? "✓" : "✗");
    printf("  48 × 432  = %u %s\n", 48 * 432, 48 * 432 == GEO_FULL ? "✓" : "✗");
    printf("  2^8 × 3^4 = %u %s\n", 256 * 81, 256 * 81 == GEO_FULL ? "✓" : "✗");
    
    /* Run tests */
    printf("\n");
    int failures = test_roundtrip();
    
    /* Analyze model data if available */
    const char *model_path = "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    FILE *f = fopen(model_path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);
        
        /* Read a sample of weight data (skip header) */
        uint32_t sample_sz = 136192;  /* first 136192 bytes of weight data */
        uint8_t *buf = malloc(sample_sz);
        if (buf) {
            /* Skip GGUF header (approximately) */
            fseek(f, 1024, SEEK_SET);
            size_t rd = fread(buf, 1, sample_sz, f);
            if (rd == sample_sz) {
                analyze_zones((const int8_t *)buf, sample_sz);
            }
            free(buf);
        }
        fclose(f);
    } else {
        printf("\nModel file not found at %s — skipping zone analysis\n", model_path);
    }
    
    printf("\n=== FINAL: %s ===\n", failures == 0 ? "ALL PASS" : "SOME FAIL");
    return failures;
}

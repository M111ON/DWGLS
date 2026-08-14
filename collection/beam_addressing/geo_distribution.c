/*
 * geo_distribution.c — ดูว่า GGUF weights ไปตกตรงไหนบน geometry
 *
 * ไม่ใช่ compression analysis — เป็น OBSERVATION
 * ดูว่า weight values กระจายตัวบน coordinate space ยังไง
 *
 * Pipeline:
 *   weight (Q8: -128..127) → stride-12-gon walk → flat_key → enc (mod 1440)
 *   → ดูว่า enc กระจุกหรือกระจายบน timeline 1440
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define GGUF_MAGIC 0x46554747
#define CYCLE 1440u
#define STRIDE 37u
#define N_FACES 12u
#define FACE_SZ 120u
#define ICO_NODES 162u

/* ── Stride-12-gon walk ────────────────────────────────────── */
/*
 * Data bytes → walk on 12-gon ring
 * Each nibble (4 bits) → 1 step on 12-gon
 * nibble 0..3:  forward (E, NE, N, SE)
 * nibble 4..7:  reverse (W, SW, S, NW)
 * nibble 8..11: double-forward
 * nibble 12..15: no-op (break)
 */
static inline uint16_t stride_walk(const uint8_t *data, size_t len)
{
    uint32_t acc = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        uint8_t lo = b & 0x0F;
        uint8_t hi = (b >> 4) & 0x0F;
        
        /* lo nibble */
        if (lo < 4)       acc += lo;           /* forward */
        else if (lo < 8)  acc += (12 - lo);    /* reverse */
        else if (lo < 12) acc += (lo * 2);     /* double-forward */
        /* else: no-op */
        
        /* hi nibble */
        if (hi < 4)       acc += hi;
        else if (hi < 8)  acc += (12 - hi);
        else if (hi < 12) acc += (hi * 2);
    }
    return (uint16_t)(acc % CYCLE);
}

/* ── Frame decomposition ───────────────────────────────────── */
typedef struct {
    uint8_t face;
    uint8_t slot;
    uint8_t phase;
    uint16_t ico_idx;
} FrameInfo;

static FrameInfo decompose(uint16_t enc)
{
    FrameInfo f;
    f.face = (uint8_t)(enc / FACE_SZ);
    f.slot = (uint8_t)(enc % FACE_SZ);
    f.phase = (uint8_t)((enc / 12) % 12);
    f.ico_idx = (uint8_t)(enc % ICO_NODES);
    return f;
}

/* ── Stats ─────────────────────────────────────────────────── */
typedef struct {
    /* Timeline distribution */
    uint64_t timeline[CYCLE];       /* how many weights land on each enc */
    
    /* Face distribution */
    uint64_t face_count[N_FACES];   /* weights per face */
    
    /* Phase distribution */
    uint64_t phase_count[12];       /* weights per phase */
    
    /* Clustering */
    uint64_t total;
    double mean_per_slot;
    double max_per_slot;
    double min_per_slot;
    uint64_t empty_slots;
    uint64_t hot_slots;             /* slots with > 2× mean */
    
    /* Adjacency — how many weights land on adjacent encs */
    uint64_t adjacent_pairs;
    uint64_t same_face_pairs;
    
    /* Entropy */
    double entropy;
} GeoStats;

static void geo_stats_init(GeoStats *s) {
    memset(s, 0, sizeof(GeoStats));
    s->min_per_slot = 1e9;
}

static void geo_stats_add(GeoStats *s, uint16_t enc) {
    s->timeline[enc]++;
    s->total++;
    
    FrameInfo f = decompose(enc);
    s->face_count[f.face]++;
    s->phase_count[f.phase]++;
}

static void geo_stats_finalize(GeoStats *s) {
    /* Per-slot stats */
    uint64_t non_empty = 0;
    for (uint32_t i = 0; i < CYCLE; i++) {
        double c = (double)s->timeline[i];
        if (c > 0) {
            non_empty++;
            if (c < s->min_per_slot) s->min_per_slot = c;
            if (c > s->max_per_slot) s->max_per_slot = c;
            if (c > s->mean_per_slot * 2) s->hot_slots++;
        } else {
            s->empty_slots++;
        }
    }
    s->mean_per_slot = (double)s->total / (double)CYCLE;
    if (s->min_per_slot > 1e8) s->min_per_slot = 0;
    
    /* Adjacency analysis */
    for (uint32_t i = 0; i < CYCLE; i++) {
        uint32_t next = (i + 1) % CYCLE;
        if (s->timeline[i] > 0 && s->timeline[next] > 0) {
            s->adjacent_pairs++;
        }
        /* Same face */
        if (s->timeline[i] > 0) {
            uint8_t face = (uint8_t)(i / FACE_SZ);
            uint8_t next_face = (uint8_t)(next / FACE_SZ);
            if (face == next_face && s->timeline[next] > 0) {
                s->same_face_pairs++;
            }
        }
    }
    
    /* Shannon entropy */
    s->entropy = 0;
    for (uint32_t i = 0; i < CYCLE; i++) {
        if (s->timeline[i] > 0) {
            double p = (double)s->timeline[i] / (double)s->total;
            s->entropy -= p * log2(p);
        }
    }
}

static void geo_stats_print(GeoStats *s) {
    printf("=== GGUF Weight → Geometry Distribution ===\n\n");
    
    printf("--- Timeline (1440 enc slots) ---\n");
    printf("Total weights:     %lu\n", s->total);
    printf("Occupied slots:    %lu / 1440 (%.1f%%)\n", 
           CYCLE - s->empty_slots, 100.0 * (CYCLE - s->empty_slots) / CYCLE);
    printf("Empty slots:       %lu (%.1f%%)\n", 
           s->empty_slots, 100.0 * s->empty_slots / CYCLE);
    printf("Mean per slot:     %.1f\n", s->mean_per_slot);
    printf("Min per slot:      %.0f\n", s->min_per_slot);
    printf("Max per slot:      %.0f\n", s->max_per_slot);
    printf("Hot slots (>2×mean): %lu\n", s->hot_slots);
    printf("\n");
    
    printf("--- Face Distribution (12 faces) ---\n");
    for (int i = 0; i < N_FACES; i++) {
        double pct = 100.0 * s->face_count[i] / s->total;
        int bars = (int)(pct / 2);
        printf("  Face %2d: %10lu (%5.1f%%) ", i, s->face_count[i], pct);
        for (int b = 0; b < bars && b < 40; b++) printf("█");
        printf("\n");
    }
    printf("\n");
    
    printf("--- Phase Distribution (12 phases) ---\n");
    for (int i = 0; i < 12; i++) {
        double pct = 100.0 * s->phase_count[i] / s->total;
        int bars = (int)(pct / 2);
        printf("  Phase %2d: %10lu (%5.1f%%) ", i, s->phase_count[i], pct);
        for (int b = 0; b < bars && b < 40; b++) printf("█");
        printf("\n");
    }
    printf("\n");
    
    printf("--- Clustering ---\n");
    printf("Adjacent pairs:   %lu (weights landing on neighboring encs)\n", s->adjacent_pairs);
    printf("Same-face pairs:  %lu (weights on same face, adjacent enc)\n", s->same_face_pairs);
    printf("Clustering ratio: %.2f%% (adjacent / total)\n", 
           100.0 * s->adjacent_pairs / (s->total > 1 ? s->total - 1 : 1));
    printf("\n");
    
    printf("--- Entropy ---\n");
    printf("Shannon entropy:  %.2f bits (max = %.2f)\n", s->entropy, log2(CYCLE));
    printf("Utilization:      %.1f%% of 1440 slots\n", 
           100.0 * (CYCLE - s->empty_slots) / CYCLE);
    printf("If uniform:       entropy = %.2f\n", log2(CYCLE));
    printf("Actual / Max:     %.2f (%.1f%% of max)\n", 
           s->entropy / log2(CYCLE), 100.0 * s->entropy / log2(CYCLE));
    printf("\n");
    
    /* Visual: timeline heatmap (12 rows × 120 cols = 1440) */
    printf("--- Timeline Heatmap (12 faces × 120 slots) ---\n");
    printf("    ");
    for (int c = 0; c < 120; c += 10) printf("%3d", c);
    printf("\n");
    
    for (int face = 0; face < 12; face++) {
        printf("F%2d ", face);
        for (int slot = 0; slot < 120; slot++) {
            uint32_t enc = face * 120 + slot;
            uint64_t count = s->timeline[enc];
            char c;
            if (count == 0) c = '.';
            else if (count < 100) c = 'o';
            else if (count < 1000) c = 'O';
            else if (count < 10000) c = '#';
            else c = '@';
            printf("  %c", c);
        }
        printf("\n");
    }
    printf("\nLegend: .=0  o=<100  O=<1K  #=<10K  @=≥10K\n");
}

/* ── Main ──────────────────────────────────────────────────── */
static int analyze_gguf(const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) { fprintf(stderr, "Cannot open: %s\n", filename); return 1; }
    
    uint32_t magic;
    fread(&magic, 4, 1, f);
    if (magic != GGUF_MAGIC) {
        fprintf(stderr, "Not GGUF (magic: 0x%08X)\n", magic);
        fclose(f); return 1;
    }
    
    uint32_t version;
    fread(&version, 4, 1, f);
    printf("GGUF version: %u\n", version);
    
    uint64_t n_tensors;
    fread(&n_tensors, 8, 1, f);
    printf("Tensors: %lu\n", n_tensors);
    
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    printf("File size: %.1f MB\n\n", file_size / (1024.0 * 1024.0));
    
    /* Skip header (1MB) and read weight data */
    long header_skip = 1024 * 1024;
    fseek(f, header_skip, SEEK_SET);
    
    long data_size = file_size - header_skip;
    uint8_t *buffer = (uint8_t *)malloc(data_size);
    if (!buffer) { fclose(f); return 1; }
    
    size_t read = fread(buffer, 1, data_size, f);
    fclose(f);
    
    printf("Analyzing %zu bytes through stride-12-gon walk...\n\n", read);
    
    /* Analyze */
    GeoStats stats;
    geo_stats_init(&stats);
    
    /* Process in 48-byte chunks (RDH standard chunk size) */
    size_t chunk_sz = 48;
    uint64_t n_chunks = read / chunk_sz;
    
    printf("Processing %lu chunks (48 bytes each)...\n", n_chunks);
    
    for (uint64_t i = 0; i < n_chunks; i++) {
        uint16_t enc = stride_walk(buffer + i * chunk_sz, chunk_sz);
        geo_stats_add(&stats, enc);
        
        if (i % 1000000 == 0 && i > 0) {
            printf("  %lu / %lu chunks...\n", i, n_chunks);
        }
    }
    
    geo_stats_finalize(&stats);
    geo_stats_print(&stats);
    
    free(buffer);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s <gguf_file>\n", argv[0]);
        printf("\nShows where GGUF weights land on the 1440-frame geometry.\n");
        return 1;
    }
    return analyze_gguf(argv[1]);
}

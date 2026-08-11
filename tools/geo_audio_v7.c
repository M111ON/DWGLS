// geo_audio_v7.c — Hilbert 64×64 maze walker
// 4096 × 5 + 256 = 20736
// Build: gcc -O2 -Wall -o tools/geo_audio_v7.exe tools/geo_audio_v7.c -lm

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#define SAMPLE_RATE  16000
#define N_FFT        400
#define HOP_SIZE     160
#define N_MELS       128
#define GEO_FULL     20736

// Structure: 4096 × 5 + 256 = 20736
#define HILBERT_N    64
#define HILBERT_CELLS 4096   // 64×64
#define N_LAYERS     5
#define ANCHOR_SIZE  256     // 20736 - 4096×5 = 256
#define ANCHOR_BASE  (HILBERT_CELLS * N_LAYERS)  // 20480

static double g_win[N_FFT];
static double g_mel_filter[N_MELS][N_FFT/2+1];

// Hilbert curve: (x,y) → 1D index on 64×64 grid
static inline uint32_t hilbert_idx(uint32_t x, uint32_t y, uint32_t n) {
    uint32_t d = 0;
    for (uint32_t s = n >> 1; s > 0; s >>= 1) {
        uint32_t rx = (x & s) > 0;
        uint32_t ry = (y & s) > 0;
        d = (d << 2) | ((3u * rx) ^ ry);
        if (ry == 0) {
            if (rx == 1) { x = n - 1u - x; y = n - 1u - y; }
            uint32_t t = x; x = y; y = t;
        }
    }
    return d;
}

// Pre-compute Hilbert LUT
static uint32_t hilbert_lut[HILBERT_CELLS];  // xy → hilbert index

void init_hilbert(void) {
    for (uint32_t y = 0; y < HILBERT_N; y++)
        for (uint32_t x = 0; x < HILBERT_N; x++)
            hilbert_lut[y * HILBERT_N + x] = hilbert_idx(x, y, HILBERT_N);
}

void init_window(void) {
    for (int i = 0; i < N_FFT; i++)
        g_win[i] = 0.5 * (1.0 - cos(2.0 * M_PI * i / N_FFT));
}

void init_mel_filters(void) {
    double sr = SAMPLE_RATE;
    double mel_lo = 2595.0 * log10(1.0 + 0.0 / 700.0);
    double mel_hi = 2595.0 * log10(1.0 + sr/2.0 / 700.0);
    for (int m = 0; m < N_MELS; m++) {
        double mel_ctr = mel_lo + (mel_hi - mel_lo) * (m + 0.5) / N_MELS;
        double freq_ctr = 700.0 * (pow(10.0, mel_ctr / 2595.0) - 1.0);
        double bin_ctr = freq_ctr * N_FFT / sr;
        double width = (mel_hi - mel_lo) / N_MELS * N_FFT / sr;
        for (int k = 0; k < N_FFT/2+1; k++) {
            double dist = fabs(k - bin_ctr);
            g_mel_filter[m][k] = (dist < width) ? 1.0 - dist / width : 0.0;
        }
    }
}

int compute_mel(const int16_t *samples, int n_samples, double mel[][N_MELS]) {
    int n_frames = (n_samples - N_FFT) / HOP_SIZE;
    if (n_frames <= 0) return 0;
    for (int f = 0; f < n_frames; f++) {
        int offset = f * HOP_SIZE;
        double fft_re[N_FFT], fft_im[N_FFT];
        for (int i = 0; i < N_FFT; i++) {
            fft_re[i] = samples[offset + i] * g_win[i] / 32768.0;
            fft_im[i] = 0.0;
        }
        double power[N_FFT/2+1];
        for (int k = 0; k < N_FFT/2+1; k++) {
            double re = 0, im = 0;
            for (int n = 0; n < N_FFT; n++) {
                double angle = 2.0 * M_PI * k * n / N_FFT;
                re += fft_re[n] * cos(angle) + fft_im[n] * sin(angle);
                im += -fft_re[n] * sin(angle) + fft_im[n] * cos(angle);
            }
            power[k] = (re*re + im*im) / N_FFT;
        }
        for (int m = 0; m < N_MELS; m++) {
            double sum = 0;
            for (int k = 0; k < N_FFT/2+1; k++)
                sum += power[k] * g_mel_filter[m][k];
            mel[f][m] = log10(fmax(sum, 1e-10));
        }
    }
    return n_frames;
}

// ═══════════════════════════════════════════════════════════
// Hilbert Walker: data walks through 4096-position maze
//
// Hilbert 64×64 = 4096 walls (static structure)
// Mel amplitude = step size (1-31)
// Current position = address on maze
// 5 layers = time context
// 256 anchor = metadata at end
// ═══════════════════════════════════════════════════════════
typedef struct {
    uint32_t pos;           // current position on Hilbert path (0-4095)
    uint32_t total_steps;   // total steps taken
    uint32_t layer;         // current layer (0-4)
} HilbertWalker;

void walker_init(HilbertWalker *w) {
    w->pos = 0;
    w->total_steps = 0;
    w->layer = 0;
}

// Walk along Hilbert path
uint32_t walker_step(HilbertWalker *w, uint32_t step_size) {
    w->pos = (w->pos + step_size) % HILBERT_CELLS;
    w->total_steps++;
    // Advance layer every 819 steps (4096/5 ≈ 819)
    if (w->total_steps % (HILBERT_CELLS / N_LAYERS + 1) == 0)
        w->layer = (w->layer + 1) % N_LAYERS;
    return w->pos;
}

// Amplitude → step size (1-31)
uint32_t amp_to_step(double amplitude) {
    return (uint32_t)(1.0 + amplitude * 30.0);
}

// Hilbert position → 20736 address
uint32_t hilbert_to_addr(uint32_t hilbert_pos, uint32_t layer) {
    return (hilbert_pos + layer * HILBERT_CELLS) % ANCHOR_BASE;
}

// Process one frame through Hilbert maze
uint32_t process_frame(HilbertWalker *w, const double mel[], int n_mels) {
    double mel_min = 1e10, mel_max = -1e10;
    for (int m = 0; m < n_mels; m++) {
        if (mel[m] < mel_min) mel_min = mel[m];
        if (mel[m] > mel_max) mel_max = mel[m];
    }
    double range = mel_max - mel_min;
    if (range < 1e-10) range = 1.0;
    
    for (int m = 0; m < n_mels; m++) {
        double norm = (mel[m] - mel_min) / range;
        double amplitude = fmax(0, fmin(1, norm));
        uint32_t step = amp_to_step(amplitude);
        walker_step(w, step);
    }
    
    return hilbert_to_addr(w->pos, w->layer);
}

// ═══════════════════════════════════════════════════════════
// Maze tracking + pattern extraction
// ═══════════════════════════════════════════════════════════
typedef struct {
    uint32_t hit_count;
    uint32_t first_hit;
    uint32_t last_hit;
    double   total_energy;
} MazeNode;

void track_maze(const double mel[][N_MELS], int n_frames, MazeNode *nodes) {
    memset(nodes, 0, sizeof(MazeNode) * GEO_FULL);
    HilbertWalker walker;
    walker_init(&walker);
    
    for (int f = 0; f < n_frames; f++) {
        uint32_t addr = process_frame(&walker, mel[f], N_MELS);
        nodes[addr].hit_count++;
        if (nodes[addr].hit_count == 1)
            nodes[addr].first_hit = (uint32_t)f;
        nodes[addr].last_hit = (uint32_t)f;
        // Reference energy
        double sum = 0;
        for (int m = 0; m < N_MELS; m++) sum += mel[f][m] * mel[f][m];
        nodes[addr].total_energy += sum;
    }
}

int extract_pattern(const MazeNode *nodes, uint32_t *pattern, uint32_t min_hits) {
    int count = 0;
    for (uint32_t i = 0; i < GEO_FULL; i++) {
        if (nodes[i].hit_count >= min_hits)
            pattern[count++] = i;
    }
    return count;
}

double jaccard(const uint32_t *a, int na, const uint32_t *b, int nb) {
    int inter = 0, i = 0, j = 0;
    while (i < na && j < nb) {
        if (a[i] == b[j]) { inter++; i++; j++; }
        else if (a[i] < b[j]) i++;
        else j++;
    }
    int uni = na + nb - inter;
    return uni > 0 ? (double)inter / uni : 0.0;
}

void sort_arr(uint32_t *arr, int n) {
    for (int i = 0; i < n-1; i++)
        for (int j = i+1; j < n; j++)
            if (arr[i] > arr[j]) {
                uint32_t t = arr[i]; arr[i] = arr[j]; arr[j] = t;
            }
}

int16_t* read_wav(const char *path, int *n_samples) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 44, SEEK_SET);
    int16_t *data = (int16_t*)malloc(size - 44);
    if (!data) { fclose(f); return NULL; }
    *n_samples = (size - 44) / 2;
    fread(data, 2, *n_samples, f);
    fclose(f);
    return data;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s <wav_file> [wav_file2 ...]\n", argv[0]);
        return 1;
    }
    
    init_window();
    init_mel_filters();
    init_hilbert();
    
    printf("=== Hilbert Maze Audio Codec v7 ===\n");
    printf("Structure: %d × %d + %d = %d\n", HILBERT_CELLS, N_LAYERS, ANCHOR_SIZE, GEO_FULL);
    printf("Hilbert: %d×%d = %d walls\n", HILBERT_N, HILBERT_N, HILBERT_CELLS);
    printf("Layers: %d (time context)\n", N_LAYERS);
    printf("Anchor: %d (metadata)\n\n", ANCHOR_SIZE);
    
    int n_files = argc - 1;
    if (n_files > 10) n_files = 10;
    
    MazeNode *all_nodes[10];
    uint32_t *all_patterns[10];
    int all_lens[10];
    
    for (int fi = 0; fi < n_files; fi++) {
        int n_samples;
        int16_t *samples = read_wav(argv[fi + 1], &n_samples);
        if (!samples) { printf("Error: %s\n", argv[fi+1]); continue; }
        
        int max_frames = (n_samples - N_FFT) / HOP_SIZE;
        double (*mel)[N_MELS] = malloc(sizeof(double) * max_frames * N_MELS);
        int n_frames = compute_mel(samples, n_samples, mel);
        
        printf("═══ File %d: %s ═══\n", fi+1, argv[fi+1]);
        printf("  Samples: %d (%.2f sec), Frames: %d\n", 
               n_samples, (double)n_samples/SAMPLE_RATE, n_frames);
        
        all_nodes[fi] = calloc(GEO_FULL, sizeof(MazeNode));
        track_maze(mel, n_frames, all_nodes[fi]);
        
        // Stats
        int hit_nodes = 0;
        uint32_t max_hits = 0;
        for (uint32_t i = 0; i < GEO_FULL; i++) {
            if (all_nodes[fi][i].hit_count > 0) {
                hit_nodes++;
                if (all_nodes[fi][i].hit_count > max_hits)
                    max_hits = all_nodes[fi][i].hit_count;
            }
        }
        
        printf("  Maze nodes hit: %d/%d (%.1f%%)\n", 
               hit_nodes, GEO_FULL, 100.0*hit_nodes/GEO_FULL);
        printf("  Max hits: %u\n", max_hits);
        
        // Extract pattern (nodes hit ≥ 2 times)
        all_patterns[fi] = malloc(sizeof(uint32_t) * GEO_FULL);
        all_lens[fi] = extract_pattern(all_nodes[fi], all_patterns[fi], 2);
        sort_arr(all_patterns[fi], all_lens[fi]);
        printf("  Pattern (≥2 hits): %d nodes\n", all_lens[fi]);
        
        // Hilbert layer distribution
        int layer_hits[N_LAYERS] = {0};
        for (uint32_t i = 0; i < ANCHOR_BASE; i++)
            if (all_nodes[fi][i].hit_count > 0)
                layer_hits[i / HILBERT_CELLS]++;
        printf("  Layer distribution: ");
        for (int l = 0; l < N_LAYERS; l++)
            printf("L%d=%d ", l, layer_hits[l]);
        printf("\n\n");
        
        free(mel);
        free(samples);
    }
    
    // Compare patterns
    if (n_files > 1) {
        printf("═══ Pattern Comparison (Jaccard) ═══\n");
        for (int i = 0; i < n_files; i++) {
            for (int j = i+1; j < n_files; j++) {
                double sim = jaccard(all_patterns[i], all_lens[i],
                                    all_patterns[j], all_lens[j]);
                printf("  File %d ↔ %d: %.4f (pat %d vs %d)\n", 
                       i+1, j+1, sim, all_lens[i], all_lens[j]);
            }
        }
    }
    
    for (int i = 0; i < n_files; i++) {
        free(all_nodes[i]);
        free(all_patterns[i]);
    }
    
    return 0;
}

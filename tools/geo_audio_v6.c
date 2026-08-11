// geo_audio_v6.c — 128 mel × 162 = 20736, Hilbert maze tracking
// Build: gcc -O2 -Wall -o tools/geo_audio_v6.exe tools/geo_audio_v6.c -lm

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#define SAMPLE_RATE  16000
#define N_FFT        400
#define HOP_SIZE     160
#define N_MELS       128
#define GEO_FULL     20736   // 128 × 162

// 162 = 2 × 81 = 2 × 9 × 9
#define STRIDE_SIDE  162     // direct multiplier
#define TOWER_SIDE   9       // sqrt(81)
#define N_LAYERS     2       // 162 / 81

// Hilbert 16×16 = 256 (maze walls)
#define HILBERT_N    16
#define HILBERT_CELLS 256

static double g_win[N_FFT];
static double g_mel_filter[N_MELS][N_FFT/2+1];

// ═══════════════════════════════════════════════════════════
// Hilbert curve: (x,y) → 1D index on 16×16 grid
// This is the MAZE WALL structure (static)
// ═══════════════════════════════════════════════════════════
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

// Pre-compute Hilbert lookup: position → index
static uint32_t hilbert_lut[HILBERT_CELLS];  // xy → hilbert index
static uint32_t hilbert_inv[HILBERT_CELLS];  // hilbert index → xy

void init_hilbert(void) {
    for (uint32_t y = 0; y < HILBERT_N; y++) {
        for (uint32_t x = 0; x < HILBERT_N; x++) {
            uint32_t idx = hilbert_idx(x, y, HILBERT_N);
            uint32_t xy = y * HILBERT_N + x;
            hilbert_lut[xy] = idx;
            hilbert_inv[idx] = xy;
        }
    }
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
// Core: mel → Hilbert maze walk
// 
// Hilbert = movement path (static structure)
// Mel data = steps along the path
// Current position = address
//
// 128 mel bins × 162 = 20736
// Hilbert path: 256 positions on 16×16 grid
// ═══════════════════════════════════════════════════════════

// Track current position on Hilbert path
typedef struct {
    uint32_t pos;         // current position on Hilbert path (0-255)
    uint32_t total_steps; // total steps taken
} HilbertWalker;

// Initialize walker at start of Hilbert path
void walker_init(HilbertWalker *w) {
    w->pos = 0;
    w->total_steps = 0;
}

// Walk along Hilbert path by step_size
// Returns new position
uint32_t walker_step(HilbertWalker *w, uint32_t step_size) {
    // Each step moves forward along Hilbert path
    // step_size determines how far to move
    w->pos = (w->pos + step_size) % HILBERT_CELLS;
    w->total_steps++;
    return w->pos;
}

// Map mel amplitude to step size
uint32_t amp_to_step(double amplitude) {
    // amplitude 0.0 → step 1 (minimum movement)
    // amplitude 1.0 → step 15 (maximum movement)
    return (uint32_t)(1.0 + amplitude * 14.0);
}

// Get 20736 address from Hilbert position + tower
uint32_t hilbert_to_addr(uint32_t hilbert_pos, uint32_t tower, uint32_t layer) {
    // hilbert_pos: 0-255 (position on 16×16 maze)
    // tower: 0-80 (9×9 amplitude × time)
    // layer: 0-1 (context)
    return (hilbert_pos + tower * HILBERT_CELLS + layer * HILBERT_CELLS * TOWER_SIDE) % GEO_FULL;
}

// Process one frame: walk through Hilbert maze
uint32_t process_frame(HilbertWalker *w, const double mel[], int n_mels, int frame_idx) {
    // Normalize this frame
    double mel_min = 1e10, mel_max = -1e10;
    for (int m = 0; m < n_mels; m++) {
        if (mel[m] < mel_min) mel_min = mel[m];
        if (mel[m] > mel_max) mel_max = mel[m];
    }
    double range = mel_max - mel_min;
    if (range < 1e-10) range = 1.0;
    
    // Walk through each mel bin
    for (int m = 0; m < n_mels; m++) {
        double norm = (mel[m] - mel_min) / range;
        double amplitude = fmax(0, fmin(1, norm));
        uint32_t step = amp_to_step(amplitude);
        walker_step(w, step);
    }
    
    // Current position → address
    uint32_t tower = (uint32_t)(frame_idx % TOWER_SIDE);
    uint32_t layer = (uint32_t)(frame_idx / TOWER_SIDE) % N_LAYERS;
    
    return hilbert_to_addr(w->pos, tower, layer);
}

// ═══════════════════════════════════════════════════════════
// Hilbert maze tracking: which nodes get hit
// This creates the PATTERN (fingerprint)
// ═══════════════════════════════════════════════════════════
typedef struct {
    uint32_t hit_count;      // how many times this node was hit
    uint32_t first_hit;      // first frame that hit this node
    uint32_t last_hit;       // last frame that hit this node
    double   total_energy;   // cumulative energy at this node
} MazeNode;

void track_maze(const double mel[][N_MELS], int n_frames, MazeNode *nodes) {
    memset(nodes, 0, sizeof(MazeNode) * GEO_FULL);
    
    HilbertWalker walker;
    walker_init(&walker);
    
    for (int f = 0; f < n_frames; f++) {
        uint32_t addr = process_frame(&walker, mel[f], N_MELS, f);
        
        nodes[addr].hit_count++;
        if (nodes[addr].hit_count == 1)
            nodes[addr].first_hit = (uint32_t)f;
        nodes[addr].last_hit = (uint32_t)f;
        nodes[addr].total_energy += mel[f][0]; // reference energy
    }
}

// Extract pattern: which nodes are ON (hit > threshold)
int extract_pattern(const MazeNode *nodes, uint32_t *pattern, double threshold) {
    int count = 0;
    for (uint32_t i = 0; i < GEO_FULL; i++) {
        if (nodes[i].hit_count > 0) {
            // ON = node was hit, weighted by energy
            double score = nodes[i].hit_count * nodes[i].total_energy;
            if (score > threshold || threshold <= 0) {
                pattern[count++] = i;
            }
        }
    }
    return count;
}

// Compare two patterns (Jaccard similarity)
double compare_patterns(const uint32_t *a, int len_a, const uint32_t *b, int len_b) {
    int intersection = 0, union_count = 0;
    int i = 0, j = 0;
    
    // Both arrays must be sorted for this to work
    while (i < len_a && j < len_b) {
        if (a[i] == b[j]) {
            intersection++;
            i++; j++;
        } else if (a[i] < b[j]) {
            i++;
        } else {
            j++;
        }
    }
    union_count = len_a + len_b - intersection;
    
    return union_count > 0 ? (double)intersection / union_count : 0.0;
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
    
    printf("Structure: 128 mel × 162 = %d\n", GEO_FULL);
    printf("162 = 2 × 9 × 9\n");
    printf("Hilbert maze: 16×16 = %d walls\n\n", HILBERT_CELLS);
    
    // Process all input files
    MazeNode *all_nodes[10];
    int n_files = argc - 1;
    if (n_files > 10) n_files = 10;
    
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
        
        // Track maze
        all_nodes[fi] = calloc(GEO_FULL, sizeof(MazeNode));
        track_maze(mel, n_frames, all_nodes[fi]);
        
        // Statistics
        int hit_nodes = 0;
        uint32_t max_hits = 0;
        double total_energy = 0;
        for (uint32_t i = 0; i < GEO_FULL; i++) {
            if (all_nodes[fi][i].hit_count > 0) {
                hit_nodes++;
                if (all_nodes[fi][i].hit_count > max_hits)
                    max_hits = all_nodes[fi][i].hit_count;
                total_energy += all_nodes[fi][i].total_energy;
            }
        }
        
        printf("  Maze nodes hit: %d/%d (%.1f%%)\n", 
               hit_nodes, GEO_FULL, 100.0*hit_nodes/GEO_FULL);
        printf("  Max hits at node: %u\n", max_hits);
        printf("  Total energy: %.1f\n", total_energy);
        
        // Extract pattern
        uint32_t *pattern = malloc(sizeof(uint32_t) * GEO_FULL);
        int pat_len = extract_pattern(all_nodes[fi], pattern, 0);
        
        // Sort pattern for comparison
        for (int i = 0; i < pat_len-1; i++)
            for (int j = i+1; j < pat_len; j++)
                if (pattern[i] > pattern[j]) {
                    uint32_t tmp = pattern[i]; pattern[i] = pattern[j]; pattern[j] = tmp;
                }
        
        printf("  Pattern size: %d nodes\n", pat_len);
        
        // Show Hilbert distribution
        printf("  Hilbert maze activity:\n");
        int hz_hit[HILBERT_CELLS] = {0};
        for (uint32_t i = 0; i < GEO_FULL; i++) {
            if (all_nodes[fi][i].hit_count > 0) {
                uint32_t h = i / STRIDE_SIDE;  // which Hilbert cell
                if (h < HILBERT_CELLS) hz_hit[h]++;
            }
        }
        int hz_active = 0;
        for (int i = 0; i < HILBERT_CELLS; i++)
            if (hz_hit[i] > 0) hz_active++;
        printf("    Hilbert cells active: %d/256\n", hz_active);
        
        free(pattern);
        free(mel);
        free(samples);
    }
    
    // Compare patterns
    if (n_files > 1) {
        printf("\n═══ Pattern Comparison ═══\n");
        for (int i = 0; i < n_files; i++) {
            for (int j = i+1; j < n_files; j++) {
                uint32_t *pat_i = malloc(sizeof(uint32_t) * GEO_FULL);
                uint32_t *pat_j = malloc(sizeof(uint32_t) * GEO_FULL);
                int len_i = extract_pattern(all_nodes[i], pat_i, 0);
                int len_j = extract_pattern(all_nodes[j], pat_j, 0);
                
                // Sort
                for (int a = 0; a < len_i-1; a++)
                    for (int b = a+1; b < len_i; b++)
                        if (pat_i[a] > pat_i[b]) {
                            uint32_t t = pat_i[a]; pat_i[a] = pat_i[b]; pat_i[b] = t;
                        }
                for (int a = 0; a < len_j-1; a++)
                    for (int b = a+1; b < len_j; b++)
                        if (pat_j[a] > pat_j[b]) {
                            uint32_t t = pat_j[a]; pat_j[a] = pat_j[b]; pat_j[b] = t;
                        }
                
                double sim = compare_patterns(pat_i, len_i, pat_j, len_j);
                printf("  File %d ↔ %d: Jaccard=%.4f (pat %d vs %d)\n", 
                       i+1, j+1, sim, len_i, len_j);
                
                free(pat_i);
                free(pat_j);
            }
        }
    }
    
    for (int i = 0; i < n_files; i++)
        free(all_nodes[i]);
    
    return 0;
}

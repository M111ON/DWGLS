// geo_audio_v7b.c — Comprehensive Hilbert maze test
// Test multiple configurations: Hilbert size, step range, word discrimination
// Build: gcc -O2 -Wall -o tools/geo_audio_v7b.exe tools/geo_audio_v7b.c -lm

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
#define MAX_FRAMES   2000

static double g_win[N_FFT];
static double g_mel_filter[N_MELS][N_FFT/2+1];

// Hilbert curve on any 2^n grid
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
    if (n_frames > MAX_FRAMES) n_frames = MAX_FRAMES;
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

// ═══════════════════════════════════════════════════════════
// Walker with configurable Hilbert size and step range
// ═══════════════════════════════════════════════════════════
typedef struct {
    uint32_t pos;
    uint32_t hilbert_cells;
    uint32_t max_step;
} Walker;

void walker_init(Walker *w, uint32_t hilbert_cells, uint32_t max_step) {
    w->pos = 0;
    w->hilbert_cells = hilbert_cells;
    w->max_step = max_step;
}

uint32_t walker_step(Walker *w, uint32_t step) {
    if (step > w->max_step) step = w->max_step;
    w->pos = (w->pos + step) % w->hilbert_cells;
    return w->pos;
}

// Process frame with configurable walker
void process_frame_cfg(Walker *w, const double mel[], int n_mels, 
                       uint32_t *hit_nodes, int *n_hit) {
    double mel_min = 1e10, mel_max = -1e10;
    for (int m = 0; m < n_mels; m++) {
        if (mel[m] < mel_min) mel_min = mel[m];
        if (mel[m] > mel_max) mel_max = mel[m];
    }
    double range = mel_max - mel_min;
    if (range < 1e-10) range = 1.0;
    
    *n_hit = 0;
    for (int m = 0; m < n_mels; m++) {
        double norm = (mel[m] - mel_min) / range;
        double amplitude = fmax(0, fmin(1, norm));
        uint32_t step = (uint32_t)(1.0 + amplitude * (w->max_step - 1));
        uint32_t pos = walker_step(w, step);
        hit_nodes[(*n_hit)++] = pos;
    }
}

// Build hit pattern from all frames
int build_pattern(const double mel[][N_MELS], int n_frames,
                  uint32_t hilbert_cells, uint32_t max_step,
                  uint32_t *pattern) {
    Walker walker;
    walker_init(&walker, hilbert_cells, max_step);
    
    uint32_t *hit_count = calloc(hilbert_cells, sizeof(uint32_t));
    uint32_t *frame_hits = malloc(sizeof(uint32_t) * N_MELS);
    int n_hit;
    
    for (int f = 0; f < n_frames; f++) {
        process_frame_cfg(&walker, mel[f], N_MELS, frame_hits, &n_hit);
        for (int i = 0; i < n_hit; i++)
            hit_count[frame_hits[i]]++;
    }
    
    int count = 0;
    for (uint32_t i = 0; i < hilbert_cells; i++)
        if (hit_count[i] >= 2)  // nodes hit at least twice
            pattern[count++] = i;
    
    free(hit_count);
    free(frame_hits);
    return count;
}

// Jaccard similarity
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

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s <wav_file> [wav_file2 ...]\n", argv[0]);
        return 1;
    }
    
    init_window();
    init_mel_filters();
    
    int n_files = argc - 1;
    if (n_files > 20) n_files = 20;
    
    printf("=== Hilbert Maze Comprehensive Test ===\n");
    printf("Files: %d\n\n", n_files);
    
    // Load all files
    double (*all_mel[20])[N_MELS];
    int all_frames[20];
    
    for (int fi = 0; fi < n_files; fi++) {
        int n_samples;
        int16_t *samples = read_wav(argv[fi + 1], &n_samples);
        if (!samples) { printf("Error: %s\n", argv[fi+1]); n_files--; fi--; continue; }
        
        int max_frames = (n_samples - N_FFT) / HOP_SIZE;
        if (max_frames > MAX_FRAMES) max_frames = MAX_FRAMES;
        all_mel[fi] = malloc(sizeof(double) * max_frames * N_MELS);
        all_frames[fi] = compute_mel(samples, n_samples, all_mel[fi]);
        printf("File %d: %s (%d frames)\n", fi+1, argv[fi+1], all_frames[fi]);
        free(samples);
    }
    printf("\n");
    
    // ═══ Test 1: Different Hilbert sizes ═══
    printf("═══ Test 1: Hilbert Size Comparison ═══\n");
    uint32_t hilbert_sizes[] = {256, 1024, 4096, 16384};
    int n_sizes = 4;
    
    for (int s = 0; s < n_sizes; s++) {
        uint32_t hs = hilbert_sizes[s];
        printf("\n  Hilbert %d×%d = %d:\n", (int)sqrt(hs), (int)sqrt(hs), hs);
        
        uint32_t *pats[20];
        int lens[20];
        
        for (int fi = 0; fi < n_files; fi++) {
            pats[fi] = malloc(sizeof(uint32_t) * hs);
            lens[fi] = build_pattern(all_mel[fi], all_frames[fi], hs, 15, pats[fi]);
            sort_arr(pats[fi], lens[fi]);
        }
        
        // Compare
        for (int i = 0; i < n_files; i++) {
            for (int j = i+1; j < n_files; j++) {
                double sim = jaccard(pats[i], lens[i], pats[j], lens[j]);
                printf("    %d↔%d: %.4f (pat %d vs %d)\n", i+1, j+1, sim, lens[i], lens[j]);
            }
        }
        
        for (int fi = 0; fi < n_files; fi++) free(pats[fi]);
    }
    
    // ═══ Test 2: Different step ranges ═══
    printf("\n═══ Test 2: Step Range Comparison (Hilbert 4096) ═══\n");
    uint32_t step_ranges[] = {3, 7, 15, 31, 63};
    int n_steps = 5;
    
    for (int s = 0; s < n_steps; s++) {
        uint32_t ms = step_ranges[s];
        printf("\n  Step range 1-%d:\n", ms);
        
        uint32_t *pats[20];
        int lens[20];
        
        for (int fi = 0; fi < n_files; fi++) {
            pats[fi] = malloc(sizeof(uint32_t) * 4096);
            lens[fi] = build_pattern(all_mel[fi], all_frames[fi], 4096, ms, pats[fi]);
            sort_arr(pats[fi], lens[fi]);
        }
        
        for (int i = 0; i < n_files; i++) {
            for (int j = i+1; j < n_files; j++) {
                double sim = jaccard(pats[i], lens[i], pats[j], lens[j]);
                printf("    %d↔%d: %.4f (pat %d vs %d)\n", i+1, j+1, sim, lens[i], lens[j]);
            }
        }
        
        for (int fi = 0; fi < n_files; fi++) free(pats[fi]);
    }
    
    // ═══ Test 3: Min-hits threshold ═══
    printf("\n═══ Test 3: Min-Hits Threshold (Hilbert 4096, step 1-15) ═══\n");
    int min_hits_list[] = {1, 2, 3, 5, 10};
    int n_thresholds = 5;
    
    for (int t = 0; t < n_thresholds; t++) {
        int mh = min_hits_list[t];
        printf("\n  Min hits ≥ %d:\n", mh);
        
        uint32_t *pats[20];
        int lens[20];
        
        for (int fi = 0; fi < n_files; fi++) {
            Walker walker;
            walker_init(&walker, 4096, 15);
            uint32_t *hit_count = calloc(4096, sizeof(uint32_t));
            uint32_t *fh = malloc(sizeof(uint32_t) * N_MELS);
            int nh;
            
            for (int f = 0; f < all_frames[fi]; f++) {
                process_frame_cfg(&walker, all_mel[fi][f], N_MELS, fh, &nh);
                for (int i = 0; i < nh; i++) hit_count[fh[i]]++;
            }
            
            lens[fi] = 0;
            pats[fi] = malloc(sizeof(uint32_t) * 4096);
            for (uint32_t i = 0; i < 4096; i++)
                if (hit_count[i] >= mh)
                    pats[fi][lens[fi]++] = i;
            sort_arr(pats[fi], lens[fi]);
            
            free(hit_count);
            free(fh);
        }
        
        for (int i = 0; i < n_files; i++) {
            for (int j = i+1; j < n_files; j++) {
                double sim = jaccard(pats[i], lens[i], pats[j], lens[j]);
                printf("    %d↔%d: %.4f (pat %d vs %d)\n", i+1, j+1, sim, lens[i], lens[j]);
            }
        }
        
        for (int fi = 0; fi < n_files; fi++) free(pats[fi]);
    }
    
    // Cleanup
    for (int fi = 0; fi < n_files; fi++)
        free(all_mel[fi]);
    
    return 0;
}

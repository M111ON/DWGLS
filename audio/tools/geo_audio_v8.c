// geo_audio_v8.c — Hilbert maze SEQUENCE tracking
// Record trajectory (position per frame), compare sequences
// Build: gcc -O2 -Wall -o tools/geo_audio_v8.exe tools/geo_audio_v8.c -lm

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
#define MAX_TRAJ     2000

static double g_win[N_FFT];
static double g_mel_filter[N_MELS][N_FFT/2+1];

// Hilbert curve: (x,y) → 1D index
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
// Hilbert Walker + SEQUENCE recording
// ═══════════════════════════════════════════════════════════
typedef struct {
    uint32_t pos;
    uint32_t hilbert_cells;
    uint32_t max_step;
} Walker;

void walker_init(Walker *w, uint32_t cells, uint32_t max_step) {
    w->pos = 0;
    w->hilbert_cells = cells;
    w->max_step = max_step;
}

uint32_t walker_step(Walker *w, uint32_t step) {
    if (step > w->max_step) step = w->max_step;
    w->pos = (w->pos + step) % w->hilbert_cells;
    return w->pos;
}

// Process frame: walk 128 mel bins → record ONE position per bin
int process_frame_seq(Walker *w, const double mel[], int n_mels, uint32_t *seq) {
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
        uint32_t step = (uint32_t)(1.0 + amplitude * (w->max_step - 1));
        seq[m] = walker_step(w, step);
    }
    return n_mels;
}

// Build full trajectory: one position per mel bin per frame
// trajectory[f * n_mels + m] = position on Hilbert path
int build_trajectory(const double mel[][N_MELS], int n_frames,
                     uint32_t hilbert_cells, uint32_t max_step,
                     uint32_t *traj) {
    Walker walker;
    walker_init(&walker, hilbert_cells, max_step);
    int total = 0;
    for (int f = 0; f < n_frames; f++) {
        process_frame_seq(&walker, mel[f], N_MELS, traj + total);
        total += N_MELS;
    }
    return total;
}

// ═══════════════════════════════════════════════════════════
// Sequence comparison methods
// ═══════════════════════════════════════════════════════════

// 1. Position transition histogram (how often does pos A → pos B)
// This captures the DYNAMICS of the walker
void transition_histogram(const uint32_t *traj, int len, uint32_t *hist, uint32_t hist_size) {
    memset(hist, 0, sizeof(uint32_t) * hist_size);
    for (int i = 1; i < len; i++) {
        uint32_t from = traj[i-1];
        uint32_t to = traj[i];
        // Quantize: from_row * sqrt(hist_size) + to_row
        uint32_t bins = (uint32_t)sqrt((double)hist_size);
        uint32_t from_q = from * bins / 64;  // normalize to grid
        uint32_t to_q = to * bins / 64;
        uint32_t key = (from_q * bins + to_q) % hist_size;
        hist[key]++;
    }
}

// 2. Direction histogram (which direction does walker move)
void direction_histogram(const uint32_t *traj, int len, uint32_t cells, int hist[8]) {
    memset(hist, 0, sizeof(int) * 8);
    for (int i = 1; i < len; i++) {
        int32_t dx = (int32_t)(traj[i] % (uint32_t)sqrt(cells)) - 
                     (int32_t)(traj[i-1] % (uint32_t)sqrt(cells));
        int32_t dy = (int32_t)(traj[i] / (uint32_t)sqrt(cells)) - 
                     (int32_t)(traj[i-1] / (uint32_t)sqrt(cells));
        // 8 directions: N, NE, E, SE, S, SW, W, NW
        int dir = 0;
        if (dy < 0) dir |= 1;  // North
        if (dy > 0) dir |= 4;  // South
        if (dx > 0) dir |= 2;  // East
        if (dx < 0) dir |= 8;  // West
        // Map to 0-7
        switch(dir) {
            case 1: hist[0]++; break;    // N
            case 3: hist[1]++; break;    // NE
            case 2: hist[2]++; break;    // E
            case 6: hist[3]++; break;    // SE
            case 4: hist[4]++; break;    // S
            case 12: hist[5]++; break;   // SW
            case 8: hist[6]++; break;    // W
            case 9: hist[7]++; break;    // NW
            default: break;
        }
    }
}

// 3. Step size distribution
void step_histogram(const uint32_t *traj, int len, uint32_t max_step, int hist[64]) {
    memset(hist, 0, sizeof(int) * 64);
    for (int i = 1; i < len; i++) {
        int32_t diff = (int32_t)traj[i] - (int32_t)traj[i-1];
        if (diff < 0) diff = -diff;
        int bucket = diff % 64;
        hist[bucket]++;
    }
}

// Cosine similarity between two histograms
double cos_sim_int(const int *a, const int *b, int n) {
    double dot = 0, na = 0, nb = 0;
    for (int i = 0; i < n; i++) {
        dot += a[i] * b[i];
        na += a[i] * a[i];
        nb += b[i] * b[i];
    }
    return dot / (sqrt(na) * sqrt(nb) + 1e-10);
}

// Normalized histogram cosine
double hist_cos(const uint32_t *a, const uint32_t *b, int n) {
    double dot = 0, na = 0, nb = 0;
    for (int i = 0; i < n; i++) {
        dot += (double)a[i] * b[i];
        na += (double)a[i] * a[i];
        nb += (double)b[i] * b[i];
    }
    return dot / (sqrt(na) * sqrt(nb) + 1e-10);
}

// DTW (Dynamic Time Warping) distance
double dtw(const uint32_t *a, int na, const uint32_t *b, int nb) {
    // Use first 200 frames max
    int max = 200;
    if (na > max) na = max;
    if (nb > max) nb = max;
    
    double *dt = malloc(sizeof(double) * (na+1) * (nb+1));
    #define DT(i,j) dt[(i)*(nb+1)+(j)]
    
    DT(0,0) = 0;
    for (int i = 1; i <= na; i++) DT(i,0) = 1e18;
    for (int j = 1; j <= nb; j++) DT(0,j) = 1e18;
    
    for (int i = 1; i <= na; i++) {
        for (int j = 1; j <= nb; j++) {
            double cost = fabs((double)a[i-1] - (double)b[j-1]);
            double min_prev = DT(i-1,j);
            if (DT(i,j-1) < min_prev) min_prev = DT(i,j-1);
            if (DT(i-1,j-1) < min_prev) min_prev = DT(i-1,j-1);
            DT(i,j) = cost + min_prev;
        }
    }
    
    double result = DT(na,nb) / (na + nb);
    free(dt);
    return result;
    #undef DT
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
    
    printf("=== Hilbert Maze SEQUENCE Tracker v8 ===\n\n");
    
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
    
    // ═══ Test: Multiple Hilbert sizes + comparison methods ═══
    uint32_t sizes[] = {256, 1024, 4096, 16384};
    int n_sizes = 4;
    
    for (int s = 0; s < n_sizes; s++) {
        uint32_t hs = sizes[s];
        uint32_t root = (uint32_t)sqrt((double)hs);
        printf("═══ Hilbert %d×%d = %d ═══\n", root, root, hs);
        
        // Build trajectories
        uint32_t *trajs[20];
        int traj_lens[20];
        for (int fi = 0; fi < n_files; fi++) {
            trajs[fi] = malloc(sizeof(uint32_t) * all_frames[fi] * N_MELS);
            traj_lens[fi] = build_trajectory(all_mel[fi], all_frames[fi], hs, 15, trajs[fi]);
        }
        
        // Method 1: Direction histogram cosine
        printf("\n  Direction histogram cosine (higher = more similar):\n");
        for (int i = 0; i < n_files; i++) {
            for (int j = i+1; j < n_files; j++) {
                int dir_a[8], dir_b[8];
                direction_histogram(trajs[i], traj_lens[i], hs, dir_a);
                direction_histogram(trajs[j], traj_lens[j], hs, dir_b);
                double sim = cos_sim_int(dir_a, dir_b, 8);
                printf("    %d↔%d: %.4f\n", i+1, j+1, sim);
            }
        }
        
        // Method 2: DTW distance (lower = more similar)
        printf("\n  DTW distance (lower = more similar):\n");
        for (int i = 0; i < n_files; i++) {
            for (int j = i+1; j < n_files; j++) {
                double dist = dtw(trajs[i], traj_lens[i], trajs[j], traj_lens[j]);
                printf("    %d↔%d: %.2f\n", i+1, j+1, dist);
            }
        }
        
        // Method 3: Trajectory sample (first 20 positions)
        printf("\n  First 20 positions:\n");
        for (int fi = 0; fi < n_files; fi++) {
            printf("    File %d: ", fi+1);
            for (int i = 0; i < 20 && i < traj_lens[fi]; i++)
                printf("%u ", trajs[fi][i]);
            printf("\n");
        }
        
        for (int fi = 0; fi < n_files; fi++) free(trajs[fi]);
        printf("\n");
    }
    
    for (int fi = 0; fi < n_files; fi++) free(all_mel[fi]);
    return 0;
}

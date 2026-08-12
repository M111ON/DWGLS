// geo_audio_v5.c — Hilbert curve mel→address mapping
// 64 Hz (Hilbert) × 4 sets × 81 tower = 20736
// Build: gcc -O2 -Wall -o tools/geo_audio_v5.exe tools/geo_audio_v5.c -lm

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

// Structure: 64 Hz (Hilbert) × 4 sets × 81 tower
#define HZ_CELLS     64
#define HZ_SIDE      8     // sqrt(64)
#define N_SETS       4
#define TOWER_CELLS  81
#define TOWER_SIDE   9

static double g_win[N_FFT];
static double g_mel_filter[N_MELS][N_FFT/2+1];

// Hilbert curve: (x,y) → 1D index on 8×8 grid
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

// Hilbert-based mel→address
uint32_t mel_to_addr_hilbert(double mel_bin, double amplitude, int frame_idx) {
    // Layer 1: Hz via Hilbert curve (8×8 = 64 cells)
    int bin_idx = (int)mel_bin % HZ_CELLS;
    int hz_x = bin_idx % HZ_SIDE;
    int hz_y = bin_idx / HZ_SIDE;
    uint32_t hilbert_cell = hilbert_idx((uint32_t)hz_x, (uint32_t)hz_y, HZ_SIDE);
    
    // Layer 2: Set (4 contexts) — based on actual frequency
    int set_id;
    double freq_hz = mel_bin * (SAMPLE_RATE / 2.0) / N_MELS;
    if (freq_hz < 2000)       set_id = 0;  // low voice
    else if (freq_hz < 4000)  set_id = 1;  // mid voice
    else if (freq_hz < 8000)  set_id = 2;  // harmonics
    else                       set_id = 3;  // high freq
    
    // Layer 3: Tower (9×9 = 81 cells)
    int amp_quant = (int)(amplitude * (TOWER_SIDE - 1));
    int time_quant = frame_idx % TOWER_SIDE;
    uint32_t tower_cell = (uint32_t)(amp_quant * TOWER_SIDE + time_quant);
    
    // Combine: hilbert(0-63) + set(0-3)×64 + tower(0-80)×64×4
    uint32_t addr = hilbert_cell + (uint32_t)set_id * HZ_CELLS 
                  + tower_cell * HZ_CELLS * N_SETS;
    
    return addr % GEO_FULL;
}

// Simple mod (for comparison)
uint32_t mel_to_addr_mod(double mel_bin, double amplitude, int frame_idx) {
    int bin_idx = (int)mel_bin % HZ_CELLS;
    int hz_x = bin_idx % HZ_SIDE;
    int hz_y = bin_idx / HZ_SIDE;
    
    int set_id;
    double freq_hz = mel_bin * (SAMPLE_RATE / 2.0) / N_MELS;
    if (freq_hz < 2000)       set_id = 0;
    else if (freq_hz < 4000)  set_id = 1;
    else if (freq_hz < 8000)  set_id = 2;
    else                       set_id = 3;
    
    int amp_quant = (int)(amplitude * (TOWER_SIDE - 1));
    int time_quant = frame_idx % TOWER_SIDE;
    uint32_t tower_cell = (uint32_t)(amp_quant * TOWER_SIDE + time_quant);
    
    uint32_t addr = (uint32_t)(hz_x + hz_y * HZ_SIDE) 
                  + (uint32_t)set_id * HZ_CELLS 
                  + tower_cell * HZ_CELLS * N_SETS;
    
    return addr % GEO_FULL;
}

int16_t* read_wav(const char *path, int *n_samples, int *sr) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 44, SEEK_SET);
    int16_t *data = (int16_t*)malloc(size - 44);
    if (!data) { fclose(f); return NULL; }
    *n_samples = (size - 44) / 2;
    *sr = 16000;
    fread(data, 2, *n_samples, f);
    fclose(f);
    return data;
}

// Compare two word fingerprints
double cosine_sim(const uint32_t *a, int len_a, const uint32_t *b, int len_b) {
    // Build frequency vectors
    int fa[GEO_FULL] = {0}, fb[GEO_FULL] = {0};
    for (int i = 0; i < len_a; i++) fa[a[i]]++;
    for (int i = 0; i < len_b; i++) fb[b[i]]++;
    
    double dot = 0, na = 0, nb = 0;
    for (int i = 0; i < GEO_FULL; i++) {
        dot += fa[i] * fb[i];
        na += fa[i] * fa[i];
        nb += fb[i] * fb[i];
    }
    return dot / (sqrt(na) * sqrt(nb) + 1e-10);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s <wav_file>\n", argv[0]);
        return 1;
    }
    
    init_window();
    init_mel_filters();
    
    int n_samples, sr;
    int16_t *samples = read_wav(argv[1], &n_samples, &sr);
    if (!samples) { printf("Error: cannot read %s\n", argv[1]); return 1; }
    
    printf("Input: %s (%d samples, %.2f sec)\n", argv[1], n_samples, (double)n_samples/sr);
    
    int max_frames = (n_samples - N_FFT) / HOP_SIZE;
    double (*mel)[N_MELS] = malloc(sizeof(double) * max_frames * N_MELS);
    int n_frames = compute_mel(samples, n_samples, mel);
    printf("Mel: %d frames × %d bins\n", n_frames, N_MELS);
    
    // Generate addresses with both methods
    uint32_t *addrs_hilbert = malloc(sizeof(uint32_t) * n_frames * N_MELS);
    uint32_t *addrs_mod = malloc(sizeof(uint32_t) * n_frames * N_MELS);
    int addr_count = 0;
    
    for (int f = 0; f < n_frames; f++) {
        double mel_min = 1e10, mel_max = -1e10;
        for (int m = 0; m < N_MELS; m++) {
            if (mel[f][m] < mel_min) mel_min = mel[f][m];
            if (mel[f][m] > mel_max) mel_max = mel[f][m];
        }
        double mel_range = mel_max - mel_min;
        if (mel_range < 1e-10) mel_range = 1.0;
        
        for (int m = 0; m < N_MELS; m++) {
            double norm = (mel[f][m] - mel_min) / mel_range;
            double amplitude = fmax(0, fmin(1, norm));
            addrs_hilbert[addr_count] = mel_to_addr_hilbert(m, amplitude, f);
            addrs_mod[addr_count] = mel_to_addr_mod(m, amplitude, f);
            addr_count++;
        }
    }
    
    // Compare fill rates
    int hit_h[GEO_FULL] = {0}, hit_m[GEO_FULL] = {0};
    for (int i = 0; i < addr_count; i++) {
        hit_h[addrs_hilbert[i]]++;
        hit_m[addrs_mod[i]]++;
    }
    int filled_h = 0, filled_m = 0;
    for (int i = 0; i < GEO_FULL; i++) {
        if (hit_h[i] > 0) filled_h++;
        if (hit_m[i] > 0) filled_m++;
    }
    
    printf("\n=== Fill Rate Comparison ===\n");
    printf("Mod:    %d/%d (%.1f%%)\n", filled_m, GEO_FULL, 100.0*filled_m/GEO_FULL);
    printf("Hilbert: %d/%d (%.1f%%)\n", filled_h, GEO_FULL, 100.0*filled_h/GEO_FULL);
    
    // Hilbert locality test: check if nearby mel bins → nearby addresses
    printf("\n=== Hilbert Locality Test ===\n");
    printf("Mel bin → Hilbert index:\n");
    for (int m = 0; m < 16; m++) {
        int x = m % HZ_SIDE;
        int y = m / HZ_SIDE;
        uint32_t h = hilbert_idx((uint32_t)x, (uint32_t)y, HZ_SIDE);
        printf("  bin %2d (%d,%d) → hilbert %2d\n", m, x, y, h);
    }
    
    // Word detection
    printf("\n=== Word Detection ===\n");
    double energy[10000];
    int n_energy = n_frames < 10000 ? n_frames : 10000;
    for (int f = 0; f < n_energy; f++) {
        double sum = 0;
        for (int m = 0; m < N_MELS; m++)
            sum += mel[f][m] * mel[f][m];
        energy[f] = sum;
    }
    
    double sorted[10000];
    memcpy(sorted, energy, sizeof(double) * n_energy);
    for (int i = 0; i < n_energy-1; i++)
        for (int j = i+1; j < n_energy; j++)
            if (sorted[i] > sorted[j]) {
                double tmp = sorted[i]; sorted[i] = sorted[j]; sorted[j] = tmp;
            }
    double thresh = sorted[n_energy/4];
    
    // Extract word fingerprints (Hilbert)
    int word_starts[100], word_ends[100], word_count = 0;
    int in_word = 0, word_start = 0;
    for (int f = 0; f < n_energy; f++) {
        if (energy[f] > thresh && !in_word) { word_start = f; in_word = 1; }
        else if (energy[f] <= thresh && in_word) {
            if (f - word_start >= 5 && word_count < 100) {
                word_starts[word_count] = word_start;
                word_ends[word_count] = f;
                word_count++;
            }
            in_word = 0;
        }
    }
    
    printf("Words detected: %d\n", word_count);
    for (int w = 0; w < word_count; w++) {
        printf("  Word %2d: frames %d-%d (%d frames)\n", 
               w+1, word_starts[w], word_ends[w], word_ends[w]-word_starts[w]);
    }
    
    // Compare word similarities
    if (word_count > 1) {
        printf("\n=== Word Similarity (Hilbert) ===\n");
        for (int w = 0; w < word_count && w < 5; w++) {
            int start = word_starts[w] * N_MELS;
            int end = word_ends[w] * N_MELS;
            double best_sim = 0;
            int best_match = -1;
            for (int w2 = 0; w2 < word_count; w2++) {
                if (w == w2) continue;
                int start2 = word_starts[w2] * N_MELS;
                int end2 = word_ends[w2] * N_MELS;
                double sim = cosine_sim(addrs_hilbert+start, end-start, 
                                       addrs_hilbert+start2, end2-start2);
                if (sim > best_sim) { best_sim = sim; best_match = w2; }
            }
            printf("  Word %d ↔ %d: sim=%.4f\n", w+1, best_match+1, best_sim);
        }
    }
    
    free(mel);
    free(addrs_hilbert);
    free(addrs_mod);
    free(samples);
    return 0;
}

// geo_audio_v4.c — 64 Hz × 4 sets × 81 tower = 20736
// Build: gcc -O2 -Wall -o tools/geo_audio_v4.exe tools/geo_audio_v4.c -lm

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

// Structure: 64 Hz × 4 sets × 81 tower
#define HZ_CELLS     64    // 8×8 frequency bins
#define HZ_SIDE      8     // sqrt(64)
#define N_SETS       4     // frequency range contexts
#define TOWER_CELLS  81    // 9×9 amplitude × time
#define TOWER_SIDE   9     // sqrt(81)

// Frequency ranges (Hz)
#define FREQ_LOW     0     // 0-20kHz speech
#define FREQ_HIGH    1     // 0-44kHz full spectrum
#define FREQ_HARM    2     // harmonic overtone
#define FREQ_NOISE   3     // noise/transient

static double g_win[N_FFT];
static double g_mel_filter[N_MELS][N_FFT/2+1];

void init_window(void) {
    for (int i = 0; i < N_FFT; i++)
        g_win[i] = 0.5 * (1.0 - cos(2.0 * M_PI * i / N_FFT));
}

// Simple mel filterbank (128 bins, 0-8kHz)
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
            if (dist < width)
                g_mel_filter[m][k] = 1.0 - dist / width;
            else
                g_mel_filter[m][k] = 0.0;
        }
    }
}

// Compute mel spectrogram
int compute_mel(const int16_t *samples, int n_samples, double mel[][N_MELS]) {
    int n_frames = (n_samples - N_FFT) / HOP_SIZE;
    if (n_frames <= 0) return 0;
    
    double fft_re[N_FFT], fft_im[N_FFT];
    
    for (int f = 0; f < n_frames; f++) {
        int offset = f * HOP_SIZE;
        
        // Window + FFT (DFT for simplicity)
        for (int i = 0; i < N_FFT; i++) {
            fft_re[i] = samples[offset + i] * g_win[i] / 32768.0;
            fft_im[i] = 0.0;
        }
        
        // Simple DFT (not optimized, but correct)
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
        
        // Apply mel filters
        for (int m = 0; m < N_MELS; m++) {
            double sum = 0;
            for (int k = 0; k < N_FFT/2+1; k++)
                sum += power[k] * g_mel_filter[m][k];
            mel[f][m] = log10(fmax(sum, 1e-10));
        }
    }
    return n_frames;
}

// Map mel to address: 64 Hz × 4 sets × 81 tower
uint32_t mel_to_addr(double mel_bin, double amplitude, int frame_idx) {
    // Layer 1: Hz frequency (8×8 = 64 cells)
    int bin_idx = (int)mel_bin % HZ_CELLS;
    int hz_x = bin_idx % HZ_SIDE;
    int hz_y = bin_idx / HZ_SIDE;
    
    // Layer 2: Set (4 contexts)
    int set_id;
    if (mel_bin < 32)       set_id = FREQ_LOW;   // 0-4kHz (low voice)
    else if (mel_bin < 64)  set_id = FREQ_HIGH;  // 4-8kHz (high voice)
    else if (mel_bin < 96)  set_id = FREQ_HARM;  // 8-12kHz (harmonics)
    else                    set_id = FREQ_NOISE; // 12-16kHz (noise)
    
    // Layer 3: Tower (9×9 = 81 cells)
    int amp_quant = (int)(amplitude * (TOWER_SIDE - 1));
    int time_quant = frame_idx % TOWER_SIDE;
    int tower_x = amp_quant % TOWER_SIDE;
    int tower_y = time_quant % TOWER_SIDE;
    
    // Combine: addr = hz + set×64 + tower×64×4
    uint32_t addr = (uint32_t)(hz_x + hz_y * HZ_SIDE 
                              + set_id * HZ_CELLS 
                              + (tower_x + tower_y * TOWER_SIDE) * HZ_CELLS * N_SETS);
    
    return addr % GEO_FULL;
}

// Read WAV (16-bit mono)
int16_t* read_wav(const char *path, int *n_samples, int *sr) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    // Skip header (44 bytes)
    fseek(f, 44, SEEK_SET);
    
    int16_t *data = (int16_t*)malloc(size - 44);
    if (!data) { fclose(f); return NULL; }
    
    *n_samples = (size - 44) / 2;
    *sr = 16000; // assume 16kHz
    fread(data, 2, *n_samples, f);
    fclose(f);
    return data;
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
    if (!samples) {
        printf("Error: cannot read %s\n", argv[1]);
        return 1;
    }
    
    printf("Input: %s (%d samples, %.2f sec)\n", argv[1], n_samples, (double)n_samples/sr);
    
    // Compute mel
    int max_frames = (n_samples - N_FFT) / HOP_SIZE;
    double (*mel)[N_MELS] = malloc(sizeof(double) * max_frames * N_MELS);
    int n_frames = compute_mel(samples, n_samples, mel);
    printf("Mel: %d frames × %d bins\n", n_frames, N_MELS);
    
    // Convert to addresses
    uint32_t *addrs = malloc(sizeof(uint32_t) * n_frames * N_MELS);
    int addr_count = 0;
    
    for (int f = 0; f < n_frames; f++) {
        // Normalize mel to [0,1]
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
            addrs[addr_count++] = mel_to_addr(m, amplitude, f);
        }
    }
    
    // Statistics
    int hit[GEO_FULL] = {0};
    for (int i = 0; i < addr_count; i++)
        hit[addrs[i]]++;
    
    int filled = 0;
    for (int i = 0; i < GEO_FULL; i++)
        if (hit[i] > 0) filled++;
    
    printf("\n=== Address Distribution ===\n");
    printf("Total: %d | Filled: %d/%d (%.1f%%)\n", 
           addr_count, filled, GEO_FULL, 100.0*filled/GEO_FULL);
    
    // Show Hz distribution
    printf("\n=== Hz Layer (64 cells) ===\n");
    int hz_hit[HZ_CELLS] = {0};
    for (int i = 0; i < addr_count; i++)
        hz_hit[addrs[i] % HZ_CELLS]++;
    for (int i = 0; i < HZ_CELLS; i++)
        if (hz_hit[i] > 0) printf("  Hz[%2d]: %d\n", i, hz_hit[i]);
    
    // Show Set distribution
    printf("\n=== Set Layer (4 sets) ===\n");
    int set_hit[N_SETS] = {0};
    for (int i = 0; i < addr_count; i++)
        set_hit[(addrs[i] / HZ_CELLS) % N_SETS]++;
    for (int i = 0; i < N_SETS; i++)
        printf("  Set[%d]: %d\n", i, set_hit[i]);
    
    // Show Tower distribution
    printf("\n=== Tower Layer (81 cells) ===\n");
    int tower_hit[TOWER_CELLS] = {0};
    for (int i = 0; i < addr_count; i++)
        tower_hit[(addrs[i] / (HZ_CELLS * N_SETS)) % TOWER_CELLS]++;
    int tower_filled = 0;
    for (int i = 0; i < TOWER_CELLS; i++)
        if (tower_hit[i] > 0) tower_filled++;
    printf("  Filled: %d/81 (%.1f%%)\n", tower_filled, 100.0*tower_filled/TOWER_CELLS);
    
    // Word detection test
    printf("\n=== Word Detection (energy threshold) ===\n");
    double energy[10000];
    int n_energy = n_frames < 10000 ? n_frames : 10000;
    for (int f = 0; f < n_energy; f++) {
        double sum = 0;
        for (int m = 0; m < N_MELS; m++)
            sum += mel[f][m] * mel[f][m];
        energy[f] = sum;
    }
    
    // Find threshold (25th percentile)
    double sorted[10000];
    memcpy(sorted, energy, sizeof(double) * n_energy);
    // Simple sort
    for (int i = 0; i < n_energy-1; i++)
        for (int j = i+1; j < n_energy; j++)
            if (sorted[i] > sorted[j]) {
                double tmp = sorted[i]; sorted[i] = sorted[j]; sorted[j] = tmp;
            }
    double thresh = sorted[n_energy/4];
    
    int in_word = 0, word_start = 0, word_count = 0;
    for (int f = 0; f < n_energy; f++) {
        if (energy[f] > thresh && !in_word) {
            word_start = f;
            in_word = 1;
        } else if (energy[f] <= thresh && in_word) {
            if (f - word_start >= 5) {
                word_count++;
                printf("  Word %2d: frames %d-%d (%d frames)\n", 
                       word_count, word_start, f, f-word_start);
            }
            in_word = 0;
        }
    }
    printf("Total words detected: %d\n", word_count);
    
    free(mel);
    free(addrs);
    free(samples);
    return 0;
}

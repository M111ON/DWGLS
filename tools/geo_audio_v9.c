// geo_audio_v9.c — Word segmentation + Hilbert trajectory per word + DTW
// Test: same word across files → low DTW, different words → high DTW
// Build: gcc -O2 -Wall -o tools/geo_audio_v9.exe tools/geo_audio_v9.c -lm

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#define SAMPLE_RATE  16000
#define N_FFT        400
#define HOP_SIZE     160
#define N_MELS       128
#define MAX_FRAMES   2000
#define MAX_WORDS    40
#define HILBERT_CELLS 4096
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
    // Parse RIFF/WAVE chunks
    char riff[4], wave[4];
    uint32_t chunk_size;
    if (fread(riff, 1, 4, f) != 4 || memcmp(riff, "RIFF", 4) != 0) { fclose(f); return NULL; }
    fread(&chunk_size, 4, 1, f);
    if (fread(wave, 1, 4, f) != 4 || memcmp(wave, "WAVE", 4) != 0) { fclose(f); return NULL; }
    
    uint16_t channels = 1, bits = 16;
    uint32_t sample_rate = 16000;
    int16_t *data = NULL;
    int capacity = 0, count = 0;
    
    while (!feof(f)) {
        char id[4];
        uint32_t sz;
        if (fread(id, 1, 4, f) != 4) break;
        fread(&sz, 4, 1, f);
        
        if (memcmp(id, "fmt ", 4) == 0) {
            uint16_t audio_fmt;
            fread(&audio_fmt, 2, 1, f);
            fread(&channels, 2, 1, f);
            fread(&sample_rate, 4, 1, f);
            fseek(f, 6, SEEK_CUR); // byte rate, block align
            fread(&bits, 2, 1, f);
            fseek(f, sz - 16, SEEK_CUR); // skip rest of fmt
        } else if (memcmp(id, "data", 4) == 0) {
            if (bits != 16) { fclose(f); free(data); return NULL; }
            int n = sz / 2;
            data = (int16_t*)malloc(n * 2);
            if (!data) { fclose(f); return NULL; }
            count = (int)fread(data, 2, n, f);
            capacity = n;
            break;
        } else {
            fseek(f, sz, SEEK_CUR); // skip unknown chunk
        }
    }
    fclose(f);
    if (!data) return NULL;
    (void)channels; (void)sample_rate;
    *n_samples = count;
    return data;
}

// ═══════════════════════════════════════════════════════════
// Word segmentation: energy-based with silence gaps
// ═══════════════════════════════════════════════════════════
typedef struct {
    int start_frame;
    int end_frame;
    double energy;
} WordSeg;

// Compute frame energy from time-domain samples (RMS)
void frame_energy(const int16_t *samples, int n_samples, int n_frames, double *energy) {
    for (int f = 0; f < n_frames; f++) {
        double sum = 0;
        int offset = f * HOP_SIZE;
        for (int i = 0; i < N_FFT && offset + i < n_samples; i++) {
            double v = samples[offset + i] * g_win[i] / 32768.0;
            sum += v * v;
        }
        energy[f] = sqrt(sum / N_FFT);
    }
}

// Segment into words via RMS energy threshold
int segment_words(const double mel[][N_MELS], int n_frames, 
                  double energy[], WordSeg *words) {
    // RMS threshold: 0.01 (silence ~0.00001, speech ~0.02-0.13)
    double threshold = 0.01;
    
    int n_words = 0;
    int in_word = 0;
    int start = 0;
    int min_gap = 3;      // min silence frames between words
    int silent_count = 0;
    int min_word_frames = 5;  // min word length
    
    for (int f = 0; f < n_frames; f++) {
        if (energy[f] > threshold) {
            if (!in_word) {
                in_word = 1;
                start = f;
                silent_count = 0;
            }
        } else {
            if (in_word) {
                silent_count++;
                if (silent_count >= min_gap) {
                    // Close word
                    int end = f - silent_count;
                    if (end - start >= min_word_frames) {
                        words[n_words].start_frame = start;
                        words[n_words].end_frame = end;
                        double en = 0;
                        for (int i = start; i <= end; i++) en += energy[i];
                        words[n_words].energy = en;
                        n_words++;
                    }
                    in_word = 0;
                    silent_count = 0;
                }
            }
        }
    }
    // Close last word
    if (in_word) {
        int end = n_frames - 1;
        if (end - start >= min_word_frames) {
            words[n_words].start_frame = start;
            words[n_words].end_frame = end;
            double en = 0;
            for (int i = start; i <= end; i++) en += energy[i];
            words[n_words].energy = en;
            n_words++;
        }
    }
    
    return n_words;
}

// ═══════════════════════════════════════════════════════════
// Hilbert walker + trajectory per word
// ═══════════════════════════════════════════════════════════
typedef struct {
    uint32_t pos;
    uint32_t max_step;
} Walker;

void walker_init(Walker *w) {
    w->pos = 0;
    w->max_step = 15;
}

uint32_t walker_step(Walker *w, uint32_t step) {
    if (step > w->max_step) step = w->max_step;
    w->pos = (w->pos + step) % HILBERT_CELLS;
    return w->pos;
}

// Build trajectory for one word (reset walker at word start)
// Stores (position, amplitude) interleaved: traj[i*2]=pos, traj[i*2+1]=amp
int word_trajectory2d(Walker *w, const double mel[][N_MELS],
                      int start, int end, uint32_t *traj) {
    int total = 0;  // counts PAIRS
    for (int f = start; f <= end; f++) {
        double mel_min = 1e10, mel_max = -1e10;
        for (int m = 0; m < N_MELS; m++) {
            if (mel[f][m] < mel_min) mel_min = mel[f][m];
            if (mel[f][m] > mel_max) mel_max = mel[f][m];
        }
        double range = mel_max - mel_min;
        if (range < 1e-10) range = 1.0;
        
        for (int m = 0; m < N_MELS; m++) {
            if (total >= MAX_TRAJ/2) return total;  // cap pairs
            double norm = (mel[f][m] - mel_min) / range;
            double amplitude = fmax(0, fmin(1, norm));
            uint32_t step = (uint32_t)(1.0 + amplitude * (w->max_step - 1));
            uint32_t pos = walker_step(w, step);
            traj[total*2] = pos;
            traj[total*2+1] = (uint32_t)(amplitude * 255.0);  // preserve amp
            total++;
        }
    }
    return total;
}

// 2D DTW: cost = dist(pos) + amp_weight * dist(amp)
double dtw2d(const uint32_t *a, int na, const uint32_t *b, int nb) {
    if (na > 100) na = 100;
    if (nb > 100) nb = 100;
    const double W_AMP = 0.5;  // amplitude weight
    
    double *dt = malloc(sizeof(double) * (na+1) * (nb+1));
    #define DT(i,j) dt[(i)*(nb+1)+(j)]
    
    DT(0,0) = 0;
    for (int i = 1; i <= na; i++) DT(i,0) = 1e18;
    for (int j = 1; j <= nb; j++) DT(0,j) = 1e18;
    
    for (int i = 1; i <= na; i++) {
        for (int j = 1; j <= nb; j++) {
            double pos_cost = fabs((double)a[(i-1)*2] - (double)b[(j-1)*2]);
            double amp_cost = fabs((double)a[(i-1)*2+1] - (double)b[(j-1)*2+1]) / 255.0;
            double cost = pos_cost + W_AMP * amp_cost;
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

// DTW distance
double dtw(const uint32_t *a, int na, const uint32_t *b, int nb) {
    // Cap lengths for speed
    if (na > 200) na = 200;
    if (nb > 200) nb = 200;
    
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

// ═══════════════════════════════════════════════════════════
// Comparison stats
// ═══════════════════════════════════════════════════════════
int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s <wav1> <wav2> [wav3 ...]\n", argv[0]);
        return 1;
    }
    
    init_window();
    init_mel_filters();
    
    int n_files = argc - 1;
    if (n_files > 20) n_files = 20;
    
    printf("=== Word-Level Hilbert DTW v9 ===\n\n");
    
    // Per-file data
    double (*all_mel[20])[N_MELS];
    int all_frames[20];
    double all_energy[20][MAX_FRAMES];
    WordSeg all_words[20][MAX_WORDS];
    int all_nwords[20];
    uint32_t (*all_word_traj[20][MAX_WORDS]);
    int all_word_lens[20][MAX_WORDS];
    
    for (int fi = 0; fi < n_files; fi++) {
        int n_samples;
        int16_t *samples = read_wav(argv[fi + 1], &n_samples);
        if (!samples) { printf("Error: %s\n", argv[fi+1]); n_files--; fi--; continue; }
        
        int max_frames = (n_samples - N_FFT) / HOP_SIZE;
        if (max_frames > MAX_FRAMES) max_frames = MAX_FRAMES;
        all_mel[fi] = malloc(sizeof(double) * max_frames * N_MELS);
        all_frames[fi] = compute_mel(samples, n_samples, all_mel[fi]);
        
        // Energy (time-domain RMS) — BEFORE freeing samples
        int frames_avail = all_frames[fi];
        if (frames_avail > max_frames) frames_avail = max_frames;
        frame_energy(samples, n_samples, frames_avail, all_energy[fi]);
        free(samples);
        
        // Segment
        all_nwords[fi] = segment_words(all_mel[fi], all_frames[fi], 
                                       all_energy[fi], all_words[fi]);
        
        printf("File %d: %s (%d frames, %d words)\n", 
               fi+1, argv[fi+1], all_frames[fi], all_nwords[fi]);
        
        // Trajectory per word
        for (int w = 0; w < all_nwords[fi]; w++) {
            Walker walker;
            walker_init(&walker);
            all_word_traj[fi][w] = malloc(sizeof(uint32_t) * MAX_TRAJ);
            all_word_lens[fi][w] = word_trajectory2d(&walker, all_mel[fi],
                all_words[fi][w].start_frame, all_words[fi][w].end_frame,
                all_word_traj[fi][w]);
        }
    }
    printf("\n");
    
    // ═══ Word-level comparison: all pairs across files ═══
    printf("═══ Cross-File Word Comparison (DTW, lower = more similar) ═══\n");
    
    // Group words: same index position across files compared first
    // (since sentences have different word counts, show positional pairs)
    int max_words = 0;
    for (int fi = 0; fi < n_files; fi++)
        if (all_nwords[fi] > max_words) max_words = all_nwords[fi];
    
    // Positional comparison
    printf("\n  Positional (same word index across files):\n");
    for (int pos = 0; pos < max_words; pos++) {
        int count = 0;
        for (int fi = 0; fi < n_files; fi++)
            if (pos < all_nwords[fi]) count++;
        if (count < 2) continue;
        
        printf("    Word %d:", pos+1);
        for (int fi = 0; fi < n_files; fi++) {
            if (pos >= all_nwords[fi]) continue;
            printf("  f%d=%dfr ", fi+1, 
                   all_words[fi][pos].end_frame - all_words[fi][pos].start_frame + 1);
        }
        printf("\n");
        
        // All pairwise DTW
        for (int fi = 0; fi < n_files; fi++) {
            if (pos >= all_nwords[fi]) continue;
            for (int fj = fi+1; fj < n_files; fj++) {
                if (pos >= all_nwords[fj]) continue;
                double dist = dtw(all_word_traj[fi][pos], all_word_lens[fi][pos],
                                  all_word_traj[fj][pos], all_word_lens[fj][pos]);
                printf("      f%d↔f%d: DTW=%.2f\n", fi+1, fj+1, dist);
            }
        }
    }
    
    // ═══ All-pairs comparison: EVERY word × EVERY word across files ═══
    printf("\n═══ ALL-PAIRS Word DTW (lower = more similar) ═══\n\n");
    printf("  Top-5 closest pairs (potential SAME word):\n");
    
    // Collect all pairs with scores
    typedef struct {
        int file_a, word_a, file_b, word_b;
        double dist;
        int len_a, len_b;
    } Pair;
    
    Pair pairs[4000];
    int n_pairs = 0;
    
    for (int fa = 0; fa < n_files; fa++) {
        for (int wa = 0; wa < all_nwords[fa]; wa++) {
            for (int fb = fa+1; fb < n_files; fb++) {
                for (int wb = 0; wb < all_nwords[fb]; wb++) {
                    double dist = dtw2d(all_word_traj[fa][wa], all_word_lens[fa][wa],
                                        all_word_traj[fb][wb], all_word_lens[fb][wb]);
                    if (n_pairs < 4000) {
                        pairs[n_pairs++] = (Pair){fa, wa, fb, wb, dist,
                            all_word_lens[fa][wa], all_word_lens[fb][wb]};
                    }
                }
            }
        }
    }
    
    // Sort by distance ascending
    for (int i = 0; i < n_pairs-1; i++)
        for (int j = i+1; j < n_pairs; j++)
            if (pairs[j].dist < pairs[i].dist) {
                Pair t = pairs[i]; pairs[i] = pairs[j]; pairs[j] = t;
            }
    
    // Print top 30 closest
    int shown = n_pairs < 30 ? n_pairs : 30;
    for (int i = 0; i < shown; i++)
        printf("  f%d-w%d ↔ f%d-w%d: DTW=%.2f (len %d vs %d)\n",
               pairs[i].file_a+1, pairs[i].word_a,
               pairs[i].file_b+1, pairs[i].word_b,
               pairs[i].dist, pairs[i].len_a, pairs[i].len_b);
    
    printf("\n  All %d pairs (sorted by distance):\n", n_pairs);
    for (int i = 0; i < n_pairs; i++)
        printf("  f%d-w%d ↔ f%d-w%d: DTW=%.2f\n",
               pairs[i].file_a+1, pairs[i].word_a,
               pairs[i].file_b+1, pairs[i].word_b,
               pairs[i].dist);
    
    // Print per-file word count summary
    for (int fi = 0; fi < n_files; fi++) {
        printf("\n  File %d words (start-end frames):\n", fi+1);
        for (int w = 0; w < all_nwords[fi]; w++)
            printf("    W%d: f[%d-%d] len=%d traj=%d\n", w,
                   all_words[fi][w].start_frame, all_words[fi][w].end_frame,
                   all_words[fi][w].end_frame - all_words[fi][w].start_frame + 1,
                   all_word_lens[fi][w]);
    }
    
    // Cleanup
    for (int fi = 0; fi < n_files; fi++) {
        for (int w = 0; w < all_nwords[fi]; w++)
            free(all_word_traj[fi][w]);
        free(all_mel[fi]);
    }
    
    return 0;
}
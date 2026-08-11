// geo_audio_v10.c — Hilbert maze PATTERN: static mapping + intensity vector
// mel bin → Hilbert position (fixed), amplitude → intensity, cosine compare
// Build: gcc -O2 -Wall -o tools/geo_audio_v10.exe tools/geo_audio_v10.c -lm

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
#define HILBERT_N    64
#define HILBERT_CELLS 4096
#define MAX_FRAMES   2000
#define MAX_WORDS    40

static double g_win[N_FFT];
static double g_mel_filter[N_MELS][N_FFT/2+1];
static uint32_t h_lut[HILBERT_CELLS];

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

void init_all(void) {
    for (int i = 0; i < N_FFT; i++)
        g_win[i] = 0.5 * (1.0 - cos(2.0*M_PI*i/N_FFT));
    for (uint32_t y = 0; y < HILBERT_N; y++)
        for (uint32_t x = 0; x < HILBERT_N; x++)
            h_lut[y*HILBERT_N+x] = hilbert_idx(x, y, HILBERT_N);
    double sr = SAMPLE_RATE;
    double mel_lo = 2595.0 * log10(1.0 + 0.0/700.0);
    double mel_hi = 2595.0 * log10(1.0 + sr/2.0/700.0);
    for (int m = 0; m < N_MELS; m++) {
        double mc = mel_lo + (mel_hi - mel_lo) * (m + 0.5) / N_MELS;
        double fc = 700.0 * (pow(10.0, mc/2595.0) - 1.0);
        double bc = fc * N_FFT / sr;
        double wd = (mel_hi - mel_lo) / N_MELS * N_FFT / sr;
        for (int k = 0; k < N_FFT/2+1; k++) {
            double d = fabs(k - bc);
            g_mel_filter[m][k] = (d < wd) ? 1.0 - d/wd : 0.0;
        }
    }
}

int compute_mel(const int16_t *samples, int n_samples, double mel[][N_MELS]) {
    int nf = (n_samples - N_FFT) / HOP_SIZE;
    if (nf > MAX_FRAMES) nf = MAX_FRAMES;
    for (int f = 0; f < nf; f++) {
        int off = f * HOP_SIZE;
        double re[N_FFT], im[N_FFT];
        for (int i = 0; i < N_FFT; i++) {
            re[i] = samples[off+i] * g_win[i] / 32768.0;
            im[i] = 0.0;
        }
        double pow_[N_FFT/2+1];
        for (int k = 0; k < N_FFT/2+1; k++) {
            double r = 0, ii = 0;
            for (int n = 0; n < N_FFT; n++) {
                double a = 2.0*M_PI*k*n/N_FFT;
                r += re[n]*cos(a) + im[n]*sin(a);
                ii += -re[n]*sin(a) + im[n]*cos(a);
            }
            pow_[k] = (r*r + ii*ii) / N_FFT;
        }
        for (int m = 0; m < N_MELS; m++) {
            double s = 0;
            for (int k = 0; k < N_FFT/2+1; k++)
                s += pow_[k] * g_mel_filter[m][k];
            mel[f][m] = log10(fmax(s, 1e-10));
        }
    }
    return nf;
}

int16_t* read_wav(const char *path, int *n_samples) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char riff[4], wave[4];
    uint32_t sz;
    fread(riff,1,4,f); fread(&sz,4,1,f); fread(wave,1,4,f);
    if (memcmp(riff,"RIFF",4)!=0 || memcmp(wave,"WAVE",4)!=0) { fclose(f); return NULL; }
    int16_t *data = NULL; int count = 0;
    while (!feof(f)) {
        char id[4]; uint32_t s;
        if (fread(id,1,4,f) != 4) break;
        fread(&s,4,1,f);
        if (memcmp(id,"data",4)==0) {
            data = malloc(s); count = (int)fread(data,2,s/2,f); break;
        } else fseek(f, s, SEEK_CUR);
    }
    fclose(f);
    *n_samples = count;
    return data;
}

// ═══════════════════════════════════════════════════════════
// Hilbert maze PATTERN (user design):
// mel bin → Hilbert position (FIXED, no accumulated walker)
// amplitude → intensity at that position
// Pattern = sparse vector length 4096 (per word)
// ═══════════════════════════════════════════════════════════
typedef struct { int start_frame, end_frame; double energy; } WordSeg;

void frame_rms(const int16_t *samples, int n_samples, int n_frames, double *energy) {
    for (int f = 0; f < n_frames; f++) {
        double sum = 0;
        int off = f * HOP_SIZE;
        for (int i = 0; i < N_FFT && off+i < n_samples; i++) {
            double v = samples[off+i] * g_win[i] / 32768.0;
            sum += v*v;
        }
        energy[f] = sqrt(sum / N_FFT);
    }
}

int segment(const double *energy, int nf, WordSeg *words) {
    double thr = 0.01;
    int nw = 0, in_word = 0, start = 0, sil = 0;
    for (int f = 0; f < nf; f++) {
        if (energy[f] > thr) {
            if (!in_word) { in_word = 1; start = f; sil = 0; }
        } else if (in_word) {
            if (++sil >= 3) {
                int end = f - sil;
                if (end - start >= 5) {
                    words[nw].start_frame = start;
                    words[nw].end_frame = end;
                    double en = 0;
                    for (int i = start; i <= end; i++) en += energy[i];
                    words[nw].energy = en;
                    nw++;
                }
                in_word = 0; sil = 0;
            }
        }
    }
    if (in_word && nf-1-start >= 5) {
        words[nw].start_frame = start;
        words[nw].end_frame = nf-1;
        double en = 0;
        for (int i = start; i < nf; i++) en += energy[i];
        words[nw].energy = en;
        nw++;
    }
    return nw;
}

// Build pattern: On/Off switch per frame (user design)
// Per frame: threshold mel at median → binary active set → mark Hilbert positions
// Pattern = temporal sequence of ON/OFF snapshots → folded into binary vector
//
// THRESHOLD policy: median per frame (adaptive, content-preserving)
// ON if amp > median → marks THAT frame's activation positions
void build_pattern(const double mel[][N_MELS], int start, int end,
                   uint8_t *pattern /* length HILBERT_CELLS: 1=ever-ON */,
                   uint8_t *temporal /* length frames × 8 bytes: fold per frame into bytes */,
                   int *n_frames_out, double *on_density) {
    memset(pattern, 0, HILBERT_CELLS);
    int nf = end - start + 1;
    *n_frames_out = nf;
    if (temporal) memset(temporal, 0, nf * 8);
    
    int total_on = 0;
    for (int f = start; f <= end; f++) {
        // Frame mel values
        double vals[N_MELS];
        for (int m = 0; m < N_MELS; m++) vals[m] = mel[f][m];
        // Median
        double sorted[N_MELS];
        memcpy(sorted, vals, sizeof(vals));
        for (int i = 0; i < N_MELS-1; i++)
            for (int j = i+1; j < N_MELS; j++)
                if (sorted[j] < sorted[i]) { double t=sorted[i]; sorted[i]=sorted[j]; sorted[j]=t; }
        double median = sorted[N_MELS/2];
        
        // On/Off: mark Hilbert positions of ON bins
        for (int m = 0; m < N_MELS; m++) {
            if (vals[m] > median) {
                uint32_t pos = h_lut[m];  // static maze wall (0..4095)
                if (pos < HILBERT_CELLS) pattern[pos] = 1;
                total_on++;
                // Fold frame's ON set: 4096 positions → 512 bytes via byte bucketing
                if (temporal) temporal[(f-start)*8 + ((pos >> 3) & 7)] |= (uint8_t)(1 << (pos & 7));
            }
        }
    }
    *on_density = (double)total_on / (nf * N_MELS);
}

// Similarity between two On/Off patterns: Jaccard on ON sets
double pattern_jaccard(const uint8_t *a, const uint8_t *b) {
    int inter = 0, uni = 0;
    for (uint32_t i = 0; i < HILBERT_CELLS; i++) {
        if (a[i] || b[i]) {
            uni++;
            if (a[i] && b[i]) inter++;
        }
    }
    return uni > 0 ? (double)inter / uni : 0.0;
}

// Temporal similarity: bit-level agreement (smoothed Hamming)
double temporal_sim(const uint8_t *a, const uint8_t *b, int na, int nb) {
    int n = na < nb ? na : nb;
    int agree = 0, total = 0;
    for (int f = 0; f < n; f++) {
        for (int k = 0; k < 8; k++) {
            // count agreeing set bits per byte
            uint8_t x = a[f*8+k], y = b[f*8+k];
            for (int b_ = 0; b_ < 8; b_++) {
                int bitx = (x >> b_) & 1, bity = (y >> b_) & 1;
                total++;
                if (bitx == bity) agree++;
            }
        }
    }
    return total > 0 ? (double)agree / total : 0.0;
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("Usage: %s <wav1> <wav2...>\n", argv[0]); return 1; }
    init_all();
    
    int nf_ = argc - 1;
    if (nf_ > 20) nf_ = 20;
    
    printf("=== Hilbert Maze PATTERN v10 (static mapping + intensity) ===\n\n");
    
    double (*mel[20])[N_MELS];
    int frames[20];
    WordSeg words[20][MAX_WORDS];
    int nwords[20];
    uint8_t (*patterns[20][MAX_WORDS]);
    uint8_t (*temporals[20][MAX_WORDS]);
    int pat_frames[20][MAX_WORDS];
    double pat_density[20][MAX_WORDS];
    
    for (int fi = 0; fi < nf_; fi++) {
        int ns;
        int16_t *s = read_wav(argv[fi+1], &ns);
        if (!s) { printf("Error: %s\n", argv[fi+1]); continue; }
        int mf = (ns - N_FFT) / HOP_SIZE;
        if (mf > MAX_FRAMES) mf = MAX_FRAMES;
        mel[fi] = malloc(sizeof(double) * mf * N_MELS);
        frames[fi] = compute_mel(s, ns, mel[fi]);
        
        double *energy = malloc(sizeof(double) * frames[fi]);
        frame_rms(s, ns, frames[fi], energy);
        nwords[fi] = segment(energy, frames[fi], words[fi]);
        free(energy); free(s);
        
        printf("File %d: %d frames, %d words\n", fi+1, frames[fi], nwords[fi]);
        
        for (int w = 0; w < nwords[fi]; w++) {
            patterns[fi][w] = malloc(HILBERT_CELLS);
            int fl = words[fi][w].end_frame - words[fi][w].start_frame + 1;
            temporals[fi][w] = malloc(fl * 8);
            build_pattern(mel[fi], words[fi][w].start_frame, words[fi][w].end_frame,
                          patterns[fi][w], temporals[fi][w],
                          &pat_frames[fi][w], &pat_density[fi][w]);
        }
    }
    
    // ═══ All-pairs comparison (Jaccard + temporal) ═══
    printf("\n═══ ALL-PAIRS Similarity (Jaccard ON-set / temporal bit-agreement) ═══\n");
    printf("  Words per file: ");
    for (int fi = 0; fi < nf_; fi++) printf("f%d=%d ", fi+1, nwords[fi]);
    printf("\n  Per-word ON density: ");
    for (int fi = 0; fi < nf_; fi++)
        for (int w = 0; w < nwords[fi]; w++)
            printf("f%d-w%d=%.2f ", fi+1, w, pat_density[fi][w]);
    printf("\n\n");
    
    typedef struct {
        int fa, wa, fb, wb; double js, ts;
    } Pair;
    Pair pairs[4000];
    int np = 0;
    
    for (int fa = 0; fa < nf_; fa++) {
        for (int wa = 0; wa < nwords[fa]; wa++) {
            for (int fb = fa+1; fb < nf_; fb++) {
                for (int wb = 0; wb < nwords[fb]; wb++) {
                    double js = pattern_jaccard(patterns[fa][wa], patterns[fb][wb]);
                    double ts = temporal_sim(temporals[fa][wa], temporals[fb][wb],
                                             pat_frames[fa][wa], pat_frames[fb][wb]);
                    if (np < 4000) pairs[np++] = (Pair){fa,wa,fb,wb,js,ts};
                }
            }
        }
    }
    
    // Sort descending by Jaccard
    for (int i = 0; i < np-1; i++)
        for (int j = i+1; j < np; j++)
            if (pairs[j].js > pairs[i].js) {
                Pair t = pairs[i]; pairs[i] = pairs[j]; pairs[j] = t;
            }
    
    printf("  Top-40 closest pairs (by Jaccard):\n");
    int shown = np < 40 ? np : 40;
    for (int i = 0; i < shown; i++)
        printf("  f%d-w%d ↔ f%d-w%d: J=%.4f T=%.4f\n",
               pairs[i].fa+1, pairs[i].wa, pairs[i].fb+1, pairs[i].wb,
               pairs[i].js, pairs[i].ts);
    
    // Reference expected matches
    printf("\n  Reference (known words):\n");
    printf("    s1: The quick brown fox jumps over the lazy dog\n");
    printf("    s2: The cat sat on the mat and watched...\n");
    printf("    s3: A gentle breeze blew through the quiet forest...\n");
    printf("  Expected same-word pairs: 'the' (s1-w0, s2-w0/w4/w8)\n");
    
    // Check specific pairs
    if (nf_ >= 3) {
        double j_tt = pattern_jaccard(patterns[0][0], patterns[1][0]);
        double t_tt = temporal_sim(temporals[0][0], temporals[1][0], pat_frames[0][0], pat_frames[1][0]);
        double j_ta = pattern_jaccard(patterns[0][0], patterns[2][0]);
        double t_ta = temporal_sim(temporals[0][0], temporals[2][0], pat_frames[0][0], pat_frames[2][0]);
        printf("\n  The(s1-w0) ↔ The(s2-w0): J=%.4f T=%.4f\n", j_tt, t_tt);
        printf("  The(s1-w0) ↔ A(s3-w0):   J=%.4f T=%.4f\n", j_ta, t_ta);
        printf("  → Discrimination: T gap = %.4f\n", t_tt - t_ta);
    }
    
    for (int fi = 0; fi < nf_; fi++) {
        for (int w = 0; w < nwords[fi]; w++) {
            free(patterns[fi][w]);
            free(temporals[fi][w]);
        }
        free(mel[fi]);
    }
    return 0;
}
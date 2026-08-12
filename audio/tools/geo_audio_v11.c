// geo_audio_v11.c — Spectral Peaks Timbre Capture
// Per frame: Top-K peaks (harmonic set) → Hilbert maze positions
// Pattern = temporal sequence of peak sets = TIMBRE signature
// Build: gcc -O2 -Wall -o tools/geo_audio_v11.exe tools/geo_audio_v11.c -lm

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#define SAMPLE_RATE  16000
#define N_FFT        400
#define HOP_SIZE     160
#define N_MELS       128
#define HILBERT_N    64
#define HILBERT_CELLS 4096
#define MAX_FRAMES   2000
#define MAX_WORDS    40
#define TOP_K        8      // 8 harmonic peaks (8 bands, 8 codebooks LFM)
#define MAX_PEAKS_F  256    // cap peaks per frame for safety

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

// ═══════════════════════════════════════════════════════════
// TIMBRE capture: Top-K spectral peaks per frame
//
// Per frame: find TOP_K strongest mel bins (harmonic peaks)
//   → these ARE the timbre signature (which harmonics are loud)
//   → map each peak bin → static Hilbert position
//   → mark in ever-ON pattern + temporal fold
//
// Top-K=8 matches: 8 bands / 8 codebooks LFM2.5
// ═══════════════════════════════════════════════════════════
void build_timbre(const double mel[][N_MELS], int start, int end,
                  uint8_t *pattern /* ever-ON, 4096 */,
                  uint32_t *peak_traj /* per-frame peak bins: [f*K + k] */,
                  int *n_frames_out, int K) {
    memset(pattern, 0, HILBERT_CELLS);
    int nf = end - start + 1;
    *n_frames_out = nf;
    
    for (int f = start; f <= end; f++) {
        // Find top-K mel bins this frame
        // Small selection sort on indices
        int top_idx[TOP_K];
        double top_val[TOP_K];
        for (int k = 0; k < K; k++) { top_idx[k] = -1; top_val[k] = -1e30; }
        
        for (int m = 0; m < N_MELS; m++) {
            double v = mel[f][m];
            for (int k = 0; k < K; k++) {
                if (v > top_val[k]) {
                    // shift down
                    for (int j = K-1; j > k; j--) {
                        top_idx[j] = top_idx[j-1];
                        top_val[j] = top_val[j-1];
                    }
                    top_idx[k] = m;
                    top_val[k] = v;
                    break;
                }
            }
        }
        
        // Mark in pattern + trajectory
        for (int k = 0; k < K; k++) {
            int bin = top_idx[k];
            if (bin < 0) continue;
            uint32_t pos = h_lut[bin];  // static maze wall
            if (pos < HILBERT_CELLS) pattern[pos] = 1;
            peak_traj[(f-start)*K + k] = (uint32_t)bin;  // store peak bin index
        }
    }
}

// Timbre similarity: how many peak bins agree per frame (smoothed over window)
// Returns avg agreement ratio of top-K sets, aligned by frame ratio
double timbre_sim(const uint32_t *a, int na, int ka,
                  const uint32_t *b, int nb, int kb) {
    int n = na < nb ? na : nb;
    double total_agree = 0;
    int frames = 0;
    
    for (int f = 0; f < n; f++) {
        // Check overlap of peak sets at frame f
        int match = 0;
        for (int i = 0; i < ka; i++) {
            for (int j = 0; j < kb; j++) {
                if (a[f*ka+i] == b[f*kb+j]) { match++; break; }
            }
        }
        int denom = ka < kb ? ka : kb;
        total_agree += (double)match / denom;
        frames++;
    }
    return frames > 0 ? total_agree / frames : 0.0;
}

// Ever-ON Jaccard (which Hilbert walls ever got lit)
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

int main(int argc, char **argv) {
    if (argc < 2) { printf("Usage: %s <wav1> <wav2...>\n", argv[0]); return 1; }
    init_all();
    
    int nf_ = argc - 1;
    if (nf_ > 20) nf_ = 20;
    
    printf("=== Hilbert Maze TIMBRE v11 (Top-%d spectral peaks/frame) ===\n\n", TOP_K);
    
    double (*mel[20])[N_MELS];
    int frames[20];
    WordSeg words[20][MAX_WORDS];
    int nwords[20];
    uint8_t (*patterns[20][MAX_WORDS]);
    uint32_t (*trajs[20][MAX_WORDS]);
    int traj_frames[20][MAX_WORDS];
    
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
            trajs[fi][w] = malloc(sizeof(uint32_t) * fl * TOP_K);
            build_timbre(mel[fi], words[fi][w].start_frame, words[fi][w].end_frame,
                         patterns[fi][w], trajs[fi][w], &traj_frames[fi][w], TOP_K);
        }
    }
    
    // ═══ All-pairs comparison ═══
    printf("\n═══ ALL-PAIRS Timbre Similarity ═══\n");
    printf("  Words: ");
    for (int fi = 0; fi < nf_; fi++) printf("f%d=%d ", fi+1, nwords[fi]);
    printf("\n\n");
    
    typedef struct {
        int fa, wa, fb, wb; double ts, js;
    } Pair;
    Pair pairs[4000];
    int np = 0;
    
    for (int fa = 0; fa < nf_; fa++) {
        for (int wa = 0; wa < nwords[fa]; wa++) {
            for (int fb = fa+1; fb < nf_; fb++) {
                for (int wb = 0; wb < nwords[fb]; wb++) {
                    double ts = timbre_sim(trajs[fa][wa], traj_frames[fa][wa], TOP_K,
                                           trajs[fb][wb], traj_frames[fb][wb], TOP_K);
                    double js = pattern_jaccard(patterns[fa][wa], patterns[fb][wb]);
                    if (np < 4000) pairs[np++] = (Pair){fa,wa,fb,wb,ts,js};
                }
            }
        }
    }
    
    // Sort by timbre sim descending
    for (int i = 0; i < np-1; i++)
        for (int j = i+1; j < np; j++)
            if (pairs[j].ts > pairs[i].ts) {
                Pair t = pairs[i]; pairs[i] = pairs[j]; pairs[j] = t;
            }
    
    printf("  Top-40 closest pairs (by timbre):\n");
    int shown = np < 40 ? np : 40;
    for (int i = 0; i < shown; i++)
        printf("  f%d-w%d ↔ f%d-w%d: T=%.4f J=%.4f\n",
               pairs[i].fa+1, pairs[i].wa, pairs[i].fb+1, pairs[i].wb,
               pairs[i].ts, pairs[i].js);
    
    // Reference checks
    printf("\n  Reference sentences:\n");
    printf("    s1: The quick brown fox jumps over the lazy dog\n");
    printf("    s2: The cat sat on the mat and watched...\n");
    printf("    s3: A gentle breeze blew through the quiet forest...\n");
    
    if (nf_ >= 3) {
        // "the" appears as s1-w0, s2-w0/w4/w8; "a" as s3-w0
        double tt = timbre_sim(trajs[0][0], traj_frames[0][0], TOP_K,
                               trajs[1][0], traj_frames[1][0], TOP_K);
        double ta = timbre_sim(trajs[0][0], traj_frames[0][0], TOP_K,
                               trajs[2][0], traj_frames[2][0], TOP_K);
        double t_the2 = nwords[1] > 4 ? 
            timbre_sim(trajs[0][0], traj_frames[0][0], TOP_K,
                       trajs[1][4], traj_frames[1][4], TOP_K) : 0;
        printf("\n  The(s1-w0) ↔ The(s2-w0): T=%.4f\n", tt);
        printf("  The(s1-w0) ↔ The(s2-w4): T=%.4f\n", t_the2);
        printf("  The(s1-w0) ↔ A(s3-w0):   T=%.4f\n", ta);
        printf("  → Discrimination (The/A gap): %.4f\n", tt - ta);
    }
    
    for (int fi = 0; fi < nf_; fi++) {
        for (int w = 0; w < nwords[fi]; w++) {
            free(patterns[fi][w]);
            free(trajs[fi][w]);
        }
        free(mel[fi]);
    }
    return 0;
}
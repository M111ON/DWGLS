// geo_audio_v12.c — Known-text word alignment + Timbre comparison
// Align words proportionally by char count (TTS consistent rate)
// Then: Top-K spectral peaks timbre per word
// Test: same word across different sentences → high T
// Build: gcc -O2 -Wall -o tools/geo_audio_v12.exe tools/geo_audio_v12.c -lm

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
#define MAX_WORDS    60
#define TOP_K        8
#define MAX_WORD_LEN 64

static double g_win[N_FFT];
static double g_mel_filter[N_MELS][N_FFT/2+1];
static uint32_t h_lut[HILBERT_CELLS];

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
// Known-text alignment: proportional by char count
// ═══════════════════════════════════════════════════════════
typedef struct {
    int start_frame, end_frame;
    char word[MAX_WORD_LEN];
} AlignedWord;

// Parse sentence into words (space-separated, strip punctuation)
int parse_sentence(const char *sentence, char words[][MAX_WORD_LEN]) {
    int n = 0;
    char buf[1024];
    strncpy(buf, sentence, 1023); buf[1023] = 0;
    char *tok = strtok(buf, " \t\n.,!?;:");
    while (tok && n < MAX_WORDS) {
        strncpy(words[n], tok, MAX_WORD_LEN-1);
        words[n][MAX_WORD_LEN-1] = 0;
        n++;
        tok = strtok(NULL, " \t\n.,!?;:");
    }
    return n;
}

// Align words proportionally across speech span [s0, s1]
// (Exclude leading/trailing silence: use energy threshold to find speech span)
int align_words(const char *sentence, const double *energy, int nf,
                AlignedWord *out) {
    // Find speech span: frames above threshold
    double thr = 0.01;
    int s0 = 0, s1 = nf-1;
    for (int f = 0; f < nf; f++) if (energy[f] > thr) { s0 = f; break; }
    for (int f = nf-1; f >= 0; f--) if (energy[f] > thr) { s1 = f; break; }
    if (s1 <= s0) return 0;
    
    // Split known words
    char words[MAX_WORDS][MAX_WORD_LEN];
    int nw = parse_sentence(sentence, words);
    if (nw == 0) return 0;
    
    // Char counts → proportional frame allocation
    int chars[MAX_WORDS];
    int total_chars = 0;
    for (int i = 0; i < nw; i++) {
        chars[i] = (int)strlen(words[i]);
        total_chars += chars[i];
    }
    
    int span = s1 - s0 + 1;
    int pos = s0;
    for (int i = 0; i < nw; i++) {
        int len = (int)((double)chars[i] / total_chars * span);
        if (i == nw-1) len = span - (pos - s0);  // last word takes remainder
        out[i].start_frame = pos;
        out[i].end_frame = pos + len - 1;
        if (out[i].end_frame > s1) out[i].end_frame = s1;
        strncpy(out[i].word, words[i], MAX_WORD_LEN-1);
        out[i].word[MAX_WORD_LEN-1] = 0;
        pos += len;
    }
    return nw;
}

// ═══════════════════════════════════════════════════════════
// Timbre capture (same as v11): Top-K peaks per frame
// ═══════════════════════════════════════════════════════════
void build_timbre(const double mel[][N_MELS], int start, int end,
                  uint8_t *pattern, uint32_t *peak_traj, int *n_frames_out, int K) {
    memset(pattern, 0, HILBERT_CELLS);
    int nf = end - start + 1;
    *n_frames_out = nf;
    
    for (int f = start; f <= end; f++) {
        int top_idx[TOP_K];
        double top_val[TOP_K];
        for (int k = 0; k < K; k++) { top_idx[k] = -1; top_val[k] = -1e30; }
        
        for (int m = 0; m < N_MELS; m++) {
            double v = mel[f][m];
            for (int k = 0; k < K; k++) {
                if (v > top_val[k]) {
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
        
        for (int k = 0; k < K; k++) {
            int bin = top_idx[k];
            if (bin < 0) continue;
            uint32_t pos = h_lut[bin];
            if (pos < HILBERT_CELLS) pattern[pos] = 1;
            peak_traj[(f-start)*K + k] = (uint32_t)bin;
        }
    }
}

double timbre_sim(const uint32_t *a, int na, int ka,
                  const uint32_t *b, int nb, int kb) {
    int n = na < nb ? na : nb;
    double total = 0;
    for (int f = 0; f < n; f++) {
        int match = 0;
        for (int i = 0; i < ka; i++)
            for (int j = 0; j < kb; j++)
                if (a[f*ka+i] == b[f*kb+j]) { match++; break; }
        int denom = ka < kb ? ka : kb;
        total += (double)match / denom;
    }
    return n > 0 ? total / n : 0.0;
}

double pattern_jaccard(const uint8_t *a, const uint8_t *b) {
    int inter = 0, uni = 0;
    for (uint32_t i = 0; i < HILBERT_CELLS; i++) {
        if (a[i] || b[i]) { uni++; if (a[i] && b[i]) inter++; }
    }
    return uni > 0 ? (double)inter / uni : 0.0;
}

// ═══════════════════════════════════════════════════════════
// Main: files + sentences (sentence per file via args after --)
// Usage: geo_audio_v12.exe -- s1.wav "The quick..." s2.wav "The cat..."
// ═══════════════════════════════════════════════════════════
int main(int argc, char **argv) {
    if (argc < 4) {
        printf("Usage: %s -- <wav1> \"sentence1\" <wav2> \"sentence2\" ...\n", argv[0]);
        printf("Example: geo_audio_v12 -- s1.wav \"The quick brown fox\" s2.wav \"The cat sat\"\n");
        return 1;
    }
    init_all();
    
    // Parse: pairs of (wav, sentence)
    int n_files = (argc - 1) / 2;
    if (n_files > 10) n_files = 10;
    int idx = 1;
    if (strcmp(argv[1], "--") == 0) idx = 2;
    
    printf("=== Known-Text Aligned Timbre v12 ===\n\n");
    
    double (*mel[10])[N_MELS];
    int frames[10];
    AlignedWord words[10][MAX_WORDS];
    int nwords[10];
    uint8_t (*patterns[10][MAX_WORDS]);
    uint32_t (*trajs[10][MAX_WORDS]);
    int traj_frames[10][MAX_WORDS];
    
    for (int fi = 0; fi < n_files; fi++) {
        const char *wavpath = argv[idx + fi*2];
        const char *sentence = argv[idx + fi*2 + 1];
        
        int ns;
        int16_t *s = read_wav(wavpath, &ns);
        if (!s) { printf("Error: %s\n", wavpath); return 1; }
        int mf = (ns - N_FFT) / HOP_SIZE;
        if (mf > MAX_FRAMES) mf = MAX_FRAMES;
        mel[fi] = malloc(sizeof(double) * mf * N_MELS);
        frames[fi] = compute_mel(s, ns, mel[fi]);
        
        double *energy = malloc(sizeof(double) * frames[fi]);
        for (int f = 0; f < frames[fi]; f++) {
            double sum = 0;
            int off = f * HOP_SIZE;
            for (int i = 0; i < N_FFT && off+i < ns; i++) {
                double v = s[off+i] * g_win[i] / 32768.0;
                sum += v*v;
            }
            energy[f] = sqrt(sum / N_FFT);
        }
        free(s);
        
        nwords[fi] = align_words(sentence, energy, frames[fi], words[fi]);
        
        printf("File %d: %s (%d frames)\n", fi+1, wavpath, frames[fi]);
        printf("  Aligned %d words: ", nwords[fi]);
        for (int w = 0; w < nwords[fi]; w++)
            printf("%s(f%d-%d) ", words[fi][w].word,
                   words[fi][w].start_frame, words[fi][w].end_frame);
        printf("\n");
        
        for (int w = 0; w < nwords[fi]; w++) {
            patterns[fi][w] = malloc(HILBERT_CELLS);
            int fl = words[fi][w].end_frame - words[fi][w].start_frame + 1;
            trajs[fi][w] = malloc(sizeof(uint32_t) * (fl+1) * TOP_K);
            build_timbre(mel[fi], words[fi][w].start_frame, words[fi][w].end_frame,
                         patterns[fi][w], trajs[fi][w], &traj_frames[fi][w], TOP_K);
        }
        free(energy);
    }
    
    // ═══ Same-word matching ═══
    printf("\n═══ SAME-WORD Matching (across files) ═══\n");
    printf("  Compare every pair whose WORD TEXT is identical\n\n");
    
    int same_pairs = 0, diff_pairs = 0;
    double same_sum = 0, diff_sum = 0;
    double same_j_sum = 0, diff_j_sum = 0;
    
    for (int fa = 0; fa < n_files; fa++) {
        for (int wa = 0; wa < nwords[fa]; wa++) {
            for (int fb = fa+1; fb < n_files; fb++) {
                for (int wb = 0; wb < nwords[fb]; wb++) {
                    if (strcmp(words[fa][wa].word, words[fb][wb].word) != 0) continue;
                    
                    double ts = timbre_sim(trajs[fa][wa], traj_frames[fa][wa], TOP_K,
                                           trajs[fb][wb], traj_frames[fb][wb], TOP_K);
                    double js = pattern_jaccard(patterns[fa][wa], patterns[fb][wb]);
                    
                    printf("  %s: f%d-w%d ↔ f%d-w%d: T=%.4f J=%.4f\n",
                           words[fa][wa].word, fa+1, wa, fb+1, wb, ts, js);
                    same_sum += ts;
                    same_j_sum += js;
                    same_pairs++;
                }
            }
        }
    }
    
    // Random different-word pairs (sample up to 50 for baseline)
    printf("\n═══ DIFFERENT-WORD Baseline (sample) ═══\n");
    int shown = 0;
    for (int fa = 0; fa < n_files && shown < 50; fa++) {
        for (int wa = 0; wa < nwords[fa] && shown < 50; wa++) {
            for (int fb = fa+1; fb < n_files && shown < 50; fb++) {
                for (int wb = 0; wb < nwords[fb] && shown < 50; wb++) {
                    if (strcmp(words[fa][wa].word, words[fb][wb].word) == 0) continue;
                    // Only compare adjacent positions (roughly same time region)
                    if (abs(wa - wb) > 2) continue;
                    
                    double ts = timbre_sim(trajs[fa][wa], traj_frames[fa][wa], TOP_K,
                                           trajs[fb][wb], traj_frames[fb][wb], TOP_K);
                    double js = pattern_jaccard(patterns[fa][wa], patterns[fb][wb]);
                    
                    printf("  %s↔%s (f%d-w%d↔f%d-w%d): T=%.4f J=%.4f\n",
                           words[fa][wa].word, words[fb][wb].word,
                           fa+1, wa, fb+1, wb, ts, js);
                    diff_sum += ts;
                    diff_j_sum += js;
                    diff_pairs++;
                    shown++;
                }
            }
        }
    }
    
    printf("\n═══ SUMMARY ═══\n");
    printf("  Same-word pairs: %d, avg T=%.4f, avg J=%.4f\n",
           same_pairs, same_pairs ? same_sum/same_pairs : 0,
           same_pairs ? same_j_sum/same_pairs : 0);
    printf("  Diff-word pairs: %d, avg T=%.4f, avg J=%.4f\n",
           diff_pairs, diff_pairs ? diff_sum/diff_pairs : 0,
           diff_pairs ? diff_j_sum/diff_pairs : 0);
    printf("  → Timbre gap: %.4f\n",
           (same_pairs ? same_sum/same_pairs : 0) - (diff_pairs ? diff_sum/diff_pairs : 0));
    
    for (int fi = 0; fi < n_files; fi++) {
        for (int w = 0; w < nwords[fi]; w++) {
            free(patterns[fi][w]);
            free(trajs[fi][w]);
        }
        free(mel[fi]);
    }
    return 0;
}
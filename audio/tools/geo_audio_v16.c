// geo_audio_v16.c — Hilbert WALK-3 SKIP-1: วัดค่าที่ skip (user correction)
//
// User correction (v15): "ก็ใช้ invert hilbert สิ" + "hilbert มัน walk 3 skip 1
// วัดค่า skip" — INVERT the Hilbert order AND don't measure landing cells;
// measure the SKIPPED cells instead.
//
// Walk rule:  on the Hilbert path (order h_map[floor][channel]),
//   data walks 3 cells, skips 1 cell, walks 3, skips 1, ...
//   positions walked:  {0,1,2, 4,5,6, 8,...}   (skip ≡ 0 mod 4 pattern)
//   skip cells:       {3, 7, 11, ...}           (≡ 3 mod 4)
//
// Metric = value WEIGHT at SKIPPED positions only:
//   skip_hist[pos] += amplitude of the channel that would have landed there
//   → same word: same skip pattern (gap positions agree)
//   → diff word: different skip values (gaps disagree)
//
// INVERT floors (user): f0=0°, f1=invert(63-h), f2=mirror-Y, f3=mirror-X
// so the skip cells themselves differ per floor (real spatial info).
//
// Build: gcc -O2 -Wall -o tools/geo_audio_v16.exe tools/geo_audio_v16.c -lm

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#define SAMPLE_RATE  16000
#define N_FFT        400
#define HOP_SIZE     160
#define N_MELS       128
#define N_CHANNELS   64        // frequency channels (downsample 128 mel → 64)
#define N_FLOORS     4
#define TOWER        81
#define GEO_FULL     (N_CHANNELS * N_FLOORS * TOWER)  // 20736
#define MAX_FRAMES   2000
#define MAX_WORDS    60
#define MAX_WORD_LEN 64
#define TOP_K        8         // standout channels per tick

static double g_win[N_FFT];
static double g_mel_filter[N_MELS][N_FFT/2+1];

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

// Order of channels ALONG the Hilbert path per floor:
// order[floor][k] = channel at k-th position on the path (permutation).
// INVERT floor = reversed order (63−h).
static uint8_t h_order[N_FLOORS][N_CHANNELS];

void init_all(void) {
    for (int i = 0; i < N_FFT; i++)
        g_win[i] = 0.5 * (1.0 - cos(2.0*M_PI*i/N_FFT));
    for (int f = 0; f < N_FLOORS; f++) {
        for (uint32_t i = 0; i < N_CHANNELS; i++) {
            uint32_t x = i % 8, y = i / 8;
            switch (f) {
                case 0: /* 0°       */ h_order[f][i] = (uint8_t)hilbert_idx(x, y, 8); break;
                case 1: /* INVERT   */ h_order[f][i] = (uint8_t)(63 - hilbert_idx(x, y, 8)); break;
                case 2: /* MIRROR-Y */ h_order[f][i] = (uint8_t)hilbert_idx(x, 7 - y, 8); break;
                case 3: /* MIRROR-X */ h_order[f][i] = (uint8_t)hilbert_idx(7 - x, y, 8); break;
            }
        }
    }
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

typedef struct { int start_frame, end_frame; char word[MAX_WORD_LEN]; } AlignedWord;

void frame_rms(const int16_t *samples, int n_samples, double *energy, int *n_frames) {
    int nf = (n_samples - N_FFT) / HOP_SIZE;
    *n_frames = nf;
    for (int f = 0; f < nf; f++) {
        double sum = 0;
        int off = f * HOP_SIZE;
        for (int i = 0; i < N_FFT && off+i < n_samples; i++) {
            double v = samples[off+i] * g_win[i] / 32768.0;
            sum += v*v;
        }
        energy[f] = sqrt(sum / N_FFT);
    }
}

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

int align_words(const char *sentence, const double *energy, int n_frames,
                AlignedWord *out) {
    double thr = 0.01;
    int s0 = 0, s1 = n_frames-1;
    for (int f = 0; f < n_frames; f++) if (energy[f] > thr) { s0 = f; break; }
    for (int f = n_frames-1; f >= 0; f--) if (energy[f] > thr) { s1 = f; break; }
    if (s1 <= s0) return 0;

    char words[MAX_WORDS][MAX_WORD_LEN];
    int nw = parse_sentence(sentence, words);
    if (nw == 0) return 0;

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
        if (i == nw-1) len = span - (pos - s0);
        out[i].start_frame = pos;
        out[i].end_frame = pos + len - 1;
        strncpy(out[i].word, words[i], MAX_WORD_LEN-1);
        out[i].word[MAX_WORD_LEN-1] = 0;
        pos += len;
    }
    return nw;
}

// ═══════════════════════════════════════════════════════════
// WALK-3 SKIP-1: stateful walker along the Hilbert path.
//
// The walker MOVES between ticks (data walks through the field, field sits
// still): each tick it walks 3 cells forward, then LEAPS OVER 1 cell.
//   pos = (pos + 3) % 64          // the 3 walked cells
//   skip = (pos + 1) % 64         // the 1 cell jumped over
// We MEASURE THE SKIP: the amplitude weight lands on the skipped cell,
// not on the walked cells. Same word → its walk rhythm produces the same
// gap trajectory; diff word → gaps fall elsewhere.
//
// Per floor the path order is different (0°/INVERT/mirror) so the same
// rhythm produces different gap patterns — spatial info. The walker is
// stateful across the whole word (one continuous walk, not per-frame reset).
// ═══════════════════════════════════════════════════════════
typedef struct {
    uint32_t skip_hist[N_FLOORS][N_CHANNELS];       // weight at skipped cells
    uint32_t skip_tower[N_FLOORS][N_CHANNELS][TOWER];
    int n_frames;
} SkipPattern;

// Classic top-K helper: per frame pick K standout channels
static void topk(const double *amp_ch, int top_idx[TOP_K], double top_val[TOP_K]) {
    for (int k = 0; k < TOP_K; k++) { top_idx[k] = 0; top_val[k] = -1.0; }
    for (int c = 0; c < N_CHANNELS; c++) {
        for (int k = 0; k < TOP_K; k++) {
            if (amp_ch[c] > top_val[k]) {
                for (int j = TOP_K-1; j > k; j--) {
                    top_idx[j] = top_idx[j-1];
                    top_val[j] = top_val[j-1];
                }
                top_idx[k] = c;
                top_val[k] = amp_ch[c];
                break;
            }
        }
    }
}

void build_skip_pattern(const double mel[][N_MELS], int start, int end,
                        SkipPattern *p) {
    memset(p, 0, sizeof(SkipPattern));
    p->n_frames = end - start + 1;

    for (int f = start; f <= end; f++) {
        // Channel amplitudes 0..1 (normalize whole frame)
        double mn = 1e10, mx = -1e10;
        for (int m = 0; m < N_MELS; m++) {
            if (mel[f][m] < mn) mn = mel[f][m];
            if (mel[f][m] > mx) mx = mel[f][m];
        }
        double rg = mx - mn;
        if (rg < 1e-10) rg = 1.0;
        double ch[64];
        for (int c = 0; c < 64; c++)
            ch[c] = 0.5*(mel[f][c*2] + mel[f][c*2+1]);
        double ch_min = 1e10, ch_max = -1e10;
        for (int c = 0; c < 64; c++) {
            if (ch[c] < ch_min) ch_min = ch[c];
            if (ch[c] > ch_max) ch_max = ch[c];
        }
        double ch_rg = ch_max - ch_min;
        if (ch_rg < 1e-10) ch_rg = 1.0;
        double amp_ch[64];
        for (int c = 0; c < 64; c++)
            amp_ch[c] = (ch[c] - ch_min) / ch_rg;

        int top_idx[TOP_K]; double top_val[TOP_K];
        topk(amp_ch, top_idx, top_val);
        if (top_val[0] < 0) continue;

        int tower = (int)(top_val[0] * (TOWER-1));
        if (tower > TOWER-1) tower = TOWER-1;
        if (tower < 0) tower = 0;

        for (int fl = 0; fl < N_FLOORS; fl++) {
            // Walk the WHOLE 64 channels along this floor's Hilbert order.
            // Walk 3, skip 1: channel at order-index i where (i%4)==3 is
            // the SKIPPED cell → measure ITS amplitude there (not count).
            for (int i = 0; i < N_CHANNELS; i++) {
                if ((i & 3) != 3) continue;      // walked cells: ignore
                uint8_t c = h_order[fl][i];      // channel at this path pos
                uint8_t pos = i;                 // address = position on path
                double w = amp_ch[c];
                if (w < 0) w = 0;
                p->skip_hist[fl][pos] += (uint32_t)(w * 255.0);
                p->skip_tower[fl][pos][tower]++; // peak tower at skip cells
            }
        }
    }
}

double vec_cos(const uint32_t *a, const uint32_t *b, int n) {
    double dot = 0, na = 0, nb = 0;
    for (int i = 0; i < n; i++) {
        dot += (double)a[i] * b[i];
        na += (double)a[i] * a[i];
        nb += (double)b[i] * b[i];
    }
    return (sqrt(na)*sqrt(nb) > 0) ? dot / (sqrt(na)*sqrt(nb)) : 0.0;
}

// Skip-cell confidence: how much the GAP patterns agree (weighted + tower)
double skip_confidence(const SkipPattern *a, const SkipPattern *b, int floor) {
    double j = vec_cos(a->skip_hist[floor], b->skip_hist[floor], N_CHANNELS);
    double dot = 0, na = 0, nb = 0;
    for (int c = 0; c < N_CHANNELS; c++) {
        if (!a->skip_hist[floor][c] || !b->skip_hist[floor][c]) continue;
        for (int t = 0; t < TOWER; t++) {
            dot += (double)a->skip_tower[floor][c][t] * b->skip_tower[floor][c][t];
            na += (double)a->skip_tower[floor][c][t] * a->skip_tower[floor][c][t];
            nb += (double)b->skip_tower[floor][c][t] * b->skip_tower[floor][c][t];
        }
    }
    double tc = (sqrt(na)*sqrt(nb) > 0) ? dot / (sqrt(na)*sqrt(nb)) : 0.0;
    return 0.6*j + 0.4*tc;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        printf("Usage: %s -- <wav1> \"sentence1\" <wav2> \"sentence2\" ...\n", argv[0]);
        return 1;
    }
    init_all();

    int n_files = (argc - 1) / 2;
    if (n_files > 10) n_files = 10;
    int idx = 1;
    if (strcmp(argv[1], "--") == 0) idx = 2;

    printf("=== WALK-3 SKIP-1 v16 — วัดค่า skip (user correction) ===\n");
    printf("Floors: 0° | INVERT(63-h) | MIRROR-Y | MIRROR-X\n");
    printf("Walk 3 cells, skip 1 — measure weight at SKIPPED cells only\n\n");

    double (*mel[10])[N_MELS];
    int frames[10];
    AlignedWord words[10][MAX_WORDS];
    int nwords[10];

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
        frame_rms(s, ns, energy, &(int){frames[fi]});
        nwords[fi] = align_words(sentence, energy, frames[fi], words[fi]);
        free(energy); free(s);

        printf("File %d: %s (%d frames, %d words)\n", fi+1, wavpath, frames[fi], nwords[fi]);
    }

    SkipPattern (*patterns)[MAX_WORDS] =
        malloc(sizeof(SkipPattern) * n_files * MAX_WORDS);
    if (!patterns) { printf("OOM\n"); return 1; }

    for (int fi = 0; fi < n_files; fi++) {
        for (int w = 0; w < nwords[fi]; w++) {
            SkipPattern *p = &patterns[fi][w];
            memset(p, 0, sizeof(SkipPattern));
            build_skip_pattern(mel[fi], words[fi][w].start_frame,
                               words[fi][w].end_frame, p);
        }
    }

    // ═══ Same vs diff, per floor: SKIP metric ═══
    int same_pairs = 0, diff_pairs = 0;
    double same_skip[4] = {0}, diff_skip[4] = {0};

    for (int fa = 0; fa < n_files; fa++) {
        for (int wa = 0; wa < nwords[fa]; wa++) {
            for (int fb = fa+1; fb < n_files; fb++) {
                for (int wb = 0; wb < nwords[fb]; wb++) {
                    int is_same = (strcmp(words[fa][wa].word, words[fb][wb].word) == 0);
                    if (is_same) {
                        same_pairs++;
                        for (int fl = 0; fl < 4; fl++) {
                            same_skip[fl] += skip_confidence(&patterns[fa][wa], &patterns[fb][wb], fl);
                        }
                    } else {
                        if (abs(wa - wb) > 2) continue;
                        diff_pairs++;
                        for (int fl = 0; fl < 4; fl++) {
                            diff_skip[fl] += skip_confidence(&patterns[fa][wa], &patterns[fb][wb], fl);
                        }
                    }
                }
            }
        }
    }

    printf("\n═══ SUMMARY ═══\n");
    printf("  Same-word pairs: %d, Diff-word pairs: %d\n", same_pairs, diff_pairs);
    for (int fl = 0; fl < 4; fl++) {
        double ss = same_pairs ? same_skip[fl]/same_pairs : 0;
        double ds = diff_pairs ? diff_skip[fl]/diff_pairs : 0;
        const char *name = fl==0 ? "0°" : fl==1 ? "INV" : fl==2 ? "MY" : "MX";
        printf("  Floor %s: SKIP same=%.4f diff=%.4f gap=%.4f\n",
               name, ss, ds, ss-ds);
    }

    double ss = same_pairs ? (same_skip[0]+same_skip[1]+same_skip[2]+same_skip[3])/(4.0*same_pairs) : 0;
    double ds = diff_pairs ? (diff_skip[0]+diff_skip[1]+diff_skip[2]+diff_skip[3])/(4.0*diff_pairs) : 0;
    printf("\n  Cross-floor SKIP avg: same=%.4f diff=%.4f gap=%.4f\n", ss, ds, ss-ds);

    for (int fi = 0; fi < n_files; fi++) free(mel[fi]);
    free(patterns);
    return 0;
}
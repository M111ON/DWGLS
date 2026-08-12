// geo_audio_v21.c — MAGNET PULL: adaptive threshold by velocity (user design)
//
// User: "เหมือน magnet pull คำที่วิ่งเร็วๆ threshold กางออกเยอะขึ้น"
//   Magnet pull: when the word (data) runs FAST through the field, its
//   pull spreads WIDER — the acceptance threshold OPENS UP. Fast attack
//   (plosives) pulls the field open; slow glide keeps it tight.
//
// Physics → code:
//   velocity v[i] = |inten[i] − inten[i−1]|   (rate of energy change)
//   adaptive threshold T[i] = T_base × (1 + k × v[i])
//   cell marked when mean ≥ T[i]  → fast cells widen, slow cells stay tight
//
// Also: pull-weighted intensity — the catch strength = inten × (1 + pull)
// so fast-moving cells leave a STRONGER mark (magnet attraction).
//
// Compare v21 vs v19 (fixed threshold) on the same batch-50 corpus.
//
// Build: gcc -O2 -Wall -o tools/geo_audio_v21.exe tools/geo_audio_v21.c -lm

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#define SAMPLE_RATE  16000
#define N_FFT        400
#define HOP_SIZE     160
#define N_MELS       128
#define N_CHANNELS   64
#define TOWER        81
#define MAX_FRAMES   2000
#define MAX_WORDS    60
#define MAX_WORD_LEN 64
#define TOP_K        8
#define MAX_CELLS    64

static double g_win[N_FFT];
static double g_mel_filter[N_MELS][N_FFT/2+1];

// Radix-2 FFT (in-place, size 512)
static void fft512(double *re, double *im) {
    const int N = 512;
    for (int i = 1, j = 0; i < N; i++) {
        int bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t;
                     t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= N; len <<= 1) {
        double ang = -2.0*M_PI/len;
        double wr = cos(ang), wi = sin(ang);
        for (int i = 0; i < N; i += len) {
            double cur_r = 1.0, cur_i = 0.0;
            for (int j = 0; j < len/2; j++) {
                int a = i + j, b = i + j + len/2;
                double tr = cur_r*re[b] - cur_i*im[b];
                double ti = cur_r*im[b] + cur_i*re[b];
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr;        im[a] += ti;
                double ncr = cur_r*wr - cur_i*wi;
                cur_i = cur_r*wi + cur_i*wr;
                cur_r = ncr;
            }
        }
    }
}

void init_all(void) {
    for (int i = 0; i < N_FFT; i++)
        g_win[i] = 0.5 * (1.0 - cos(2.0*M_PI*i/N_FFT));
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
    double re[512], im[512];
    for (int f = 0; f < nf; f++) {
        int off = f * HOP_SIZE;
        for (int i = 0; i < 512; i++) { re[i] = 0.0; im[i] = 0.0; }
        for (int i = 0; i < N_FFT; i++)
            re[i] = samples[off+i] * g_win[i] / 32768.0;
        fft512(re, im);
        double pow_[N_FFT/2+1];
        for (int k = 0; k < N_FFT/2+1; k++)
            pow_[k] = (re[k]*re[k] + im[k]*im[k]) / N_FFT;
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
// MAGNET PULL MESH — adaptive threshold by velocity
//   cell = CELL_SAMPLES of audio (time address)
//   velocity v[i] = |inten[i] − inten[i−1]|  (after first pass)
//   threshold  T[i] = T_BASE × (1 + PULL_K × v[i])   ← magnet opens
//   catch strength = inten × (1 + PULL_K × v[i])     ← pull weights mark
// ═══════════════════════════════════════════════════════════
#define T_BASE    0.05
#define PULL_K    2.0     // how much velocity opens the threshold

typedef struct {
    uint8_t  fp[MAX_CELLS][TOP_K];
    double   inten[MAX_CELLS];      // pull-weighted intensity
    double   vel[MAX_CELLS];        // raw velocity (for reference)
    int      n_cells;
    int      cell_samples;
} MeshPattern;

static void topk8(const double *amp, int out[TOP_K]) {
    int idx[8] = {0}; double val[8] = {-1,-1,-1,-1,-1,-1,-1,-1};
    for (int c = 0; c < N_CHANNELS; c++) {
        for (int k = 0; k < TOP_K; k++) {
            if (amp[c] > val[k]) {
                for (int j = TOP_K-1; j > k; j--) { idx[j]=idx[j-1]; val[j]=val[j-1]; }
                idx[k] = c; val[k] = amp[c];
                break;
            }
        }
    }
    for (int k = 0; k < TOP_K; k++) out[k] = idx[k];
}

void build_mesh(const double mel[][N_MELS], int start, int end,
                int cell_samples, double pull_k, MeshPattern *p) {
    memset(p, 0, sizeof(MeshPattern));
    p->cell_samples = cell_samples;
    int nf = end - start + 1;
    if (nf <= 0) return;
    int frames_per_cell = cell_samples / HOP_SIZE;
    if (frames_per_cell < 1) frames_per_cell = 1;

    p->n_cells = (nf + frames_per_cell - 1) / frames_per_cell;
    if (p->n_cells > MAX_CELLS) p->n_cells = MAX_CELLS;

    // Pass 1: raw cell means (for velocity + adaptive threshold)
    double raw[MAX_CELLS] = {0};
    for (int ci = 0; ci < p->n_cells; ci++) {
        int f0 = start + ci * frames_per_cell;
        int f1 = f0 + frames_per_cell - 1;
        if (f1 > end) f1 = end;
        int nc = f1 - f0 + 1;

        double ch[64] = {0};
        for (int f = f0; f <= f1; f++)
            for (int c = 0; c < 64; c++)
                ch[c] += 0.5*(mel[f][c*2] + mel[f][c*2+1]);
        for (int c = 0; c < 64; c++) ch[c] /= nc;

        double mn = 1e10, mx = -1e10;
        for (int c = 0; c < 64; c++) { if (ch[c] < mn) mn = ch[c]; if (ch[c] > mx) mx = ch[c]; }
        double rg = mx - mn; if (rg < 1e-10) rg = 1.0;
        double amp[64];
        for (int c = 0; c < 64; c++) amp[c] = (ch[c]-mn)/rg;

        double mean = 0;
        for (int c = 0; c < 64; c++) mean += amp[c];
        raw[ci] = mean / 64.0;
    }

    // Pass 2: velocity + adaptive threshold (magnet pull)
    for (int ci = 0; ci < p->n_cells; ci++) {
        double v = 0;
        if (ci > 0) v = fabs(raw[ci] - raw[ci-1]);
        if (ci < p->n_cells-1) v = fmax(v, fabs(raw[ci+1] - raw[ci]));
        p->vel[ci] = v;
        double thr = T_BASE * (1.0 + pull_k * v);   // magnet opens wider
        if (raw[ci] < thr) { p->inten[ci] = 0; continue; }

        // re-derive fingerprint from this cell's amp (second pass)
        int f0 = start + ci * frames_per_cell;
        int f1 = f0 + frames_per_cell - 1;
        if (f1 > end) f1 = end;
        int nc = f1 - f0 + 1;
        double ch[64] = {0};
        for (int f = f0; f <= f1; f++)
            for (int c = 0; c < 64; c++)
                ch[c] += 0.5*(mel[f][c*2] + mel[f][c*2+1]);
        for (int c = 0; c < 64; c++) ch[c] /= nc;
        double mn = 1e10, mx = -1e10;
        for (int c = 0; c < 64; c++) { if (ch[c] < mn) mn = ch[c]; if (ch[c] > mx) mx = ch[c]; }
        double rg = mx - mn; if (rg < 1e-10) rg = 1.0;
        double amp[64];
        for (int c = 0; c < 64; c++) amp[c] = (ch[c]-mn)/rg;
        int top[TOP_K];
        topk8(amp, top);
        for (int k = 0; k < TOP_K; k++)
            p->fp[ci][k] = (uint8_t)top[k];
        // pull-weighted intensity: fast cells leave a STRONGER mark
        p->inten[ci] = raw[ci] * (1.0 + pull_k * v);
    }
}

double mesh_confidence(const MeshPattern *a, const MeshPattern *b) {
    int n = a->n_cells < b->n_cells ? a->n_cells : b->n_cells;
    if (n == 0) return 0;
    double ov = 0;
    for (int i = 0; i < n; i++) {
        if (a->inten[i] <= 0 || b->inten[i] <= 0) continue;
        int shared = 0;
        for (int k = 0; k < TOP_K; k++)
            for (int j = 0; j < TOP_K; j++)
                if (a->fp[i][k] == b->fp[i][j]) { shared++; break; }
        ov += (double)shared / TOP_K;
    }
    ov /= n;

    double dot = 0, na = 0, nb = 0;
    for (int i = 0; i < n; i++) {
        dot += a->inten[i] * b->inten[i];
        na += a->inten[i] * a->inten[i];
        nb += b->inten[i] * b->inten[i];
    }
    double ic = (sqrt(na)*sqrt(nb) > 0) ? dot/(sqrt(na)*sqrt(nb)) : 0;
    return 0.7*ov + 0.3*ic;
}

int main(int argc, char **argv) {
    int cell_samples = 500;   // champion from v19 sweep
    double pull_k = PULL_K;
    for (int i = 1; i < argc-1; i++) {
        if (strcmp(argv[i], "--cell") == 0) cell_samples = atoi(argv[i+1]);
        if (strcmp(argv[i], "--pull") == 0) pull_k = atof(argv[i+1]);
    }
    init_all();

    double (*mel[50])[N_MELS];
    int frames[50];
    AlignedWord words[50][MAX_WORDS];
    int nwords[50];

    FILE *sf = fopen("tts_sentences_50.txt", "r");
    if (!sf) { printf("Error: tts_sentences_50.txt not found (run from tools/)\n"); return 1; }
    char line[1024];
    int n_files = 0;
    while (n_files < 50 && fgets(line, sizeof line, sf)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strlen(line) == 0) continue;
        static char pbuf[50][128];
        snprintf(pbuf[n_files], 128, "tts_data/s%d.wav", n_files+1);
        int ns;
        int16_t *s = read_wav(pbuf[n_files], &ns);
        if (!s) { printf("Error: %s\n", pbuf[n_files]); return 1; }
        int mf = (ns - N_FFT) / HOP_SIZE;
        if (mf > MAX_FRAMES) mf = MAX_FRAMES;
        mel[n_files] = malloc(sizeof(double) * mf * N_MELS);
        frames[n_files] = compute_mel(s, ns, mel[n_files]);
        double *energy = malloc(sizeof(double) * frames[n_files]);
        frame_rms(s, ns, energy, &(int){frames[n_files]});
        nwords[n_files] = align_words(line, energy, frames[n_files], words[n_files]);
        free(energy); free(s);
        n_files++;
    }
    fclose(sf);

    printf("=== MAGNET PULL v21 — cell=%d, pull_k=%.1f ===\n",
           cell_samples, pull_k);
    printf("Files: %d\n", n_files);

    // rebuild with the sweep pull_k (PULL_K macro is compile-time; use local)
    MeshPattern (*patterns)[MAX_WORDS] =
        malloc(sizeof(MeshPattern) * n_files * MAX_WORDS);
    if (!patterns) { printf("OOM\n"); return 1; }

    for (int fi = 0; fi < n_files; fi++)
        for (int w = 0; w < nwords[fi]; w++) {
            MeshPattern *p = &patterns[fi][w];
            memset(p, 0, sizeof(MeshPattern));
            build_mesh(mel[fi], words[fi][w].start_frame,
                       words[fi][w].end_frame, cell_samples, pull_k, p);
        }

    int same_pairs = 0, diff_pairs = 0;
    double same_sum = 0, diff_sum = 0;
    double td_sum[5] = {0}; int td_cnt[5] = {0};
    const char *td_names[5] = {"<0.5s","0.5-1s","1-2s","2-4s",">4s"};
    int frames_per_sec = SAMPLE_RATE / HOP_SIZE;

    for (int fa = 0; fa < n_files; fa++) {
        for (int wa = 0; wa < nwords[fa]; wa++) {
            for (int fb = fa+1; fb < n_files; fb++) {
                for (int wb = 0; wb < nwords[fb]; wb++) {
                    int is_same = (strcmp(words[fa][wa].word, words[fb][wb].word) == 0);
                    double c = mesh_confidence(&patterns[fa][wa], &patterns[fb][wb]);
                    if (is_same) {
                        same_pairs++; same_sum += c;
                        int fd = abs(words[fa][wa].start_frame - words[fb][wb].start_frame);
                        int bin = 0;
                        if (fd >= 4*frames_per_sec) bin = 4;
                        else if (fd >= 2*frames_per_sec) bin = 3;
                        else if (fd >= 1*frames_per_sec) bin = 2;
                        else if (fd >= frames_per_sec/2) bin = 1;
                        td_sum[bin] += c; td_cnt[bin]++;
                    } else {
                        if (abs(wa - wb) > 2) continue;
                        diff_pairs++; diff_sum += c;
                    }
                }
            }
        }
    }

    printf("═══ SUMMARY ═══\n");
    printf("  Same-word pairs: %d (avg %.4f) | Diff: %d (avg %.4f) | gap: %.4f\n",
           same_pairs, same_pairs?same_sum/same_pairs:0,
           diff_pairs, diff_pairs?diff_sum/diff_pairs:0,
           (same_pairs?same_sum/same_pairs:0) - (diff_pairs?diff_sum/diff_pairs:0));
    printf("  Same-word by TIME DISTANCE:\n");
    for (int b = 0; b < 5; b++)
        if (td_cnt[b])
            printf("    %-7s: %4d pairs, avg=%.4f\n", td_names[b], td_cnt[b], td_sum[b]/td_cnt[b]);

    for (int fi = 0; fi < n_files; fi++) free(mel[fi]);
    free(patterns);
    return 0;
}
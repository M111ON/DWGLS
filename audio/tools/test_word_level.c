#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#define SAMPLE_RATE  16000
#define N_FFT        400
#define HOP_SIZE     1600
#define N_MELS       128
#define N_CHANNELS   64
#define MAX_FRAMES   2000
#define TOP_K        8
#define MAX_CELLS    64
#define CELL_SAMPLES 500
#define N_WORDS      9

static double g_win[N_FFT];
static double g_mel_filter[N_MELS][N_FFT/2+1];

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
                re[a] += tr; im[a] += ti;
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
    if (nf <= 0) return 0;
    double re[512], im[512];
    for (int f = 0; f < nf; f++) {
        int off = f * HOP_SIZE;
        for (int i = 0; i < 512; i++) { re[i] = 0.0; im[i] = 0.0; }
        for (int i = 0; i < N_FFT && off+i < n_samples; i++)
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
    if (!f) { printf("Cannot open %s\n", path); return NULL; }
    char riff[4], wave[4]; uint32_t sz;
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

typedef struct {
    uint8_t fp[MAX_CELLS][TOP_K];
    double inten[MAX_CELLS];
    int n_cells;
} MeshPattern;

void build_mesh(const double mel[][N_MELS], int nf, MeshPattern *p) {
    memset(p, 0, sizeof(MeshPattern));
    int fpc = CELL_SAMPLES / HOP_SIZE;
    if (fpc < 1) fpc = 1;
    p->n_cells = (nf + fpc - 1) / fpc;
    if (p->n_cells > MAX_CELLS) p->n_cells = MAX_CELLS;
    for (int ci = 0; ci < p->n_cells; ci++) {
        int f0 = ci * fpc;
        int f1 = f0 + fpc - 1;
        if (f1 >= nf) f1 = nf - 1;
        int nc = f1 - f0 + 1;
        double ch[64] = {0};
        for (int f = f0; f <= f1; f++)
            for (int c = 0; c < 64; c++)
                ch[c] += 0.5*(mel[f][c*2] + mel[f][c*2+1]);
        for (int c = 0; c < 64; c++) ch[c] /= nc;
        double mn = 1e10, mx = -1e10;
        for (int c = 0; c < 64; c++) { if (ch[c]<mn) mn=ch[c]; if (ch[c]>mx) mx=ch[c]; }
        double rg = mx - mn; if (rg < 1e-10) rg = 1.0;
        double amp[64];
        for (int c = 0; c < 64; c++) amp[c] = (ch[c]-mn)/rg;
        double mean = 0;
        for (int c = 0; c < 64; c++) mean += amp[c];
        mean /= 64.0;
        if (mean < 0.05) { p->inten[ci] = 0; continue; }
        int top[TOP_K];
        topk8(amp, top);
        for (int k = 0; k < TOP_K; k++) p->fp[ci][k] = (uint8_t)top[k];
        p->inten[ci] = mean;
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

int main(void) {
    init_all();

    // FS2 durations (proportions) for "The quick brown fox jumps over the lazy dog"
    // From actual FS2 encode_phoneme output
    double dur_prop[N_WORDS] = {0.042, 0.091, 0.095, 0.108, 0.124, 0.075, 0.035, 0.102, 0.096};
    const char *words[N_WORDS] = {"The","quick","brown","fox","jumps","over","the","lazy","dog"};

    // Load full audio files
    int ns_pb, ns_fs2;
    int16_t *pb_full = read_wav("tts_data/pb_fox_16k.wav", &ns_pb);
    int16_t *fs2_full = read_wav("tts_data/fs2_fox_16k.wav", &ns_fs2);
    if (!pb_full || !fs2_full) { printf("Error\n"); return 1; }

    // Compute full mel
    double (*mel_pb)[N_MELS] = malloc(sizeof(double)*MAX_FRAMES*N_MELS);
    double (*mel_fs2)[N_MELS] = malloc(sizeof(double)*MAX_FRAMES*N_MELS);
    int nf_pb = compute_mel(pb_full, ns_pb, mel_pb);
    int nf_fs2 = compute_mel(fs2_full, ns_fs2, mel_fs2);
    printf("PB: %d frames (%.2fs) | FS2: %d frames (%.2fs)\n",
           nf_pb, nf_pb*HOP_SIZE/(double)SAMPLE_RATE,
           nf_fs2, nf_fs2*HOP_SIZE/(double)SAMPLE_RATE);

    // Split into word-level segments using proportions
    MeshPattern pb_words[N_WORDS], fs2_words[N_WORDS];
    printf("\n=== WORD-LEVEL MESH PATTERNS ===\n");
    for (int w = 0; w < N_WORDS; w++) {
        double start_prop = 0;
        for (int i = 0; i < w; i++) start_prop += dur_prop[i];
        double end_prop = start_prop + dur_prop[w];

        int pb_start = (int)(start_prop * nf_pb);
        int pb_end = (int)(end_prop * nf_pb);
        int fs2_start = (int)(start_prop * nf_fs2);
        int fs2_end = (int)(end_prop * nf_fs2);

        int pb_nf = pb_end - pb_start;
        int fs2_nf = fs2_end - fs2_start;
        if (pb_nf <= 0) pb_nf = 1;
        if (fs2_nf <= 0) fs2_nf = 1;

        build_mesh(mel_pb + pb_start, pb_nf, &pb_words[w]);
        build_mesh(mel_fs2 + fs2_start, fs2_nf, &fs2_words[w]);
        printf("  %6s: PB %d cells, FS2 %d cells\n", words[w], pb_words[w].n_cells, fs2_words[w].n_cells);
    }

    // Compare same word across speakers
    printf("\n=== SAME WORD, DIFF SPEAKER ===\n");
    double same_sum = 0; int same_n = 0;
    for (int w = 0; w < N_WORDS; w++) {
        double c = mesh_confidence(&pb_words[w], &fs2_words[w]);
        printf("  %6s: %.4f\n", words[w], c);
        same_sum += c; same_n++;
    }
    printf("  AVERAGE: %.4f\n", same_sum/same_n);

    // Compare different words, same speaker (PB)
    printf("\n=== DIFF WORD, SAME SPEAKER (PB) ===\n");
    double diff_sum = 0; int diff_n = 0;
    for (int i = 0; i < N_WORDS; i++) {
        for (int j = i+1; j < N_WORDS; j++) {
            double c = mesh_confidence(&pb_words[i], &pb_words[j]);
            diff_sum += c; diff_n++;
        }
    }
    printf("  AVERAGE: %.4f (%d pairs)\n", diff_sum/diff_n, diff_n);

    printf("\n=== VERDICT ===\n");
    printf("  Same word avg: %.4f\n", same_sum/same_n);
    printf("  Diff word avg: %.4f\n", diff_sum/diff_n);
    printf("  Gap: %.4f\n", (same_sum/same_n) - (diff_sum/diff_n));

    free(pb_full); free(fs2_full);
    free(mel_pb); free(mel_fs2);
    return 0;
}

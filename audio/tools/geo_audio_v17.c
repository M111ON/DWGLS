// geo_audio_v17.c — HILBERT = MAZE FIELD SWITCH (user correction)
//
// "hilbert= maze field switch ไม่เดินตามใคร มันเป็นโครงสร้าง"
//   Hilbert is a STATIC STRUCTURE (maze) with SWITCHES at its cells.
//   It does NOT walk. No walker, no path-following. Data runs through the
//   field; switches flip when data slams into their position.
//
// "hilbert มัน walk 3 skip 1 วัดค่า skip"
//   The STRUCTURE itself alternates: 3 switch cells, 1 skip cell
//   (measured by position along Hilbert order: i%4==3 = skip/port).
//   Walked cells = switches (ON/OFF by data slam).
//   SKIP cells   = measurement ports — read the VALUE that passes the gap.
//
// Per tick: 64 channels → amplitude 0..1 → top-K standout
//   switch  cells (i%4!=3): hit count          → switch cosine (set agree)
//   skip    cells (i%4==3): amplitude weighted  → skip value cosine (gap agree)
//
// Floors (INVERT family, user): 0° | invert(63-h) | mirror-Y | mirror-X
// — distinct spatial order → skip ports land on different channels per floor.
//
// Build: gcc -O2 -Wall -o tools/geo_audio_v17.exe tools/geo_audio_v17.c -lm

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
#define N_FLOORS     4
#define TOWER        81
#define MAX_FRAMES   2000
#define MAX_WORDS    60
#define MAX_WORD_LEN 64
#define TOP_K        8

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

// Static structure per floor. h_map[floor][channel] = fixed Hilbert position.
// skip_mask[pos] = 1 → this cell is a SKIP port (i%4==3 along path order).
static uint8_t h_map[N_FLOORS][N_CHANNELS];
static uint8_t skip_mask[N_FLOORS][N_CHANNELS];

void init_all(void) {
    for (int i = 0; i < N_FFT; i++)
        g_win[i] = 0.5 * (1.0 - cos(2.0*M_PI*i/N_FFT));
    for (int f = 0; f < N_FLOORS; f++) {
        for (uint32_t i = 0; i < N_CHANNELS; i++) {   // i = channel
            uint32_t x = i % 8, y = i / 8;
            switch (f) {
                case 0: /* 0°       */
                    h_map[f][i] = (uint8_t)hilbert_idx(x, y, 8); break;
                case 1: /* INVERT   */
                    h_map[f][i] = (uint8_t)(63 - hilbert_idx(x, y, 8)); break;
                case 2: /* MIRROR-Y */
                    h_map[f][i] = (uint8_t)hilbert_idx(x, 7 - y, 8); break;
                case 3: /* MIRROR-X */
                    h_map[f][i] = (uint8_t)hilbert_idx(7 - x, y, 8); break;
            }
        }
        // skip_mask over POSITIONS: walk 3, skip 1 along the path.
        // position p is a skip port iff its reverse-order index ≡ 3 (mod 4).
        // (we label skip ports by the channel that sits there on this floor)
        for (int p = 0; p < N_CHANNELS; p++)
            skip_mask[f][p] = (((int)h_map[f][p] & 3) == 3) ? 1 : 0;
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
// MAZE FIELD SWITCH: static structure. Data slams in, switches flip.
//   switch_hist[floor][pos]  — how many times a switch cell was lit
//   skip_val[floor][pos]     — accumulated amplitude at SKIP ports
//   skip_tower[floor][pos][] — tower histogram at skip ports
// ═══════════════════════════════════════════════════════════
typedef struct {
    uint32_t switch_hist[N_FLOORS][N_CHANNELS];
    uint32_t skip_val[N_FLOORS][N_CHANNELS];
    uint32_t skip_tower[N_FLOORS][N_CHANNELS][TOWER];
    int n_frames;
} MazePattern;

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

void build_maze(const double mel[][N_MELS], int start, int end, MazePattern *p) {
    memset(p, 0, sizeof(MazePattern));
    p->n_frames = end - start + 1;

    for (int f = start; f <= end; f++) {
        double mn = 1e10, mx = -1e10;
        for (int m = 0; m < N_MELS; m++) {
            if (mel[f][m] < mn) mn = mel[f][m];
            if (mel[f][m] > mx) mx = mel[f][m];
        }
        double rg = mx - mn;
        if (rg < 1e-10) rg = 1.0;
        double ch[64];
        for (int c = 0; c < 64; c++) {
            double v = 0.5*(mel[f][c*2] + mel[f][c*2+1]);
            ch[c] = (v - mn) / rg;                  // 0..1 within frame
        }
        double ch_min = 1e10, ch_max = -1e10;
        for (int c = 0; c < 64; c++) {
            if (ch[c] < ch_min) ch_min = ch[c];
            if (ch[c] > ch_max) ch_max = ch[c];
        }
        double ch_rg = ch_max - ch_min;
        if (ch_rg < 1e-10) ch_rg = 1.0;
        double amp_ch[64];
        for (int c = 0; c < 64; c++)
            amp_ch[c] = (ch[c] - ch_min) / ch_rg;   // 0..1 relative

        int top_idx[TOP_K]; double top_val[TOP_K];
        topk(amp_ch, top_idx, top_val);
        if (top_val[0] < 0) continue;

        int tower = (int)(top_val[0] * (TOWER-1));
        if (tower > TOWER-1) tower = TOWER-1;
        if (tower < 0) tower = 0;

        for (int k = 0; k < TOP_K; k++) {
            int c = top_idx[k];
            if (top_val[k] < 0) break;
            for (int fl = 0; fl < N_FLOORS; fl++) {
                uint8_t pos = h_map[fl][c];
                if (!skip_mask[fl][pos]) {
                    // SWITCH cell: flips on (hit count)
                    p->switch_hist[fl][pos]++;
                } else {
                    // SKIP port: read the value that passes the gap
                    p->skip_val[fl][pos] += (uint32_t)(top_val[k] * 255.0);
                    p->skip_tower[fl][pos][tower]++;
                }
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

double maze_confidence(const MazePattern *a, const MazePattern *b, int fl) {
    // switch agreement + skip-value agreement
    double sw = vec_cos(a->switch_hist[fl], b->switch_hist[fl], N_CHANNELS);
    double sv = vec_cos(a->skip_val[fl], b->skip_val[fl], N_CHANNELS);
    // tower at skip ports
    double dot = 0, na = 0, nb = 0;
    for (int c = 0; c < N_CHANNELS; c++) {
        if (!a->skip_val[fl][c] || !b->skip_val[fl][c]) continue;
        for (int t = 0; t < TOWER; t++) {
            dot += (double)a->skip_tower[fl][c][t] * b->skip_tower[fl][c][t];
            na += (double)a->skip_tower[fl][c][t] * a->skip_tower[fl][c][t];
            nb += (double)b->skip_tower[fl][c][t] * b->skip_tower[fl][c][t];
        }
    }
    double st = (sqrt(na)*sqrt(nb) > 0) ? dot / (sqrt(na)*sqrt(nb)) : 0.0;
    return 0.5*sw + 0.3*sv + 0.2*st;
}

int main(int argc, char **argv) {
    // v17b: --batch mode reads tools/tts_data/s*.wav + tts_sentences_50.txt
    int batch = 0;
    if (argc >= 2 && strcmp(argv[1], "--batch") == 0) batch = 1;

    double (*mel[50])[N_MELS];
    int frames[50];
    AlignedWord words[50][MAX_WORDS];
    int nwords[50];
    const char *paths[50];
    char sentences[50][1024];
    int n_files = 0;

    if (batch) {
        // Read sentences file
        FILE *sf = fopen("tts_sentences_50.txt", "r");
        if (!sf) { printf("Error: tts_sentences_50.txt not found (run from tools/)\n"); return 1; }
        char line[1024];
        while (n_files < 50 && fgets(line, sizeof line, sf)) {
            line[strcspn(line, "\r\n")] = 0;
            if (strlen(line) == 0) continue;
            snprintf(sentences[n_files], 1024, "%s", line);
            static char pbuf[50][128];
            snprintf(pbuf[n_files], 128, "tts_data/s%d.wav", n_files+1);
            paths[n_files] = pbuf[n_files];
            n_files++;
        }
        fclose(sf);
    } else {
        n_files = (argc - 1) / 2;
        if (n_files > 50) n_files = 50;
        int idx = 1;
        if (strcmp(argv[1], "--") == 0) idx = 2;
        for (int i = 0; i < n_files; i++) {
            paths[i] = argv[idx + i*2];
            snprintf(sentences[i], 1024, "%s", argv[idx + i*2 + 1]);
        }
    }

    init_all();

    printf("=== MAZE FIELD SWITCH v17%s — hilbert = structure, not walker ===\n",
           batch ? " BATCH" : "");
    printf("Floors: 0° | INVERT | MIRROR-Y | MIRROR-X (static, INVERT family)\n");
    printf("Structure: walk-3 skip-1 → switch cells + skip value ports\n\n");

    for (int fi = 0; fi < n_files; fi++) {
        const char *wavpath = paths[fi];
        const char *sentence = sentences[fi];
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
    }
    printf("Files loaded: %d\n", n_files);

    MazePattern (*patterns)[MAX_WORDS] =
        malloc(sizeof(MazePattern) * n_files * MAX_WORDS);
    if (!patterns) { printf("OOM\n"); return 1; }

    for (int fi = 0; fi < n_files; fi++)
        for (int w = 0; w < nwords[fi]; w++) {
            MazePattern *p = &patterns[fi][w];
            memset(p, 0, sizeof(MazePattern));
            build_maze(mel[fi], words[fi][w].start_frame,
                       words[fi][w].end_frame, p);
        }

    int same_pairs = 0, diff_pairs = 0;
    double same_c[4] = {0}, diff_c[4] = {0};

    for (int fa = 0; fa < n_files; fa++) {
        for (int wa = 0; wa < nwords[fa]; wa++) {
            for (int fb = fa+1; fb < n_files; fb++) {
                for (int wb = 0; wb < nwords[fb]; wb++) {
                    int is_same = (strcmp(words[fa][wa].word, words[fb][wb].word) == 0);
                    if (is_same) {
                        same_pairs++;
                        for (int fl = 0; fl < 4; fl++)
                            same_c[fl] += maze_confidence(&patterns[fa][wa], &patterns[fb][wb], fl);
                    } else {
                        if (abs(wa - wb) > 2) continue;
                        diff_pairs++;
                        for (int fl = 0; fl < 4; fl++)
                            diff_c[fl] += maze_confidence(&patterns[fa][wa], &patterns[fb][wb], fl);
                    }
                }
            }
        }
    }

    printf("\n═══ SUMMARY ═══\n");
    printf("  Same-word pairs: %d, Diff-word pairs: %d\n", same_pairs, diff_pairs);
    double ss_all = 0, ds_all = 0;
    for (int fl = 0; fl < 4; fl++) {
        double ss = same_pairs ? same_c[fl]/same_pairs : 0;
        double ds = diff_pairs ? diff_c[fl]/diff_pairs : 0;
        ss_all += ss; ds_all += ds;
        const char *name = fl==0 ? "0°" : fl==1 ? "INV" : fl==2 ? "MY" : "MX";
        printf("  Floor %s: same=%.4f diff=%.4f gap=%.4f\n", name, ss, ds, ss-ds);
    }
    printf("\n  Cross-floor avg: same=%.4f diff=%.4f gap=%.4f\n",
           ss_all/4.0, ds_all/4.0, (ss_all-ds_all)/4.0);

    for (int fi = 0; fi < n_files; fi++) free(mel[fi]);
    free(patterns);
    return 0;
}
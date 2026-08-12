// geo_audio_v15.c — HYBRID: direct primary path, Hilbert ON-DEMAND only
//
// User correction (v14 aftermath): "hilbert ใช้เท่าที่จำเป็นไม่ต้องใช้ตลอด"
//   Hilbert = ruler kept beside the field. Data flows through normally (direct).
//   Pull the ruler out ONLY when it is needed:
//     1. ambiguous match (two candidates too close on direct score)
//     2. cross-verify (confirm a weak direct hit with 4 rotated views)
//
// Architecture:
//   PRIMARY path (every tick, cheap):
//     Top-8 standout channels → DIRECT address (channel c → pos c)
//     hits[64]++  +  tower_hist[64][81]++        (no Hilbert at all)
//   ON-DEMAND path (only for pairs where direct score is AMBIGUOUS):
//     same tick patterns slammed into 4 rotated Hilbert grids (0/90/180/270)
//     cross-view consensus: mean − variance across 4 floors
//
//   Decision:
//     d = direct_confidence(A,B)
//     if |d − 0.5| > MARGIN  → decide from direct alone (Hilbert NOT invoked)
//     else (ambiguous zone)   → invoke Hilbert 4-floor consensus to decide
//
// Report: how many pairs needed Hilbert, direct-only accuracy vs gated accuracy.
//
// Build: gcc -O2 -Wall -o tools/geo_audio_v15.exe tools/geo_audio_v15.c -lm

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
#define N_FLOORS     4         // 4 Hilbert rotations (parallel views)
#define TOWER        81        // 9×9 amplitude quantization
#define GEO_FULL     (N_CHANNELS * N_FLOORS * TOWER)  // 20736
#define MAX_FRAMES   2000
#define MAX_WORDS    60
#define MAX_WORD_LEN 64
#define AMBIG_MARGIN 0.10      // |d−0.5| < margin → ambiguous → Hilbert invoked

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

// 4 Hilbert maps: channel → hilbert position (8×8 = 64)
// User correction: "ก็ใช้ invert hilbert สิ" — INVERT (mirror) instead of
// only rotations. Rotations are pure permutations (cosine-invariant);
// invert/mirror break the spatial order DIFFERENTLY → floor-relative
// metrics get real information.
//   Floor 0: 0°        (identity orientation)
//   Floor 1: INVERT    (traverse curve in reverse: 63 − h)
//   Floor 2: MIRROR-Y  (reflect along horizontal axis: y → 7−y)
//   Floor 3: MIRROR-X  (reflect along vertical axis: x → 7−x)
static uint8_t h_map[N_FLOORS][N_CHANNELS];
// Direct map: channel c → position c (identity — the default path)
static uint8_t d_map[N_CHANNELS];

void init_all(void) {
    for (int i = 0; i < N_FFT; i++)
        g_win[i] = 0.5 * (1.0 - cos(2.0*M_PI*i/N_FFT));
    // Base: pure Hilbert 8×8 (identity orientation)
    uint8_t h_base[N_CHANNELS];
    for (uint32_t i = 0; i < N_CHANNELS; i++)
        h_base[i] = (uint8_t)hilbert_idx(i % 8, i / 8, 8);
    for (int f = 0; f < N_FLOORS; f++) {
        for (uint32_t i = 0; i < N_CHANNELS; i++) {
            uint32_t x = i % 8, y = i / 8;
            switch (f) {
                case 0: /* 0° */      h_map[f][i] = h_base[i];        break;
                case 1: /* INVERT */  h_map[f][i] = (uint8_t)(63 - h_base[i]); break;
                case 2: /* MIRROR-Y */h_map[f][i] = (uint8_t)hilbert_idx(x, 7 - y, 8); break;
                case 3: /* MIRROR-X */h_map[f][i] = (uint8_t)hilbert_idx(7 - x, y, 8); break;
            }
        }
    }
    for (int c = 0; c < N_CHANNELS; c++) d_map[c] = (uint8_t)c;
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
// HYBRID pattern: DIRECT is the primary path (every tick),
// the same tick is ALSO captured into 4 Hilbert rotations — but those
// are only READ (scored) when a match is ambiguous. Data always walks
// the direct field; Hilbert ruler stays on the shelf until needed.
// ═══════════════════════════════════════════════════════════
typedef struct {
    uint32_t d_hits[N_CHANNELS];              // direct pos hits (primary)
    uint32_t d_tower[N_CHANNELS][TOWER];      // direct amplitude dist
    uint32_t h_hits[N_FLOORS][N_CHANNELS];    // hilbert views (on-demand)
    uint32_t h_tower[N_FLOORS][N_CHANNELS][TOWER];
    int n_frames;
} HybridPattern;

void build_hybrid_pattern(const double mel[][N_MELS], int start, int end,
                          HybridPattern *p) {
    memset(p, 0, sizeof(HybridPattern));
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
            amp_ch[c] = (ch[c] - ch_min) / ch_rg;   // 0..1

        // STANDOUT: TOP-K channels (sparse harmonic signature)
        const int TOP_K = 8;
        int top_idx[8] = {0};
        double top_val[8] = {-1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0};
        for (int c = 0; c < 64; c++) {
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

        for (int k = 0; k < TOP_K; k++) {
            int c = top_idx[k];
            if (top_val[k] < 0) break;
            double amp = top_val[k];
            int tower = (int)(amp * (TOWER-1));
            if (tower > TOWER-1) tower = TOWER-1;
            if (tower < 0) tower = 0;

            // PRIMARY: direct address (no Hilbert)
            uint8_t dp = d_map[c];
            p->d_hits[dp]++;
            p->d_tower[dp][tower]++;

            // CAPTURE (not scored yet): 4 Hilbert views of the same tick
            for (int fl = 0; fl < N_FLOORS; fl++) {
                uint8_t pos = h_map[fl][c];
                p->h_hits[fl][pos]++;
                p->h_tower[fl][pos][tower]++;
            }
        }
    }
}

static double vec_cos(const uint32_t *a, const uint32_t *b, int n) {
    double dot = 0, na = 0, nb = 0;
    for (int i = 0; i < n; i++) {
        dot += (double)a[i] * b[i];
        na += (double)a[i] * a[i];
        nb += (double)b[i] * b[i];
    }
    return (sqrt(na)*sqrt(nb) > 0) ? dot / (sqrt(na)*sqrt(nb)) : 0.0;
}

// DIRECT confidence — the cheap primary judge
double direct_confidence(const HybridPattern *a, const HybridPattern *b) {
    double j = vec_cos(a->d_hits, b->d_hits, N_CHANNELS);

    double dot = 0, na = 0, nb = 0;
    for (int c = 0; c < N_CHANNELS; c++) {
        if (!a->d_hits[c] || !b->d_hits[c]) continue;
        for (int t = 0; t < TOWER; t++) {
            dot += (double)a->d_tower[c][t] * b->d_tower[c][t];
            na += (double)a->d_tower[c][t] * a->d_tower[c][t];
            nb += (double)b->d_tower[c][t] * b->d_tower[c][t];
        }
    }
    double tc = (sqrt(na)*sqrt(nb) > 0) ? dot / (sqrt(na)*sqrt(nb)) : 0.0;
    return 0.6*j + 0.4*tc;
}

// HILBERT cross-view consensus — the ruler, pulled out only when needed
// mean − variance across 4 rotated floors: same word = high & stable,
// diff word = lower & noisy.
double hilbert_consensus(const HybridPattern *a, const HybridPattern *b) {
    double c[4];
    for (int fl = 0; fl < 4; fl++) {
        double j = vec_cos(a->h_hits[fl], b->h_hits[fl], N_CHANNELS);
        double dot = 0, na = 0, nb = 0;
        for (int cc = 0; cc < N_CHANNELS; cc++) {
            if (!a->h_hits[fl][cc] || !b->h_hits[fl][cc]) continue;
            for (int t = 0; t < TOWER; t++) {
                dot += (double)a->h_tower[fl][cc][t] * b->h_tower[fl][cc][t];
                na += (double)a->h_tower[fl][cc][t] * a->h_tower[fl][cc][t];
                nb += (double)b->h_tower[fl][cc][t] * b->h_tower[fl][cc][t];
            }
        }
        double tc = (sqrt(na)*sqrt(nb) > 0) ? dot / (sqrt(na)*sqrt(nb)) : 0.0;
        c[fl] = 0.6*j + 0.4*tc;
    }
    double mean = 0;
    for (int fl = 0; fl < 4; fl++) mean += c[fl];
    mean /= 4.0;
    double var = 0;
    for (int fl = 0; fl < 4; fl++) var += (c[fl]-mean)*(c[fl]-mean);
    var /= 4.0;
    return mean - var;
}

// Floor-RELATIVE consensus — the ONLY metric where Hilbert rotation
// carries real information: for each floor, compare the SPATIAL POSITIONS
// of the two patterns' lit cells. Same word → images superpose on every
// floor (small distance). Diff word → images drift apart (large distance).
// Direct identity has no rotation axis → this cannot be computed without
// the 4 rotated Hilbert grids. This is the "ใช้เท่าที่จำเป็น" case: the
// ruler is needed when position-based (not count-based) verification matters.
typedef struct { uint8_t pos; uint32_t cnt; } LitCell;

static int lit_cells(const uint32_t *hits, LitCell *out, int maxn) {
    int n = 0;
    for (int c = 0; c < N_CHANNELS && n < maxn; c++)
        if (hits[c]) { out[n].pos = (uint8_t)c; out[n].cnt = hits[c]; n++; }
    return n;
}

// Weighted spatial distance of two patterns on ONE floor (positions differ
// → distance; same position → 0 regardless of which channel lit it).
// Uses circular distance on the 8×8 Hilbert grid.
static double floor_spatial_dist(const HybridPattern *a, const HybridPattern *b, int fl) {
    // Build position→weight maps for both patterns on this floor
    double wa[64] = {0}, wb[64] = {0};
    // Map: channel hits → floor position (flatten: for each channel, its
    // Hilbert position on floor fl; add channel's hit weight there)
    for (int c = 0; c < N_CHANNELS; c++) {       // direct channel hits
        if (a->d_hits[c]) {
            uint8_t pos = h_map[fl][c];
            wa[pos] += (double)a->d_hits[c];
        }
        if (b->d_hits[c]) {
            uint8_t pos = h_map[fl][c];
            wb[pos] += (double)b->d_hits[c];
        }
    }
    // Normalize to unit sums
    double sa = 0, sb = 0, dist = 0;
    for (int p = 0; p < 64; p++) { sa += wa[p]; sb += wb[p]; }
    if (sa <= 0 || sb <= 0) return 1.0;
    for (int p = 0; p < 64; p++) { wa[p] /= sa; wb[p] /= sb; }
    // EMD-lite: Wasserstein-1 over the 1D Hilbert order (positions sorted by
    // their Hilbert index = spatial order along the curve)
    double cum = 0;
    for (int p = 0; p < 64; p++) {
        cum += wa[p] - wb[p];
        dist += fabs(cum);
    }
    return dist;
}

// Spatial consensus across 4 floors: mean distance (all floors). Same word
// → low on every floor; diff word → high on every floor. This metric USES
// the rotation (each floor is a different spatial embedding).
double hilbert_spatial(const HybridPattern *a, const HybridPattern *b) {
    double d[4], mean = 0;
    for (int fl = 0; fl < 4; fl++) {
        d[fl] = floor_spatial_dist(a, b, fl);
        mean += d[fl];
    }
    mean /= 4.0;
    double var = 0;
    for (int fl = 0; fl < 4; fl++) var += (d[fl]-mean)*(d[fl]-mean);
    var /= 4.0;
    return mean; // lower = more similar; -var optional (stability witness)
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

    printf("=== HYBRID v15 — direct primary, Hilbert ON-DEMAND ===\n");
    printf("Primary: Top-8 → direct 64-cell field (no Hilbert)\n");
    printf("Hilbert: invoked ONLY when |direct − 0.5| < %.2f (ambiguous)\n\n",
           AMBIG_MARGIN);

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

    // ═══ Build patterns (heap: each ~88KB) ═══
    HybridPattern (*patterns)[MAX_WORDS] =
        malloc(sizeof(HybridPattern) * n_files * MAX_WORDS);
    if (!patterns) { printf("OOM\n"); return 1; }

    for (int fi = 0; fi < n_files; fi++) {
        for (int w = 0; w < nwords[fi]; w++) {
            HybridPattern *p = &patterns[fi][w];
            memset(p, 0, sizeof(HybridPattern));
            build_hybrid_pattern(mel[fi], words[fi][w].start_frame,
                                 words[fi][w].end_frame, p);
        }
    }

    // ═══ Score all cross-file pairs; classify: same word? ═══
    int total = 0, direct_correct = 0, gated_correct = 0, hilbert_used = 0;
    int spat_correct = 0;
    double sum_d_same = 0, sum_d_diff = 0, sum_h_same = 0, sum_h_diff = 0;
    double sum_s_same = 0, sum_s_diff = 0;
    int n_same = 0, n_diff = 0;

    printf("\n═══ PAIR DECISIONS (d = direct, h = Hilbert consensus) ═══\n");
    for (int fa = 0; fa < n_files; fa++) {
        for (int wa = 0; wa < nwords[fa]; wa++) {
            for (int fb = fa+1; fb < n_files; fb++) {
                for (int wb = 0; wb < nwords[fb]; wb++) {
                    int is_same = (strcmp(words[fa][wa].word, words[fb][wb].word) == 0);
                    double d = direct_confidence(&patterns[fa][wa], &patterns[fb][wb]);
                    double h = hilbert_consensus(&patterns[fa][wa], &patterns[fb][wb]);
                    double s = hilbert_spatial(&patterns[fa][wa], &patterns[fb][wb]);

                    int direct_guess = (d >= 0.5);
                    int direct_ok = (direct_guess == is_same);
                    direct_correct += direct_ok;

                    // Spatial: LOWER distance = more similar → classify at 0.5
                    int spat_guess = (s <= 0.5);
                    spat_correct += (spat_guess == is_same);

                    int guess = direct_guess, used_hilbert = 0;
                    if (fabs(d - 0.5) < AMBIG_MARGIN) {
                        used_hilbert = 1;
                        hilbert_used++;
                        guess = (h >= 0.5);
                    }
                    gated_correct += (guess == is_same);
                    total++;

                    if (is_same) { sum_d_same += d; sum_h_same += h; sum_s_same += s; n_same++; }
                    else         { sum_d_diff += d; sum_h_diff += h; sum_s_diff += s; n_diff++; }

                    if (used_hilbert)
                        printf("  %-12s vs %-12s d=%.3f h=%.3f → %s (Hilbert)\n",
                               words[fa][wa].word, words[fb][wb].word, d, h,
                               guess == is_same ? "correct" : "WRONG");
                }
            }
        }
    }

    // ═══ Summary ═══
    printf("\n═══ SUMMARY ═══\n");
    printf("Direct-only:        %d/%d = %.1f%%\n", direct_correct, total,
           100.0*direct_correct/total);
    printf("Gated (+Hilbert):   %d/%d = %.1f%%  ", gated_correct, total,
           100.0*gated_correct/total);
    printf("(Hilbert invoked on %d/%d pairs = %.1f%%)\n",
           hilbert_used, total, 100.0*hilbert_used/total);
    printf("\nDirect score   — same: %.4f | diff: %.4f | gap: %.4f\n",
           n_same ? sum_d_same/n_same : 0, n_diff ? sum_d_diff/n_diff : 0,
           (n_same ? sum_d_same/n_same : 0) - (n_diff ? sum_d_diff/n_diff : 0));
    printf("Hilbert cons.   — same: %.4f | diff: %.4f | gap: %.4f\n",
           n_same ? sum_h_same/n_same : 0, n_diff ? sum_h_diff/n_diff : 0,
           (n_same ? sum_h_same/n_same : 0) - (n_diff ? sum_h_diff/n_diff : 0));
    printf("\nSpatial (W1 dist, LOWER = more similar):\n");
    printf("  same: %.4f | diff: %.4f | gap: %.4f | acc@0.5: %d/%d = %.1f%%\n",
           n_same ? sum_s_same/n_same : 0, n_diff ? sum_s_diff/n_diff : 0,
           (n_diff ? sum_s_diff/n_diff : 0) - (n_same ? sum_s_same/n_same : 0),
           spat_correct, total, 100.0*spat_correct/total);

    for (int fi = 0; fi < n_files; fi++) free(mel[fi]);
    free(patterns);
    return 0;
}
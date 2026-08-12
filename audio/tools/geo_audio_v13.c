// geo_audio_v13.c — Hilbert SWITCH GRID + delta fingerprint
// 
// Concept (user): Hilbert = ruler, passive switch board
//   - 64 ช่อง × 4 ชั้น (64×4×81 = 20736 structure)
//   - สิ่งที่เสียง stand out → trigger switch (light ON)
//   - ไม่มี compute — ข้อมูลชนเอง เราแค่จูน threshold
//   - Pattern = ชุด lights ON (binary)
//   - Follow delta sample → confidence + hitrate
//
// Build: gcc -O2 -Wall -o tools/geo_audio_v13.exe tools/geo_audio_v13.c -lm

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#define SAMPLE_RATE  16000
#define MAX_SAMPLES  200000
#define MAX_WORDS    60
#define MAX_WORD_LEN 64
#define N_CHANNELS   64        // 64 ช่อง time slots
#define N_FLOORS     4         // 4 ชั้น frequency contexts
#define TOWER        81        // 9×9 amplitude × position
#define GEO_FULL     (N_CHANNELS * N_FLOORS * TOWER)  // 20736
#define TOP_PEAKS    4         // peaks per channel per floor

static double g_win[400];

// Hilbert 8×8 for switch grid (64 positions) — STATIC RULER
static uint8_t hilbert_switch[N_CHANNELS];     // which channels light ON
static uint16_t hilbert_delta[N_CHANNELS];     // delta position per lit channel
static uint8_t hilbert_hitrate[N_CHANNELS];    // how often each lights up

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

// Hilbert switch map: channel → hilbert position (8×8 = 64)
// Each FLOOR = Hilbert curve ROTATED 90° — different start corner (user rule:
// flip/rotate between floors, every floor starts at a different corner)
//   Floor 0: 0°     start corner A (identity)
//   Floor 1: 90°CW  start corner B
//   Floor 2: 180°   start corner C
//   Floor 3: 270°CW start corner D
// Same data lands at 4 DIFFERENT maze positions per floor (4 perspectives)
static uint8_t h_map[4][N_CHANNELS];

void init_hilbert(void) {
    for (int f = 0; f < 4; f++) {
        for (uint32_t i = 0; i < N_CHANNELS; i++) {
            uint32_t x = i % 8, y = i / 8;
            uint32_t tx = x, ty = y;
            switch (f) {
                case 0: /* identity */               break;
                case 1: /* rotate  90° CW */ tx = 7 - y; ty = x;     break;
                case 2: /* rotate 180°    */ tx = 7 - x; ty = 7 - y; break;
                case 3: /* rotate 270° CW */ tx = y;     ty = 7 - x; break;
            }
            h_map[f][i] = (uint8_t)hilbert_idx(tx, ty, 8);
        }
    }
    for (int i = 0; i < 400; i++)
        g_win[i] = 0.5 * (1.0 - cos(2.0*M_PI*i/400.0));
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

typedef struct { int start_sample, end_sample; char word[MAX_WORD_LEN]; } AlignedWord;

// Windowed RMS energy at each sample position (frame-based, hop=160)
void frame_rms(const int16_t *samples, int n_samples, double *energy, int *n_frames) {
    int nf = (n_samples - 400) / 160;
    *n_frames = nf;
    for (int f = 0; f < nf; f++) {
        double sum = 0;
        int off = f * 160;
        for (int i = 0; i < 400 && off+i < n_samples; i++) {
            double v = samples[off+i] * g_win[i] / 32768.0;
            sum += v*v;
        }
        energy[f] = sqrt(sum / 400.0);
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

// Align words proportionally (frames → sample positions)
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
        out[i].start_sample = pos * 160;
        out[i].end_sample = (pos + len - 1) * 160 + 400;
        strncpy(out[i].word, words[i], MAX_WORD_LEN-1);
        out[i].word[MAX_WORD_LEN-1] = 0;
        pos += len;
    }
    return nw;
}

// ═══════════════════════════════════════════════════════════
// SWITCH GRID: the core
// Per word segment:
//   - Split samples into 64 channels (equal time slices)
//   - Each channel: find standout position (peak energy sample)
//   - Threshold → switch ON (light) at Hilbert position
//   - Record delta = peak position within channel (0-999)
//   - 4 floors = 4 different thresholds (sensitivity contexts)
//
// The switch grid is PASSIVE:
//   Hilbert doesn't compute — data slams into it
//   We just tune the threshold
// ═══════════════════════════════════════════════════════════
typedef struct {
    uint8_t lights[N_FLOORS][N_CHANNELS];     // binary ON/OFF per floor
    uint16_t deltas[N_FLOORS][N_CHANNELS];    // peak position within channel
    uint8_t hitrate[N_FLOORS][N_CHANNELS];    // avg intensity (0-255)
} SwitchGrid;

// Build switch grid for a word segment
void build_switch_grid(const int16_t *samples, int s0, int s1, SwitchGrid *grid) {
    memset(grid, 0, sizeof(SwitchGrid));
    int span = s1 - s0;
    if (span < N_CHANNELS) span = N_CHANNELS;
    int ch_len = span / N_CHANNELS;  // samples per channel
    
    // 4 floors = 4 threshold levels (adaptive sensitivity)
    // Base = mean energy of segment, floors scale it
    double *ch_peak = malloc(sizeof(double) * N_CHANNELS);
    int *ch_pos = malloc(sizeof(int) * N_CHANNELS);
    double seg_mean = 0;
    int n = 0;
    for (int i = s0; i < s1; i += 160) {
        seg_mean += fabs((double)samples[i]);
        n++;
    }
    if (n > 0) seg_mean /= n;
    
    // Channel peaks
    for (int c = 0; c < N_CHANNELS; c++) {
        int cs = s0 + c * ch_len;
        int ce = cs + ch_len;
        if (ce > s1) ce = s1;
        double pk = 0;
        int pos = cs;
        for (int i = cs; i < ce; i++) {
            double a = fabs((double)samples[i]);
            if (a > pk) { pk = a; pos = i; }
        }
        ch_peak[c] = pk;
        ch_pos[c] = pos - cs;
    }
    
    // Floor thresholds: relative to global peak
    double global_peak = 0;
    for (int c = 0; c < N_CHANNELS; c++)
        if (ch_peak[c] > global_peak) global_peak = ch_peak[c];
    if (global_peak < 1) global_peak = 1;
    
    double floor_thr[4] = { 0.45, 0.60, 0.75, 0.88 };  // selective
    
    for (int f = 0; f < N_FLOORS; f++) {
        for (int c = 0; c < N_CHANNELS; c++) {
            double ratio = ch_peak[c] / global_peak;
            if (ratio >= floor_thr[f]) {
                uint8_t hil = h_map[f][c];  // floor-specific Hilbert position
                grid->lights[f][hil] = 1;
                int delta = ch_pos[c] * 1000 / ch_len;
                if (delta > 999) delta = 999;
                grid->deltas[f][hil] = (uint16_t)delta;
                grid->hitrate[f][hil] = (uint8_t)(ratio * 255.0);
            }
        }
    }
    
    free(ch_peak);
    free(ch_pos);
}

// Pattern match: Jaccard on lit Hilbert positions (per floor)
double grid_jaccard(const SwitchGrid *a, const SwitchGrid *b, int floor) {
    int inter = 0, uni = 0;
    for (int c = 0; c < N_CHANNELS; c++) {
        if (a->lights[floor][c] || b->lights[floor][c]) {
            uni++;
            if (a->lights[floor][c] && b->lights[floor][c]) inter++;
        }
    }
    return uni > 0 ? (double)inter / uni : 0.0;
}

// Delta agreement: how close are delta positions on shared lights
double delta_agree(const SwitchGrid *a, const SwitchGrid *b, int floor) {
    int shared = 0;
    long total_err = 0;
    for (int c = 0; c < N_CHANNELS; c++) {
        if (a->lights[floor][c] && b->lights[floor][c]) {
            shared++;
            int d = (int)a->deltas[floor][c] - (int)b->deltas[floor][c];
            if (d < 0) d = -d;
            total_err += d;
        }
    }
    if (shared == 0) return 0.0;
    double avg_err = (double)total_err / shared / 1000.0;  // normalized
    return 1.0 - avg_err;  // 1.0 = identical deltas
}

// Hitrate agreement: how similar intensities are
double hitrate_agree(const SwitchGrid *a, const SwitchGrid *b, int floor) {
    int shared = 0;
    long total_diff = 0;
    for (int c = 0; c < N_CHANNELS; c++) {
        if (a->lights[floor][c] && b->lights[floor][c]) {
            shared++;
            int d = (int)a->hitrate[floor][c] - (int)b->hitrate[floor][c];
            if (d < 0) d = -d;
            total_diff += d;
        }
    }
    if (shared == 0) return 0.0;
    return 1.0 - (double)total_diff / shared / 255.0;
}

// Composite confidence: Jaccard + delta + hitrate
double confidence(const SwitchGrid *a, const SwitchGrid *b, int floor) {
    double j = grid_jaccard(a, b, floor);
    double d = delta_agree(a, b, floor);
    double h = hitrate_agree(a, b, floor);
    return 0.5*j + 0.3*d + 0.2*h;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        printf("Usage: %s -- <wav1> \"sentence1\" <wav2> \"sentence2\" ...\n", argv[0]);
        return 1;
    }
    init_hilbert();
    
    int n_files = (argc - 1) / 2;
    if (n_files > 10) n_files = 10;
    int idx = 1;
    if (strcmp(argv[1], "--") == 0) idx = 2;
    
    printf("=== Hilbert SWITCH GRID v13 ===\n");
    printf("Structure: %d channels × %d floors × %d tower = %d\n",
           N_CHANNELS, N_FLOORS, TOWER, GEO_FULL);
    printf("Hilbert = passive ruler (64 positions), data slams in, threshold decides\n\n");
    
    AlignedWord words[10][MAX_WORDS];
    int nwords[10];
    SwitchGrid grids[10][MAX_WORDS];
    
    for (int fi = 0; fi < n_files; fi++) {
        const char *wavpath = argv[idx + fi*2];
        const char *sentence = argv[idx + fi*2 + 1];
        
        int ns;
        int16_t *samples = read_wav(wavpath, &ns);
        if (!samples) { printf("Error: %s\n", wavpath); return 1; }
        
        double *energy = malloc(sizeof(double) * (ns/160 + 1));
        int nf;
        frame_rms(samples, ns, energy, &nf);
        nwords[fi] = align_words(sentence, energy, nf, words[fi]);
        free(energy);
        
        printf("File %d: %s (%d samples, %d words)\n", fi+1, wavpath, ns, nwords[fi]);
        for (int w = 0; w < nwords[fi]; w++) {
            build_switch_grid(samples, words[fi][w].start_sample,
                              words[fi][w].end_sample, &grids[fi][w]);
            
            // Show how many lights per floor
            int lit[4] = {0,0,0,0};
            for (int fl = 0; fl < 4; fl++)
                for (int c = 0; c < N_CHANNELS; c++)
                    if (grids[fi][w].lights[fl][c]) lit[fl]++;
            printf("  %s: lights f0=%d f1=%d f2=%d f3=%d\n",
                   words[fi][w].word, lit[0], lit[1], lit[2], lit[3]);
        }
        free(samples);
    }
    
    // ═══ Same-word matching ═══
    printf("\n═══ SAME-WORD Matching (per floor) ═══\n");
    int same_pairs = 0;
    double same_c[4] = {0,0,0,0};
    double same_j[4] = {0,0,0,0};
    
    for (int fa = 0; fa < n_files; fa++) {
        for (int wa = 0; wa < nwords[fa]; wa++) {
            for (int fb = fa+1; fb < n_files; fb++) {
                for (int wb = 0; wb < nwords[fb]; wb++) {
                    if (strcmp(words[fa][wa].word, words[fb][wb].word) != 0) continue;
                    same_pairs++;
                    for (int fl = 0; fl < 4; fl++) {
                        same_c[fl] += confidence(&grids[fa][wa], &grids[fb][wb], fl);
                        same_j[fl] += grid_jaccard(&grids[fa][wa], &grids[fb][wb], fl);
                    }
                }
            }
        }
    }
    
    printf("═══ DIFFERENT-WORD Baseline ═══\n");
    int diff_pairs = 0;
    double diff_c[4] = {0,0,0,0};
    double diff_j[4] = {0,0,0,0};
    
    for (int fa = 0; fa < n_files; fa++) {
        for (int wa = 0; wa < nwords[fa]; wa++) {
            for (int fb = fa+1; fb < n_files; fb++) {
                for (int wb = 0; wb < nwords[fb]; wb++) {
                    if (strcmp(words[fa][wa].word, words[fb][wb].word) == 0) continue;
                    if (abs(wa - wb) > 2) continue;  // sample adjacent positions
                    diff_pairs++;
                    for (int fl = 0; fl < 4; fl++) {
                        diff_c[fl] += confidence(&grids[fa][wa], &grids[fb][wb], fl);
                        diff_j[fl] += grid_jaccard(&grids[fa][wa], &grids[fb][wb], fl);
                    }
                }
            }
        }
    }
    
    printf("\n═══ SUMMARY ═══\n");
    printf("  Same-word pairs: %d, Diff-word pairs: %d\n", same_pairs, diff_pairs);
    for (int fl = 0; fl < 4; fl++) {
        double sm = same_pairs ? same_c[fl]/same_pairs : 0;
        double df = diff_pairs ? diff_c[fl]/diff_pairs : 0;
        double smj = same_pairs ? same_j[fl]/same_pairs : 0;
        double dfj = diff_pairs ? diff_j[fl]/diff_pairs : 0;
        printf("  Floor %d: same C=%.4f (J=%.4f) | diff C=%.4f (J=%.4f) | gap=%.4f\n",
               fl, sm, smj, df, dfj, sm - df);
    }
    
    // Cross-floor consensus: same word must agree across all 4 rotated views
    // (variance across floors LOW for same word, HIGH for different)
    printf("\n  Cross-floor consensus (4 rotated views):\n");
    double cons_same = 0, cons_diff = 0;
    int cs = 0, cd = 0;
    
    for (int fa = 0; fa < n_files; fa++) {
        for (int wa = 0; wa < nwords[fa]; wa++) {
            for (int fb = fa+1; fb < n_files; fb++) {
                for (int wb = 0; wb < nwords[fb]; wb++) {
                    int is_same = (strcmp(words[fa][wa].word, words[fb][wb].word) == 0);
                    if (abs(wa - wb) > 2 && !is_same) continue;
                    
                    // Per-floor confidence
                    double c[4];
                    for (int fl = 0; fl < 4; fl++)
                        c[fl] = confidence(&grids[fa][wa], &grids[fb][wb], fl);
                    
                    // Mean + variance
                    double mean = 0;
                    for (int fl = 0; fl < 4; fl++) mean += c[fl];
                    mean /= 4.0;
                    double var = 0;
                    for (int fl = 0; fl < 4; fl++) var += (c[fl]-mean)*(c[fl]-mean);
                    var /= 4.0;
                    
                    // Consensus = mean − variance penalty (agree = high, stable)
                    double cons = mean - var;
                    if (is_same) { cons_same += cons; cs++; }
                    else { cons_diff += cons; cd++; }
                }
            }
        }
    }
    printf("  Same-word consensus: %.4f | Diff-word consensus: %.4f | gap=%.4f\n",
           cs ? cons_same/cs : 0, cd ? cons_diff/cd : 0,
           (cs ? cons_same/cs : 0) - (cd ? cons_diff/cd : 0));
    
    return 0;
}
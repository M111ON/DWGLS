// geo_audio_polar.c — Circular/Polar Geometric Audio Encoder
// Maps audio to 20736 (144×144) polar addresses
// Angle = time, Radius = frequency band energy
// 3 concentric rings: inner=low, mid=vocal, outer=treble
//
// Build: gcc -Wall -Wextra -O2 -o tools/geo_audio_polar.exe tools/geo_audio_polar.c -lm
// Run:   ./tools/geo_audio_polar.exe <wav_file>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

// === Constants ===
#define SAMPLE_RATE     16000
#define FFT_SIZE        400
#define HOP_SIZE        160
#define MAX_PROCESS     (SAMPLE_RATE * 30)  // 30 sec max
#define ANGLES          144
#define RADII           144
#define GEO_SPACE       (ANGLES * RADII)

// FFT bin boundaries for frequency bands
#define BIN_BAND1_END   7       // 0-300 Hz (7 bins)
#define BIN_BAND2_END   75      // 300-3000 Hz (68 bins)
#define BIN_BAND3_END   200     // 3000-8000 Hz (125 bins)
#define NFFT_HALF       (FFT_SIZE / 2 + 1)

// Band radius ranges
#define R_INNER_END     47
#define R_MID_END       95
#define R_OUTER_END     143

// Word boundary
#define MAX_WORDS       2000

// === WAV structures ===
#pragma pack(push, 1)
typedef struct { char riff[4]; uint32_t size; char wave[4]; } WavRiff;
typedef struct { char id[4]; uint32_t size; } WavChunk;
typedef struct { uint16_t af; uint16_t ch; uint32_t sr; uint32_t br; uint16_t ba; uint16_t bps; } FmtData;
#pragma pack(pop)

// === Word segment ===
typedef struct {
    int start_frame, end_frame, peak_frame;
    double peak_amp, avg_energy;
    int active_bands;
    double band_energy[3];
    int addr_first, addr_last;
} WordSegment;

// === Static buffers ===
static double g_re[FFT_SIZE], g_im[FFT_SIZE];
static double g_mag[NFFT_HALF];
static double g_addr_energy[GEO_SPACE];
static int    g_addr_hit[GEO_SPACE];
static int    g_angle_hit[ANGLES];
static double g_envelope[160000];
static double g_band_energy[160000][3];
static int    g_frame_n;
static WordSegment g_words[MAX_WORDS];
static int    g_n_words;

// === Cooley-Tukey FFT (separate real/imag arrays, in-place) ===
static void fft(double *re, double *im, int n) {
    // Bit-reversal permutation
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            double tr = re[i]; re[i] = re[j]; re[j] = tr;
            double ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
    }
    // Butterfly stages
    for (int len = 2; len <= n; len <<= 1) {
        double ang = -2.0 * M_PI / (double)len;
        double wr = cos(ang), wi = sin(ang);
        for (int i = 0; i < n; i += len) {
            double cr = 1.0, ci = 0.0;
            for (int j = 0; j < len / 2; j++) {
                double ur = re[i+j],      ui = im[i+j];
                double vr = re[i+j+len/2]*cr - im[i+j+len/2]*ci;
                double vi = re[i+j+len/2]*ci + im[i+j+len/2]*cr;
                re[i+j]       = ur + vr;
                im[i+j]       = ui + vi;
                re[i+j+len/2] = ur - vr;
                im[i+j+len/2] = ui - vi;
                double t = cr*wr - ci*wi;
                ci = cr*wi + ci*wr;
                cr = t;
            }
        }
    }
}

// === Compute 3-band energy from FFT magnitude ===
// Returns log-compressed total RMS for envelope/word detection
static double compute_bands(int n_bins, double *band_out) {
    double e[3] = {0};
    int    c[3] = {0};
    for (int i = 1; i < n_bins; i++) {
        int b = -1;
        if (i < BIN_BAND1_END)      b = 0;
        else if (i < BIN_BAND2_END) b = 1;
        else if (i < BIN_BAND3_END) b = 2;
        if (b >= 0) {
            double v = g_mag[i];
            if (v != v || v > 1e10) continue;  // skip NaN/Inf
            e[b] += v * v; c[b]++;
        }
    }
    double total = 0;
    for (int b = 0; b < 3; b++) {
        if (c[b] > 0) e[b] /= c[b];
        // Log-compress for address mapping (normalize to [0, 1])
        double log_e = log10(fmax(e[b], 1e-10));
        band_out[b] = fmax(0.0, fmin(1.0, (log_e + 8.0) / 8.0));
        total += e[b];
    }
    // Return log-compressed RMS for envelope
    double rms = sqrt(total);
    double log_rms = log10(fmax(rms, 1e-10));
    return fmax(0.0, fmin(1.0, (log_rms + 4.0) / 4.0));
}

// === Polar address from frame + band energy ===
static int polar_addr(int frame, int total, double *band, int *ring) {
    int angle = (int)((double)frame * ANGLES / total);
    if (angle >= ANGLES) angle = ANGLES - 1;

    // Find dominant band
    int dom = 0;
    if (band[1] > band[0]) dom = 1;
    if (band[2] > band[dom]) dom = 2;

    int r_start, r_end;
    switch (dom) {
        case 0: r_start = 0;           r_end = R_INNER_END; break;
        case 1: r_start = R_INNER_END + 1; r_end = R_MID_END; break;
        default: r_start = R_MID_END + 1;  r_end = R_OUTER_END; break;
    }

    double norm = band[dom];
    if (norm > 1.0) norm = 1.0;
    int radius = r_start + (int)(norm * (r_end - r_start));
    if (radius > r_end) radius = r_end;

    *ring = dom;
    return angle * RADII + radius;
}

// === All-band polar addresses ===
static void polar_all(int frame, int total, double *band, int *addrs, int *n) {
    int angle = (int)((double)frame * ANGLES / total);
    if (angle >= ANGLES) angle = ANGLES - 1;
    *n = 0;
    for (int b = 0; b < 3; b++) {
        if (band[b] < 0.005) continue;
        int rs, re;
        switch (b) {
            case 0: rs = 0;           re = R_INNER_END; break;
            case 1: rs = R_INNER_END + 1; re = R_MID_END; break;
            default: rs = R_MID_END + 1;  re = R_OUTER_END; break;
        }
        double norm = band[b] > 1.0 ? 1.0 : band[b];
        int r = rs + (int)(norm * (re - rs));
        if (r > re) r = re;
        addrs[*n] = angle * RADII + r;
        (*n)++;
    }
}

// === Read WAV ===
static int read_wav(const char *path, int16_t **out, int *out_n) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "Cannot open %s\n", path); return -1; }

    WavRiff riff;
    if (fread(&riff, sizeof(riff), 1, fp) != 1) { fclose(fp); return -1; }
    if (memcmp(riff.riff, "RIFF", 4) || memcmp(riff.wave, "WAVE", 4)) {
        fclose(fp); return -1;
    }

    int sr = 0, ch = 0, bps = 0;
    while (!feof(fp)) {
        WavChunk c;
        if (fread(&c, sizeof(c), 1, fp) != 1) break;
        if (memcmp(c.id, "fmt ", 4) == 0) {
            FmtData f; fread(&f, sizeof(f), 1, fp);
            sr = f.sr; ch = f.ch; bps = f.bps;
            if (c.size > 16) fseek(fp, c.size - 16, SEEK_CUR);
        } else if (memcmp(c.id, "data", 4) == 0) {
            int total = c.size / (bps / 8);
            int16_t *raw = malloc(total * sizeof(int16_t));
            if (bps == 16) {
                fread(raw, sizeof(int16_t), total, fp);
            } else if (bps == 8) {
                uint8_t *b = malloc(total);
                fread(b, 1, total, fp);
                for (int i = 0; i < total; i++) raw[i] = (int16_t)((b[i]-128)<<8);
                free(b);
            } else {
                fread(raw, sizeof(int16_t), total, fp);
            }
            fclose(fp);

            // Resample to 16kHz
            int out_count = (int)((long long)total * SAMPLE_RATE / sr);
            int16_t *resampled = malloc(out_count * sizeof(int16_t));
            for (int i = 0; i < out_count; i++) {
                long long s = (long long)i * sr / SAMPLE_RATE;
                resampled[i] = raw[s >= total ? total-1 : s];
            }
            free(raw);

            // Mono (take first channel)
            if (ch > 1) {
                int16_t *mono = malloc(out_count * sizeof(int16_t));
                for (int i = 0; i < out_count; i++) mono[i] = resampled[i * ch];
                free(resampled);
                resampled = mono;
            }

            *out = resampled;
            *out_n = out_count;
            return 0;
        } else {
            fseek(fp, c.size, SEEK_CUR);
        }
    }
    fclose(fp);
    return -1;
}

// === Word boundary detection ===
// Strategy: find peaks in smoothed envelope, boundaries = minima between peaks
static void detect_words(double *env, int n) {
    g_n_words = 0;
    if (n < 3) return;

    // Smooth (moving avg, win=16 frames ≈ 160ms)
    static double sm[160000];
    int w = 16;
    for (int i = 0; i < n; i++) {
        double s = 0; int c = 0;
        for (int j = i - w/2; j <= i + w/2; j++) {
            if (j >= 0 && j < n) { s += env[j]; c++; }
        }
        sm[i] = c > 0 ? s / c : 0;
    }

    // Mean of nonzero values
    double sum = 0; int cnt = 0;
    for (int i = 0; i < n; i++) { if (sm[i] > 0.001) { sum += sm[i]; cnt++; } }
    // Fixed threshold for log-compressed speech envelope
    // Speech: ~0.75, Silence: ~0.0, threshold at 0.35
    double thresh = 0.35;

    // Find peaks (local maxima)
    static int peaks[8000];
    int np = 0;
    for (int i = 2; i < n - 2 && np < 8000; i++) {
        if (sm[i] > thresh && sm[i] >= sm[i-1] && sm[i] >= sm[i+1] &&
            sm[i] >= sm[i-2] && sm[i] >= sm[i+2] &&
            (sm[i] > sm[i-1] || sm[i] > sm[i+1])) {
            peaks[np++] = i;
        }
    }

    // Merge nearby peaks (within 8 frames)
    static int merged[8000];
    int nm = 0;
    for (int i = 0; i < np; i++) {
        if (nm == 0 || (peaks[i] - merged[nm-1]) > 8) {
            merged[nm++] = peaks[i];
        } else if (sm[peaks[i]] > sm[merged[nm-1]]) {
            merged[nm-1] = peaks[i];
        }
    }

    // Word boundaries = minima between consecutive peaks
    for (int p = 0; p < nm && g_n_words < MAX_WORDS; p++) {
        int pk = merged[p];
        int lb = p > 0 ? merged[p-1] : 0;
        int rb = p < nm-1 ? merged[p+1] : n-1;

        // Find minimum to left of peak
        int ml = lb; double mlv = sm[lb];
        for (int i = lb; i <= pk; i++) { if (sm[i] < mlv) { mlv = sm[i]; ml = i; } }

        // Find minimum to right of peak
        int mr = pk; double mrv = sm[pk];
        for (int i = pk; i <= rb; i++) { if (sm[i] < mrv) { mrv = sm[i]; mr = i; } }

        int ws = (ml + pk) / 2;
        int we = (pk + mr) / 2;
        if (we <= ws) we = ws + 1;

        g_words[g_n_words].start_frame = ws;
        g_words[g_n_words].end_frame = we;
        g_words[g_n_words].peak_frame = pk;
        g_words[g_n_words].peak_amp = sm[pk];
        g_n_words++;
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <wav_file>\n", argv[0]);
        printf("  Circular/Polar Geometric Audio Encoder\n");
        printf("  Maps audio to %d (144x144) polar addresses\n", GEO_SPACE);
        return 1;
    }

    printf("=== POLAR GEOMETRIC AUDIO ENCODER ===\n");
    printf("Input: %s\n\n", argv[1]);

    int16_t *samples = NULL;
    int n_samples = 0;
    if (read_wav(argv[1], &samples, &n_samples) < 0) return 1;

    if (n_samples > MAX_PROCESS) n_samples = MAX_PROCESS;
    double duration = (double)n_samples / SAMPLE_RATE;
    int total_frames = (n_samples - FFT_SIZE) / HOP_SIZE + 1;
    printf("Duration: %.2f sec, %d samples, %d frames\n\n", duration, n_samples, total_frames);

    // Process frames
    printf("Processing frames...\n");
    g_frame_n = 0;
    memset(g_addr_energy, 0, sizeof(g_addr_energy));
    memset(g_addr_hit, 0, sizeof(g_addr_hit));
    memset(g_angle_hit, 0, sizeof(g_angle_hit));

    for (int f = 0; f < total_frames; f++) {
        int off = f * HOP_SIZE;

        // Hann window + fill FFT buffers
        for (int i = 0; i < FFT_SIZE; i++) {
            double h = 0.5 * (1.0 - cos(2.0 * M_PI * i / FFT_SIZE));
            g_re[i] = h * samples[off + i] / 32768.0;
            g_im[i] = 0.0;
        }

        // FFT
        fft(g_re, g_im, FFT_SIZE);

        // Magnitude (normalized by FFT size, clamped to prevent inf/NaN)
        for (int i = 0; i < NFFT_HALF; i++) {
            double m2 = g_re[i]*g_re[i] + g_im[i]*g_im[i];
            if (m2 != m2 || m2 > 1e20) m2 = 1e20;  // NaN/inf guard
            g_mag[i] = sqrt(m2) / FFT_SIZE;
        }

        // 3-band energy
        double band[3];
        double rms = compute_bands(NFFT_HALF, band);

        // Store
        g_envelope[g_frame_n] = rms;
        memcpy(g_band_energy[g_frame_n], band, sizeof(band));

        // Polar address (dominant band)
        int ring;
        int addr = polar_addr(f, total_frames, band, &ring);
        g_addr_energy[addr] += rms;
        g_addr_hit[addr]++;
        g_angle_hit[f * ANGLES / total_frames]++;

        // All-band addresses
        int aa[3]; int na;
        polar_all(f, total_frames, band, aa, &na);
        for (int a = 0; a < na; a++) {
            g_addr_energy[aa[a]] += band[a];
            g_addr_hit[aa[a]]++;
        }

        g_frame_n++;
        if (g_frame_n >= 160000) break;
    }
    printf("Processed %d frames\n\n", g_frame_n);

    // Word boundary detection
    printf("=== WORD BOUNDARY DETECTION ===\n");
    detect_words(g_envelope, g_frame_n);
    printf("Detected %d word segments\n\n", g_n_words);

    // Word-level analysis
    for (int w = 0; w < g_n_words; w++) {
        WordSegment *s = &g_words[w];
        s->active_bands = 0;
        s->band_energy[0] = s->band_energy[1] = s->band_energy[2] = 0;
        s->avg_energy = 0;
        s->addr_first = s->addr_last = -1;

        int nf = s->end_frame - s->start_frame + 1;
        if (nf <= 0) continue;

        for (int f = s->start_frame; f <= s->end_frame && f < g_frame_n; f++) {
            for (int b = 0; b < 3; b++) s->band_energy[b] += g_band_energy[f][b];
            s->avg_energy += g_envelope[f];
        }
        for (int b = 0; b < 3; b++) {
            s->band_energy[b] /= nf;
            if (s->band_energy[b] > 0.01) s->active_bands |= (1 << b);
        }
        s->avg_energy /= nf;

        // Address range for this word
        for (int f = s->start_frame; f <= s->end_frame && f < g_frame_n; f++) {
            int ring;
            int a = polar_addr(f, g_frame_n, g_band_energy[f], &ring);
            if (s->addr_first < 0) s->addr_first = a;
            s->addr_last = a;
        }
    }

    // === REPORTS ===
    printf("=== ADDRESS FILL RATE ===\n");
    int filled = 0;
    for (int i = 0; i < GEO_SPACE; i++) if (g_addr_hit[i] > 0) filled++;
    printf("Total: %d | Filled: %d (%.1f%%) | Empty: %d (%.1f%%)\n\n",
           GEO_SPACE, filled, filled*100.0/GEO_SPACE, GEO_SPACE-filled, (GEO_SPACE-filled)*100.0/GEO_SPACE);

    printf("=== 3-BAND DISTRIBUTION ===\n");
    int bf[3] = {0}, bh[3] = {0};
    for (int i = 0; i < GEO_SPACE; i++) {
        int r = i % RADII;
        int b = r <= R_INNER_END ? 0 : r <= R_MID_END ? 1 : 2;
        if (g_addr_hit[i] > 0) { bf[b]++; bh[b] += g_addr_hit[i]; }
    }
    printf("Ring 1 (inner, r=0-47):    %d filled / 6912 (%.1f%%)\n", bf[0], bf[0]*100.0/6912);
    printf("Ring 2 (mid,   r=48-95):   %d filled / 6912 (%.1f%%)\n", bf[1], bf[1]*100.0/6912);
    printf("Ring 3 (outer, r=96-143):  %d filled / 6912 (%.1f%%)\n\n", bf[2], bf[2]*100.0/6912);

    printf("=== ANGLE DISTRIBUTION ===\n");
    int active_ang = 0;
    for (int a = 0; a < ANGLES; a++) if (g_angle_hit[a] > 0) active_ang++;
    printf("Active angles: %d / 144 (%.1f%%)\n\n", active_ang, active_ang*100.0/144);

    printf("=== WORD-LEVEL POLAR PATTERNS ===\n");
    printf("Total: %d words\n\n", g_n_words);
    if (g_n_words > 0) {
        printf("Word  Start  End   Peak  PeakA  AvgE  Bands     AddrRange\n");
        printf("----  -----  ----  ----  -----  ----  --------- ----------\n");
        int show = g_n_words < 40 ? g_n_words : 40;
        for (int w = 0; w < show; w++) {
            WordSegment *s = &g_words[w];
            const char *ring = "???";
            if (s->active_bands == 1) ring = "LOW";
            else if (s->active_bands == 2) ring = "MID";
            else if (s->active_bands == 4) ring = "HIGH";
            else if (s->active_bands == 3) ring = "LOW+MID";
            else if (s->active_bands == 5) ring = "LOW+HIGH";
            else if (s->active_bands == 6) ring = "MID+HIGH";
            else if (s->active_bands == 7) ring = "ALL";
            printf("%3d   %5d  %4d  %4d  %5.3f  %4.3f  %s  %d-%d\n",
                   w, s->start_frame, s->end_frame, s->peak_frame,
                   s->peak_amp, s->avg_energy, ring, s->addr_first, s->addr_last);
        }
        if (g_n_words > 40) printf("... and %d more\n", g_n_words - 40);
        printf("\n");
    }

    printf("=== SUMMARY ===\n");
    printf("Address space: %d (144×144)\n", GEO_SPACE);
    printf("Fill rate: %d / %d (%.1f%%)\n", filled, GEO_SPACE, filled*100.0/GEO_SPACE);
    printf("Words: %d (%.1f/sec)\n", g_n_words, g_n_words / duration);
    printf("3 rings: inner=%d mid=%d outer=%d hits\n", bh[0], bh[1], bh[2]);

    free(samples);
    return 0;
}

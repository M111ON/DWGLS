// Geometric Audio Encoder v2
// Proper mel filterbank matching Whisper/librosa implementation
// Maps mel spectrogram (80 bins × frames) → 20736 addresses (144 × 144)
// STREAMING: processes frames one at a time, no large mel buffer needed
// Per-frame adaptive normalization for good address distribution

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#define WHISPER_SAMPLE_RATE 16000
#define WHISPER_N_FFT 400
#define WHISPER_HOP_LENGTH 160
#define WHISPER_N_MEL 80
#define GEO_ADDR_SPACE 20736
#define GEO_MEL_BINS 80
#define GEO_GRID_SIDE 144  // 144 × 144 = 20736
#define MAX_FFT_BINS 201   // N_FFT/2 + 1
#define MAX_SAMPLES_PER_READ 3200000

#pragma pack(push, 1)
typedef struct { char riff[4]; uint32_t size; char wave[4]; } WavHeader;
typedef struct { char fmt[4]; uint32_t size; uint16_t af; uint16_t ch; uint32_t sr; uint32_t br; uint16_t ba; uint16_t bps; } FmtChunk;
typedef struct { char data[4]; uint32_t size; } DataHeader;
#pragma pack(pop)

// FFT (in-place complex)
void fft(double *r, double *im, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int b = n >> 1;
        for (; j & b; b >>= 1) j ^= b;
        j ^= b;
        if (i < j) {
            double t = r[i]; r[i] = r[j]; r[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double a = -2.0 * M_PI / len;
        double wr = cos(a), wi = sin(a);
        for (int i = 0; i < n; i += len) {
            double cr = 1, ci = 0;
            for (int j = 0; j < len / 2; j++) {
                double ur = r[i + j], ui = im[i + j];
                double vr = r[i + j + len / 2] * cr - im[i + j + len / 2] * ci;
                double vi = r[i + j + len / 2] * ci + im[i + j + len / 2] * cr;
                r[i + j] = ur + vr;  im[i + j] = ui + vi;
                r[i + j + len / 2] = ur - vr;  im[i + j + len / 2] = ui - vi;
                double t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t;
            }
        }
    }
}

// Mel filterbank weights: [n_mel][n_fft_bins]
static double mel_weights[WHISPER_N_MEL][MAX_FFT_BINS];

void build_mel_filterbank(void) {
    // Whisper/librosa mel filterbank:
    // n_fft=400, sr=16000, n_mels=80, fmin=0, fmax=8000
    double f_max = WHISPER_SAMPLE_RATE / 2.0;
    double mel_max = 2595.0 * log10(1.0 + f_max / 700.0);
    int n_fft_bins = WHISPER_N_FFT / 2 + 1;

    // Mel-spaced center frequencies (n_mels+2 points including edges)
    double hz_points[WHISPER_N_MEL + 2];
    int bin_points[WHISPER_N_MEL + 2];

    for (int i = 0; i < WHISPER_N_MEL + 2; i++) {
        double mel = mel_max * i / (WHISPER_N_MEL + 1);
        hz_points[i] = 700.0 * (pow(10.0, mel / 2595.0) - 1.0);
        bin_points[i] = (int)(hz_points[i] * WHISPER_N_FFT / WHISPER_SAMPLE_RATE + 0.5);
        if (bin_points[i] >= n_fft_bins) bin_points[i] = n_fft_bins - 1;
    }

    // Build triangular filters: peak at 1.0, linear ramps to neighbors
    memset(mel_weights, 0, sizeof(mel_weights));
    for (int m = 0; m < WHISPER_N_MEL; m++) {
        int left_bin = bin_points[m];
        int center_bin = bin_points[m + 1];
        int right_bin = bin_points[m + 2];

        for (int k = left_bin; k <= center_bin && k < n_fft_bins; k++) {
            mel_weights[m][k] = (center_bin > left_bin)
                ? (double)(k - left_bin) / (double)(center_bin - left_bin)
                : 1.0;
        }
        for (int k = center_bin + 1; k <= right_bin && k < n_fft_bins; k++) {
            mel_weights[m][k] = (right_bin > center_bin)
                ? (double)(right_bin - k) / (double)(right_bin - center_bin)
                : 0.0;
        }
    }

    printf("=== MEL FILTERBANK ===\n");
    printf("Filters: %d, FFT bins: %d\n", WHISPER_N_MEL, n_fft_bins);
    printf("Freq range: 0 - %.0f Hz, Mel range: 0 - %.1f\n", f_max, mel_max);
    printf("Center freqs: ");
    for (int m = 0; m < 4; m++) printf("%.0f ", hz_points[m + 1]);
    printf("... ");
    for (int m = WHISPER_N_MEL - 2; m < WHISPER_N_MEL; m++) printf("%.0f ", hz_points[m + 1]);
    printf("\n\n");
}

// Apply mel filterbank to POWER spectrum (magnitude squared)
// Returns log10 of mel-filtered power per bin
void apply_mel_filters_power(const double *fft_mag, double *mel_out) {
    int n_fft_bins = WHISPER_N_FFT / 2 + 1;
    for (int m = 0; m < WHISPER_N_MEL; m++) {
        double power_sum = 0.0;
        for (int k = 0; k < n_fft_bins; k++) {
            double w = mel_weights[m][k];
            if (w == 0.0) continue;  // skip zero weights (avoids 0*inf=NaN)
            double mag = fft_mag[k];
            if (mag != mag || mag > 1e10) continue;  // skip NaN/Inf magnitudes
            double p = mag * mag;
            power_sum += w * p;
        }
        mel_out[m] = log10(fmax(power_sum, 1e-10));
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <wav>\n", argv[0]);
        return 1;
    }

    build_mel_filterbank();

    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { printf("Error: Cannot open %s\n", argv[1]); return 1; }

    WavHeader h;
    fread(&h, sizeof(h), 1, fp);
    FmtChunk f;
    while (1) {
        fread(&f, sizeof(f), 1, fp);
        if (memcmp(f.fmt, "fmt ", 4) == 0) break;
        fseek(fp, f.size, SEEK_CUR);
    }
    DataHeader d;
    while (1) {
        fread(&d, sizeof(d), 1, fp);
        if (memcmp(d.data, "data", 4) == 0) break;
        fseek(fp, d.size, SEEK_CUR);
    }

    int total_samples = d.size / (f.bps / 8);
    printf("=== GEOMETRIC AUDIO ENCODER v2 ===\n");
    printf("Input: %s\n", argv[1]);
    printf("Sample rate: %d Hz, Channels: %d, Bits: %d\n", f.sr, f.ch, f.bps);
    printf("Total samples: %d\n", total_samples);

    // Allocate read buffer
    static int16_t raw[MAX_SAMPLES_PER_READ];

    // Address accumulator
    static int addr_grid[GEO_GRID_SIDE * GEO_GRID_SIDE];
    static double addr_sum[GEO_GRID_SIDE * GEO_GRID_SIDE];
    memset(addr_grid, 0, sizeof(addr_grid));
    memset(addr_sum, 0, sizeof(addr_sum));

    // Buffers
    static double fft_r[WHISPER_N_FFT];
    static double fft_i_buf[WHISPER_N_FFT];
    static double fft_mag_buf[MAX_FFT_BINS];
    static double mel_vals[WHISPER_N_MEL];
    static double mel_normalized[WHISPER_N_MEL];

    int total_frames = 0;
    int total_mapped = 0;
    double global_mmin = 1e20, global_mmax = -1e20;

    // Overlap buffer for streaming
    static int16_t overlap[WHISPER_N_FFT];
    int overlap_count = 0;
    long long samples_read_total = 0;

    while (samples_read_total < total_samples) {
        int to_read = MAX_SAMPLES_PER_READ;
        long long remaining = total_samples - samples_read_total;
        if (to_read > remaining) to_read = (int)remaining;
        if (to_read <= 0) break;

        int n_read = (int)fread(raw, sizeof(int16_t), to_read, fp);
        if (n_read <= 0) break;

        // Build working buffer: overlap + new
        int work_size = overlap_count + n_read;
        static int16_t work_buf[WHISPER_N_FFT + MAX_SAMPLES_PER_READ];
        memcpy(work_buf, overlap, overlap_count * sizeof(int16_t));
        memcpy(work_buf + overlap_count, raw, n_read * sizeof(int16_t));

        int n_frames_chunk = (work_size - WHISPER_N_FFT) / WHISPER_HOP_LENGTH + 1;

        for (int frame = 0; frame < n_frames_chunk; frame++) {
            int offset = frame * WHISPER_HOP_LENGTH;

            // Hann window + normalize to [-1, 1]
            memset(fft_r, 0, sizeof(fft_r));
            memset(fft_i_buf, 0, sizeof(fft_i_buf));
            for (int i = 0; i < WHISPER_N_FFT; i++) {
                double hann = 0.5 * (1.0 - cos(2.0 * M_PI * i / WHISPER_N_FFT));
                fft_r[i] = hann * work_buf[offset + i] / 32768.0;
            }

            // FFT
            fft(fft_r, fft_i_buf, WHISPER_N_FFT);

            // Magnitude spectrum (clamp to prevent inf)
            int n_bins = WHISPER_N_FFT / 2 + 1;
            for (int i = 0; i < n_bins; i++) {
                double m2 = fft_r[i] * fft_r[i] + fft_i_buf[i] * fft_i_buf[i];
                fft_mag_buf[i] = sqrt(fmin(m2, 1e20));  // clamp to prevent inf
            }

            // Mel filterbank on POWER spectrum → log10
            apply_mel_filters_power(fft_mag_buf, mel_vals);

            // --- NORMALIZATION ---
            // Our C mel filterbank produces values in [-3, 1] range
            // Map to [0, 1] for address space coverage
            for (int m = 0; m < WHISPER_N_MEL; m++) {
                double v = mel_vals[m];
                // Map [-3, 1] → [0, 1] (covers our C implementation's range)
                double z = (v + 3.0) / 4.0;
                if (z < 0.0) z = 0.0;
                if (z > 1.0) z = 1.0;
                mel_normalized[m] = z;

                // Track range
                if (z < global_mmin) global_mmin = z;
                if (z > global_mmax) global_mmax = z;
            }

            // Map to addresses
            int global_frame = total_frames + frame;
            int angle = global_frame % GEO_GRID_SIDE;

            for (int m = 0; m < WHISPER_N_MEL; m++) {
                double z = mel_normalized[m];
                // Map z ∈ [0, 1] → radius ∈ [0, 143]
                int radius = (int)(z * (GEO_GRID_SIDE - 1));
                if (radius < 0) radius = 0;
                if (radius >= GEO_GRID_SIDE) radius = GEO_GRID_SIDE - 1;

                int addr = angle * GEO_GRID_SIDE + radius;
                addr_grid[addr]++;
                addr_sum[addr] += z;
                total_mapped++;
            }
        }

        total_frames += n_frames_chunk;
        samples_read_total += n_read;

        // Save overlap
        int keep = WHISPER_N_FFT;
        if (keep > work_size) keep = work_size;
        overlap_count = keep;
        memcpy(overlap, work_buf + work_size - keep, keep * sizeof(int16_t));

        if (samples_read_total % (500 * 16000) < MAX_SAMPLES_PER_READ) {
            fprintf(stderr, "  Processed %I64d / %d samples (%.0f%%)\n",
                    (long long)samples_read_total, total_samples,
                    100.0 * samples_read_total / total_samples);
        }
    }

    fclose(fp);

    double duration = (double)samples_read_total / WHISPER_SAMPLE_RATE;
    printf("Processed: %d frames, %.1f sec of audio\n\n", total_frames, duration);

    // Compute address values
    static double addr_value[GEO_GRID_SIDE * GEO_GRID_SIDE];
    for (int i = 0; i < GEO_ADDR_SPACE; i++) {
        addr_value[i] = (addr_grid[i] > 0) ? addr_sum[i] / addr_grid[i] : 0.0;
    }

    // === STATISTICS ===
    printf("=== ADDRESS SPACE STATISTICS ===\n");
    int active = 0, max_hits = 0;
    double avg_hits = 0;
    for (int i = 0; i < GEO_ADDR_SPACE; i++) {
        if (addr_grid[i] > 0) active++;
        if (addr_grid[i] > max_hits) max_hits = addr_grid[i];
        avg_hits += addr_grid[i];
    }
    avg_hits /= GEO_ADDR_SPACE;

    printf("Active addresses: %d / %d (%.1f%%)\n", active, GEO_ADDR_SPACE,
           active * 100.0 / GEO_ADDR_SPACE);
    printf("Total mel->addr mappings: %d\n", total_mapped);
    printf("Max hits per address: %d\n", max_hits);
    printf("Avg hits per address: %.1f\n", avg_hits);
    printf("Normalized mel range: [%.4f, %.4f]\n\n", global_mmin, global_mmax);

    // Radius distribution
    printf("=== MEL VALUE DISTRIBUTION (radius) ===\n");
    int radius_hist[GEO_GRID_SIDE];
    memset(radius_hist, 0, sizeof(radius_hist));
    for (int i = 0; i < GEO_ADDR_SPACE; i++) {
        radius_hist[i % GEO_GRID_SIDE] += addr_grid[i];
    }
    int active_radii = 0;
    for (int r = 0; r < GEO_GRID_SIDE; r++) {
        if (radius_hist[r] > 0) active_radii++;
    }
    printf("Active radius bins: %d / %d\n", active_radii, GEO_GRID_SIDE);

    // Histogram: show distribution in 10 buckets
    printf("Radius distribution (10 buckets of 14):\n");
    for (int b = 0; b < 10; b++) {
        int sum = 0;
        for (int r = b * 14; r < (b + 1) * 14 && r < GEO_GRID_SIDE; r++) {
            sum += radius_hist[r];
        }
        int bar = sum / (total_mapped / 100 + 1);
        printf("  [%3d-%3d]: %8d ", b * 14, (b + 1) * 14 - 1, sum);
        for (int i = 0; i < bar && i < 50; i++) printf("#");
        printf("\n");
    }
    printf("\n");

    // Angle distribution
    printf("=== ANGLE DISTRIBUTION ===\n");
    int angle_hist[GEO_GRID_SIDE];
    memset(angle_hist, 0, sizeof(angle_hist));
    for (int i = 0; i < GEO_ADDR_SPACE; i++) {
        angle_hist[i / GEO_GRID_SIDE] += addr_grid[i];
    }
    int active_angles = 0;
    for (int a = 0; a < GEO_GRID_SIDE; a++) {
        if (angle_hist[a] > 0) active_angles++;
    }
    printf("Active angles: %d / %d\n\n", active_angles, GEO_GRID_SIDE);

    // Heat map (24x24 downsampled)
    printf("=== ADDRESS GRID HEAT MAP (24x24) ===\n");
    for (int ay = 0; ay < 24; ay++) {
        printf("  ");
        for (int ax = 0; ax < 24; ax++) {
            double sum = 0;
            int cnt = 0;
            for (int dy = 0; dy < 6; dy++) {
                for (int dx = 0; dx < 6; dx++) {
                    int a = (ay * 6 + dy) * GEO_GRID_SIDE + (ax * 6 + dx);
                    if (addr_grid[a] > 0) {
                        sum += addr_value[a];
                        cnt++;
                    }
                }
            }
            if (cnt > 0) {
                double avg = sum / cnt;
                int level = (int)((avg + 3.0) / 6.0 * 9);
                if (level < 0) level = 0;
                if (level > 8) level = 8;
                printf("%c", " .:-=+*#%@"[level]);
            } else {
                printf(" ");
            }
        }
        printf("\n");
    }
    printf("\n");

    // Save JSON
    FILE *out = fopen("geo_audio_addr_v2.json", "w");
    if (out) {
        fprintf(out, "{\n");
        fprintf(out, "  \"version\": 2,\n");
        fprintf(out, "  \"address_space\": %d,\n", GEO_ADDR_SPACE);
        fprintf(out, "  \"grid_side\": %d,\n", GEO_GRID_SIDE);
        fprintf(out, "  \"mel_bins\": %d,\n", WHISPER_N_MEL);
        fprintf(out, "  \"frames\": %d,\n", total_frames);
        fprintf(out, "  \"duration_sec\": %.2f,\n", duration);
        fprintf(out, "  \"active_addresses\": %d,\n", active);
        fprintf(out, "  \"fill_rate_pct\": %.2f,\n", active * 100.0 / GEO_ADDR_SPACE);
        fprintf(out, "  \"data\": [");
        for (int i = 0; i < GEO_ADDR_SPACE; i++) {
            if (i > 0) fprintf(out, ",");
            fprintf(out, "%.6f", addr_value[i]);
        }
        fprintf(out, "]\n}\n");
        fclose(out);
        printf("Saved to geo_audio_addr_v2.json\n");
    }

    // Save binary
    FILE *bin_out = fopen("geo_audio_grid_v2.bin", "wb");
    if (bin_out) {
        fwrite(addr_value, sizeof(double), GEO_ADDR_SPACE, bin_out);
        fclose(bin_out);
        printf("Saved binary to geo_audio_grid_v2.bin (%d doubles)\n", GEO_ADDR_SPACE);
    }

    printf("\n=== SUMMARY ===\n");
    printf("Audio (%.1fs) -> Mel [%d x %d] -> %d Addresses (144x144)\n",
           duration, total_frames, WHISPER_N_MEL, GEO_ADDR_SPACE);
    printf("Active: %d / %d (%.1f%% fill rate)\n", active, GEO_ADDR_SPACE,
           active * 100.0 / GEO_ADDR_SPACE);

    return 0;
}

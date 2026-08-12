// Geometric Speech Segmentation - Peak Detection
// Detect word boundaries from acceleration pattern

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#pragma pack(push, 1)
typedef struct { char riff[4]; uint32_t size; char wave[4]; } WavHeader;
typedef struct { char fmt[4]; uint32_t size; uint16_t af; uint16_t ch; uint32_t sr; uint32_t br; uint16_t ba; uint16_t bps; } FmtChunk;
typedef struct { char data[4]; uint32_t size; } DataHeader;
#pragma pack(pop)

typedef struct {
    int position;    // sample index
    double time;     // seconds
    double amplitude;// peak amplitude
    double energy;   // local energy
} Peak;

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <wav_file> [seconds]\n", argv[0]);
        return 1;
    }
    
    const char *filename = argv[1];
    int duration_sec = (argc > 2) ? atoi(argv[2]) : 5;
    
    FILE *fp = fopen(filename, "rb");
    if (!fp) { printf("Error: Cannot open %s\n", filename); return 1; }
    
    WavHeader header;
    fread(&header, sizeof(WavHeader), 1, fp);
    
    FmtChunk fmt;
    while (1) {
        fread(&fmt, sizeof(FmtChunk), 1, fp);
        if (memcmp(fmt.fmt, "fmt ", 4) == 0) break;
        fseek(fp, fmt.size, SEEK_CUR);
    }
    
    DataHeader data_hdr;
    while (1) {
        fread(&data_hdr, sizeof(DataHeader), 1, fp);
        if (memcmp(data_hdr.data, "data", 4) == 0) break;
        fseek(fp, data_hdr.size, SEEK_CUR);
    }
    
    int total_samples = data_hdr.size / (fmt.bps / 8);
    int samples_to_read = fmt.sr * duration_sec;
    if (samples_to_read > total_samples) samples_to_read = total_samples;
    
    // Start from middle of file
    int skip_samples = total_samples / 2;
    fseek(fp, skip_samples * (fmt.bps / 8), SEEK_CUR);
    
    int16_t *samples = (int16_t *)malloc(samples_to_read * sizeof(int16_t));
    fread(samples, sizeof(int16_t), samples_to_read, fp);
    fclose(fp);
    
    printf("=== GEOMETRIC SPEECH SEGMENTATION ===\n");
    printf("File: %s\n", filename);
    printf("Duration: %d seconds\n", duration_sec);
    printf("Samples: %d\n", samples_to_read);
    printf("Sample rate: %d Hz\n", fmt.sr);
    printf("\n");
    
    // Calculate acceleration (derivative)
    float *accel = (float *)malloc(samples_to_read * sizeof(float));
    for (int i = 1; i < samples_to_read; i++) {
        accel[i] = (float)(samples[i] - samples[i-1]);
    }
    accel[0] = 0;
    
    // Smooth acceleration (moving average)
    int window = fmt.sr / 100;  // 10ms window
    float *smooth_accel = (float *)malloc(samples_to_read * sizeof(float));
    for (int i = 0; i < samples_to_read; i++) {
        float sum = 0;
        int count = 0;
        for (int j = i - window/2; j <= i + window/2; j++) {
            if (j >= 0 && j < samples_to_read) {
                sum += accel[j];
                count++;
            }
        }
        smooth_accel[i] = sum / count;
    }
    
    // Detect peaks (acceleration changes from + to -)
    Peak *peaks = (Peak *)malloc(samples_to_read * sizeof(Peak));
    int num_peaks = 0;
    
    int min_distance = fmt.sr / 50;  // Minimum 20ms between peaks
    int last_peak_pos = -min_distance;
    
    for (int i = 1; i < samples_to_read - 1; i++) {
        // Peak detection: positive → negative
        if (smooth_accel[i-1] > 0 && smooth_accel[i] <= 0) {
            // Check minimum distance
            if (i - last_peak_pos >= min_distance) {
                // Check if amplitude is significant
                if (abs(samples[i]) > 500) {  // Threshold
                    peaks[num_peaks].position = i;
                    peaks[num_peaks].time = (double)i / fmt.sr;
                    peaks[num_peaks].amplitude = samples[i] / 32768.0;
                    
                    // Calculate local energy
                    float energy = 0;
                    int energy_window = fmt.sr / 20;  // 50ms window
                    for (int j = i - energy_window; j <= i + energy_window; j++) {
                        if (j >= 0 && j < samples_to_read) {
                            energy += samples[j] * samples[j];
                        }
                    }
                    peaks[num_peaks].energy = energy / (2 * energy_window);
                    
                    num_peaks++;
                    last_peak_pos = i;
                }
            }
        }
    }
    
    printf("=== PEAK DETECTION RESULTS ===\n");
    printf("Total peaks detected: %d\n", num_peaks);
    printf("Peak rate: %.1f peaks/second\n", (double)num_peaks / duration_sec);
    printf("\n");
    
    // Analyze peak distribution
    printf("=== PEAK STATISTICS ===\n");
    double total_energy = 0;
    double min_energy = 1e20, max_energy = 0;
    for (int i = 0; i < num_peaks; i++) {
        total_energy += peaks[i].energy;
        if (peaks[i].energy < min_energy) min_energy = peaks[i].energy;
        if (peaks[i].energy > max_energy) max_energy = peaks[i].energy;
    }
    printf("Average energy: %.0f\n", total_energy / num_peaks);
    printf("Min energy: %.0f\n", min_energy);
    printf("Max energy: %.0f\n", max_energy);
    printf("\n");
    
    // Output peaks as JSON
    printf("=== PEAKS JSON ===\n");
    printf("[\n");
    for (int i = 0; i < num_peaks && i < 100; i++) {  // Limit to 100
        if (i > 0) printf(",\n");
        printf("  {\"t\":%.3f, \"amp\":%.3f, \"energy\":%.0f}", 
               peaks[i].time, peaks[i].amplitude, peaks[i].energy);
    }
    printf("\n]\n");
    
    // Save peaks to file
    FILE *out = fopen("peaks.json", "w");
    if (out) {
        fprintf(out, "[\n");
        for (int i = 0; i < num_peaks; i++) {
            if (i > 0) fprintf(out, ",\n");
            fprintf(out, "  {\"t\":%.3f, \"amp\":%.3f, \"energy\":%.0f}", 
                   peaks[i].time, peaks[i].amplitude, peaks[i].energy);
        }
        fprintf(out, "\n]\n");
        fclose(out);
        printf("\nSaved to peaks.json\n");
    }
    
    free(samples);
    free(accel);
    free(smooth_accel);
    free(peaks);
    
    return 0;
}
